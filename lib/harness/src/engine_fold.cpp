#include "nimbus/harness/engine.h"
#include "engine_internal.h"   // trimInPlace - shared with engine.cpp

#include <string>
#include <vector>

#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"   // ScopedModelOverride - a rung on its own model
#include "nimbus/orch/compact.h"           // ORCH_COMPACT_PROMPT + buildCompactInputs + capSummary
#include "nimbus/orch/connector_grant.h"   // whose connectors a fold's request carries (CUM-828)
#include "nimbus/orch/reply_markup.h"      // cleanReplyMarkup - the summary passes the reply seam
#include "nimbus/orch/spend_ceiling.h"     // paidCallAllowed - the fold waits at the spend ceiling
#include "nimbus/orch/turn.h"              // parseHeadReply - the lenient summary read

// engine_fold - the TurnEngine's compaction fold: the fold host ladder, its gate and the
// fold call itself. Moved out of engine.cpp verbatim (lane P0 of the history fix, 16 Nov
// 2026) so the fold's owner edits this file alone; engine.h declares every function here.

namespace agent {

using detail::trimInPlace;

namespace {
// The model a fold request on `h` runs: a rung's pinned model, else the host's own.
std::string foldModelFor(const TurnEngine::Deps& d, const std::string& h) {
  const std::string pinned = providers::modelOverrideFor(h.c_str());
  if (!pinned.empty()) return pinned;
  return d.cfg.provider.orchModel ? d.cfg.provider.orchModel(h) : std::string();
}
}  // namespace

// Fold host candidates: explicit orchHost (if usable) first, then the priority
// list in order - each must exist, have a key, and be in budget. Bounded at 3.
// Shared by canFoldNow (any candidate => a fold can run) and runFold (the
// failover ladder walks this exact list).
std::vector<std::string> TurnEngine::foldHostCandidates() const {
  std::vector<std::string> out;
  auto usable = [&](const std::string& h) {
    if (h.empty() || !d_.hosts.has(h)) return false;
    if (d_.cfg.provider.hasKey && !d_.cfg.provider.hasKey(h)) return false;
    if (d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(h)) return false;
    return !d_.hosts.isParked(h);   // lane K: a host refused for billing or auth waits
  };
  auto add = [&](std::string h) {
    trimInPlace(h);
    if (!usable(h)) return;
    for (const auto& e : out)
      if (e == h) return;
    if (out.size() < 3) out.push_back(std::move(h));
  };
  if (d_.cfg.provider.orchHost) add(d_.cfg.provider.orchHost());
  std::string pr =
      d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
  size_t p = 0;
  while (p <= pr.size()) {
    size_t c = pr.find(',', p);
    add(pr.substr(p, (c == std::string::npos ? pr.size() : c) - p));
    if (c == std::string::npos) break;
    p = c + 1;
  }
  // CUM-288: with NO keyed BYOK head in the ladder, the verified router key
  // (Cumulo, then Z.ai) is the fallback head that runs the whole assistant -
  // exactly as runTurn resolves its head (a keyed BYOK head still wins, so this
  // only fires when the walk above found none). The router key is not a BYOK
  // slot (hasKey() does not report it), so it is gated by routerFallbackHost()
  // returning non-empty (its own key check) plus registered + in budget, NOT by
  // hasKey. Without this a Cumulo-only instance chats fine but never folds:
  // context grows unbounded until turns fail.
  if (out.empty() && d_.cfg.provider.routerFallbackHost) {
    std::string rf = d_.cfg.provider.routerFallbackHost();
    trimInPlace(rf);
    if (!rf.empty() && d_.hosts.has(rf) &&
        !(d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(rf)) && !d_.hosts.isParked(rf))
      out.push_back(std::move(rf));
  }
  return out;
}

bool TurnEngine::canFoldNow() const {
  if (turnInFlight_) return false;
  // CUM-404: the fold dials a provider, so it needs the largest-block floor too, not
  // just total free. Unwired closures read 0xFFFFFFFF => the guard is a host-test no-op.
  if (!turnHeapOk(freeHeap(), largestFreeBlock(), t_.autoTurnMinHeap, t_.turnMinLargestBlock))
    return false;
  return !foldHostCandidates().empty();
}

TurnEngine::FoldResult TurnEngine::runFold(const std::string& chatId,
                                           const std::string& prevSummary,
                                           const std::string& digest,
                                           std::string& outSummary,
                                           size_t* producedBytes) {
  return runFoldOn(chatId, prevSummary, digest, nullptr, outSummary, producedBytes);
}

TurnEngine::FoldResult TurnEngine::runFoldOn(const std::string& chatId, const std::string& prevSummary,
                                             const std::string& digest, const FoldRung* onlyRung,
                                             std::string& outSummary, size_t* producedBytes) {
  namespace orch = nimbus::orch;
  outSummary.clear();
  const orch::ConnectorGrantScope grant(orch::connectorGrantFor(d_.apply.whoFor(chatId)));   // CUM-828
  // Gate like an unattended turn: the fold is background work - defer, never
  // squeeze, under memory pressure. Defers are NOT failures (no breaker burn).
  if (!turnHeapOk(freeHeap(), largestFreeBlock(), t_.autoTurnMinHeap, t_.turnMinLargestBlock)) {
    hlog::logf("fold(%s): deferred, heap %u/floor %u largest %u/floor %u", chatId.c_str(),
               (unsigned)freeHeap(), (unsigned)t_.autoTurnMinHeap,
               (unsigned)largestFreeBlock(), (unsigned)t_.turnMinLargestBlock);
    return FoldResult::Deferred;
  }
  if (turnInFlight_) return FoldResult::Deferred;   // never interleave with a live turn
  // Lane DEF: at the monthly spend ceiling the fold waits (a defer, not a failure).
  if (!orch::paidCallAllowed(orch::SpendSeam::Fold)) {
    hlog::logf("fold(%s): deferred - %s", chatId.c_str(), orch::kSpendFrozenError);
    return FoldResult::Deferred;
  }

  // Failover ladder (field 2026-08-11): the fold used to pin ONE host - while
  // the head turn's fabric failed over openai->anthropic and succeeded, the
  // fold burned its breaker retrying the same dead provider and spammed the
  // owner. A fold is replay-safe by construction (the reply IS the summary; no
  // applyTurn - no device actions, no mem writes, no spawns), so walking the
  // same <=3-host candidate list the fabric uses is strictly better.
  const std::vector<std::string> hostsToTry =
      onlyRung ? std::vector<std::string>{onlyRung->host} : foldHostCandidates();
  if (hostsToTry.empty() || hostsToTry.front().empty()) return FoldResult::Deferred;

  // ALWAYS a fresh, throwaway provider thread: the fold must never ride or
  // extend the chat's chain (that is the thing being reset), and its convId is
  // never persisted.
  std::string outJson, err;
  std::string host;
  orch::TokenUsage usage;
  const std::string inputs = orch::buildCompactInputs(prevSummary, digest, foldDayContext(chatId));
  bool ok = false;
  // Every attempt's spend, failed ones included, filed under its own host and model and
  // metered once as "compact" (lane T: the same per-host rule and R4 cache split as a
  // turn). The turn's spend table is free here: a fold never runs while a turn does.
  orch::TurnSpend& foldSpend = facts().spend;
  foldSpend.clear();
  orch::Turn t;
  for (const auto& h : hostsToTry) {
    std::string convId;
    outJson.clear(); err.clear();
    orch::TokenUsage u;
    const providers::ScopedModelOverride rungModel(h, onlyRung ? onlyRung->model : std::string());
    ok = d_.hosts.run(h, convId, orch::ORCH_COMPACT_PROMPT, inputs,
                      outJson, err, /*tools=*/nullptr, &u);
    foldSpend.add(h.c_str(), foldModelFor(d_, h).c_str(), u);
    if (!ok) {
      d_.hosts.noteFailed(h, err);
      hlog::logf("fold(%s): %s failed: %s", chatId.c_str(), h.c_str(), err.c_str());
      continue;
    }
    d_.hosts.noteAnswered(h);
    // Same lenient read as a head turn (CUM-495): a prose summary IS the summary. An
    // answer with no summary in it is no fold: the next host gets the job (CUM-890),
    // and the log says what came back.
    t = orch::Turn();
    orch::ParseError perr;
    if (orch::parseHeadReply(outJson, t, perr) != orch::ReplyShape::Unusable && !t.reply.empty()) {
      host = h;
      usage = u;
      break;
    }
    ok = false;
    hlog::logf("fold(%s): no summary in reply on %s (%s, %u B answered)", chatId.c_str(), h.c_str(),
               perr.detail.empty() ? "the reply field was empty" : perr.detail.c_str(), (unsigned)outJson.size());
  }
  meterSpend(foldSpend, "compact");
  if (!ok) return FoldResult::Failed;
  // The reply IS the summary; every other field of the turn is deliberately
  // ignored (no applyTurn - device actions, mem writes, spawns are all inert).
  // The summary feeds every later prompt: a leaked tag here would come back in
  // later replies, so it passes the same reply seam (lane L, formatting F4).
  const std::string raw = orch::cleanReplyMarkup(t.reply);
  if (producedBytes) *producedBytes = raw.size();
  outSummary = orch::capSummary(raw);
  hlog::logf("fold(%s): ok on %s (%u in / %u out)", chatId.c_str(), host.c_str(),
             (unsigned)usage.promptTokens, (unsigned)usage.completionTokens);
  return FoldResult::Ok;
}

std::vector<FoldRung> TurnEngine::foldRungs() const {
  const std::string head = headHost();
  const std::string router = d_.cfg.provider.routerFallbackHost ? d_.cfg.provider.routerFallbackHost() : std::string();
  std::vector<FoldRung> rungs;
  for (const std::string& h : foldHostCandidates()) {
    FoldRung r;
    r.host = h;
    if (h == head) r.kind = FoldRungKind::HeadHost;
    else if (h == router) r.kind = FoldRungKind::RouterFallback;
    else r.kind = FoldRungKind::OtherHost;
    rungs.push_back(std::move(r));
  }
  return rungs;
}

TurnEngine::FoldResult TurnEngine::runFoldRung(const std::string& chatId, const std::string& prevSummary,
                                               const std::string& digest, const FoldRung& rung,
                                               std::string& outSummary, FoldRungReport* report) {
  size_t produced = 0;
  const FoldResult r = runFoldOn(chatId, prevSummary, digest, &rung, outSummary, &produced);
  if (report) {
    report->result = r;
    report->host = rung.host;
    report->model = rung.model;
    report->producedBytes = produced;
  }
  return r;
}

}  // namespace agent
