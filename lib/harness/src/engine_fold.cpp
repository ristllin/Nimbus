#include "nimbus/harness/engine.h"
#include "engine_internal.h"   // trimInPlace - shared with engine.cpp

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "nimbus/harness/log.h"
#include "nimbus/harness/output_caps.h"   // mistralHeadEffort - which heads are forced reasoners
#include "nimbus/harness/providers.h"   // ScopedModelOverride - a rung on its own model
#include "nimbus/orch/budget.h"            // deriveRequestContext - the head's summary cap (lane B)
#include "nimbus/orch/compact.h"
#include "nimbus/orch/model_family.h"    // mistralForcedReasoner - the fold's step-down           // ORCH_COMPACT_PROMPT + buildCompactInputs + capSummary
#include "nimbus/orch/request_budget.h"    // requestBudgetFor
#include "nimbus/orch/connector_grant.h"   // whose connectors a fold's request carries (CUM-828)
#include "nimbus/orch/head_loop.h"         // classifyStepError - a rung's failure kind
#include "nimbus/orch/provider_error.h"    // oneLineText - the rung line's detail
#include "nimbus/orch/reply_markup.h"      // cleanReplyMarkup - the summary passes the reply seam
#include "nimbus/orch/spend_ceiling.h"     // paidCallAllowed - the fold waits at the spend ceiling
#include "nimbus/orch/turn.h"              // parseHeadReply - the lenient summary read

// engine_fold - the TurnEngine's compaction fold: the fold ladder, its gate and the
// fold request itself. Moved out of engine.cpp by lane P0 of the history fix (16 Nov
// 2026); lane F builds the whole ladder here (fold_ladder.h) and paces a cycle in
// fold_driver.cpp.

namespace agent {

using detail::trimInPlace;
namespace orch = nimbus::orch;

namespace {
// The model a fold request on `h` runs: a rung's pinned model, else the host's own.
std::string foldModelFor(const TurnEngine::Deps& d, const std::string& h) {
  const std::string pinned = providers::modelOverrideFor(h.c_str());
  if (!pinned.empty()) return pinned;
  return d.cfg.provider.orchModel ? d.cfg.provider.orchModel(h) : std::string();
}

// The host's own model (no pin).
std::string ownModel(const TurnEngine::Deps& d, const std::string& h) {
  return d.cfg.provider.orchModel ? d.cfg.provider.orchModel(h) : std::string();
}

// The head's request context (the one seam, budget.h): the summary cap the fold writes
// for, the same the turn's prompt carries it at.
orch::ContextBudget headFoldContext(const TurnEngine::Deps& d) {
  const std::string host = detail::resolveTurnHost(d);
  return orch::headContextBudget(host, detail::headModelOf(d, host), detail::toolLoopSetting(d), {});
}

// A direct host: its requests read orchModel, so a rung there can run another model.
bool directHost(const std::string& h) { return h == "anthropic" || h == "openai" || h == "mistral"; }

std::string lowered(const std::string& s) {
  std::string o = s;
  std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return o;
}

// A wire that reads its own fold answer (Mistral's plain completion) reports a 2xx with
// no text as an error line: that is an empty answer, not a transport failure.
bool emptyAnswerError(const std::string& err) { return err.find("came back with no text") != std::string::npos; }

FoldFailKind failKindOf(const std::string& err) {
  if (emptyAnswerError(err)) return FoldFailKind::Empty;
  switch (orch::classifyStepError(err)) {
    case orch::StepErr::Timeout: return FoldFailKind::Timeout;
    case orch::StepErr::Billing:
    case orch::StepErr::Auth:
    case orch::StepErr::Paused: return FoldFailKind::Park;
    case orch::StepErr::RateLimited:
    case orch::StepErr::BadRequest:
    case orch::StepErr::Rejected: return FoldFailKind::Http4xx;
    case orch::StepErr::ServerError: return FoldFailKind::Http5xx;
    case orch::StepErr::Cut: return FoldFailKind::Cut;
    default: return FoldFailKind::Transport;
  }
}
}  // namespace

const char* foldFailKindName(FoldFailKind k) {
  switch (k) {
    case FoldFailKind::None: return "none";
    case FoldFailKind::Transport: return "transport";
    case FoldFailKind::Timeout: return "timeout";
    case FoldFailKind::Http4xx: return "4xx";
    case FoldFailKind::Park: return "402 park";
    case FoldFailKind::Http5xx: return "5xx";
    case FoldFailKind::Cut: return "cut twice";
    case FoldFailKind::Empty: return "empty";
    case FoldFailKind::Thin: return "thin";
  }
  return "unknown";
}

std::string foldModelForHead(const std::string& host, const std::string& model) {
  // Mistral directly, or a Mistral model on Cumulo's router ("mistral/<id>", the same
  // upstream and the same thinking).
  const bool routed = host == "cumulo" && model.rfind("mistral/", 0) == 0;
  if ((host != "mistral" && !routed) || model.empty()) return std::string();
  // A forced reasoner is a model that refuses effort "none" (Large 4 under every id Mistral
  // serves it as, mistral-large-latest included, and the hosted GLM): the one predicate
  // (model_family.h). Small folds at "none", well inside the 60 s gateway at a 53.9 KB step.
  if (!orch::mistralForcedReasoner(model)) return std::string();
  const std::string small = "mistral-small-latest";
  return routed ? "mistral/" + small : small;
}

int foldSectionsNamed(const std::string& summary) {
  // The prompt's five sections, each by a few lenient keywords: a model may number
  // them, bold them, rename them a little or translate the punctuation.
  static const char* const kSections[][3] = {
      {"decision", "preference", nullptr},
      {"pending", "next step", nullptr},
      {"fact", "long-term", "remember"},
      {"intent", "thread", nullptr},
      {"device", "task state", nullptr},
  };
  const std::string s = lowered(summary);
  int named = 0;
  for (const auto& sec : kSections)
    for (const char* k : sec)
      if (k && s.find(k) != std::string::npos) {
        named++;
        break;
      }
  return named;
}

bool foldAnswerThin(const std::string& summary, size_t digestBytes, size_t prevSummaryBytes) {
  if (digestBytes < kFoldQualityDigestMin) return false;   // a small digest may fold small
  const size_t floor = std::min(kFoldThinFloor, prevSummaryBytes * 30 / 100);   // 0 with no previous summary
  return summary.size() < floor || foldSectionsNamed(summary) < 2;
}

std::string foldRungFailLine(const std::string& chatId, size_t index, size_t count, const FoldRungReport& r) {
  return "fold(" + chatId + "): rung " + std::to_string(index + 1) + "/" + std::to_string(count) + " " + r.host + "/" +
         (r.model.empty() ? std::string("-") : r.model) + " failed -> " + foldFailKindName(r.fail) + " (" +
         r.detail + ")";
}

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
  // At the monthly spend ceiling every fold request would defer: say so here, once per
  // pass and silently, rather than from each rung's request (lane DEF).
  if (!nimbus::orch::paidCallAllowed(nimbus::orch::SpendSeam::Fold)) return false;
  return !foldRungs().empty();
}

TurnEngine::FoldResult TurnEngine::runFold(const std::string& chatId,
                                           const std::string& prevSummary,
                                           const std::string& digest,
                                           std::string& outSummary,
                                           size_t* producedBytes) {
  outSummary.clear();
  const std::vector<FoldRung> rungs = foldRungs();
  if (rungs.empty()) return FoldResult::Deferred;
  for (size_t i = 0; i < rungs.size(); i++) {
    FoldRungReport rep;
    const FoldResult r = runFoldRung(chatId, prevSummary, digest, rungs[i], outSummary, &rep);
    if (r == FoldResult::Ok) {
      if (producedBytes) *producedBytes = rep.producedBytes;
      return r;
    }
    if (r == FoldResult::Deferred) return r;   // a gate closed: not a failure
    hlog::log(foldRungFailLine(chatId, i, rungs.size(), rep).c_str());
  }
  return FoldResult::Failed;
}

TurnEngine::FoldResult TurnEngine::runFoldOn(const std::string& chatId, const std::string& prevSummary,
                                             const std::string& digest, const std::string& host,
                                             std::string& outSummary, FoldRungReport& rep) {
  outSummary.clear();
  rep = FoldRungReport();
  rep.host = host;
  const orch::ConnectorGrantScope grant(orch::connectorGrantFor(d_.apply.whoFor(chatId)));   // CUM-828
  // Gate like an unattended turn: the fold is background work - defer, never
  // squeeze, under memory pressure. Defers are NOT failures (no backoff, no count).
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
  // A fold is replay-safe by construction (the reply IS the summary; no applyTurn -
  // no device actions, no mem writes, no spawns), so a cycle may ask every keyed
  // provider in turn (fold_ladder.h). ALWAYS a fresh, throwaway provider thread: the
  // fold must never ride or extend the chat's chain (that is the thing being reset),
  // and its convId is never persisted. The head's summary cap (lane B: 6,144 B at a
  // 60k-token target and above, else 4,096 B) is named in the input when it is the larger one.
  const size_t cap = headFoldContext(d_).chatSummaryBytes;
  const std::string inputs = orch::buildCompactInputs(prevSummary, digest, foldDayContext(chatId), cap);
  // The attempt's spend, a failed one included, filed under its own host and model and
  // metered once as "compact" (lane T: the same per-host rule and R4 cache split as a
  // turn). The turn's spend table is free here: a fold never runs while a turn does.
  orch::TurnSpend& foldSpend = facts().spend;
  foldSpend.clear();
  std::string convId, outJson, err;
  orch::TokenUsage u;
  const uint32_t t0 = nowMs();
  const bool ok = d_.hosts.run(host, convId, orch::ORCH_COMPACT_PROMPT, inputs, outJson, err, /*tools=*/nullptr, &u);
  rep.elapsedMs = nowMs() - t0;
  rep.model = foldModelFor(d_, host);
  foldSpend.add(host.c_str(), rep.model.c_str(), u);
  meterSpend(foldSpend, "compact");
  rep.promptTokens = u.promptTokens;
  rep.completionTokens = u.completionTokens;
  // A 2xx counts even when its usage object was missing (the spend ceiling's rule).
  rep.paidRequests = u.responses ? u.responses : (ok ? 1u : 0u);
  if (!ok && orch::isTimeoutError(err)) rep.paidRequests++;
  if (!ok) {
    d_.hosts.noteFailed(host, err);
    rep.fail = failKindOf(err);
    rep.degraded = rep.fail == FoldFailKind::Empty;   // a 2xx with nothing in it goes last next cycle
    rep.detail = orch::oneLineText(err, 160);
    rep.result = FoldResult::Failed;
    return FoldResult::Failed;
  }
  d_.hosts.noteAnswered(host);
  // Same lenient read as a head turn (CUM-495): a prose summary IS the summary. An
  // answer with no summary in it, or too thin to be one, is no fold: the next rung
  // gets the job (CUM-890), and this (host, model) goes last next cycle.
  orch::Turn t;
  orch::ParseError perr;
  if (orch::parseHeadReply(outJson, t, perr) == orch::ReplyShape::Unusable || t.reply.empty()) {
    rep.fail = FoldFailKind::Empty;
    rep.degraded = true;
    rep.detail = "no summary in reply: " + (perr.detail.empty() ? std::string("the reply field was empty")
                                                                : orch::oneLineText(perr.detail, 100)) +
                 ", " + std::to_string(outJson.size()) + " B answered";
    rep.result = FoldResult::Failed;
    return FoldResult::Failed;
  }
  // The reply IS the summary; every other field of the turn is deliberately
  // ignored (no applyTurn - device actions, mem writes, spawns are all inert).
  // The summary feeds every later prompt: a leaked tag here would come back in
  // later replies, so it passes the same reply seam (lane L, formatting F4).
  const std::string raw = orch::cleanReplyMarkup(t.reply);
  if (foldAnswerThin(raw, digest.size(), prevSummary.size())) {
    rep.fail = FoldFailKind::Thin;
    rep.degraded = true;
    if (raw.size() >= kFoldThinFloor) rep.thinSummary = orch::capSummary(raw, cap);   // named too few sections only
    rep.detail = std::to_string(raw.size()) + " B naming " + std::to_string(foldSectionsNamed(raw)) +
                 " of 5 sections for a " + std::to_string(digest.size()) + " B digest";
    rep.result = FoldResult::Failed;
    return FoldResult::Failed;
  }
  rep.producedBytes = raw.size();
  outSummary = orch::capSummary(raw, cap);
  hlog::logf("fold(%s): ok on %s (%u in / %u out, %s)", chatId.c_str(), host.c_str(),
             (unsigned)u.promptTokens, (unsigned)u.completionTokens, rep.model.c_str());
  rep.result = FoldResult::Ok;
  return FoldResult::Ok;
}

namespace {
// The ladder as it is built: each (host, model) once, in the order added.
struct LadderBuild {
  const TurnEngine::Deps& d;
  std::vector<FoldRung> rungs;

  // In budget and not parked (lane K: a host refused for billing or auth waits).
  bool inBudget(const std::string& h) const {
    return !(d.cfg.budget.overBudget && d.cfg.budget.overBudget(h)) && !d.hosts.isParked(h);
  }
  // A keyed provider: registered, keyed, in budget and not parked.
  bool keyed(const std::string& h) const {
    return !h.empty() && d.hosts.has(h) && (!d.cfg.provider.hasKey || d.cfg.provider.hasKey(h)) && inBudget(h);
  }
  void add(std::string h, std::string model, FoldRungKind kind) {
    if (model.empty()) model = ownModel(d, h);
    // Never a forced reasoner on any rung (lane W's live legs: Large 4 folding on itself is
    // cut at its fit): its host's non-reasoning model instead, deduplicated below.
    const std::string alt = foldModelForHead(h, model);
    if (!alt.empty()) model = alt;
    for (const FoldRung& r : rungs)
      if (r.host == h && r.model == model) return;   // no (host, model) twice
    rungs.push_back(FoldRung{std::move(h), std::move(model), kind});
  }
  bool isRouterHead(const std::string& head, const std::string& router) const {
    if (head.empty() || head != router) return false;
    return !(d.cfg.provider.hasKey && d.cfg.provider.hasKey(head));
  }
  void addRouter(const std::string& router) {
    if (!router.empty() && d.hosts.has(router) && inBudget(router)) add(router, std::string(), FoldRungKind::RouterFallback);
  }
  void addHead(const std::string& head, bool isRouter) {
    if (isRouter) return addRouter(head);
    if (keyed(head)) add(head, std::string(), FoldRungKind::HeadHost);   // add() steps a forced reasoner down
  }
  void addSub(const std::string& head) {
    if (!directHost(head) || !keyed(head) || !d.cfg.provider.subModel) return;
    // A forced-reasoner head already stepped down on its first rung (Large 4 -> Small): its
    // only rung on that host is that one.
    if (!foldModelForHead(head, ownModel(d, head)).empty()) return;
    add(head, d.cfg.provider.subModel(head), FoldRungKind::SubModel);
  }
};

// The owner's order of providers: the pinned head, then the priority list, and nothing else
// (privacy ruling, lane CTXR1): a keyed provider or a custom endpoint the owner left out of
// the list (a key kept for voice, embeddings or sub-agents) never receives the digest.
std::vector<std::string> ownerOrder(const ProviderConfig& p) {
  std::vector<std::string> order;
  if (p.orchHost) order.push_back(p.orchHost());
  const std::string pr = p.providerPriority ? p.providerPriority() : std::string();
  for (size_t at = 0; at <= pr.size();) {
    const size_t comma = pr.find(',', at);
    order.push_back(pr.substr(at, (comma == std::string::npos ? pr.size() : comma) - at));
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  for (std::string& h : order) trimInPlace(h);
  return order;
}
}  // namespace

std::vector<FoldRung> TurnEngine::foldRungs() const {
  const auto& p = d_.cfg.provider;
  LadderBuild b{d_, {}};
  const std::string head = headHost();
  std::string router = p.routerFallbackHost ? p.routerFallbackHost() : std::string();
  trimInPlace(router);
  // 1. The head host on its fold model. A head that is the router fallback (no
  //    provider key set: CUM-288, the verified router key runs the assistant) is
  //    gated by its own key check, not by hasKey.
  const bool headIsRouter = b.isRouterHead(head, router);
  b.addHead(head, headIsRouter);
  // 2. Every other keyed provider in the owner's priority list, in its order (the owner's
  //    consent: "every provider in your priority list"; Cumulo only with a Cumulo key).
  for (const std::string& h : ownerOrder(p))
    if (h != head && b.keyed(h)) b.add(h, std::string(), FoldRungKind::OtherHost);
  // 3. The router fallback: when no provider key is set (CUM-288), or when the router is
  //    Cumulo and the device holds a Cumulo key (RULINGS-v3 item 15).
  if (b.rungs.empty() || (router == "cumulo" && b.keyed(router))) b.addRouter(router);
  // 4. The head host on its Sub model, last (a direct host only: the pin needs one).
  if (!headIsRouter) b.addSub(head);
  return b.rungs;
}

TurnEngine::FoldResult TurnEngine::runFoldRung(const std::string& chatId, const std::string& prevSummary,
                                               const std::string& digest, const FoldRung& rung,
                                               std::string& outSummary, FoldRungReport* report) {
  // Only a host the ladder holds now (keyed, in budget, not parked), on the rung's model
  // while the request runs (a direct host's; any other runs on its own model).
  outSummary.clear();
  FoldRungReport rep;
  rep.host = rung.host;
  rep.model = rung.model;
  FoldResult r = FoldResult::Deferred;
  const std::vector<FoldRung> ladder = foldRungs();
  const bool held = std::any_of(ladder.begin(), ladder.end(), [&](const FoldRung& l) { return l.host == rung.host; });
  if (held) {
    const bool pin = !rung.model.empty() && rung.model != ownModel(d_, rung.host);
    const providers::ScopedModelOverride scope(pin ? rung.host : std::string(), pin ? rung.model : std::string());
    r = runFoldOn(chatId, prevSummary, digest, rung.host, outSummary, rep);
  }
  rep.result = r;
  if (report) *report = rep;
  return r;
}

}  // namespace agent
