#include "nimbus/harness/engine.h"

#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/turn_anatomy.h"   // measureHistory / providerChainHeld (/api/lastturn)

#include "nimbus/orch/budget.h"    // deriveBudget - per-turn caps from the head model's window
#include "nimbus/orch/compact.h"   // ORCH_COMPACT_PROMPT + buildCompactInputs + capSummary (runFold)

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>   // std::bad_alloc: a failed allocation ends a turn honestly
#include <set>

#include "nimbus/mem_cap.h"              // utf8CapLen - the owner notice's cause cap
#include "nimbus/orch/billing_refusal.h"  // billingNoticeDue / billingNotice - one notice per episode
#include "nimbus/orch/provider_park.h"     // recheckInMs - the wait the short billing answer names
#include "nimbus/orch/fallback_rules.h"  // CUM-41 unified fallback engine
#include "nimbus/orch/model_catalog.h"   // modelSizeClass for the size-class predicate
#include "nimbus/orch/models_tool.h"     // R5: the [AVAILABLE MODELS] pointer line
#include "nimbus/orch/provider_slots.h"  // CUM-486: [AVAILABLE MODELS] walks the registry
#include "nimbus/orch/connectors_wire.h"  // resolveHeadHost - the one head-host rule
#include "nimbus/orch/servedby.h"        // CUM-236 served-by fallback disclosure
#include "nimbus/orch/served_model.h"    // the per-turn line: the model this request runs on (HFM)
#include "nimbus/orch/model_defaults.h"  // lane DEF: background turns run on the cheapest tier
#include "nimbus/orch/spend_ceiling.h"   // lane DEF: the paid-call gate (monthly spend ceiling)
#include "nimbus/orch/usage_ledger.h"    // ledgerDelta - a spend slot in ledger tokens

#include "nimbus/harness/conversation.h"   // histV2: historyTurns / fitHistory / foldSlice
#include "nimbus/harness/dream.h"      // the dream turn's memory-only surface (lane M)
#include "nimbus/harness/turn_inbox.h"     // histV2: what the channel says the sender sent
#include "nimbus/orch/turn_images.h"       // R1: the turn's photos, shown to the head as images
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"   // lastFabricSwitch - the switch notice cause
#include "nimbus/harness/rate_limit.h"   // CUM-460 honest 429 reply
#include "nimbus/harness/router_refusal.h"   // a router refusal is not an outage (failover notice)
#include "nimbus/harness/turn_contract.h"    // lane L: pseudo-tool salvage, final check, salvage
#include "nimbus/harness/turn_memory.h"      // a turn never spends the last internal SRAM (L24)
#include "nimbus/orch/reply_markup.h"        // lane L: cleanReplyMarkup at every reply seam
#include "nimbus/orch/tool_taint.h"          // lane L: the per-turn external-content flag
#include "nimbus/orch/turn.h"

// Lifted from src/agent/orchestrator.cpp (Stage G - the final lift: runTurn /
// handleMessage / injectScheduledTurn / maybeConsolidate / buildDynamicContext /
// the stuck-turn reaper). Every owner-visible string, log line, and gate moved
// BYTE-IDENTICAL - grep-diff the literals against the pre-lift file when in
// doubt; test_harness_turn pins the load-bearing ones (the retry/failover
// ladder, the budget refusal, the salvage path, the scheduled-turn rails).

namespace agent {

// The fold summary as raw text: the device closure renders it with a
// "## CONVERSATION SUMMARY..." header line (the histV1 window shape); the
// history pair frames the raw text itself, so the header is dropped once here.
static std::string rawFoldSummary(const std::string& summary) {
  const size_t at = summary.find("## CONVERSATION SUMMARY");
  if (at == std::string::npos) return summary;
  const size_t e = summary.find('\n', at);
  return e == std::string::npos ? std::string() : summary.substr(e + 1);
}

namespace {
// CUM-544: a TIMEOUT is model latency, not a dead link. Only the shared explicit
// token every head writes ("timeout: ..."): a provider error that merely mentions
// "timeout" (a router 504 text) is not one.
bool isTimeout(const std::string& e) { return nimbus::orch::isTimeoutError(e); }

// The failover notice's reason for `host`, up to "switching to ": a router
// refusal (its cause, which carries the fix), else the typed cause with its HTTP
// status ("anthropic says the account is out of credit (HTTP 400)"); a failure
// that names no cause stays "is unavailable".
std::string failoverReason(const std::string& host, const std::string& err) {
  if (router_refusal::isRefusalCause(err)) {
    std::string why = err;
    why[0] = (char)toupper((unsigned char)why[0]);
    return why + ". Switching to ";
  }
  int status = 0;
  const nimbus::orch::StepErr k = nimbus::orch::classifyStepError(err, &status);
  if (k == nimbus::orch::StepErr::Transport) return host + " is unavailable - switching to ";
  return nimbus::orch::stepErrCause(k, host, status) + " - switching to ";
}

// The model-facing [FALLBACK] class for a typed step error.
nimbus::orch::ErrorClass errorClassOf(nimbus::orch::StepErr k) {
  using nimbus::orch::ErrorClass;
  using nimbus::orch::StepErr;
  switch (k) {
    case StepErr::RateLimited: return ErrorClass::RateLimited;
    case StepErr::Timeout:     return ErrorClass::Timeout;
    case StepErr::ServerError: return ErrorClass::ServerError;
    case StepErr::Transport:   return ErrorClass::Network;
    case StepErr::Auth:        return ErrorClass::Auth;
    case StepErr::Billing:     return ErrorClass::Disabled;
    case StepErr::BadRequest:
    case StepErr::Rejected:    return ErrorClass::BadRequest;
    default:                   return ErrorClass::None;
  }
}

// Lane T (19 Oct triage): what the owner hears about a failed turn. The cause comes
// from the turn's FIRST cause through the one typed rule (stepErrCause), never as raw
// provider text ("messages HTTP 400: The request body ..."), and asking again is
// offered only when it can help: a cause in transit (transport, server error or
// overload, rate limit, timeout). A refused request (bad request, auth, billing, a
// rejected model, a cut answer) comes back the same, so it gets the plain cause.
struct OwnerCause {
  std::string text;
  bool        transient = false;
};

bool transientKind(nimbus::orch::StepErr k) {
  using nimbus::orch::StepErr;
  return k == StepErr::Transport || k == StepErr::ServerError || k == StepErr::RateLimited ||
         k == StepErr::Timeout;
}

OwnerCause ownerCause(const std::string& host, const std::string& why) {
  if (why.empty()) return {"the provider gave no response", true};
  if (router_refusal::isRefusalCause(why)) return {why, false};   // an owner sentence with its fix
  int status = 0;
  const nimbus::orch::StepErr k = nimbus::orch::classifyStepError(why, &status);
  const bool untyped = status == 0 && k == nimbus::orch::StepErr::Transport &&
                       why.rfind("network", 0) != 0;
  if (untyped && why.find("budget") != std::string::npos)
    return {"this turn hit its limits before a final answer", false};
  if (untyped && why.find("key") != std::string::npos)
    return {"no working provider key (check Assistant > Models on the web page)", false};
  if (untyped) {   // a short device or adapter token ("loop failed"): no HTTP text in it
    std::string c = host + ": " + why;
    const int keep = nimbus::utf8CapLen(c.c_str(), (int)c.size(), (int)router_refusal::kEngineCauseMax);
    if (keep < (int)c.size()) c = c.substr(0, (size_t)keep) + "…";
    return {c, true};
  }
  return {nimbus::orch::stepErrCause(k, host, status), transientKind(k)};
}

// The sentence after a failure: "Nothing is still running" only when no helper is
// active; otherwise how many helpers are still working and that their results will
// arrive. `advice` ("" for none) follows it.
std::string afterFailure(int activeJobs, const std::string& advice) {
  std::string out;
  if (activeJobs <= 0) out = "Nothing is still running";
  else if (activeJobs == 1) out = "1 helper is still working; its result will arrive when it finishes";
  else out = std::to_string(activeJobs) + " helpers are still working; their results will arrive when they finish";
  if (advice.empty()) return out + ".";
  if (activeJobs <= 0) return out + "; " + advice + ".";
  std::string a = advice;
  a[0] = (char)toupper((unsigned char)a[0]);
  return out + ". " + a + ".";
}

// L19-14: what a failed turn had already done, from its transcript brief (one
// "[result] <tool> ..." line per tool that ran): "Already done this turn:
// memory_write, web_search (2)." so the owner keeps the work, not just the failure.
std::string workDoneLine(const std::string& brief) {
  std::vector<std::pair<std::string, int>> ran;
  size_t at = 0;
  while ((at = brief.find("[result] ", at)) != std::string::npos) {
    at += 9;
    const size_t end = brief.find_first_of(" \n", at);
    const std::string name = brief.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (name.empty()) continue;
    bool seen = false;
    for (auto& r : ran)
      if (r.first == name) { r.second++; seen = true; }
    if (!seen && ran.size() < 8) ran.push_back({name, 1});
  }
  if (ran.empty()) return std::string();
  std::string out = "Already done this turn: ";
  for (size_t i = 0; i < ran.size(); ++i) {
    out += (i ? ", " : "") + ran[i].first;
    if (ran[i].second > 1) out += " (" + std::to_string(ran[i].second) + ")";
  }
  return out + ".";
}
}  // namespace

namespace orch = nimbus::orch;
namespace attn = nimbus::attn;
using solide::ring::Status;

// The honest local answer when no provider has a key (HOST-FALLBACK, 22 Oct): one
// sentence on every channel, before any request or paid turn.
static const char* const kNoProviderKeyReply =
    "No AI provider has a key: add one under Assistant > Models.";

// The turn's internal-SRAM record from admission to the provider request
// (turn_memory.h). Turn task only: turns are serialized. handleMessage starts it at
// its admission sample; runTurn starts it for a turn that came another way (a
// routine, a synthesis turn). Heap-held, not static: static internal RAM never comes
// back, and the first access is inside a turn, so the device places it in PSRAM.
namespace {
struct TurnMemoryState {
  TurnMemoryGate gate;
  bool admitted = false;   // handleMessage took this turn's admission sample
  bool started = false;    // onTurnStart fired, so the turn's end hook is owed
  bool refused = false;    // the turn ended for memory (a routine reads it as a defer)
  bool applied = false;    // apply delivered the turn's answer: no apology after it
  bool told = false;       // the short-of-memory reply already went out this turn
  bool asked = false;      // it reached its provider request: what it took is consumed
  bool putBack = false;    // what the turn took is back in its queues (putBackTaken ran)
  // What the turn took from the queues before it knew it could run: put back when it
  // ends for memory before asking any provider (putBackTaken), dropped once it asks.
  std::vector<JobEngine::FreshResult> fresh;
  std::string memChat, memBody;
  std::string spawnChat, spawnNotes;
  bool spawnMayHaveRun = false;
  void clearTaken() {
    fresh.clear();
    memChat.clear();
    memBody.clear();
    spawnChat.clear();
    spawnNotes.clear();
    spawnMayHaveRun = false;
  }
  // An entry (an owner message, a synthesis, a scheduled fire) starts with nothing taken.
  void beginEntry() {
    clearTaken();
    asked = false;
    putBack = false;
  }
};

TurnMemoryState& turnMem() {
  static TurnMemoryState* const s = new TurnMemoryState();
  return *s;
}

// L24: apply's first delivery marks the turn as answered, so a failed allocation
// after it adds no apology on top of the answer (the ApplyCapture swap pattern).
// CUM-826: a delivery that RETURNED (handed over) marks the fire as having spoken; one
// that threw before anything was queued does not. When that delivery queued nothing and
// the chat's history row is the owner's copy (needsRow, asked right after it returned),
// the mark waits for that row to be written. Speech marks itself through the device's
// sink (noteSpoke), only when something was heard or sent.
struct AnswerMark {
  ApplyDeps& deps;
  volatile bool& spoke;
  const std::function<bool(const std::string&)>& needsRow;
  bool rowPending = false;   // a delivery whose row is the copy: the next row marks it
  std::function<void(const std::string&, const std::string&)> inner;
  std::function<void(const std::string&, const std::string&)> innerCapture;
  AnswerMark(ApplyDeps& d, volatile bool& spokeFlag, const std::function<bool(const std::string&)>& rowRule)
      : deps(d), spoke(spokeFlag), needsRow(rowRule), inner(d.deliver), innerCapture(d.captureAssistant) {
    if (inner) {
      deps.deliver = [this](const std::string& chat, const std::string& text) {
        turnMem().applied = true;   // the apology rule only
        inner(chat, text);
        if (needsRow && needsRow(chat)) rowPending = true;
        else spoke = true;
      };
    }
    if (innerCapture) {
      deps.captureAssistant = [this](const std::string& chat, const std::string& text) {
        innerCapture(chat, text);
        if (!rowPending) return;
        rowPending = false;
        spoke = true;   // the row was the owner's copy, and it is written
      };
    }
  }
  ~AnswerMark() {
    if (inner) deps.deliver = std::move(inner);
    if (innerCapture) deps.captureAssistant = std::move(innerCapture);
  }
  AnswerMark(const AnswerMark&) = delete;
  AnswerMark& operator=(const AnswerMark&) = delete;
};

// The inputs a memory-refused turn took, back where the next turn reads them: the fresh
// results to the job engine (in front of newer ones), the parked memory results to the
// chat's slot, the spawn notes to the job engine. `Parked` is the engine's parked map.
template <class Parked>
void putBackTaken(TurnMemoryState& tm, JobEngine* jobs, Parked& parked, uint32_t& parkSeq) {
  if (tm.asked) return;   // a provider was asked: the inputs went out (a failed one is not a refusal)
  if (jobs && !tm.fresh.empty()) jobs->restoreFresh(std::move(tm.fresh));
  if (!tm.memChat.empty() && !tm.memBody.empty()) {
    auto& slot = parked[tm.memChat];
    slot.body = tm.memBody + slot.body;
    slot.seq = ++parkSeq;
  }
  if (jobs && !tm.spawnChat.empty()) jobs->restoreSpawnNotes(tm.spawnChat, tm.spawnNotes, tm.spawnMayHaveRun);
  tm.clearTaken();
  tm.putBack = true;
}

// Nothing went out and nothing answered: no provider was asked (putBackTaken's rule:
// once asked, a request may have left, billed and run tools), apply did not start
// delivering and no tool replied. Only such a turn may end as a memory deferral a
// retry repeats (F2). Whether the owner actually got model text is the engine's
// separate spoke_ fact (CUM-826).
bool nothingWentOut(const TurnMemoryState& tm, bool toolReplied) { return !tm.asked && !tm.applied && !toolReplied; }

bool ownerWaits(TurnSource s) { return s == TurnSource::Owner || s == TurnSource::Serial; }
// A turn whose setup cost at least this much internal SRAM logs its memory line.
constexpr uint32_t kTurnMemoryLogCost = 1024;
// A one-shot reminder or wakeup deferred for memory tries again this soon.
constexpr uint32_t kMemoryRetrySec = 120;
}  // namespace

// A provider's display name for an owner notice ("Anthropic"; the LAN endpoint is
// "your custom endpoint"), else its slug.
static std::string providerLabel(const std::string& slug) {
  if (slug == "custom") return "your custom endpoint";
  const orch::ProviderSlot* ps = orch::findProviderSlot(slug.c_str());
  return ps ? std::string(ps->label) : slug;
}

static void trimInPlace(std::string& s);

// The per-turn line for a request on host `h` (served_model.h runsOnLine): its asked model, the
// concrete model it answered with this boot (else the alias resolved through its stored list),
// and, when `h` is not the chosen head, why the chosen head is not answering.
static std::string runsOnText(const TurnEngine::Deps& d, const std::string& h, const std::string& chosen) {
  const std::string asked = d.cfg.provider.orchModel ? d.cfg.provider.orchModel(h) : std::string();
  std::string concrete = orch::servedModelFor(h, asked);
  if (concrete.empty() && !asked.empty()) {
    char b[orch::kModelKeyCap];
    orch::resolveServedModel(h.c_str(), asked.c_str(), b, sizeof(b));
    concrete = b;
  }
  std::string fact;
  if (!chosen.empty() && h != chosen) {
    orch::HeadPause hp = d.apply.headPause ? d.apply.headPause(chosen) : orch::HeadPause();
    if (d.cfg.budget.overBudget && d.cfg.budget.overBudget(chosen)) hp.overBudget = true;
    fact = orch::headPauseFact(providerLabel(chosen), hp);
  }
  return orch::runsOnLine(h, asked, concrete, fact);
}

// The head the next turn will ACTUALLY run on - the one resolution the turn core
// reads: nimbus::orch::resolveHeadHost,
// the rule the device's screen, catalog and web page share. The pinned orchHost while
// it has a key; else the first KEYED provider in priority order (CUM-201: BYOK-override
// first); else the verified router fallback head (Cumulo, then Z.ai - CUM-242); else
// any other keyed provider; only with no key anywhere the pin or the bare first token
// (the turn is then answered locally, never dialed). HOST-FALLBACK (22 Oct bench): a
// pin whose key was cleared won here and the turn dialed it keyless (HTTP 401); with
// no keyed provider in the priority list the turn ran on nothing and said "Done."
// One rule on purpose: when the context line read the raw first token instead, a
// hosted anthropic-only instance INTRODUCED ITSELF as "mistral /
// mistral-large-latest" - the model parrots this line, so it must name the real head
// (CUM-374 e2e, 2026-09-18).
static std::string resolveTurnHost(const TurnEngine::Deps& d) {
  const auto& p = d.cfg.provider;
  return orch::resolveHeadHost(p.orchHost ? p.orchHost() : std::string(),
                               p.providerPriority ? p.providerPriority() : std::string(), p.hasKey,
                               p.routerFallbackHost ? p.routerFallbackHost() : std::string());
}

std::string TurnEngine::headHost() const { return resolveTurnHost(d_); }

static void trimInPlace(std::string& s) {
  const char* ws = " \t\r\n";
  size_t b = s.find_first_not_of(ws);
  size_t e = s.find_last_not_of(ws);
  s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

// ---- per-chat conversation map (Release B2) ---------------------------------
// One provider conversation PER CHAT - the old single "host|convId" slot made
// every channel (telegram chats, web, voice, serial) interleave into ONE
// provider thread: chat bleed, and each channel's history polluted by the rest.
static std::string convSanitize(std::string v) {
  for (char& c : v) if (c == '=' || c == ';' || c == '|') c = '_';
  return v;
}

std::string convMapGet(const std::string& raw, const std::string& chat,
                       const std::string& host) {
  if (raw.empty() || raw.find('=') == std::string::npos) return "";  // legacy/empty
  std::string key = convSanitize(chat) + "=";
  size_t start = 0;
  while (start < raw.size()) {
    size_t end = raw.find(';', start);
    if (end == std::string::npos) end = raw.size();
    if (raw.compare(start, key.size(), key) == 0) {
      std::string val = raw.substr(start + key.size(), end - start - key.size());
      size_t bar = val.find('|');
      if (bar == std::string::npos || bar == 0) return "";
      if (val.substr(0, bar) != host) return "";   // host switched -> fresh thread
      return val.substr(bar + 1);
    }
    start = end + 1;
  }
  return "";
}

std::string convMapSet(std::string raw, const std::string& chat,
                       const std::string& host, const std::string& convId) {
  if (raw.find('=') == std::string::npos) raw.clear();   // discard legacy format
  std::string key = convSanitize(chat) + "=";
  // Rebuild without this chat's entry (preserving order = age order).
  std::string out;
  int entries = 0;
  size_t start = 0;
  while (start < raw.size()) {
    size_t end = raw.find(';', start);
    if (end == std::string::npos) end = raw.size();
    std::string entry = raw.substr(start, end - start);
    start = end + 1;
    if (entry.empty() || entry.compare(0, key.size(), key) == 0) continue;
    out += entry + ";";
    entries++;
  }
  if (!convId.empty()) {
    // LRU bound: drop the OLDEST entries down to 7 so the append makes 8.
    while (entries >= 8) {
      size_t cut = out.find(';');
      out = (cut == std::string::npos) ? std::string() : out.substr(cut + 1);
      entries--;
    }
    out += key + convSanitize(host) + "|" + convSanitize(convId) + ";";
  }
  return out;
}

TurnEngine::TurnEngine(Deps d, Tuning t) : d_(std::move(d)), t_(t) {}

void ProviderHosts::noteFailed(const std::string& host, const std::string& err) const {
  if (!failed || nimbus::orch::isParkedRefusal(err)) return;   // no request was answered
  int status = 0;
  const nimbus::orch::StepErr kind = nimbus::orch::classifyStepError(err, &status);
  failed(host, kind, status);
}

bool TurnEngine::deliver(const std::string& chatId, const std::string& text, bool modelText) {
  // Every ENGINE notice (failure replies, provider-switch and budget notices) goes
  // through here; a turn's own reply goes through ApplyDeps. A quiet background turn
  // (the nightly dream, the post-update awareness turn) has nobody waiting for it,
  // so its notices are held: the owner was being woken at 03:30 by "That didn't
  // finish ... ask again to retry" for a turn nobody asked for. Its outcome is
  // recorded where the owner reads it later (the dream row, FireOutcome.error).
  if (holdingNotices()) {
    ++quietNoticesHeld_;
    hlog::logf("quiet turn: notice held, not sent (%.96s)", text.c_str());
    return false;
  }
  if (d_.deliver) d_.deliver(chatId, text);
  // CUM-826: handed over, the model's text spoke - before the row can fail, unless the
  // row is the owner's only copy (then after it is written).
  const bool rowIsCopy = d_.deliveryNeedsRow && d_.deliveryNeedsRow(chatId);
  if (modelText && !rowIsCopy) spoke_ = true;
  // Storm finding 2 (24 Oct): one history row per message the owner received.
  // A failed turn's honest refusal, the parked short answer, a rate-limit or
  // pre-turn local reply is a message the owner received, so it is captured as the
  // chat's assistant row like a served turn's reply. Without this, a device with
  // no armed Telegram channel (the storm protocol's no-bot unit) delivered its
  // notices into a queue it drains-and-discards, and the owner - who reads the
  // history rows on the web page - never saw a word.
  captureAssistantRow(chatId, text, "reply");
  if (modelText && rowIsCopy) spoke_ = true;   // the row is the owner's copy: written now
  return true;
}

// One notice per billing episode on every path that meets the refusal (the fabric's
// switch, the single-shot ladder, a turn that starts past a parked head). A quiet
// turn holds the notice (deliver) without taking the latch, so the next seam that
// can say it still does (lane M x lane K).
void TurnEngine::deliverBillingNotice(const std::string& chatId, const std::string& host,
                                      const std::string& text) {
  if (!holdingNotices() && !nimbus::orch::billingNoticeDue(host.c_str())) {
    hlog::logf("orchestrator: %s billing notice already given this episode", host.c_str());
    return;
  }
  deliver(chatId, text);
}

void TurnEngine::holdNotice(const std::string& text, const std::string& latchHost, orch::StepErr kind) {
  if (!latchHost.empty() && !holdingNotices() && !nimbus::orch::billingNoticeDue(latchHost.c_str())) {
    hlog::logf("orchestrator: %s %s notice already given this episode", latchHost.c_str(),
               kind == orch::StepErr::Auth ? "key" : "billing");
    return;
  }
  heldNotices_.push_back(HeldNotice{text, latchHost, kind});
}

void TurnEngine::flushHeldNotices(const std::string& chatId) {
  std::vector<HeldNotice> held;
  held.swap(heldNotices_);
  for (const HeldNotice& n : held) deliver(chatId, n.text);
}

std::string TurnEngine::foldHeldNotices() {
  std::string out;
  for (const HeldNotice& n : heldNotices_) {
    if (n.latchHost.empty()) continue;   // a switch notice: its cause is in the final notice
    const std::string line = n.kind == orch::StepErr::Auth
                                 ? "Update " + providerLabel(n.latchHost) + "'s key under Assistant > Models."
                                 : "Add credit on " + n.latchHost + "'s billing page.";
    if (out.find(line) == std::string::npos) out += (out.empty() ? "" : " ") + line;
  }
  if (!heldNotices_.empty())
    hlog::logf("orchestrator: %u switch notice(s) folded into the final notice", (unsigned)heldNotices_.size());
  heldNotices_.clear();
  return out;
}

void TurnEngine::deliverFailure(const std::string& chatId, const std::string& head, int activeJobs,
                                const std::string& advice, bool nothingRan) {
  std::string text = head;
  if (nothingRan) {
    text += " Your message was not answered.";
  } else {
    const std::string work = workDoneLine(lastBrief_);
    if (!work.empty()) text += " " + work;
  }
  text += " " + afterFailure(activeJobs, advice);
  // Every held next-step line names a provider this turn LEFT (no path returns to a
  // host it switched away from), never the one the final notice names, and its latch
  // was taken when it was held: it always rides along, or that provider is never told.
  const std::string fold = foldHeldNotices();
  if (!fold.empty()) text += " " + fold;
  deliver(chatId, text);
}

// Lane PARK (storm leg c): a turn that failed on a parked kind of refusal - out of
// credit or a refused key, refused at the provider or ended by the park with no request
// because every host it could use is closed - tells the owner once per episode, the
// notice with the next step; the owner's next such message gets one short answer;
// every later one of the episode gets nothing (the owner was told, and the park's
// timed re-check or a new key is what changes it). A routine or wakeup may carry the
// notice, never the short answer. The latches are the park's, shared with every seam
// and kept on flash. A quiet turn holds its words and takes no latch (deliver holds
// them).
void TurnEngine::deliverParkedRefusal(const std::string& chatId, const std::string& host, orch::StepErr kind,
                                      int activeJobs, bool nothingRan) {
  const bool key = kind == orch::StepErr::Auth;
  const std::string label = providerLabel(host);
  const std::string notice =
      key ? "That didn't finish - " + label + " rejected its key. Update it under Assistant > Models."
          : "That didn't finish - " + label +
                " says the account is out of credit. Add credit on its billing page, or switch providers.";
  if (holdingNotices()) {
    deliverFailure(chatId, notice, activeJobs, "", nothingRan);
    return;
  }
  const bool ownerWrote = turnSource_ == TurnSource::Owner || turnSource_ == TurnSource::Serial;
  const orch::RefusalTurnAnswer answer = orch::refusalTurnAnswer(host.c_str(), ownerWrote);
  if (answer == orch::RefusalTurnAnswer::Notice) {
    deliverFailure(chatId, notice, activeJobs, "", nothingRan);
    return;
  }
  // Another provider's new episode met in this turn still reaches the owner.
  const std::string fold = foldHeldNotices();
  if (answer == orch::RefusalTurnAnswer::Short) {
    const std::string text = key ? orch::keyShortAnswer(label)
                                 : orch::billingShortAnswer(label, orch::providerPark().recheckInMs(host.c_str()));
    deliver(chatId, fold.empty() ? text : text + " " + fold);
    return;
  }
  hlog::logf("orchestrator: %s %s and the owner was told this episode - no reply", host.c_str(),
             key ? "rejected its key" : "is out of credit");
  if (!fold.empty()) deliver(chatId, fold);
}

void TurnEngine::emitHead(uint8_t ringStatus) {
  if (!d_.event) return;
  attn::Event e;
  e.type = attn::Event::Type::JobState;
  e.key = keyFromTag("head");
  e.status = ringStatus;
  d_.event(e);
}

void TurnEngine::setHeadArc(bool lit) {
  emitHead((uint8_t)(lit ? Status::Running : Status::Offline));
}

// P6: the loop is the default turn path, but it needs headroom above the
// single-TLS + per-round floor.
// The loop gate evaluated against a SPECIFIC heap reading. runTurn passes the
// turn-ENTRY heap (the admission the turn floor granted) so the verdict is not
// corrupted by recall's transient dip; the out-of-band callers below pass live
// heap, which is fine - they are describing capability, not gating a turn.
bool TurnEngine::loopActiveAt(uint32_t heap) const {
  if (boundedSingleShot()) return false;   // a bounded turn on a fallback head: no loop
  // +2000 (was +6000): a tool round's INTERNAL cost is a few hundred bytes (the
  // body/JSON are PSRAM-routed), so the old 34 KB gate silently dropped the loop to
  // single-shot on basically every turn in the 28-34 KB band. See docs/memory-model.md.
  return d_.cfg.loop.toolLoopOn && d_.cfg.loop.toolLoopOn() &&
         heap > (t_.loopMinHeap + 2000U);
}
bool TurnEngine::loopActiveNow() const { return loopActiveAt(freeHeap()); }

bool TurnEngine::headOnFallback() const {
  const std::string host = resolveTurnHost(d_);
  if (host.empty()) return false;
  return (d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(host)) || d_.hosts.isParked(host);
}

namespace {
// The tighter of a configured cap and a bound (0 = unlimited on either side).
size_t tighterCap(size_t cfg, size_t bound) { return (bound && (!cfg || bound < cfg)) ? bound : cfg; }
}  // namespace

// CUM-536: what a tool call gets on a turn that stores nothing in long-term
// memory, "" when the call stores nothing. memory.update stores new content too
// (it falls back to a plain store when nothing matches), so both doors are shut;
// a head that calls the mem_write reply field as a tool hears that the field is
// shut too, never the usual "call memory_write now" recovery.
static bool storesMemory(const char* name) {
  return name && (!std::strcmp(name, "memory.write") || !std::strcmp(name, "memory.update"));
}
static std::string unverifiedStoreRefusal(const std::string& regName) {
  if (storesMemory(regName.c_str())) return agent::kUnverifiedMemoryRefusal;
  if (!storesMemory(orch::ToolRegistry::turnFieldTool(regName))) return std::string();
  return regName + " is not a tool, and memory writes are refused this turn, through the "
         "mem_write field of your orch_turn reply too. " + agent::kUnverifiedMemoryRefusal;
}

std::string TurnEngine::takeSpawnNotes(const std::string& chatId) {
  // L24: the taken notes are kept until the turn asks a provider (putBackTaken). They
  // land in the keep by a move, so no failed allocation sits between the take and it.
  TurnMemoryState& tm = turnMem();
  bool mayHaveRun = false;
  tm.spawnChat = chatId;
  if (d_.jobs) tm.spawnNotes = d_.jobs->takeSpawnNotes(chatId, &mayHaveRun);
  tm.spawnMayHaveRun = mayHaveRun;
  const std::string& notes = tm.spawnNotes;
  // Lane L: this chat's spawn ledger rides with its refusals - the device's own
  // record of what it started, so the head never tracks it in free-text notes.
  const std::string ledger = d_.jobs ? d_.jobs->spawnLedgerText(chatId) : std::string();
  if (notes.empty()) return ledger.empty() ? "" : "\n" + ledger;
  return refusedBlock(notes, mayHaveRun) + ledger;
}

std::string TurnEngine::refusedBlock(const std::string& notes, bool mayHaveRun) {
  // Same background framing as [MEMORY RESULTS]: the owner was already told each
  // of these; the model needs the REAL reason (not a guessed "capacity 0") and the
  // fact that nothing is running for them. Live room is in [SPAWN CAPACITY].
  if (!mayHaveRun)
    return "\n[SPAWN REFUSED] (background: sub-agents you asked for that did NOT start; "
           "the owner was already told. Nothing is running for them - spawn again if "
           "still needed, within [SPAWN CAPACITY].)\n" + notes;
  // A timed-out dispatch most likely ran: "spawn again" would pay for it twice.
  return "\n[SPAWN REFUSED] (background: sub-agents you asked for that did not run as "
         "asked; the owner was already told. A 'did not start' one is not running - spawn "
         "it again if still needed, within [SPAWN CAPACITY]. A 'may have started' one may "
         "have run on its provider - do NOT spawn it again unless the owner asks.)\n" + notes;
}

void TurnEngine::parkMemResults(const std::string& chatId) {
  auto it = pendingMemResults_.find(chatId);
  if (it == pendingMemResults_.end()) return;
  if (it->second.body.empty()) {
    pendingMemResults_.erase(it);
    return;
  }
  it->second.seq = ++pendingMemSeq_;
  // Bounded: a chat that never speaks again must not pin its parked results
  // forever. The chat parked longest ago goes first (never by key order).
  while (pendingMemResults_.size() > kPendingMemChats) {
    auto victim = pendingMemResults_.end();
    for (auto c = pendingMemResults_.begin(); c != pendingMemResults_.end(); ++c)
      if (c->first != chatId && (victim == pendingMemResults_.end() || c->second.seq < victim->second.seq))
        victim = c;
    if (victim == pendingMemResults_.end()) break;
    pendingMemResults_.erase(victim);
  }
}

std::string TurnEngine::takePendingMemResults(const std::string& chatId) {
  // Per chat (lane M): the results belong to the chat (and so the principal) whose
  // turn ran the mem_query. An engine-global slot handed the owner's memory search
  // results to whichever chat spoke next - a member's or a guest's turn included.
  // L24: the taken block is kept until the turn asks a provider (putBackTaken). It
  // lands in the keep by a move, so no failed allocation sits between the take and it.
  TurnMemoryState& tm = turnMem();
  tm.memChat = chatId;
  std::string& body = tm.memBody;
  auto it = pendingMemResults_.find(chatId);
  if (it != pendingMemResults_.end()) {
    body = std::move(it->second.body);
    pendingMemResults_.erase(it);
  }
  // Writes queued during an embedding outage that were stored (or refused) since:
  // the head promised "I'll remember" and can now confirm it (lane P). The drained
  // confirmations join this chat's parked block, never a global slot.
  if (d_.memNotes) body += d_.memNotes(chatId);
  if (body.empty()) return "";
  // BACKGROUND framing (owner field bug 2026-07-16): this block used to lead the
  // turn input with no do-not-latch instruction - the owner sent "try now?" (about
  // Telegram) and the model answered about the injected memory results instead.
  std::string s = std::string("\n[MEMORY RESULTS] (background: outcomes of YOUR memory ops "
                              "from last turn - reference material only. The owner has NOT "
                              "seen this and is NOT asking about it; answer the [USER] "
                              "message below on ITS topic.)\n") + body;
  return s;
}

// The input channel a turn arrived on, keyed by the routing chatId (P6). The
// reply routes back to the SAME channel by default; the model can reach another
// via the reply.speak / reply.telegram tools when the loop is on.
bool isTelegramOnlyTool(const std::string& name) {
  return name == "reply.telegram" || name == "files.send";
}

// Where a one-shot's reply lands, in words that read well after "delivered to".
static const char* deliveryPlace(const std::string& chatId) {
  if (isWebChat(chatId)) return "the web chat";
  if (chatId == "voice") return "the device screen";
  if (chatId == "serial") return "the serial console";
  return "this Telegram chat";
}

bool TurnEngine::telegramOn(const std::string& chatId) const {
  return !d_.cfg.telegramDeliverable || d_.cfg.telegramDeliverable(chatId);
}

const char* TurnEngine::channelOf(const std::string& chatId) {
  if (chatId == "serial") return "the developer serial console";
  if (chatId == "voice")  return "the device microphone (hold-to-talk); your reply shows on the panel";
  if (isWebChat(chatId))  return "the web UI";
  return "Telegram; your reply is delivered as a Telegram message to that chat";
}

// [ACTIVE SESSIONS] body - byte-identical to the device jobsSummary() (same
// journal source, reached through JobEngine::sessionInfos).
// L19-35: after a restart the job table and the spawn ledger start empty, so the jobs
// that finished before it follow (from the journal's finished ring): without them the
// head read its own "waiting for the results" note and ran the same paid jobs again.
// Nothing is added when none finished before this boot (the block stays byte-stable).
std::string TurnEngine::jobsSummaryText() const {
  std::vector<orch::SessionInfo> infos =
      d_.jobs ? d_.jobs->sessionInfos() : std::vector<orch::SessionInfo>{};
  std::string s;
  if (infos.empty()) s = "No active jobs.";
  for (const auto& r : infos)
    s += r.id + " (" + r.provider + "/" + r.model + ") " + r.title + " [" + r.state + "]\n";
  const std::string finished = d_.jobs ? d_.jobs->finishedBeforeRestartText() : std::string();
  if (finished.empty()) return s;
  if (infos.empty()) s += "\n";
  return s + "Finished before the device last restarted (their results were already delivered "
             "or reported; do not start them again unless the owner asks):\n" + finished;
}

// Lane L (subagents F6.2): which keyed sub-agent providers hold the device for
// the whole run and which run remotely, from the one backend table
// (kDeviceBackendSync) - the old line named only mistral, so a cumulo, zai or
// custom sub that blocked the device and lost its result on a timeout was a
// surprise. No wait times here: each adapter owns its own.
std::string TurnEngine::syncCapacityText() const {
  std::vector<std::string> sync, async;
  for (int i = 0; i < kDeviceBackendCount; i++) {
    const std::string b = kDeviceBackends[i];
    const bool keyed = !d_.cfg.provider.hasKey || d_.cfg.provider.hasKey(b) ||
                       (b == "custom" && d_.hosts.has("custom"));
    if (!keyed) continue;
    (kDeviceBackendSync[i] ? sync : async).push_back(b);
  }
  auto list = [](const std::vector<std::string>& v, const char* last) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++)
      out += (i == 0 ? "" : (i + 1 == v.size() ? std::string(" ") + last + " " : ", ")) + v[i];
    return out;
  };
  std::string out;
  if (!sync.empty())
    out += list(sync, "and") + " sub-agents run one at a time INSIDE the device: each holds it "
           "for its whole run and must finish within the device's wait (about one to two "
           "minutes), or its work is lost. ";
  if (!async.empty())
    out += list(async, "and") + " sub-agents run in parallel on the provider while the device "
           "collects them one by one" + (sync.empty() ? "." : std::string("; use ") +
           list(async, "or") + " for research, long work or anything that needs the web.");
  else if (!sync.empty())
    out += "Keep each sub-agent to one focused step.";
  return out;
}

void TurnEngine::noteUnappliedSpawnClaim(const std::string& chatId) {
  hlog::log("orchestrator: reply claims sub-agents but the answer was not applied - correction added");
  hlog::log("orchestrator: spawn ledger spawned=0 claimed=1");
  if (d_.jobs)
    d_.jobs->noteSpawnIntentRefused(chatId, "", "",
                                    "your reply said sub-agents were started, but that answer was "
                                    "cut off or unreadable, so nothing in it ran and nothing is "
                                    "running for them");
}

std::string TurnEngine::buildDynamicContext(const std::string& chatId) {
  std::string ctx;
  // F1 (lane I): the approved skills attached to THIS chat (pinned context
  // skills, plus what it read) ride the per-turn INPUT, never the static
  // prompt - the static block must stay byte-identical across turns that
  // differ only in a skill read, or every read would drop the provider cache.
  // The picks join this turn's skills: row tag (the honesty trail).
  attachPicks_ = d_.skillsPicks ? d_.skillsPicks(chatId) : std::vector<AttachPick>{};
  if (!attachPicks_.empty() && d_.skillBody) {
    ctx += agent::skillsAttachText(attachPicks_,
                                   [this](const std::string& id) { return d_.skillBody(id); });
  }
  // A provider fallback from the previous turn, surfaced once (mention only if
  // relevant/asked), labelled with whose turn it was and how long ago so this turn
  // never reads it as its own switch. Cleared after it is handed to the model.
  if (!pendingFallbackPath_.empty()) {
    const long ago =
        d_.platform.nowMs ? (long)((nowMs() - pendingFallbackMs_) / 60000u) : -1L;
    ctx += orch::fallbackNote(pendingFallbackWhose_, ago, pendingFallbackPath_,
                              pendingFallbackReason_) +
           "\n";
    pendingFallbackPath_.clear();
  }
  ctx += "[ACTIVE SESSIONS]\n";
  ctx += jobsSummaryText();
  // The spawn cap (kAgentMaxJobs) was invisible to the model: it would promise
  // to start N sub-agents, the engine would silently refuse past the cap, and
  // the refusal never reached the turn. Surface the real headroom so it plans
  // within it and says so when it can't start them all (ORCH_D_SOPS).
  {
    // [SPAWN CAPACITY] - the HONEST throughput model (W5). The old "[SPAWN SLOTS]
    // N of 6 free ... over the cap is refused" read as a hard total ceiling, so
    // the head under-spawned a deep run ("only 6 slots"). The truth: the device
    // is single-threaded (one network call at a time) and drains a QUEUE one
    // dispatch per cycle into a small concurrency window - so a big fan-out is
    // fine, it just runs SEQUENTIALLY over successive waves. NOT concurrency.
    const int live   = d_.jobs ? (int)(d_.jobs->sessionInfos().size()) : 0;
    const int queued = d_.jobs ? d_.jobs->pendingCount() : 0;
    // Bound by the per-TURN parse cap (parseTurn keeps at most kAgentMaxJobs
    // session_ops) AND the queue room, so the number never promises more than a
    // turn can actually contain - no silent truncation. spawnRoomFor is shared
    // with the unknown-tool recovery text (CUM-538): one number everywhere.
    const int canStart = orch::spawnRoomFor(queued);
    // A dispatch that timed out may still be running on its provider with no job
    // here to wait for: counted as possibly running. The "do not spawn it again"
    // instruction rides only the spawning chat's [SPAWN REFUSED] line.
    const int maybe = d_.jobs ? d_.jobs->maybeRunningCount() : 0;
    const std::string maybeText =
        maybe > 0 ? " (+" + std::to_string(maybe) + " possibly still running on its "
                    "provider after a timeout; its result will not arrive)"
                  : std::string();
    ctx += "[SPAWN CAPACITY] " + std::to_string(live) + " running" + maybeText + ", " +
           std::to_string(queued) + " queued. You can start up to " +
           std::to_string(canStart) + " sub-agents this turn; they run "
           "SEQUENTIALLY (the device does one network call at a time, up to " +
           std::to_string(orch::kMaxActiveInflight) +
           " in flight), draining as each finishes. This is NOT a hard total "
           "limit - a big fan-out runs over successive waves: start a wave now, "
           "and when they finish you get an automatic turn to spawn the next, so "
           "you can work through dozens over a run. " + syncCapacityText() + "\n";
  }
  return ctx;
}

// The custom LAN endpoint is a real head and spawn target when configured.
static bool customHostOn(const TurnEngine::Deps& d_) {
  return d_.hosts.has("custom") && d_.cfg.provider.hasKey && d_.cfg.provider.hasKey("custom");
}
static std::string customModelOf(const TurnEngine::Deps& d_) {
  return d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel("custom") : std::string();
}

// R5, 5d of the token table: on a turn where the head can call models.list,
// [AVAILABLE MODELS] is one pointer line naming the keyed providers (the
// per-provider lists cost about 26K tokens a day on Lumi).
static std::string modelsPointer(const TurnEngine::Deps& d_) {
  std::vector<std::string> keyed;
  for (const orch::ProviderSlot& slot : orch::kProviderSlots)
    if (d_.cfg.provider.hasKey && d_.cfg.provider.hasKey(slot.slug)) keyed.push_back(slot.slug);
  std::string line = orch::modelsPointerLine(keyed);
  if (customHostOn(d_))
    line.insert(line.size() - 1, " custom runs " + customModelOf(d_) + " (local network model).");
  return line;
}

// [AVAILABLE MODELS]: one line, part of the stable catalogs - the pointer line
// when models.list is callable, otherwise the lists themselves, the router's
// under cumulo only (L19-39).
static std::string availableModelsLine(const TurnEngine::Deps& d_, bool modelsTool) {
  if (modelsTool) return modelsPointer(d_);
  std::string ctx;
  ctx += "[AVAILABLE MODELS] (yours + sub-agents')";
  bool any = false;
  bool router = false;
  // CUM-486: walk the canonical provider registry (kProviderSlots), never a hand
  // list, so a provider added there is advertised here with no edit. The router
  // heads (cumulo/zai) are first-class keyed hosts (CUM-242); cumulo choices are
  // the "<upstream>/<model>" ids the router harvests. Each is gated on hasKey().
  // The list is Deps::modelChoices - the SAME derivation the spawn validator
  // accepts - so every advertised model spawns as named. A keyed provider whose
  // list comes back empty is advertised with the model a spawn there actually
  // runs (its sub default), never "zai: ;", which the honesty rails read as "no
  // models" and refused to route to.
  for (const orch::ProviderSlot& slot : orch::kProviderSlots) {
    const std::string p = slot.slug;
    if (!d_.cfg.provider.hasKey || !d_.cfg.provider.hasKey(p)) continue;
    std::string list = d_.modelChoices ? d_.modelChoices(p) : std::string();
    if (list.empty() && d_.cfg.provider.subModel) list = d_.cfg.provider.subModel(p);
    if (list.empty()) continue;
    const std::string note = d_.modelListNote ? d_.modelListNote(p) : std::string();
    ctx += " " + p + ": " + list + (note.empty() ? "" : " " + note) + ";";
    any = true;
    router = router || p == "cumulo";
  }
  // The custom LAN endpoint, when configured, is a REAL spawn/head target - its
  // absence here made the honesty rails (correctly) refuse "spawn on custom"
  // even though the registry had it (live-caught 2026-08-12 on the Ollama E2E).
  // No verify pass runs for custom, so the model name comes from config, not
  // modelChoices.
  if (customHostOn(d_)) {
    ctx += " custom: " + customModelOf(d_) + " (local network model);";
    any = true;
  }
  if (!any) ctx += " (none configured)";
  // L19-39: the head read "openai/gpt-5.6" under cumulo and asked openai for it.
  if (router) ctx += " The cumulo ids are router ids (<upstream>/<model>) and run on cumulo only.";
  ctx += "\n";
  return ctx;
}

// The catalogs that change only with the device config. They ride the END of the
// static prompt block (ComposeInputs::catalogs, prompt_layout.h), where a
// provider reads them from its cache, instead of the per-turn input after the
// volatile session counters.
std::string TurnEngine::buildStableCatalogs(bool modelsTool) {
  std::string ctx = "\n";
  ctx += "[AVAILABLE PROVIDERS] sub-session priority: " +
         (d_.cfg.provider.subPriority ? d_.cfg.provider.subPriority() : std::string()) + "\n";
  // The model the agent runs on is NOT named here: this block is cached and outlives a
  // failover, so a "[YOUR MODEL]" line named the configured head while a paused head's turns
  // ran elsewhere (Lumi, 26 Oct). Each request's input names its own host and model instead
  // ([THIS TURN RUNS ON], runsOnText; lane HFM).
  ctx += availableModelsLine(d_, modelsTool);

  // R3: the skills index rides the SHELF block (its own cache point after the
  // static one - a skill edit re-caches only the shelf), not the static catalogs.
  // Per-provider capability + connector catalog (Phase C): what each provider's
  // turns/sub-agents can actually reach, so spawns are chosen knowingly.
  if (d_.connectorsCatalog) ctx += d_.connectorsCatalog();
  return ctx;
}

// ---- the turn core ----------------------------------------------------------

// For one applyTurn: apply captures the spoken line and one row per message the
// owner received through one hook (a question appended to the reply rides the
// reply's row; a question sent on its own is its own row). Under histV2 the hook
// is wrapped so each row carries its out: kind (a capture right after speak() is
// the tts line; the first other one is the reply when the turn has one; the rest
// are the question) and a seedless turn's trigger row lands first.
struct TurnEngine::ApplyCapture {
  TurnEngine* e = nullptr;
  std::function<void(const std::string&, const std::string&)> capture;
  std::function<void(const std::string&)> speak;

  ApplyCapture(TurnEngine* eng, bool hasReply) {
    if (!eng->histV2On() || !eng->d_.apply.captureAssistant) return;
    e = eng;
    capture = e->d_.apply.captureAssistant;
    speak = e->d_.apply.speak;
    e->replyPending_ = hasReply;
    e->nextSpoken_ = false;
    e->d_.apply.captureAssistant = [this](const std::string& chat, const std::string& text) {
      const char* kind = e->nextSpoken_ ? "tts" : e->replyPending_ ? "reply" : "ask";
      if (!e->nextSpoken_) e->replyPending_ = false;
      e->nextSpoken_ = false;
      e->flushTrigger(chat);   // a seedless turn's trigger row lands before its first reply row
      e->outKind_ = kind;
      capture(chat, text);
      e->outKind_.clear();
    };
    if (speak)
      e->d_.apply.speak = [this](const std::string& text) {
        e->nextSpoken_ = true;
        speak(text);
      };
  }
  ~ApplyCapture() {
    if (!e) return;
    e->d_.apply.captureAssistant = std::move(capture);
    e->d_.apply.speak = std::move(speak);
  }
  ApplyCapture(const ApplyCapture&) = delete;
  ApplyCapture& operator=(const ApplyCapture&) = delete;
};

bool TurnEngine::runTurn(const std::string& inputs, const std::string& chatId,
                         const std::string& userText) {
  return runTurn(inputs, chatId, userText, telegramOn(chatId));
}

bool TurnEngine::runTurn(const std::string& inputs, const std::string& chatId,
                         const std::string& userText, bool telegram) try {
  // The whole turn keeps its small blocks in PSRAM (L24, turn_memory.h); a turn
  // handleMessage admitted already holds the scope and its admission sample.
  const PsramFirstScope psramFirst;
  const bool admitted = turnMem().admitted;   // else the entry sample below starts the record
  turnMem().admitted = false;                 // the next turn takes its own sample
  if (!admitted) turnMem().clearTaken();      // only an entry's own takes can be put back
  turnMem().started = false;
  turnMem().refused = false;
  turnMem().applied = false;
  turnMem().told = false;
  turnMem().asked = false;
  turnMem().putBack = false;
  toolReplied_ = false;   // before anything can throw: a stale flag would silence the apology
  spoke_ = false;         // CUM-826: this turn's own "spoke" fact
  // Turn lifecycle guard: flags the in-flight state AND paints the head turn on
  // the ring as a live Running job ("head" key, blue comet in Full) so the user
  // SEES the device thinking - previously a text/voice turn gave no ring feedback
  // at all in Full posture. Offline on destruction frees the arc (also on error
  // paths, since the dtor always runs).
  curChat_ = chatId;   // reply.speak / reply.telegram default target for this turn
  // F1 (lane I): this turn's skills start from what buildDynamicContext
  // attached (buildDynamicContext runs just before runTurn); a mid-loop
  // skill.get adds its id through noteSkillUsed.
  skillsUsed_.clear();
  for (const auto& p : attachPicks_) noteSkillUsed(p.id);
  turnTelegram_ = telegram;   // CUM-523: the one sample every Telegram decision reads
  // files.send needs an allowlisted chat: a Telegram turn's own answer may come from
  // public mode, so it also asks for no chat (""); any other turn's answer already
  // meant an allowlisted chat. No second closure: the static config stays as it was.
  turnFileSend_ = telegram && (!isTelegramChat(chatId) || telegramOn(std::string()));
  toolReplied_ = false;
  lastBrief_.clear();  // per-turn: a single-shot turn must not inherit the last loop's brief
  heldNotices_.clear();   // L19-13 d: this turn's switch notices start empty
  // Lane L: sub-agent results folded into this turn are external content (the
  // leak-repair gate reads it); a tool that returns external content sets it too.
  externalContentSeen_ = freshInTurn_;
  // Lane T: this turn's spend and causes start empty (filled per attempt, metered and
  // reported once in fireTurnEnd on whichever exit the turn takes).
  TurnFacts& tf = facts();
  tf.spend.clear();
  tf.chain.clear();
  tf.metered = false;
  tf.switched = false;
  // Lane DEF: the monthly spend ceiling. While paid calls are paused no turn reaches
  // a provider (recall's embed below included). The owner hears why once per channel
  // from the device's notice pump, so the refused turn itself sends nothing.
  if (!nimbus::orch::paidCallAllowed(nimbus::orch::SpendSeam::Head)) {
    tf.chain.noteDevice(nimbus::orch::kSpendFrozenError);
    lastTurnUsage_ = {};
    hlog::logf("orchestrator: %s turn refused - %s",
               turnSourceSlug(turnSource_, attribution_), nimbus::orch::kSpendFrozenError);
    return false;
  }
  // Lane S: a fresh effect ledger; the sub-agent results the entry wrapper
  // folded in count as read (sourced or with no lookup behind them).
  claims_.beginTurn();
  claims_.noteFresh(freshSourced_, freshUnsourced_);
  freshSourced_ = freshUnsourced_ = 0;   // consumed: never carried into a later turn
  claims_.setHostedTools(d_.cfg.headHostedTools && d_.cfg.headHostedTools());
  // Lifecycle hook: turn start (observer-only; source set by the entry wrapper).
  if (d_.hooks.onTurnStart) {
    TurnStartEv ev;
    ev.source = turnSource_;
    ev.chatId = chatId;
    d_.hooks.onTurnStart(ev);
  }
  turnMem().started = true;
  struct TurnGuard {
    TurnEngine* e;
    explicit TurnGuard(TurnEngine* eng) : e(eng) {
      e->turnInFlight_ = true;
      e->turnStartMs_ = e->nowMs();   // arm the stuck-turn reaper
      e->emitHead((uint8_t)Status::Running);   // styleFor(Running): blue comet
    }
    ~TurnGuard() {
      e->turnInFlight_ = false;
      // W6: if this turn spawned sub-agents, DON'T collapse the head arc now -
      // the children are still running (or pending dispatch), and the main-loop
      // head-arc reconciler keeps it lit until the last one finishes. Only a
      // childless turn frees the arc here. (An empty jobs dep => behave as before.)
      const int children = e->d_.jobs ? e->d_.jobs->activeCount() : 0;
      if (children == 0) e->emitHead((uint8_t)Status::Offline);
    }
  } turnGuard(this);

  // Associative recall (Phase 2): one embed of the user text, fail-open. Skipped
  // for synthesis turns (empty userText) and when heap lacks the headroom for a
  // second TLS handshake ahead of the LLM call (ORCH_RECALL_MIN_HEAP rationale).
  if (d_.fire) d_.fire("turnstart");   // heavy tier: "Roger that."
  // ⚠ Decide the tool loop from the TURN-ENTRY heap, ONCE, before recall spends it.
  //
  // The head-loop's own contract (orch_head_loop.cpp round-0 gate) says entry
  // gating is the CALLER's job - the turn floor already admitted this turn - and
  // that blocking round 0 makes the loop strictly LESS available than a
  // single-shot turn at the same heap. But the caller was re-sampling LIVE heap
  // (loopActiveNow()) a few lines below, AFTER recall's TLS embed and prompt
  // composition transiently depress it. Measured on Board 1, identical across
  // three turns: entry ~41 KB -> afterRecall ~30 KB -> afterCompose ~25 KB, so
  // the gate read ~25 KB and deferred the loop EVERY time recall ran - the device
  // could converse but never use a mid-turn tool. The ~10.4 KB recall dip is a
  // recovering transient (lwIP pbufs/PCB freed async on the tcpip thread a few ms
  // after tlsClose): entry heap is byte-identical turn to turn, so it is NOT a
  // leak. Gating on the admission heap fixes the inconsistency and frees no bytes.
  // Rounds AFTER the first still re-gate live (ORCH_LOOP_MIN_HEAP), and a round-0
  // OOM fails soft, so this cannot wedge the device.
  const uint32_t heapAtEntry = freeHeap();
  if (admitted) turnMem().gate.phase("entry", heapAtEntry, largestFreeBlock());
  else turnMem().gate.begin(heapAtEntry, largestFreeBlock(), t_.turnHardFloor, t_.turnMinLargestBlock);
  const bool loopOnThisTurn = loopActiveAt(heapAtEntry);
  // ONE principal for the whole turn: recall, the tool advertisement below, and
  // applyTurn's dispatch all read the same identity, so a chat cannot be
  // advertised one capability set and judged by another (W14).
  const orch::Principal turnWho = d_.apply.whoFor(chatId);
  orch::RecallOut recall;
  if (!userText.empty() && freeHeap() >= t_.recallMinHeap && d_.recall)
    recall = d_.recall(userText, turnWho);
  const std::vector<std::string>& recalled = recall.hits;
  {
    // The figure check's sources: the input block (the owner's text, captions,
    // the window, fresh results) and, apart, the recalled memories. Only their
    // numbers are kept.
    std::string rec;
    for (const std::string& r : recalled) rec += r + "\n";
    claims_.setSources(inputs, rec);
  }
  const uint32_t heapAfterRecall = freeHeap();
  turnMem().gate.phase("recall", heapAfterRecall, largestFreeBlock());

  // Gather the LIVE inputs (device closure: HAL health, faults, sessions,
  // scratchpad); the engine fills tools/loopOn/recalled so the prompt's
  // advertisement and the loop wiring below share ONE source.
  // W14: scoped to THIS caller - admin-only tools are omitted for a member or
  // guest turn (they refuse at dispatch, so advertising them only produced a
  // failed call and a confusing walk-back, and described the admin surface).
  std::vector<orch::ToolRegistry::Spec> specs =
      d_.toolSpecs ? d_.toolSpecs(turnWho) : std::vector<orch::ToolRegistry::Spec>{};
  // CUM-523: a turn that cannot deliver over Telegram gets no Telegram tool. A web
  // reminder's wakeup turn on a
  // Telegram-less device called reply.telegram, failed, and told the owner it
  // "couldn't send a separate Telegram alert". Dropping the spec here removes it
  // from the prompt's tool list AND the loop's callable set (both read `specs`).
  // A public-mode guest turn keeps reply.telegram (its own chat) but not files.send,
  // which delivers only to an allowlisted chat.
  const bool fileSend = turnFileSend_;
  if (!telegram || !fileSend)
    specs.erase(std::remove_if(specs.begin(), specs.end(),
                               [telegram](const orch::ToolRegistry::Spec& s) {
                                 return telegram ? s.name == "files.send" : isTelegramOnlyTool(s.name);
                               }),
                specs.end());
  // The nightly dream reads and writes memory, nothing else (lane M): no files,
  // sessions, web, routines or replies at 03:30. The same `specs` feed the prompt's
  // tool list and the loop's callable set, and the dispatch below refuses any other
  // name the model calls anyway.
  const bool memoryOnly = dreamTurn_;
  if (memoryOnly)
    specs.erase(std::remove_if(specs.begin(), specs.end(),
                               [](const orch::ToolRegistry::Spec& s) {
                                 return !dream::toolAllowed(s.name);
                               }),
                specs.end());
  turnMem().gate.phase("specs", freeHeap(), largestFreeBlock());
  // Pick the host BEFORE composing: the context budget is derived from the head
  // model's window (owner ask 2026-08-05 - caps derive from the context length
  // allowed for the task), so the model must be known when the prompt is sized.
  // The resolution is the ONE shared rule (resolveTurnHost); the per-turn line names the
  // host each request then really runs on.
  std::string host = resolveTurnHost(d_);
  // The head chosen BEFORE any failover - the "requested" side of served-by
  // disclosure (CUM-236). `host` mutates below on budget/error failover; this and
  // its model stay fixed so a substitution can be reported honestly at turn end.
  const std::string requestedHost = host;
  const std::string requestedModel =
      d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(host) : std::string();

  ComposeInputs ci = d_.composeInputs ? d_.composeInputs(curChat_) : ComposeInputs{};
  turnMem().gate.phase("context", freeHeap(), largestFreeBlock());
  // W10 (prism): the speaker line is for a REAL human message only. The device
  // closure knows the chat, not the turn source - a synthesis turn ([FRESH
  // RESULTS] from untrusted sub-agent output) and a scheduled loop both carry a
  // chat id with nobody behind it, and must not be attributed to the owner.
  ci.speakerPresent = !userText.empty() && turnSource_ != TurnSource::Synthesis &&
                      turnSource_ != TurnSource::Loop;
  // Derive the per-turn context budget from the head model's window. Owner
  // overrides (non-zero MemConfig.maxContextBytes / set NVS loop caps) ride in
  // as BudgetOverrides and win verbatim; 0 = auto. At the 200K anchor the
  // derived values equal the old constants, so a fleet on defaults is
  // byte-identical (pinned by test_orch_budget).
  orch::BudgetOverrides bov;
  bov.maxContextBytes = ci.budgetBytes;  // device closure passes MemConfig's value (0 = auto)
  bov.toolResultCap = d_.cfg.loop.resultCap ? d_.cfg.loop.resultCap() : 0;
  bov.toolTotalCap = d_.cfg.loop.totalCap ? d_.cfg.loop.totalCap() : 0;
  const std::string headModel =
      d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(host) : std::string();
  const orch::ContextBudget budget =
      orch::deriveBudget(orch::modelCtxTokens(host, headModel), bov);
  ci.budgetBytes = budget.systemPromptBytes;
  // H2/R3: the turn-structured history as REAL messages the providers send as
  // history items (prompt_layout.h) - the text window stays empty. Without
  // histV2 the device's rendered window rides the per-turn block and only the
  // summary pair rides as history.
  if (!ci.chatSummary.empty())
    ci.chatSummary = rawFoldSummary(ci.chatSummary);   // header stripped; measureHistory sees it below
  if (histV2On()) {
    ci.history = historyMessages(chatId, budget.recentConvBytes, ci.chatSummary);
    ci.recentConversation.clear();   // the history rides as messages; the closure's
                                     // text window was the histV1 shape
  } else if (!ci.chatSummary.empty()) {
    ci.history = renderHistory({}, ci.chatSummary, 0);   // the summary pair alone
  }
  turnMem().gate.phase("history", freeHeap(), largestFreeBlock());
  // R3: this chat's TTL table choice and its per-chat cache key.
  turnGaps_.note(chatId, nowMs());
  ci.cache.longTtl = turnGaps_.longTtl(chatId);
  ci.cache.key = layout::cacheKey(ci.devName, chatId);
  hlog::logf("budget: ctx=%uK sys=%d res=%u tot=%u (%s)", (unsigned)(budget.ctxTokens / 1000),
             budget.systemPromptBytes, (unsigned)budget.toolResultBytes,
             (unsigned)budget.toolTotalBytes,
             (bov.maxContextBytes > 0 || bov.toolResultCap > 0 || bov.toolTotalCap > 0)
                 ? "owner"
                 : "auto");
  // Advertise only tools that are LIVE on this fabric, under the SANITIZED names
  // the wire accepts (loopToolHidden/loopToolName - shared with the dispatch
  // wiring below so "advertised == callable" can't drift). Verified live:
  // without this block Mistral/OpenAI skip mid-turn tools entirely.
  ci.loopOn = loopOnThisTurn;   // decided from entry heap (above), not live
  // R3: the CONFIG's loop toggle decides whether the tools ride the request
  // (a cached-manifest property), never this turn's heap gate.
  ci.toolsOnWire = d_.cfg.loop.toolLoopOn && d_.cfg.loop.toolLoopOn();
  ci.loopRounds = d_.cfg.loop.rounds ? d_.cfg.loop.rounds() : kLoopRoundsDefault;
  if (bounds_) ci.loopRounds = std::min(ci.loopRounds, bounds_->maxRounds);   // a bounded turn's cap
  // CUM-523: the turn's one sample (unwired = true, wording unchanged). It also
  // narrows the manifest's Telegram flag, so the capability line never offers a
  // channel the tool list just withheld.
  ci.telegram = telegram;
  ci.fileSend = fileSend;
  ci.hw.telegram = ci.hw.telegram && telegram;
  // N11: opt this turn into the simplified v2 prompt when the device flag is set
  // (nullable closure => v1 when unwired: byte-identical, so a default fleet is
  // untouched). The eval gate decides promotion.
  if (d_.cfg.promptV2 && d_.cfg.promptV2()) ci.promptV2 = true;
  ci.tools.clear();
  for (const auto& s : specs) {
    if (loopToolHidden(s.name)) continue;
    ci.tools.push_back(orch::ToolInfo{loopToolName(s.name), s.description});
  }
  ci.recalled = recalled;
  // R5: the model lists move behind models.list when the head can call it: the
  // CONFIG's tool loop is on (toolsOnWire - the same cached-manifest property
  // as the capability list, never this turn's heap gate), the tool is
  // registered, and the head is one with a tool loop (the custom-dialect heads,
  // Z.ai and a custom endpoint, run single-shot: custom.cpp advertises no tool,
  // so they keep the inline lists). PRISM row 1: modelsTool used to key on
  // ci.loopOn (the per-turn heap gate), so a device near the loop-heap band
  // flipped the CACHED static block between the pointer line and the inline
  // lists on alternating turns, busting the static-prefix cache each flip -
  // against the stated policy (compose.cpp: "The CONFIG flag, never this turn's
  // heap gate"). The loop-off caveat rides the dynamic note instead
  // (ci.modelsPointer).
  const bool headLoops = !d_.hosts.fabricSupports || d_.hosts.fabricSupports(host);
  const bool modelsTool =
      ci.toolsOnWire && headLoops &&
      std::any_of(specs.begin(), specs.end(), [](const orch::ToolRegistry::Spec& s) {
        return s.name == "models.list" && !loopToolHidden(s.name);
      });
  ci.catalogs = buildStableCatalogs(modelsTool);
  ci.modelsPointer = modelsTool;   // the loop-off note names models.list (compose.cpp)
  // R3: the skills shelf (its own cached block; "" = none on this fabric).
  if (d_.skillsIndex) ci.skillsShelf = d_.skillsIndex();
  ci.recallDegraded = recall.degraded;
  std::string instructions = agent::composeInstructions(ci);
  turnMem().gate.phase("prompt", freeHeap(), largestFreeBlock());
  // ONE line, only when the loop was actually withheld - the interesting case,
  // and it keeps a healthy device's log clean. entry -> recall -> compose shows
  // which phase spent the headroom that the gate then found missing.
  if (!ci.loopOn && d_.cfg.loop.toolLoopOn && d_.cfg.loop.toolLoopOn())
    hlog::logf("turn heap: entry=%u afterRecall=%u afterCompose=%u floor=%u prompt=%uB",
               (unsigned)heapAtEntry, (unsigned)heapAfterRecall, (unsigned)freeHeap(),
               (unsigned)t_.loopMinHeap, (unsigned)instructions.size());
  // Diagnostics show the cacheable parts as labelled lines (layout::display).
  lastInstructions_ = layout::display(instructions);   // the PROMPT? console command
  // What the device itself sends as history this turn, measured on the prompt as
  // composed (the turn anatomy header; never inferred from the provider chain).
  const HistoryFacts history = measureHistory(instructions, ci.chatSummary, ci.runningMemory);

  // (host was picked above, before compose - the budget derivation needs it.)
  // Conversation state is per-CHAT + per-host (Release B2): the map
  // "chat=host|convId;..." replaces the single global slot that interleaved
  // every channel into one provider thread. Host-switch reset happens inside
  // convMapGet; legacy single-slot values are discarded (one fresh thread per
  // chat at upgrade - the same reset a failover does routinely).
  std::string stored = d_.cfg.provider.convId ? d_.cfg.provider.convId() : std::string();
  std::string convId = convMapGet(stored, chatId, host);

  // Head multi-turn tool-use loop context (opt-in via the tool-loop toggle). When
  // on, the head can call the memory.*/session.*/web.search registry tools mid-turn
  // and iterate, terminating on orch_turn. The dispatch closure routes through the
  // SAME handleMcp path external MCP clients use - so it takes the memory Lock and
  // persists/appliesConfig after a mutating tool, exactly once, in one place.
  // nullptr => adapters run their original single-shot structured turn.
  HeadTools headTools;
  const HeadTools* headToolsPtr = nullptr;
  // Counts tools the loop actually EXECUTED this turn. Guards the retry/failover
  // below: replaying a whole tool loop would re-run its side effects (mem writes,
  // spawns), so once any tool ran we fail soft instead of retrying.
  auto loopDispatched = std::make_shared<int>(0);
  // Lane L (subagents F2): spawn items the head sends by calling the session_ops
  // FIELD as a tool are read and carried into this turn, deduped against its
  // final orch_turn, instead of being answered "not a tool" and lost.
  auto pseudo = std::make_shared<PseudoSpawns>();
  // P6: the loop is the default turn path, but it needs headroom above the
  // single-TLS + per-round floor. Under memory pressure at turn start, fall back
  // to a single-shot turn (still fully functional - no tools mid-turn) and tell
  // the model why, rather than risk a mid-loop OOM. The ~2x round floor margin
  // keeps this rare on the SD-mounted firmware (~30 KB at turn time).
  if (d_.cfg.loop.toolLoopOn && d_.cfg.loop.toolLoopOn() && !loopOnThisTurn) {
    if (boundedSingleShot())
      hlog::logf("orchestrator: tool-loop OFF - a bounded turn on a fallback head runs single-shot");
    else
      hlog::logf("orchestrator: tool-loop DEFERRED - low entry heap %u (single-shot this turn)",
                 (unsigned)heapAtEntry);
  }
  if (loopOnThisTurn) {
    // Advertise only tools that are LIVE on this fabric, under the SANITIZED names
    // the wire accepts (loopToolHidden/loopToolName - shared with the prompt block
    // above so "advertised == callable" can't drift); map back to the registry
    // name on dispatch.
    auto nameMap = std::make_shared<std::map<std::string, std::string>>();
    for (const auto& s : specs) {
      if (loopToolHidden(s.name)) continue;
      std::string adv = loopToolName(s.name);
      (*nameMap)[adv] = s.name;
      headTools.specs.push_back(orch::ToolRegistry::Spec{adv, s.description, s.schemaJson});
    }
    auto mcp = d_.mcpDispatch;
    // v3.7.0: mid-turn tool calls run under the TURN's principal - same chat
    // namespace and owner status the turn's applyTurn writes use, so a member's
    // mid-loop memory.write cannot reach the shared namespace either.
    const nimbus::orch::Principal loopWho = d_.apply.whoFor(chatId);
    // Round prose ("thinking") observer (Glass Box A4): providers forward each
    // round's non-tool text here; the device persists it per-chat.
    headTools.onRoundText = [this](const std::string& text, int round) {
      if (!d_.hooks.onThinking) return;
      ThinkingEv ev;
      ev.chatId = curChat_;
      ev.text = text;
      ev.round = round;
      d_.hooks.onThinking(ev);
    };
    // Canonical-transcript observer (Glass Box P3): each loop hands its brief
    // over on the way out (success OR failure) so the turn dossier can carry the
    // tool loop's actual middle, not just the turn's entry and exit.
    headTools.onBrief = [this](const std::string& brief) { lastBrief_ = brief; };
    // Every failed fabric attempt, with its host (lane T): the chain keeps each
    // host's FIRST cause, which the fabric's own `err` (the last one) cannot.
    headTools.onStepError = [this](const std::string& h, const std::string& e) {
      facts().chain.note(h, e);
    };
    headTools.dispatch = [this, nameMap, loopDispatched, mcp, loopWho, memoryOnly](const orch::HeadToolCall& call)
        -> orch::HeadToolResult {
      if (d_.hooks.onToolCall) d_.hooks.onToolCall(call);   // observer-only
      orch::HeadToolResult r;
      r.id = call.id;
      r.name = call.name;
      // Every exit routes through here so onToolResult fires exactly once per call.
      auto done = [this](orch::HeadToolResult& res) -> orch::HeadToolResult {
        if (d_.hooks.onToolResult) d_.hooks.onToolResult(res);
        return res;
      };
      ++(*loopDispatched);
      // Resolve the advertised (sanitized) name back to the registry name.
      auto it = nameMap->find(call.name);
      const std::string& regName = (it != nameMap->end()) ? it->second : call.name;
      // A memory-only turn (the dream) never runs a tool outside its allowlist, even
      // one the model names without it being advertised.
      if (memoryOnly && !dream::toolAllowed(regName)) {
        r.isError = true;
        r.output = regName + " is not available in this background turn (memory tools only)";
        hlog::logf("dream: refused tool %s (memory tools only)", regName.c_str());
        return done(r);
      }
      // CUM-536: a turn carrying an unverified connector result stores nothing in
      // long-term memory (unverifiedStoreRefusal).
      if (unverifiedTurn_) {
        std::string refusal = unverifiedStoreRefusal(regName);
        if (!refusal.empty()) {
          r.isError = true;
          r.output = std::move(refusal);
          return done(r);
        }
      }
      JsonDocument rq;
      rq["jsonrpc"] = "2.0";
      rq["id"] = 1;
      rq["method"] = "tools/call";
      rq["params"]["name"] = regName;
      JsonDocument ad;
      if (deserializeJson(ad, call.argsJson) == ArduinoJson::DeserializationError::Ok &&
          ad.is<ArduinoJson::JsonObject>())
        rq["params"]["arguments"] = ad;
      else
        rq["params"]["arguments"].to<ArduinoJson::JsonObject>();
      std::string reqStr; serializeJson(rq, reqStr);
      std::string respStr = mcp ? mcp(reqStr, loopWho) : std::string();   // Lock + dispatch + persist
      JsonDocument rd;
      if (deserializeJson(rd, respStr) != ArduinoJson::DeserializationError::Ok) {
        r.isError = true; r.output = "tool response parse error"; return done(r);
      }
      if (!rd["error"].isNull()) {
        r.isError = true;
        r.output = std::string(rd["error"]["message"] | "dispatch error");
        return done(r);
      }
      ArduinoJson::JsonObjectConst res = rd["result"].as<ArduinoJson::JsonObjectConst>();
      r.isError = res["isError"] | false;
      ArduinoJson::JsonArrayConst content = res["content"].as<ArduinoJson::JsonArrayConst>();
      if (!content.isNull() && content.size() > 0)
        r.output = std::string(content[0]["text"] | "");
      return done(r);
    };
    // Lane S: every head tool call joins this turn's effect ledger, from its
    // effect (the shared tool table) and its own result text.
    {
      auto inner = std::move(headTools.dispatch);
      headTools.dispatch = [this, inner, nameMap](const orch::HeadToolCall& call) -> orch::HeadToolResult {
        orch::HeadToolResult r = inner(call);
        auto it = nameMap->find(call.name);
        const std::string& regName = (it != nameMap->end()) ? it->second : call.name;
        claims_.noteTool(fxKindForTool(regName), r.isError, r.output,
                         ClaimCheck::toolCallFlags(regName, call.argsJson));
        return r;
      };
    }
    // Lane L: the per-turn taint flag (one table, orch::toolTaint) and the
    // pseudo-tool salvage wrap the dispatch above; every other call goes
    // through it untouched.
    {
      auto inner = std::move(headTools.dispatch);
      headTools.dispatch = [this, inner, nameMap, loopDispatched, pseudo](
                               const orch::HeadToolCall& call) -> orch::HeadToolResult {
        if (orch::ToolRegistry::isSpawnFieldName(call.name)) {
          orch::HeadToolResult r = takePseudoSpawns(call, *pseudo);
          if (r.output.empty()) return inner(call);   // nothing usable: the recovery text
          if (d_.hooks.onToolCall) d_.hooks.onToolCall(call);
          ++(*loopDispatched);   // a tool ran: the turn is never replayed
          hlog::logf("orchestrator: %s called as a tool - its spawn item(s) join this turn",
                     call.name.c_str());
          if (d_.hooks.onToolResult) d_.hooks.onToolResult(r);
          return r;
        }
        auto it = nameMap->find(call.name);
        const std::string& regName = (it != nameMap->end()) ? it->second : call.name;
        if (!orch::ToolRegistry::isTurnFieldName(regName) &&
            orch::toolTaint(regName) == orch::Taint::External)
          externalContentSeen_ = true;
        return inner(call);
      };
    }
    // Lane I: a successful head skill.get names its id - the device notes the
    // (chat, id) pair in its RAM attach table, and the id joins this turn's
    // skills: row tag. Recording here (not inside the registry handler) keeps
    // the rule in the portable engine, host-tested, and safe from a provider
    // that sanitizes the tool name (regName is resolved through the map). The
    // same wrap publishes the turn's trust facts (skillTurnTainted) to the
    // device's skill.save handler: a save that follows outside content is
    // never self-approved.
    {
      auto inner = std::move(headTools.dispatch);
      headTools.dispatch = [this, inner, nameMap](const orch::HeadToolCall& call)
                               -> orch::HeadToolResult {
        const agent::SkillTurnScope scope(externalContentSeen_);
        const orch::HeadToolResult r = inner(call);
        auto it = nameMap->find(call.name);
        const std::string& regName = (it != nameMap->end()) ? it->second : call.name;
        if (regName != "skill.get" || r.isError || r.output.empty()) return r;
        std::string id;
        {
          JsonDocument a;
          if (deserializeJson(a, call.argsJson) == ArduinoJson::DeserializationError::Ok)
            id = std::string(a["id"] | "");
        }
        if (id.empty()) return r;
        noteSkillUsed(id);
        if (d_.skillUsed) d_.skillUsed(curChat_, id);
        return r;
      };
    }
    // The final-answer checks share ONE correction round (orch::chainFinalChecks):
    // lane L's turn-shape check (a spawn list that cannot be read, a reply that
    // promises sub-agents it does not start) and lane S's claim check (a reply
    // claim that neither the tools this turn ran nor the final turn's own items
    // back). The spawn class is the shape check's: it also counts the spawn items
    // salvaged from pseudo-tool calls, which the final turn's JSON does not carry.
    headTools.cfg.validateFinal = orch::chainFinalChecks({
        [pseudo](const std::string& finalTurn) { return turnShapeCorrection(finalTurn, pseudo.get()); },
        [this](const std::string& finalTurn) { return claims_.finalCheck(finalTurn, /*skipSpawned=*/true); },
    });
    headTools.cfg.maxRounds          = ci.loopRounds;   // user-tunable (P6); the prompt states the same cap
    headTools.cfg.deadlineMs         =
        (uint32_t)(d_.cfg.loop.deadlineS ? d_.cfg.loop.deadlineS() : 600) * 1000U;
    headTools.cfg.roundMinHeap       = t_.loopMinHeap;                 // total-free backstop - not tunable
    headTools.cfg.roundMinLargest    = t_.turnMinLargestBlock;         // CUM-404: per-round fragmentation guard
    // Derived (or owner-overridden) caps - deriveBudget already folded the NVS
    // overrides in. Null hooks (host tests without a loop config) keep the
    // legacy constants so their pins stay meaningful.
    headTools.cfg.maxToolResultBytes = d_.cfg.loop.resultCap ? budget.toolResultBytes : 4096;
    headTools.cfg.maxTotalToolBytes  = d_.cfg.loop.totalCap ? budget.toolTotalBytes : 24576;
    if (bounds_) {   // a bounded turn takes the tighter byte caps (its rounds are in ci.loopRounds)
      headTools.cfg.maxTotalToolBytes = tighterCap(headTools.cfg.maxTotalToolBytes, bounds_->maxToolBytes);
      headTools.cfg.maxToolResultBytes = tighterCap(headTools.cfg.maxToolResultBytes, bounds_->maxResultBytes);
    }
    if (d_.spillResult)
      headTools.spill = [this, loopWho](const orch::HeadToolResult& full) {
        return d_.spillResult(full.name, full.output, loopWho.ns);
      };
    headToolsPtr = &headTools;
    hlog::logf("orchestrator: tool-loop ON (%d tools, %d rounds/%ds)",
               (int)headTools.specs.size(), headTools.cfg.maxRounds,
               d_.cfg.loop.deadlineS ? d_.cfg.loop.deadlineS() : 600);
  }
  turnMem().gate.phase("tools", freeHeap(), largestFreeBlock());

  std::string outJson, err;
  // Lifecycle hook: turn end - fired exactly once on EVERY exit path below
  // (budget refusal, provider failure, parse fail, success), observer-only.
  // `rounds` carries the executed tool-call count (v1 proxy; see hooks.h).
  // Lane T: the one place a turn's spend is metered - once, on every exit, under
  // the hosts and models that billed it (a failed turn's billed rounds included) -
  // and where lastTurnUsage_ is set, so a scheduled fire reports its own spend. A
  // failed exit always carries a cause (the guard below names the host and error for
  // an exit that noted none; test_harness_turn counts every exit).
  auto fireTurnEnd = [&](bool okFlag, size_t replyBytes) {
    if (!okFlag && tf.chain.empty())
      tf.chain.note(host, err.empty() ? std::string("failed with no error text") : err);
    if (!tf.metered) {
      tf.metered = true;
      orch::takeVisionSpend(tf.spend);   // R1: the caption step's spend since the last turn
      meterSpend(tf.spend, attribution_);
    }
    const orch::TokenUsage total = tf.spend.total();
    lastTurnUsage_ = total;
    if (!total.empty()) sessionUsage_ += total;
    {
      // Four slots with their model ids outgrow logf's line: built off the stack, logged whole.
      std::string line(400, '\0');
      line.resize(orch::spendLine(tf.spend, &line[0], line.size()));
      hlog::log((std::string("turn spend: ") + (okFlag ? "ok " : "FAIL ") +
                 turnSourceSlug(turnSource_, attribution_) + " | " + line).c_str());
    }
    // Lane T (19 Oct triage): a turn no host billed was never served by a fallback -
    // fallback=false on its row, and the next turn gets no [FALLBACK] note about it.
    const orch::SpendSlot* last = tf.spend.lastBilled();
    if (!last && tf.switched) pendingFallbackPath_.clear();
    if (!d_.hooks.onTurnEnd) return;
    TurnEndEv ev;
    ev.ok = okFlag;
    ev.host = host;
    ev.usage = total;
    ev.rounds = *loopDispatched;
    ev.replyBytes = replyBytes;
    ev.chatId = chatId;
    if (!okFlag) ev.error = err;   // last provider/parse error (reactive-fold seam)
    // Served-by disclosure (CUM-236): ev.host is the failover-final (served) head;
    // servedModel is the model key it billed under (none when it billed nothing).
    // Compare to the head requested before failover so a display can say "served by
    // <p> <m>" honestly - and never name a model that billed nothing.
    ev.requestedHost = requestedHost;
    ev.requestedModel = requestedModel;
    for (size_t i = tf.spend.n; i-- > 0;)
      if (host == tf.spend.slot[i].host) { ev.servedModel = tf.spend.slot[i].model; break; }
    ev.fallback = last && orch::servedByDisclosure(requestedHost, requestedModel, host,
                                                   ev.servedModel).fallback;
    ev.model = last ? last->model : "";
    ev.source = turnSource_;
    ev.attribution = attribution_;
    ev.spend = &tf.spend;
    ev.chain = okFlag ? nullptr : &tf.chain;
    d_.hooks.onTurnEnd(ev);
  };
  // L24: under the floor at its provider request the turn ends before asking, with
  // one honest reply an owner reads in the chat history (deliver() records it); a
  // routine or synthesis turn ends quietly and its fire is a defer. It runs before
  // the turn takes any held notice, so none is lost.
  auto endShortOfMemory = [&]() -> bool {
    turnMem().refused = true;
    hlog::logf("orchestrator: turn ended for memory before the provider request (under the "
               "turn floor at %s)", turnMem().gate.refusedAt());
    hlog::log(turnMem().gate.line(true).c_str());
    tf.chain.noteDevice(("low memory: internal memory under the turn floor at " +
                         std::string(turnMem().gate.refusedAt())).c_str());
    putBackTaken(turnMem(), d_.jobs, pendingMemResults_, pendingMemSeq_);   // nothing taken is lost
    turnMem().told = true;   // set first: a failed history capture must not send it twice
    if (ownerWaits(turnSource_)) deliver(chatId, kShortOfMemoryReply);
    fireTurnEnd(false, 0);
    return false;
  };
  // runTurnHost: OpenAI / Anthropic / Mistral through the ProviderHosts registry.
  // Every host receives the SAME canonical orch schema via its structured-output
  // mode; an unknown host string is treated as unavailable so it fails over.
  // With headToolsPtr set, every host runs the bounded multi-turn tool-use loop
  // instead of the single shot.
  // The turn's input as each request sends it: the per-turn line for the host it runs on, then
  // the input (lane HFM; the cached system prompt carries none of it). Rebuilt per host tried.
  std::string turnInputs = inputs;
  auto inputsFor = [&](const std::string& h) -> const std::string& {
    turnInputs = runsOnText(d_, h, requestedHost) + inputs;
    return turnInputs;
  };
  auto runTurnHost = [&](const std::string& h, std::string& cv) -> bool {
    // Each attempt bills into its own counter and is filed under its own host and
    // the model configured for it now (lane T): a retry or a failover never books
    // one attempt's spend under another host, and a failed attempt's billed rounds
    // are still filed. Only this attempt's echo can name its model (gate G5).
    orch::TokenUsage attempt;
    // R1: a head loop whose model takes images shows the turn's photos itself; any
    // other path (a single shot, a model without image input) reads them as their
    // machine transcriptions in place of their labels.
    orch::TurnImages* photos = orch::activeTurnImages();
    const std::string model = d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(h) : std::string();
    const bool blind = photos && !photos->empty() && !(headToolsPtr && orch::headTakesImages(h, model));
    const std::string& in = inputsFor(h);
    auto fileAttempt = [&] {
      tf.spend.add(h.c_str(), d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(h).c_str() : "", attempt);
    };
    bool r = false;
    try {
      r = d_.hosts.run(h, cv, instructions, blind ? photos->blindText(in) : in, outJson, err, headToolsPtr,
                       &attempt);
    } catch (const std::bad_alloc&) {
      // A failed allocation after a round billed (the next request being built): the
      // attempt's billed rounds are filed on the way out, so the turn's memory end
      // meters them (F2: round 1 of a thrown head loop was never metered).
      fileAttempt();
      throw;
    }
    fileAttempt();
    if (r) {
      d_.hosts.noteAnswered(h);   // an answer ends a park and its billing episode
    } else {
      tf.chain.note(h, err);
      d_.hosts.noteFailed(h, err);   // a billing or auth refusal parks the host (lane K)
    }
    return r;
  };
  // Keyed AND runnable: the device registered exactly the three orchTurn* hosts,
  // so registry membership mirrors the pre-lift openai/anthropic/mistral check.
  auto hostHasKey = [&](const std::string& h) -> bool {
    return d_.hosts.has(h) && d_.cfg.provider.hasKey && d_.cfg.provider.hasKey(h);
  };
  auto fabricCanHost = [&](const std::string& h) {
    return !d_.hosts.fabricSupports || d_.hosts.fabricSupports(h);
  };
  // Split the providerPriority CSV into trimmed, non-empty entries.
  auto splitPriority = [&]() {
    std::vector<std::string> prio;
    std::string p =
        d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
    for (size_t s2 = 0; s2 < p.length();) {
      size_t c = p.find(',', s2);
      if (c == std::string::npos) c = p.length();
      std::string t = p.substr(s2, c - s2);
      trimInPlace(t);
      s2 = c + 1;
      if (t.length()) prio.push_back(t);
    }
    return prio;
  };
  // Raw providerPriority walk (the pre-engine midFail order) - the safety net the
  // rule engine falls back to, so failover can never regress to "no alternates".
  auto rawAlternates = [&](bool needFabric) {
    std::vector<std::string> out;
    for (const std::string& cand : splitPriority()) {
      if (cand == host || !hostHasKey(cand)) continue;
      if (needFabric && !fabricCanHost(cand)) continue;
      if (d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(cand)) continue;
      out.push_back(cand);
    }
    return out;
  };
  // Classify a head-turn error line into a fallback error class through the ONE
  // typed rule (classifyStepError reads the "HTTP <status>" token and the billing
  // and unreadable-body markers). The old substring test read "generate" and
  // "temperature" as rate limits and any number holding 500 as a server error.
  auto classifyErr = [&](const std::string& e) -> orch::ErrorClass {
    return errorClassOf(orch::classifyStepError(e));
  };
  // The unified fallback ordering (CUM-41): the admin/default rule set decides which
  // providers to try, in what order, for the head model's size class + error class.
  // Applied to BOTH the mid-turn fabric host list and the between-turn ladder, so
  // one rule set governs all turns. Empty result -> the raw priority walk above.
  auto fallbackOrder = [&](orch::ErrorClass ec, bool needFabric) {
    orch::FallbackRuleSet rs;
    std::string rulesJson =
        d_.cfg.provider.fallbackRules ? d_.cfg.provider.fallbackRules() : std::string();
    if (!rulesJson.empty()) orch::parseFallbackRules(rulesJson, rs);
    if (rs.rules.empty()) rs = orch::defaultRuleSet(splitPriority());
    std::string model = d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(host) : std::string();
    orch::TurnContext ctx;
    ctx.provider = host;
    ctx.model = model;
    ctx.sizeClass = orch::sizeClassWord(orch::modelSizeClass(host, model));
    ctx.capability = "chat";
    ctx.errorClass = ec;
    std::set<std::string> used;
    std::vector<std::string> out;
    for (int i = 0; i < 3; ++i) {
      auto avail = [&](const std::string& p, const std::string&) {
        if (p == host || used.count(p) || !hostHasKey(p)) return false;
        if (needFabric && !fabricCanHost(p)) return false;
        return !(d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(p));
      };
      orch::FallbackChoice ch = orch::selectFallback(rs, ctx, avail);
      if (!ch.found) break;
      out.push_back(ch.target.provider);
      used.insert(ch.target.provider);
    }
    if (out.empty()) out = rawAlternates(needFabric);  // never regress below the priority walk
    return out;
  };

  // The setup is done: the floor the turn was admitted on decides again (L24), before
  // any notice is taken or any provider is asked. A sample under it gets a few short
  // waits first: the recall embed's network buffers are freed a few ms after its
  // connection closes, and work beside the turn ends.
  {
    bool fits = turnMem().gate.dispatch("dispatch", freeHeap(), largestFreeBlock());
    for (int i = 0; !fits && i < kDispatchRechecks; ++i) {
      if (d_.platform.delayMs) d_.platform.delayMs(kDispatchRecheckMs);
      fits = turnMem().gate.dispatch("recheck", freeHeap(), largestFreeBlock());
    }
    if (!fits) return endShortOfMemory();
    turnMem().clearTaken();   // it asks a provider now: what it took is consumed
    turnMem().asked = true;
    if (turnMem().gate.setupCost() >= kTurnMemoryLogCost) hlog::log(turnMem().gate.line(true).c_str());
  }
  // HOST-FALLBACK (22 Oct bench): the resolution skips a keyless pin and walks every
  // keyed provider, so with no provider configured at all the head has no key: answer
  // honestly instead of dialing it keyless (a 401) or running on nothing ("Done.").
  // A routine or synthesis turn reaches this without handleMessage's own check.
  if (d_.cfg.provider.hasKey && (host.empty() || !d_.cfg.provider.hasKey(host)) &&
      !anyProviderConfigured()) {
    hlog::logf("orchestrator: no provider has a key (head '%s') - honest local reply", host.c_str());
    deliver(chatId, kNoProviderKeyReply);
    tf.chain.noteDevice("no provider has a key");
    fireTurnEnd(false, 0);
    return false;
  }
  // A pinned head whose key was cleared: the turn runs on the keyed host the
  // resolution found, and the owner hears it once per pin (not every turn).
  {
    const std::string pin = d_.cfg.provider.orchHost ? d_.cfg.provider.orchHost() : std::string();
    if (!pin.empty() && pin != host && d_.cfg.provider.hasKey && !d_.cfg.provider.hasKey(pin)) {
      hlog::logf("orchestrator: pinned head %s has no key - running on %s", pin.c_str(), host.c_str());
      if (keylessPinTold_ != pin && !holdingNotices()) {
        keylessPinTold_ = pin;
        std::string note = providerLabel(pin) + " has no key, so " + providerLabel(host) +
                           " is answering instead. Add a key or choose another provider under Assistant > Models.";
        note[0] = (char)toupper((unsigned char)note[0]);
        holdNotice(note);
      }
    } else if (pin == host) {
      keylessPinTold_.clear();   // keyed again: a later clear is told again
    }
  }
  // A stored model pick the provider's fresh list no longer names keeps running; the
  // owner hears it once (lane F2), on a turn someone is waiting for.
  if (d_.takeModelNotice && !holdingNotices()) {
    const std::string note = d_.takeModelNotice();
    if (!note.empty()) holdNotice(note);
  }
  // Per-provider LLM token budget (owner: "limit budget per provider"). Opt-in - a
  // limit defaults to 0 (unlimited), so this only fires when the owner SET one and the
  // month's ceiling is reached. Prefer failing OVER to a keyed provider still in
  // budget; only refuse the turn when every keyed provider is exhausted.
  if (d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(host)) {
    std::string pr =
        d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
    std::string alt;
    for (size_t start = 0; start < pr.length();) {
      size_t c = pr.find(',', start);
      if (c == std::string::npos) c = pr.length();
      std::string cand = pr.substr(start, c - start);
      trimInPlace(cand);
      start = c + 1;
      if (cand.length() && hostHasKey(cand) && !d_.cfg.budget.overBudget(cand)) { alt = cand; break; }
    }
    if (alt.length()) {
      hlog::logf("orchestrator: %s over token budget -> failover %s", host.c_str(), alt.c_str());
      host = alt; convId = "";
    } else {
      deliver(chatId, std::string("\xE2\x9A\xA0\xEF\xB8\x8F ") + host +
              " has hit its monthly token budget. Raise the limit in Usage & Budget or wait for the reset.");
      tf.chain.noteDevice(("budget: " + host + " and every keyed alternate are over their monthly "
                           "token budget").c_str());
      fireTurnEnd(false, 0);
      return false;
    }
  }

  // Introspection: a provider-side conversation at entry (a stateless marker such
  // as "messages" holds nothing on the provider, so it is not one).
  bool convContinued = providerChainHeld(convId);   // false again if the turn starts past a parked head
  hlog::logf("orchestrator: turn host=%s (conv=%d heap=%u)", host.c_str(),
             (int)convContinued, (unsigned)freeHeap());
  // Stage 2 phase 5: LOOP turns run the engine-owned FABRIC loop when the gate
  // is on - retry/failover happens INSIDE the loop, per round, against the
  // shared transcript (executed results are data; nothing re-dispatches). The
  // legacy between-turn retry/failover below then does not apply: it exists
  // precisely because the old loops could not carry a turn across providers.
  // A head the fabric cannot drive (custom) takes the legacy single-shot path
  // below - its step table only knows the cloud providers. (fabricCanHost above.)
  const bool fabricOn = headToolsPtr && d_.hosts.fabric &&
                        d_.cfg.loop.midTurnFailover && d_.cfg.loop.midTurnFailover() &&
                        fabricCanHost(host);
  bool ok;
  // The hosts this turn ran on, in order, for the next turn's [FALLBACK] note.
  std::string switchPath = host;
  auto noteSwitch = [&](orch::ErrorClass reason) {
    tf.switched = true;
    pendingFallbackPath_ = switchPath;
    pendingFallbackReason_ = reason;
    pendingFallbackWhose_ = turnLabel();
    pendingFallbackMs_ = nowMs();
  };
  // Lane K's park on the single-shot path (the fabric does this inside its loop): a
  // head that refused for billing or auth is not asked again; the turn starts on the
  // first alternate that is not parked, and the next turn's [FALLBACK] note names the
  // head it started past. With no open alternate a head closed by a refusal (out of
  // credit, a refused key) or paused gets no request: the turn ends here with the
  // park's cause (true); its timed re-check or a new key opens it again.
  auto startPastParkedHead = [&]() -> bool {
    providers::HostPark why;
    if (!d_.hosts.isParked(host, &why)) return false;
    for (const std::string& alt : fallbackOrder(errorClassOf(why.kind), /*needFabric=*/false)) {
      if (d_.hosts.isParked(alt)) continue;
      hlog::logf("orchestrator: %s is parked (%s) - starting on %s", host.c_str(),
                 orch::stepErrSlug(why.kind), alt.c_str());
      if (why.kind == orch::StepErr::Billing)   // told already, unless a quiet turn held it
        holdNotice("\xE2\x9A\xA0\xEF\xB8\x8F " + orch::billingNotice(host, why.httpStatus, alt), host, why.kind);
      else if (why.kind == orch::StepErr::Auth)
        holdNotice("\xE2\x9A\xA0\xEF\xB8\x8F " + orch::keyNotice(host, why.httpStatus, alt), host, why.kind);
      switchPath += " -> " + alt;
      noteSwitch(errorClassOf(why.kind));
      host = alt;
      convId = "";
      convContinued = false;
      return false;
    }
    err = why.kind == orch::StepErr::Paused ? "paused: paid requests to " + host + " are paused"
                                            : orch::parkedRefusalError(host, why.kind, why.httpStatus);
    hlog::logf("orchestrator: %s is parked (%s) and no other host is open - no request",
               host.c_str(), orch::stepErrSlug(why.kind));
    tf.chain.note(host, err);
    return true;
  };
  if (fabricOn) {
    std::vector<std::string> hostList{host};
    // Alternates chosen by the unified fallback engine (CUM-41). The error is not
    // known until a round fails, so precompute with a representative transport class.
    for (const std::string& alt : fallbackOrder(orch::ErrorClass::Network, /*needFabric=*/true)) {
      if (hostList.size() >= 3) break;
      hostList.push_back(alt);
    }
    // Each request's input leads with the host it really runs on: the fabric starts past a
    // parked or paused head and may move a round, so it builds the lead per host (HFM).
    headTools.inputLead = [this, &requestedHost](const std::string& h) {
      return runsOnText(d_, h, requestedHost);
    };
    ok = d_.hosts.fabric(
        hostList, instructions, inputs, outJson, err, *headToolsPtr, &tf.spend,
        [&](const std::string& from, const std::string& to) {
          // The notice carries the cause and its HTTP status (lastFabricSwitch).
          // A billing refusal or a refused key is told once per episode (shared with
          // the other seams); a turn that merely started past a host parked for
          // anything else says nothing new.
          const providers::FabricSwitch& sw = providers::lastFabricSwitch();
          const std::string warn = "\xE2\x9A\xA0\xEF\xB8\x8F ";
          // L19-13 d: held until the turn ends (one message for a failed turn).
          if (sw.kind == orch::StepErr::Billing) {
            holdNotice(warn + orch::billingNotice(from, sw.httpStatus, to), from, sw.kind);
          } else if (sw.kind == orch::StepErr::Auth) {
            holdNotice(warn + orch::keyNotice(from, sw.httpStatus, to), from, sw.kind);
          } else if (!sw.parkedStart) {
            holdNotice(warn + orch::stepErrCause(sw.kind, from, sw.httpStatus) + " - switching to " + to +
                       ". Your progress this turn carries over.");
          }
          // The next turn's [FALLBACK] note (lane T) names whose turn this was, the
          // whole path and why the host it left failed: the fabric's typed record of
          // THIS switch (a parked start included: the path then shows the head it
          // started past), else the cause the chain noted for that host just before
          // the switch (a fabric that keeps no switch record).
          switchPath += " -> " + to;
          const orch::FailStep* failed = tf.chain.last();
          const bool typed = sw.from == from && sw.to == to && sw.kind != orch::StepErr::None;
          noteSwitch(typed ? errorClassOf(sw.kind)
                           : classifyErr(failed ? failed->text : std::string()));
          host = to;   // TurnEndEv / conv bookkeeping attribute to the final host
        });
    convId = "";   // stateless loops keep no provider-side conversation
    inputsFor(host);   // the debug capture shows the input as the final host's requests carried it
  } else {
    ok = !startPastParkedHead() && runTurnHost(host, convId);
  }
  // Retry/failover ONLY when no loop tool executed: a replay re-runs the whole tool
  // loop, duplicating side effects (memory writes, spawns). Once tools ran, a failed
  // turn fails soft to the error reply instead. (Fabric turns handled all of this
  // inside the loop - never re-enter here.)
  // CUM-404: a memory-pressure failure ("low memory" from the transport) will fail
  // IDENTICALLY on a same-host retry or on any alternate provider - the internal-heap
  // starvation is device-wide, not provider-specific - so retrying/failing over just
  // burns round-trips and, worse, makes an OOM read as a wrong-provider fallback (the
  // exact symptom CUM-404 tracks). Skip the ladder on OOM and let the honest
  // low-memory reply below fire.
  auto isLowMem = [](const std::string& e) {
    return e.find("low memory") != std::string::npos ||
           e.find("out of memory") != std::string::npos ||
           e.rfind("unsent:", 0) == 0;   // L19-14: the device could not send it (same on every host)
  };
  if (!ok && *loopDispatched > 0)
    hlog::logf("orchestrator: loop ran %d tool(s) before failing - skipping retry/failover",
               *loopDispatched);
  if (!ok && isLowMem(err))
    hlog::logf("orchestrator: low-memory turn failure - skipping retry/failover (%s)", err.c_str());
  // CUM-544: a TIMEOUT is model latency, not a dead link - the same prompt on the
  // same host times out again (Z.ai: 2 x 120 s, then failover at 268 s). Fail over
  // straight away; a transient network error still gets its one same-host retry.
  // Only the custom head's explicit token: a provider error that merely mentions
  // "timeout" (a router 504 text) keeps its same-host retry.
  const bool timedOut = !ok && isTimeout(err);
  if (timedOut && !fabricOn && *loopDispatched == 0)
    hlog::logf("orchestrator: %s did not answer in time - failing over without a retry", host.c_str());
  // The typed rule: only a cause in transit is retried on the same host; a refusal
  // of this request (its content, key, account or model) comes back identical.
  const orch::StepErr errKind = ok ? orch::StepErr::None : orch::classifyStepError(err);
  const bool noRetry = !ok && !orch::fabricRuleFor(errKind).retrySameHost;
  if (noRetry && !timedOut && !fabricOn && *loopDispatched == 0 && !isLowMem(err) && !orch::isParkedRefusal(err))
    hlog::logf("orchestrator: %s refused the request (%s) - no same-host retry", host.c_str(),
               orch::stepErrSlug(errKind));
  if (!ok && !fabricOn && *loopDispatched == 0 && !isLowMem(err) && !noRetry) {   // one same-host retry on a fresh conversation
    hlog::logf("orchestrator: turn err (%s) -> fresh conv retry", err.c_str());
    if (d_.platform.delayMs) d_.platform.delayMs(400);
    // Each attempt re-arms the stuck-turn reaper: it exists to spot a DEAD turn
    // task, and a ladder of live attempts (a head request may take 120 s since
    // CUM-495, then a fallback runs its own loop) must not read as one.
    turnStartMs_ = nowMs();
    convId = ""; ok = runTurnHost(host, convId);
  }
  if (!ok && !fabricOn && *loopDispatched == 0 && !isLowMem(err)) {   // unified fallback ladder (CUM-41): up to 2 alternates
    const orch::ErrorClass ec = classifyErr(err);
    int tried = 0;
    for (const std::string& fb : fallbackOrder(ec, /*needFabric=*/false)) {
      if (ok || tried >= 2) break;
      if (d_.hosts.isParked(fb)) {   // lane K: a parked alternate is not asked again
        hlog::logf("orchestrator: %s is parked - not a failover target", fb.c_str());
        continue;
      }
      tried++;
      hlog::logf("orchestrator: %s down -> failover to %s", host.c_str(), fb.c_str());
      // The notice names the real reason for THIS host (err is the host that just
      // failed - the first host, then each fallback): a provider that did not
      // answer in time is not "unavailable" (CUM-544), and neither is a Cumulo
      // router that refused the model or the account (its own cause and fix,
      // router_refusal.h; bench CUM-530). A billing refusal is told once per
      // episode, like the fabric's (deliverBillingNotice).
      const std::string notice = std::string("\xE2\x9A\xA0\xEF\xB8\x8F ") + failoverReason(host, err) +
                                 fb + " (your recent messages carry over; the provider-side thread restarts).";
      const orch::StepErr failKind = orch::classifyStepError(err);
      const bool told = failKind == orch::StepErr::Billing || failKind == orch::StepErr::Auth;
      holdNotice(notice, told ? host : std::string(), failKind);   // L19-13 d
      // Record the switch as a model-facing context note for the next turn.
      switchPath += " -> " + fb;
      noteSwitch(errorClassOf(failKind));
      turnStartMs_ = nowMs();   // a live attempt, not a stuck turn (see the retry above)
      convId = ""; host = fb; ok = runTurnHost(host, convId);
    }
  }
  // Introspection snapshot - success AND failure, so /api/lastturn always shows
  // what the model actually received on the most recent attempt.
  if (d_.hooks.onTurnDebug) {
    TurnDebugEv ev;
    ev.host = host;
    ev.convContinued = convContinued;
    ev.instructions = &lastInstructions_;   // the labelled form (layout::display)
    ev.inputs = &turnInputs;   // what the last request really sent
    ev.rawOut = &outJson;
    ev.transcriptBrief = &lastBrief_;
    ev.ok = ok && !outJson.empty();
    ev.failure = ev.ok ? nullptr : &tf.chain;
    ev.loopTurn = !lastBrief_.empty();   // only a tool loop hands back a transcript brief
    ev.history = history;
    for (size_t i = tf.spend.n; i-- > 0;)
      if (host == tf.spend.slot[i].host) { ev.servedModel = tf.spend.slot[i].served; break; }
    ev.askedModel = d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(host) : std::string();
    d_.hooks.onTurnDebug(ev);
  }
  if (!ok || outJson.empty()) {
    hlog::logf("orchestrator: turn failed (%s)", err.c_str());
    // Lane DEF: a round crossed the monthly spend ceiling mid-turn and the next request
    // was refused before it left. No error reply: the notice pump tells the owner why,
    // once per channel. Keyed on THIS turn's error, never the global state, so an
    // unrelated failure still gets its honest reply.
    // The same for a turn whose every host the park answered "paused" while the ceiling
    // holds (the park carries the freeze; the owner's own pause still gets its reply).
    const bool ceilingPause = orch::classifyStepError(err) == orch::StepErr::Paused &&
                              nimbus::orch::spendFrozen();
    if (nimbus::orch::isSpendFrozenError(err) || ceilingPause) {
      tf.chain.noteDevice(nimbus::orch::kSpendFrozenError);
      fireTurnEnd(false, 0);
      return false;
    }
    // Lane L (parsing P3): an answer the provider cut at its output limit is not
    // a provider fault. Salvage what it wrote (when the adapter kept the partial
    // turn) with the honest marker, capture it, and record the cut.
    if (orch::isCutError(err)) {
      const SalvageOutcome so = salvageForOwner(outJson, "truncated", /*reportedCut=*/true);
      if (so.claimsSpawn) noteUnappliedSpawnClaim(chatId);
      const std::string fold = foldHeldNotices();   // L19-13 d: one message
      // deliver() captures the row: one message the owner received (salvage text
      // plus any folded credit line), one history row. Salvaged text is the model's
      // own: handed over, the fire spoke (CUM-826).
      deliver(chatId, fold.empty() ? so.text : so.text + "\n\n" + fold, /*modelText=*/so.salvaged);
      err = "cut: " + err;
      fireTurnEnd(false, so.salvaged ? so.text.size() : 0);
      return false;
    }
    // Honest failure reply (owner, 2026-08-05): the old fixed "trouble reaching
    // the orchestrator" line was itself a falsehood in most failures - the
    // provider often DID answer (4xx / schema refusal), tools may already have
    // run, and the QA judge correctly flagged the wording as dishonest when work
    // had happened. Name the real cause class and state that nothing is still
    // running, so a failed turn can't imply phantom background work.
    std::string cause;
    auto has = [&](const char* s) { return err.find(s) != std::string::npos; };
    // CUM-404: a memory-pressure failure is NOT a provider fault. Degrade with an
    // honest low-memory reply instead of a transport-error cause that reads as a
    // wrong-provider fallback. (Device copy: no em dash, no space-hyphen-space.)
    // CUM-477: the copy names what happened and the two things that help.
    const int activeJobs = d_.jobs ? d_.jobs->activeCount() : 0;
    // L19-13 e: every host failed before anything ran - the message itself went
    // unanswered, and the owner is told so (the turn-end row is tagged with its id).
    const bool nothingRan = *loopDispatched == 0;
    if (orch::classifyStepError(err) == orch::StepErr::Unsent) {
      // L19-14: the device could not send its request - its fault, not a provider's.
      deliverFailure(chatId, "That didn't finish - " + orch::stepErrCause(orch::StepErr::Unsent, host, 0) + ".",
                     activeJobs, "try again in a minute", nothingRan);
      if (tf.chain.empty()) tf.chain.noteDevice(err.c_str());
      fireTurnEnd(false, 0);
      return false;
    }
    if (isLowMem(err)) {
      deliverFailure(chatId, "That didn't finish - the device ran out of working memory for this request.",
                     activeJobs, "try a shorter question or wait a minute", nothingRan);
      if (tf.chain.empty()) tf.chain.noteDevice(("low memory: " + err).c_str());
      fireTurnEnd(false, 0);
      return false;
    }
    // A billing refusal is not a rate limit (OpenAI's "no credits" comes as a
    // 429): waiting never helps, credit does; a refused key needs a new key. Both are
    // told once per episode (lane PARK), whether the provider refused or the park
    // ended the turn with no request. (A Cumulo router refusal sentence names no HTTP
    // status, so it never classifies as either and keeps its own words below.)
    const orch::StepErr parkedKind = orch::classifyStepError(err);
    if (parkedKind == orch::StepErr::Billing || parkedKind == orch::StepErr::Auth) {
      const orch::FailStep* last = tf.chain.last();
      const bool named = last && last->host != orch::FailureChain::kDeviceHost && !last->host.empty();
      deliverParkedRefusal(chatId, named ? last->host : host, parkedKind, activeJobs, nothingRan);
      fireTurnEnd(false, 0);
      return false;
    }
    // CUM-460: a 429 says WHICH quota window refused (the adapter's [rl:...] tag
    // from the provider's headers, else the provider's own text). "Wait a minute"
    // is only true for a per-minute window; a spent daily quota or a plan that
    // allows 0 requests gets its own honest reply.
    if (has("HTTP 429")) {
      const std::string fold = foldHeldNotices();   // L19-13 d: one message
      const std::string rl = rateLimitReply(rateLimitFromError(err));
      deliver(chatId, fold.empty() ? rl : rl + " " + fold);
      fireTurnEnd(false, 0);
      return false;
    }
    // Lane T: the owner hears the FIRST cause, not the last host's (the 18 Oct dream
    // died on an Anthropic 400 and was reported as Mistral's bare "chat HTTP 400"),
    // in the typed words (ownerCause), and which other hosts failed after it.
    const orch::FailStep* first = tf.chain.first();
    const OwnerCause oc = ownerCause(first ? first->host : host, first ? first->text : err);
    cause = oc.text;
    std::vector<std::string> alsoFailed;
    for (size_t i = 1; i < tf.chain.size(); ++i)
      if (tf.chain.at(i).host != orch::FailureChain::kDeviceHost &&
          (!first || tf.chain.at(i).host != first->host))
        alsoFailed.push_back(tf.chain.at(i).host);
    std::string also;
    for (size_t i = 0; i < alsoFailed.size(); ++i)
      also += (i == 0 ? "" : (i + 1 == alsoFailed.size() ? " and " : ", ")) + alsoFailed[i];
    if (!also.empty()) cause += "; " + also + " also failed";
    deliverFailure(chatId, "That didn't finish - " + cause + ".", activeJobs,
                   oc.transient ? "ask again to retry" : "", nothingRan);
    fireTurnEnd(false, 0);
    return false;
  }
  flushHeldNotices(chatId);   // L19-13 d: a served turn says what it held, before its reply
  if (d_.cfg.provider.setConvId) {
    // Re-read at write time (a web convReset may have cleared the slot mid-turn)
    // and upsert only THIS chat's entry - other chats' threads stay intact.
    std::string raw = d_.cfg.provider.convId ? d_.cfg.provider.convId() : std::string();
    d_.cfg.provider.setConvId(convMapSet(raw, chatId, host, convId));
  }
  // A real turn completed here (the early-return above already rejected failures), so
  // COUNT it unconditionally - a provider that returns no parseable `usage` object
  // must not leave the turn counter stuck at 0 (owner: 'used chat, count stayed 0').
  // The spend itself (per host, cache reads and writes included) is metered once in
  // fireTurnEnd on every exit, with its "turn spend:" log line (lane T).
  turnCount_++;
  hlog::logf("orchestrator: turn -> %.80s", outJson.c_str());

  // Parse + validate via the PORTABLE contract. On parse failure SALVAGE the reply
  // - never deliver raw wire JSON to a human (owner field bug 2026-07-16: a strict-
  // parse trip sent {"reply":...,"memory":"",...} verbatim to Telegram). The model's
  // JSON is usually syntactically valid (it came from a parsed tool_use input), so
  // pull reply/ask out of it loosely and deliver just that; only when nothing is
  // extractable fall back to a short human-readable error. Full JSON goes to the log.
  orch::Turn turn;
  orch::ParseError perr;
  // CUM-495: read the reply LENIENTLY. A head whose wire cannot pin the schema
  // (Z.ai ignored json_schema and answered in Markdown) still wrote a good
  // answer; it used to be thrown away for the "formatting error" apology. Prose
  // is delivered as the reply - and only the reply (no device actions, memory
  // writes or spawns ride prose); a fenced JSON turn is the turn.
  const orch::ReplyShape shape = orch::parseHeadReply(outJson, turn, perr);
  // One line per field the parse boundary repaired or could not use - the
  // count the live checks grep for (lane L).
  for (const std::string& note : turn.parseNotes)
    hlog::logf("orchestrator: turn field %s", note.c_str());
  if (shape == orch::ReplyShape::Prose)
    hlog::logf("orchestrator: head reply was prose (%u B), delivered as the reply only",
               (unsigned)turn.reply.size());
  if (shape == orch::ReplyShape::Unusable) {
    hlog::logf("orchestrator: turn parse fail (%s) json=%.200s", perr.detail.c_str(),
               outJson.c_str());
    // Lane L (parsing P3): only the reply's leading JSON object is read (bare or
    // fenced, possibly cut short), and only its top-level reply/ask - never the
    // raw text and never a nested field. A cut answer gets the honest marker; the
    // delivered text is captured so the next turn sees what the owner saw.
    const SalvageOutcome so = salvageForOwner(outJson, perr.detail, /*reportedCut=*/false);
    if (so.claimsSpawn) noteUnappliedSpawnClaim(chatId);
    deliver(chatId, so.text, /*modelText=*/so.salvaged);   // captured as the row; salvaged text spoke
    err = (so.cut ? "cut: " : "parse: ") + perr.detail;
    // Lane T: the parse exit notes its synthetic device cause, so the chain and every
    // row name it (the provider answered; the device could not use the answer).
    tf.chain.noteDevice(((so.cut ? std::string("cut: the reply was cut off before it finished (")
                                 : std::string("parse: the reply could not be read as a turn (")) +
                         perr.detail + ")").c_str());
    fireTurnEnd(false, so.salvaged ? so.text.size() : 0);
    return false;
  }
  // Lane L (formatting F4): the reply seam - no channel (Telegram, web, voice,
  // the screen) ever gets reasoning, tool-call markup or a wrapper tag.
  turn.reply = orch::cleanReplyMarkup(turn.reply);
  turn.ask = orch::cleanReplyMarkup(turn.ask);
  // Lane L (formatting F1): fields recovered from leaked tool markup carry
  // actions, and quoted external content (a mail body, a web page) can carry
  // the same markers - so they apply only on an attended owner turn that read
  // no external content. On any other turn only the cut reply stays.
  if (turn.repair.recoveredMask) {
    const bool attended = !scheduledTurn_ && turnSource_ != TurnSource::Synthesis &&
                          turnSource_ != TurnSource::Loop;
    if (!attended || !d_.apply.whoFor(chatId).owner || externalContentSeen_ || unverifiedTurn_) {
      dropRecoveredContainers(turn);
      hlog::log("orchestrator: leaked-markup actions dropped (unattended turn or external content)");
    } else {
      hlog::log("orchestrator: leaked tool markup repaired, recovered actions kept");
    }
  }
  // Lane L (subagents F2): spawn items sent through the pseudo-tool join the turn.
  if (pseudo->calls) {
    const int added = mergePseudoSpawns(turn, *pseudo);
    hlog::logf("orchestrator: %d spawn item(s) from %d spawn-field tool call(s) joined the turn",
               added, pseudo->calls);
  }
  d_.apply.ttsEnabled = d_.cfg.ttsEnabled ? d_.cfg.ttsEnabled() : false;   // live per-turn (owner toggle)
  ApplyState st;
  st.scheduledTurn       = scheduledTurn_;
  st.quietFallback       = quietTurn_;
  st.toolRepliedThisTurn = toolReplied_;
  st.riskNote            = riskNote_;
  PendingMem& parked     = pendingMemResults_[chatId];   // this chat's next turn only
  st.pendingMemResults   = &parked.body;
  st.unverifiedConnector = unverifiedTurn_;
  st.spawnAttempted      = pseudo->calls > 0;
  // Every spawn that does not start reaches the head's next turn as a
  // [SPAWN REFUSED] line for this chat (and the owner line rides this reply).
  st.noteSpawnRefused = [this, chatId](const std::string& name, const std::string& task,
                                       const std::string& reason) {
    if (d_.jobs) d_.jobs->noteSpawnIntentRefused(chatId, name, task, reason);
  };
  st.claims              = &claims_;
  bool spawned = false;
  if (memoryOnly) {
    const int dropped = dream::restrictTurn(turn);
    if (dropped) hlog::logf("dream: dropped %d device/session item(s) (memory only)", dropped);
  }
  if (memoryOnly) {
    // The nightly dream messages nobody while it runs (PG4; unit L, 21 Oct: its
    // "memory was unavailable tonight" summary went to Telegram). Its reply, ask and
    // correction lines are held for the owner's next conversation (FireOutcome.held),
    // and its sound, attention and completion cues stay off; deliver() already holds
    // its engine notices.
    agent::ApplyDeps held = d_.apply;
    held.deliver = [this](const std::string&, const std::string& text) {
      if (text.empty()) return;
      heldReply_ += heldReply_.empty() ? text : "\n\n" + text;
      hlog::logf("quiet turn: reply held for the owner's next turn (%.96s)", text.c_str());
    };
    held.emitAsk = nullptr;
    held.fire = nullptr;
    held.turnComplete = nullptr;
    held.speak = nullptr;
    held.captureAssistant = nullptr;
    agent::applyTurn(turn, chatId, held, st, spawned);
  } else {
    ApplyCapture capture(this, !turn.reply.empty());
    AnswerMark answered(d_.apply, spoke_, d_.deliveryNeedsRow);
    agent::applyTurn(turn, chatId, d_.apply, st, spawned);
  }
  parkMemResults(chatId);
  riskNote_  = st.riskNote;   // consumed (cleared) by the portable apply
  lastReply_ = st.lastReply;  // Local Loops semantic-repeat hash
  // Lane L: whether this turn put anything in front of the owner (the
  // synthesis fallback reads it - a result must never vanish into a silent turn).
  lastTurnSpoke_ = !turn.reply.empty() || !turn.ask.empty() || toolReplied_;
  fireTurnEnd(true, lastReply_.size());
  return true;
} catch (const std::bad_alloc&) {
  // L24: a failed allocation ends the turn honestly, never an abort. The body's
  // guards already ran (the in-flight flag, the head arc, the apply hooks, its PSRAM
  // scope); what is left is the turn end the body would have fired: its spend
  // metered once, the owner told, the end hook paired with the start hook. Nothing
  // was billed before the start hook, and tf is this turn's only after it.
  const PsramFirstScope psramFirst;
  try {
    endTurnAfterFailedAllocation(chatId);
  } catch (...) {   // a second failure while ending: the turn still ends, quietly
    hlog::log("orchestrator: a second allocation failed while ending the turn");
  }
  return false;
}

void TurnEngine::endTurnAfterFailedAllocation(const std::string& chatId) {
  TurnMemoryState& tm = turnMem();
  TurnFacts& tf = facts();
  const bool ended = tm.started && tf.metered;   // fireTurnEnd ran for this turn
  // A routine is deferred (fired again later) only when nothing went out and nothing
  // answered (nothingWentOut): a retry must never repeat a paid round or a sent reply
  // (F2: the post-update turn ran twice and messaged the owner twice). Then what the
  // turn took is put back too, for the next turn.
  tm.refused = nothingWentOut(tm, toolReplied_);
  if (tm.refused) putBackTaken(tm, d_.jobs, pendingMemResults_, pendingMemSeq_);
  hlog::log("orchestrator: turn ended for memory - an allocation failed");
  const bool endOwed = !ended && tm.started;   // the start hook fired, its end did not
  if (endOwed) {
    tf.chain.noteDevice("low memory: an allocation failed during the turn");
    tf.metered = true;
    orch::takeVisionSpend(tf.spend);
    meterSpend(tf.spend, attribution_);
    lastTurnUsage_ = tf.spend.total();
    if (!lastTurnUsage_.empty()) sessionUsage_ += lastTurnUsage_;
  }
  if (!ended && !tm.applied && !tm.told && !toolReplied_ && ownerWaits(turnSource_)) {
    flushHeldNotices(chatId);   // a provider-switch notice it held is said, not dropped
    tm.told = true;
    deliver(chatId, kShortOfMemoryReply);
  }
  if (endOwed && d_.hooks.onTurnEnd) {
    TurnEndEv ev;
    ev.ok = false;
    ev.chatId = chatId;
    ev.usage = lastTurnUsage_;
    ev.error = "low memory: an allocation failed";
    ev.source = turnSource_;
    ev.attribution = attribution_;
    ev.spend = &tf.spend;
    ev.chain = &tf.chain;
    d_.hooks.onTurnEnd(ev);
  }
}

// ---- consolidation ----------------------------------------------------------

void TurnEngine::maybeConsolidate(const std::string& chatId) {
  if (!d_.jobs || !d_.jobs->hasFreshResults()) return;
  // ⚠ The synthesis turn is UNATTENDED input built from sub-agent results - the
  // classic injection vector (research agents ingest untrusted web content, and
  // their output is spliced into this prompt verbatim). It must carry the same
  // no-risk-switches guard as a scheduled turn: without this, a poisoned result
  // could flip sleepOvr/brightOvr/reboot with nobody at the keyboard (review).
  struct UnattendedGuard {
    TurnEngine* e;
    bool prev;
    std::string prevAttr;
    TurnSource prevSrc;
    explicit UnattendedGuard(TurnEngine* eng)
        : e(eng), prev(eng->scheduledTurn_), prevAttr(eng->attribution_),
          prevSrc(eng->turnSource_) {
      e->scheduledTurn_ = true;
      e->attribution_ = "synthesis";              // spend attribution + hook source
      e->turnSource_ = TurnSource::Synthesis;
    }
    ~UnattendedGuard() {
      e->scheduledTurn_ = prev;
      e->attribution_ = prevAttr;
      e->turnSource_ = prevSrc;
    }
  } unattendedGuard(this);
  // CUM-404: the synthesis turn dials a provider too, so it must clear the SAME
  // two-floor guard - the largest-contiguous-block floor, not just total free. A
  // fragmented CYD (largest block below the mbedTLS floor) would otherwise admit an
  // unattended turn that OOMs mid-handshake. Defer (never squeeze) under pressure;
  // the fresh results stay parked and the 60 s fallback delivers them raw if needed.
  if (!turnHeapOk(freeHeap(), largestFreeBlock(), t_.autoTurnMinHeap, t_.turnMinLargestBlock)) {
    hlog::logf("orchestrator: defer auto-synthesis (heap %u/floor %u largest %u/floor %u)",
               (unsigned)freeHeap(), (unsigned)t_.autoTurnMinHeap,
               (unsigned)largestFreeBlock(), (unsigned)t_.turnMinLargestBlock);
    return;
  }
  // L24: admitted - the synthesis setup keeps its small blocks in PSRAM and samples
  // internal SRAM from here (turn_memory.h).
  const PsramFirstScope psramFirst;
  turnMem().gate.begin(freeHeap(), largestFreeBlock(), t_.autoTurnMinHeap, t_.turnMinLargestBlock);
  turnMem().beginEntry();
  // Snapshot the fresh block BEFORE consumption: if the synthesis turn fails
  // (provider outage / failover exhausted), the results must still reach the
  // owner RAW - takeFreshResults() frees the entries, so without this snapshot a
  // failed turn silently destroyed completed sub-agent work (review finding,
  // 2026-07-13: the "60 s fallback" only covers the PRE-consumption defer case).
  // The owner gets the HUMAN rendering (CUM-480), never the model-facing seed.
  const std::string ownerRaw = d_.jobs->ownerResultsText();
  turnMem().fresh = d_.jobs->peekFresh();   // L24: put back if the turn ends for memory
  std::string fresh  = d_.jobs->takeFreshResults(&unverifiedTurn_, &freshSourced_, &freshUnsourced_);
  freshInTurn_ = !fresh.empty();   // lane L: sub-agent text is external content
  bool ok = false;
  try {   // L24: a failed allocation from here is a failed synthesis: the raw results still go out
    std::string inputs = takePendingMemResults(chatId) + takeSpawnNotes(chatId) + fresh + buildDynamicContext(chatId) +
      "\n[SYSTEM]\nOne or more of your sub-agents just finished (others may still be "
      "running - see [ACTIVE SESSIONS]). There is no new user message. "
      "Synthesize their results for the owner in reply, and fold anything worth keeping "
      "into memory.";
    // CUM-536: only when a result's connector never ran (the prompt is otherwise
    // byte-identical), say what that marker means and that memory is closed.
    if (unverifiedTurn_)
      inputs += " A result above opens with a note that its connector was NOT called, or "
                "with [NOT CHECKED] (no lookup ran): the text after that note is the "
                "sub-agent's own guess. Tell the owner plainly that it was not checked, never "
                "present that text as their real or live data, and do not store it (memory "
                "writes are refused this turn).";
    hlog::log("orchestrator: auto-synthesis turn (batch complete)");
    lastTurnSpoke_ = false;
    setTrigger("[sub-agent results arrived]", "synth");
    turnMem().admitted = true;   // runTurn continues this record
    ok = runTurn(inputs, chatId, "");
  } catch (const std::bad_alloc&) {   // before its turn began: nothing was asked or billed
    try {
      putBackTaken(turnMem(), d_.jobs, pendingMemResults_, pendingMemSeq_);
      hlog::log("orchestrator: synthesis turn ended for memory - an allocation failed");
    } catch (...) {
    }
  }
  triggerPending_ = false;
  unverifiedTurn_ = false;
  freshInTurn_ = false;
  freshSourced_ = freshUnsourced_ = 0;
  if (!ok && turnMem().putBack) {
    // L24: ended for memory before asking: the results are parked again (putBackTaken),
    // so the next pump synthesizes them instead of the owner getting a raw dump (the
    // stuck-synthesis fallback still delivers them raw if memory never recovers).
    hlog::log("orchestrator: synthesis turn deferred for memory - results kept for the next try");
  } else if (!ok && fresh.length()) {
    hlog::log("orchestrator: synthesis turn failed - delivering raw results (never lost)");
    d_.jobs->deliverResults(chatId, ownerRaw);   // the job engine's sink, like the stuck fallback
  } else if (ok && fresh.length() && !lastTurnSpoke_) {
    // Lane L (the spawn conservation law): a synthesis turn that "succeeds" with
    // an empty reply would drop the results it was handed - seen live on a
    // mistral-small head that stored a memory and said nothing. The owner gets
    // them raw instead, exactly like a failed synthesis.
    hlog::log("orchestrator: synthesis turn said nothing - delivering raw results (never lost)");
    d_.jobs->deliverResults(chatId, ownerRaw);
  }
  if (d_.journalGc) d_.journalGc();
}

// ---- owner-message turn core ------------------------------------------------

void TurnEngine::handleMessage(const std::string& text, const std::string& fromName,
                               const std::string& chatId) try {
  // R1: the photos the channel offered for this turn (their bytes, refs and labels),
  // taken first so a refused or duplicate turn still frees them, and held until the
  // turn ends: every head loop of the turn shows them to the head as images.
  const std::unique_ptr<nimbus::orch::TurnImages> photos = nimbus::orch::takeTurnImages(chatId, text);
  const nimbus::orch::ActiveTurnImages photosInFlight(photos.get());
  // CUM-211: fail HONEST, never silent. With no routable provider a turn would
  // otherwise burn a keyless round-trip and deliver a misleading cause (a 401
  // "rejected this device's key" when there IS no key), or - on a build without
  // a keyed host wired at all - nothing. Answer LOCALLY instead, before arming a
  // paid turn or the head ring arc, naming the real fix. This is the one turn
  // core every channel funnels through (device screen, Telegram, web, tunnel,
  // voice, serial), so the honest reply reaches all of them from one place.
  if (!anyProviderConfigured()) {
    hlog::log("orchestrator: turn refused - no provider configured (honest local reply)");
    deliver(chatId, kNoProviderKeyReply);
    return;
  }
  // CUM-404: admit on BOTH the total-free backstop AND the largest-contiguous-block
  // guard. Fragmentation (a low largest block while total free is ample) is what
  // actually fails the turn's mbedTLS handshake alloc, so gate on it here rather than
  // let the turn run into a transport error that reads as a provider fault. The reply
  // is the same honest low-memory defer either way (no em dash, no space-hyphen-space).
  if (!turnHeapOk(freeHeap(), largestFreeBlock(), t_.turnHardFloor, t_.turnMinLargestBlock)) {
    hlog::logf("orchestrator: user turn deferred, heap %u/floor %u largest %u/floor %u",
               (unsigned)freeHeap(), (unsigned)t_.turnHardFloor,
               (unsigned)largestFreeBlock(), (unsigned)t_.turnMinLargestBlock);
    deliver(chatId, "One moment, I'm finishing some background work and low on "
                    "working memory. Please resend in a few seconds.");
    return;
  }
  // L24: from here to the turn's end the turn keeps its small blocks in PSRAM, so
  // its setup costs no internal SRAM and the floor above still holds at its provider
  // request, where it is checked again (turn_memory.h).
  const PsramFirstScope psramFirst;
  turnMem().gate.begin(freeHeap(), largestFreeBlock(), t_.turnHardFloor, t_.turnMinLargestBlock);
  turnMem().beginEntry();
  turnMem().told = false;
  // Episodic auto-capture (Q2): record the owner's message before the turn so the
  // history is durable + queryable even if the turn later fails.
  // Tag the row with the SENDER (owner ask: "who sent what" in the unified chat -
  // matters once several Telegram accounts talk to one device). Telegram first_name
  // is attacker-influenced: sanitize like approvePending (strip the tags delimiters
  // + control chars, cap 32). Pseudo-channels carry their channel name here.
  std::string fromTag;
  if (!fromName.empty()) {
    fromTag = "from:";
    for (size_t i = 0; i < fromName.length() && fromTag.length() < 37; i++) {
      char ch = fromName[i];
      if (ch >= 0x20 && ch != ',' && ch != ':') fromTag += ch;
    }
  }
  if (!captureOwnerRow(chatId, text, fromTag)) return;   // H7: already captured, no paid call
  turnMem().gate.phase("row", freeHeap(), largestFreeBlock());

  attribution_ = "turn";                       // owner-driven spend (the default)
  turnSource_ = (chatId == "serial") ? TurnSource::Serial : TurnSource::Owner;
  // CUM-536: results folded into an owner turn carry their provenance flag too.
  unverifiedTurn_ = false;
  if (d_.jobs) turnMem().fresh = d_.jobs->peekFresh();   // L24: put back on a memory refusal
  const std::string fresh =
      d_.jobs ? d_.jobs->takeFreshResults(&unverifiedTurn_, &freshSourced_, &freshUnsourced_) : std::string();
  freshInTurn_ = !fresh.empty();   // lane L: sub-agent text is external content
  // reply.speak / reply.telegram hints ride only a loop turn; the Telegram one only
  // when this turn can deliver over Telegram (CUM-523) - the SAME sample the
  // turn's tool list follows, taken once here.
  const bool telegram = telegramOn(chatId);
  std::string loopHint;
  if (loopActiveNow()) {
    loopHint = " reply.speak additionally speaks a line on the device speaker;";
    if (telegram)
      loopHint += " reply.telegram is ONLY for messaging a DIFFERENT allowlisted Telegram"
                  " chat, never for answering this one.";
  }
  std::string inputs = takePendingMemResults(chatId) + takeSpawnNotes(chatId) + fresh + buildDynamicContext(chatId) +
    "\n[CHANNEL] This message arrived via " + channelOf(chatId) +
    ". Your orch_turn `reply` field is ALREADY delivered to this same channel - that is"
    " the one and only answer; never send the same answer twice." +
    loopHint +
    "\n\n[USER]\n" + text;
  turnMem().gate.phase("inputs", freeHeap(), largestFreeBlock());
  turnMem().admitted = true;   // runTurn continues this record
  runTurn(inputs, chatId, text, telegram);   // text drives associative recall
  unverifiedTurn_ = false;
  freshInTurn_ = false;
  freshSourced_ = freshUnsourced_ = 0;
} catch (const std::bad_alloc&) {
  // L24: an allocation failed before the turn began (runTurn ends its own): the
  // same honest reply, no provider request.
  const PsramFirstScope psramFirst;
  turnMem().admitted = false;
  unverifiedTurn_ = false;
  freshInTurn_ = false;
  freshSourced_ = freshUnsourced_ = 0;
  try {
    putBackTaken(turnMem(), d_.jobs, pendingMemResults_, pendingMemSeq_);   // nothing taken is lost
    hlog::log("orchestrator: turn ended for memory - an allocation failed before it began");
    if (!turnMem().told) {
      turnMem().told = true;
      deliver(chatId, kShortOfMemoryReply);
    }
  } catch (...) {   // a second failure: the turn still ends, quietly
  }
}

// ---- Local Loops: scheduled-turn executor -----------------------------------

nimbus::orch::FireOutcome TurnEngine::injectBoundedTurn(const std::string& chatId, const std::string& prompt,
                                                        const std::string& name, const TurnBounds& bounds) {
  bounds_ = &bounds;
  const nimbus::orch::FireOutcome o = injectScheduledTurn(chatId, prompt, name, "system", /*quietOk=*/true);
  bounds_ = nullptr;
  return o;
}

nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(const std::string& chatId,
                                                          const std::string& prompt,
                                                          const std::string& name,
                                                          const std::string& loopId,
                                                          bool quietOk, bool once,
                                                          bool ownerReminder) try {
  // This fire's flags start clear before anything can throw: the handler below reads
  // them to tell a fire whose turn never went out (a memory deferral) from one that
  // asked a provider or reached the owner (never a deferral), and one whose turn was
  // served (ok, whatever its bookkeeping did after) from one that failed. A previous
  // owner turn's flags must never decide this fire (CUM-826).
  spoke_ = false;
  toolReplied_ = false;
  fireServed_ = false;
  turnMem().asked = false;
  turnMem().applied = false;
  nimbus::orch::FireOutcome o;
  // CUM-404: gate a scheduled fire BEFORE the handshake alloc (round 0) on BOTH the
  // total-free backstop AND the largest-contiguous-block guard - fragmentation (a low
  // largest block while total free is ample) is what actually fails the turn's mbedTLS
  // handshake, so a fragmented CYD must be deferred here rather than admitted into an
  // OOM mid-handshake. CUM-403: a memory defer is NOT a failure - mark it deferred so
  // the tick reschedules and retries instead of burning a daily fire or the breaker.
  if (!turnHeapOk(freeHeap(), largestFreeBlock(), t_.turnHardFloor, t_.turnMinLargestBlock)) {
    o.deferred = true;
    o.detail = "deferred: low memory";
    hlog::logf("loops: scheduled turn deferred, heap %u/floor %u largest %u/floor %u",
               (unsigned)freeHeap(), (unsigned)t_.turnHardFloor,
               (unsigned)largestFreeBlock(), (unsigned)t_.turnMinLargestBlock);
    return o;
  }
  // Lane DEF: at the monthly spend ceiling a routine, a wakeup or the dream is not a
  // fire (deferred: no daily fire burned, no failure breaker rolled) and dials nothing.
  const nimbus::orch::SpendSeam seam = dream::isReserved(loopId) ? nimbus::orch::SpendSeam::Dream
                                      : (once || ownerReminder) ? nimbus::orch::SpendSeam::Wakeup
                                                                : nimbus::orch::SpendSeam::Routine;
  if (!nimbus::orch::paidCallAllowed(seam)) {
    o.deferred = true;
    o.detail = "deferred: monthly spend ceiling reached";
    o.error = nimbus::orch::kSpendFrozenError;   // the scheduler tells it from a memory defer
    hlog::logf("loops: scheduled %s refused - %s", nimbus::orch::spendSeamSlug(seam),
               nimbus::orch::kSpendFrozenError);
    return o;
  }
  // L24: admitted - the fire's setup (its context included) keeps its small blocks in
  // PSRAM and samples internal SRAM from here (turn_memory.h).
  const PsramFirstScope psramFirst;
  turnMem().gate.begin(freeHeap(), largestFreeBlock(), t_.turnHardFloor, t_.turnMinLargestBlock);
  turnMem().beginEntry();
  // A scheduled turn runs on the cheapest tier with a low effort cap whatever the
  // head is (22 Oct: a routine on the priciest head thought for 3K tokens and sent
  // nothing): store::orchModel and the output caps read this role on this task.
  const nimbus::orch::BackgroundTurnScope backgroundTurn;
  lastReply_.clear();
  heldReply_.clear();
  scheduledTurn_ = true;
  quietTurn_ = quietOk;
  attribution_ = loopId.empty() ? "loop" : ("loop:" + loopId);   // spend attribution
  turnSource_ = TurnSource::Loop;
  loopName_ = name;   // the [FALLBACK] note names the routine a switch happened in
  const std::string target =
      chatId.length() ? chatId : (d_.firstAllowedChat ? d_.firstAllowedChat() : std::string());
  // Mirror maybeConsolidate but with a [SCHEDULED LOOP] preamble; do NOT consume
  // fresh results (that clock belongs to sub-agent synthesis). prompt as userText
  // => associative recall runs for the scheduled task. A Once wakeup gets its own
  // honest framing: the model set it FOR ITSELF, it fires exactly once, and the
  // note is the context it asked to have restored.
  const std::string preamble =
    ownerReminder
      ? "\n[REMINDER]\nThe owner set a one-time reminder for this moment (with "
        "/remind) and it just fired. Deliver the reminder to them now - do what "
        "it asks if it's an action, otherwise just remind them plainly. There is "
        "no new owner message; this is the note they left:\n\n"
    : once
      ? "\n[WAKEUP]\nYour one-time wakeup \"" + name + "\" just fired - you scheduled it "
        "for yourself and it has now retired (it will not fire again; set another with "
        "wakeup.set if the work needs one). There is no new owner message. Your note "
        "when you set it:\n\n"
    : quietOk
      // A quiet loop (the dream) succeeds by saying nothing: telling it to "reply to
      // the owner" contradicted its own instructions (lane M). The 22 Oct amendment
      // (SILENT-ROUTINE-COST) makes the two cases explicit: when silence is right,
      // and when a short reply is expected.
      ? "\n[SCHEDULED LOOP]\nYour recurring task \"" + name + "\" is firing on its schedule. "
        "There is no new owner message. Do the task now. Silence is right when the owner "
        "needs nothing: the task found nothing worth telling them, or it did its upkeep "
        "with nothing to change - then reply with nothing at all and the run stays "
        "quiet. If the task DID find something the owner must see or act on, reply "
        "with one short line that says what it is and, if you changed anything, what "
        "you did.\n\n"
      : "\n[SCHEDULED LOOP]\nYour recurring task \"" + name + "\" is firing on its schedule. "
        "There is no new owner message - do the task now and reply to the owner concisely.\n\n";
  // CUM-523: a one-shot (wakeup / reminder) is answered where it was set. Saying
  // so stops the model sending a "separate alert" down another channel - the web
  // reminder's turn did exactly that on a Telegram-less device.
  // Only for a device channel, or a Telegram chat this turn can deliver to (the
  // turn's one sample, which its tool list follows too); "" names no channel.
  const bool telegram = telegramOn(target);
  const bool namedTarget = !target.empty() && (!isTelegramChat(target) || telegram);
  const std::string where =
      ((once || ownerReminder) && namedTarget)
          ? "\n\n(Your reply is delivered to " + std::string(deliveryPlace(target)) +
                ". Do not send a second copy over another messaging channel.)"
          : std::string();
  std::string inputs = takePendingMemResults(target) + takeSpawnNotes(target) + buildDynamicContext(target) +
                       preamble + prompt + where;
  unverifiedTurn_ = false;   // a scheduled turn reads no fresh results
  dreamTurn_ = dream::isReserved(loopId);
  // This fire reports its OWN spend and cause: a turn that fails before any billed
  // round must not carry the previous turn's tokens into the loop caps and the dream
  // row (the 18 Oct dream reported 37,771 tokens it never spent).
  lastTurnUsage_ = {};
  if (ownerReminder)
    setTrigger("[reminder fired]", "remind:" + loopId);
  else if (once)
    setTrigger("[wakeup \"" + name + "\" fired]", "wake:" + loopId);
  else
    setTrigger("[routine \"" + name + "\" ran]", loopId.empty() ? std::string("loop") : "loop:" + loopId);
  turnMem().admitted = true;   // runTurn continues this record
  o.ok = runTurn(inputs, target, prompt, telegram);
  // Recorded before any bookkeeping below can allocate: a served fire whose
  // bookkeeping then fails still reports ok (the handler reads it).
  fireServed_ = o.ok;
  // CUM-826: whether model text reached the owner. A failed fire that did is not run
  // again (a one-shot retires instead of its short retry): the retry would repeat it.
  o.answered = spoke_;
  // 22 Oct amendment (SILENT-ROUTINE-COST): a quiet routine that ends silent
  // records WHY it stayed silent - the run still spent its provider rounds, and
  // without a row the spend reads as a lost turn. The still-pending trigger row
  // (nothing flushed it: no reply was captured) lands now, tagged with the
  // quiet reason, so the fire leaves a trace in the chat's history.
  if (quietOk && triggerPending_ && !turnMem().refused &&
      lastReply_.find_first_not_of(" \t\r\n") == std::string::npos) {
    triggerTags_ += ",silent:quiet-routine";
    hlog::logf("loop: %s stayed silent (quiet routine, empty reply) in=%u out=%u",
               loopId.c_str(), (unsigned)lastTurnUsage_.promptTokens,
               (unsigned)lastTurnUsage_.completionTokens);
    flushTrigger(target);
  }
  triggerPending_ = false;
  scheduledTurn_ = false;
  quietTurn_ = false;
  dreamTurn_ = false;
  attribution_ = "turn";                       // back to the owner default
  turnSource_ = TurnSource::Owner;
  loopName_.clear();
  o.tokens  = lastTurnUsage_;   // this fire's own spend, set on every exit (lane T)
  o.detail  = lastReply_;
  o.held = std::move(heldReply_);
  heldReply_.clear();
  if (!o.ok) o.error = facts().chain.render(nimbus::orch::FailureChain::kTextMax);
  if (!o.ok && turnMem().refused) {   // L24: a memory end is a defer, not a failure (CUM-403)
    o.deferred = true;
    o.detail = "deferred: low memory";
    o.error.clear();
    // A one-shot reminder or wakeup has no next slot worth waiting for (its next slot
    // is its whole delay again): it tries again as soon as memory may have recovered.
    if (once || ownerReminder) o.retryAfterSec = kMemoryRetrySec;
  }
  return o;
} catch (const std::bad_alloc&) {
  // L24: an allocation failed while the fire was set up, or after its turn ended
  // (runTurn ends its own failures). The fire's flags are put back as its normal end
  // does. It is a defer like any memory refusal only when its turn never went out
  // (nothingWentOut, the turn's own rule); a turn that asked a provider or reached the
  // owner is reported failed, never deferred, so it is not run again for memory.
  triggerPending_ = false;
  scheduledTurn_ = false;
  quietTurn_ = false;
  dreamTurn_ = false;
  turnSource_ = TurnSource::Owner;
  nimbus::orch::FireOutcome o;
  o.answered = spoke_;
  o.ok = fireServed_;   // served, then its bookkeeping failed: still a served fire
  bool wentOut = o.ok;
  try {   // the turn record is unreadable only if it never existed (nothing went out)
    wentOut = wentOut || !nothingWentOut(turnMem(), toolReplied_);
  } catch (...) {
  }
  if (wentOut) {
    try {   // allocating from here on: a second failure still returns this outcome, not a defer
      attribution_ = "turn";
      o.tokens = lastTurnUsage_;
      if (o.ok) {
        o.detail = lastReply_;
        o.held = std::move(heldReply_);
        heldReply_.clear();
        hlog::log("loops: scheduled turn served, then an allocation failed in its bookkeeping - reported ok");
      } else {
        o.error = "low memory: an allocation failed after the turn ran";
        hlog::log("loops: scheduled turn ran, then an allocation failed - reported failed, not deferred");
      }
    } catch (...) {
    }
    return o;
  }
  o.deferred = true;
  if (once || ownerReminder) o.retryAfterSec = kMemoryRetrySec;
  try {   // allocating from here on: a second failure still returns the defer
    attribution_ = "turn";
    putBackTaken(turnMem(), d_.jobs, pendingMemResults_, pendingMemSeq_);
    hlog::log("loops: scheduled turn deferred - an allocation failed while it was set up");
    o.detail = "deferred: low memory";
  } catch (...) {
  }
  return o;
}

// ---- stuck-turn reaper ------------------------------------------------------

// (owner bug: a Telegram turn left the ring pulsing blue forever). The head
// "Running" arc + the in-flight flag are cleared ONLY by the TurnGuard dtor, so
// if the turn task dies (OOM / TLS stall mid-call) the dtor never runs and the blue
// processing ring strands with nothing to reset it (the attention watchdog skips
// Running by design). This runs on the always-alive main loop: if a turn has been in
// flight longer than the loop budget + 2 min margin (so it never trips a legitimately
// long agentic turn), it's presumed dead - free the head arc + clear the flag. Returns
// true when it fired (caller repaints).
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

// CUM-211: any registered + keyed host = a routable provider. Mirrors the host
// pick in runTurn (orchHost first, else the priority walk) plus the keyless-OK
// custom/LAN endpoint, but IGNORES budget - an over-budget provider is still
// "configured" and owns its own reply, so it must not surface as "no provider".
bool TurnEngine::anyProviderConfigured() const {
  // Device-truth first: this reports EVERY key slot, including the router providers
  // (Cumulo, Z.ai) that are configured + verified but are not single-shot head
  // hosts - so a Cumulo-only device is "configured" and is never shown the "no
  // provider set up" reply (it falls through to the normal turn path). The head
  // walk below is the fallback for host tests that do not wire this closure.
  if (d_.cfg.provider.anyKeyed) return d_.cfg.provider.anyKeyed();
  auto routable = [&](std::string h) {
    trimInPlace(h);
    if (h.empty() || !d_.hosts.has(h)) return false;                 // not registered
    return !d_.cfg.provider.hasKey || d_.cfg.provider.hasKey(h);     // has a key
  };
  if (d_.cfg.provider.orchHost && routable(d_.cfg.provider.orchHost())) return true;
  if (routable("custom")) return true;   // configured keyless LAN head (registered only when set)
  std::string pr =
      d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
  for (size_t p = 0; p <= pr.size();) {
    size_t c = pr.find(',', p);
    if (routable(pr.substr(p, (c == std::string::npos ? pr.size() : c) - p))) return true;
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return false;
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
  namespace orch = nimbus::orch;
  outSummary.clear();
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
  const std::vector<std::string> hostsToTry = foldHostCandidates();
  if (hostsToTry.empty()) return FoldResult::Deferred;

  // ALWAYS a fresh, throwaway provider thread: the fold must never ride or
  // extend the chat's chain (that is the thing being reset), and its convId is
  // never persisted.
  std::string outJson, err;
  std::string host;
  orch::TokenUsage usage;
  const std::string inputs = orch::buildCompactInputs(prevSummary, digest);
  bool ok = false;
  // Every attempt's spend, failed ones included, filed under its own host and model and
  // metered once as "compact" (lane T: the same per-host rule and R4 cache split as a
  // turn). The turn's spend table is free here: a fold never runs while a turn does.
  orch::TurnSpend& foldSpend = facts().spend;
  foldSpend.clear();
  for (const auto& h : hostsToTry) {
    std::string convId;
    outJson.clear(); err.clear();
    orch::TokenUsage u;
    ok = d_.hosts.run(h, convId, orch::ORCH_COMPACT_PROMPT, inputs,
                      outJson, err, /*tools=*/nullptr, &u);
    foldSpend.add(h.c_str(), d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(h).c_str() : "", u);
    if (ok) { d_.hosts.noteAnswered(h); host = h; usage = u; break; }
    d_.hosts.noteFailed(h, err);
    hlog::logf("fold(%s): %s failed: %s", chatId.c_str(), h.c_str(), err.c_str());
  }
  meterSpend(foldSpend, "compact");
  if (!ok) return FoldResult::Failed;
  orch::Turn t;
  orch::ParseError perr;
  // Same lenient read as a head turn (CUM-495): a prose summary IS the summary.
  if (orch::parseHeadReply(outJson, t, perr) == orch::ReplyShape::Unusable || t.reply.empty()) {
    hlog::logf("fold(%s): no summary in reply (%s)", chatId.c_str(), perr.detail.c_str());
    return FoldResult::Failed;
  }
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

// ---- histV2: the turn-structured history (conversation.h) --------------------

namespace {
// A routine or loop id inside a tag: id characters only, bounded.
std::string tagSafe(const std::string& v) {
  std::string o;
  for (char c : v) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '_' || c == '-' || c == ':';
    if (ok && o.size() < 40) o += c;
  }
  return o;
}
}  // namespace

bool TurnEngine::histV2On() const {
  return d_.histV2 && d_.histV2() && d_.historyRows && d_.captureUserRow;
}

std::string TurnEngine::assistantRowTags() const {
  std::string tags = claims_.effects().fxTag();
  if (!outKind_.empty()) tags += std::string(",") + kOutTag + outKind_;
  // F1 (lane I): the ids this turn read (skill.get) or had attached, so a
  // later turn answers "did you use the skill for that?" from the row - the
  // renderer turns the tag into " (skills: a, b)". Ids join with '.' (the att:
  // precedent): the tag list is comma-separated, so a comma would read as the
  // next tag's key.
  std::string ids;
  for (const auto& id : skillsUsed_) {
    const std::string s = tagSafe(id);
    if (s.empty() || ids.size() + s.size() + 1 > 60) break;
    ids += (ids.empty() ? "" : ".") + s;
  }
  if (!ids.empty()) tags += std::string(",") + kSkillsTag + ids;
  return tags;
}

void TurnEngine::noteSkillUsed(const std::string& id) {
  for (const auto& s : skillsUsed_)
    if (s == id) return;
  if (skillsUsed_.size() >= 4) return;   // the attach block carries at most 4 anyway
  skillsUsed_.push_back(id);
}

namespace {

constexpr size_t kHistoryPageRows = 24;   // one read of the chat's rows (transient)
constexpr int kHistoryMaxPages = 32;      // a flood of one repeated line folds as it is read

// The chat's rows after `startAfter`, newest first, a page at a time. An owner
// row identical to the one read just before it folds into that row's count as
// it is read, so a flood of one message holds one row of memory; reading stops
// at the newest /clear, at `startAfter` and at `maxRows` distinct rows.
void readHistory(const TurnEngine::Deps& d, const std::string& chatId, const std::string& startAfter,
                 size_t maxRows, std::vector<nimbus::orch::EpisodicMessage>& rows,
                 std::vector<uint32_t>& repeats) {
  std::string before;
  for (int page = 0; page < kHistoryMaxPages; page++) {
    std::vector<nimbus::orch::EpisodicMessage> got = d.historyRows(chatId, kHistoryPageRows, before);
    const bool full = got.size() >= kHistoryPageRows;
    if (full) before = got.back().id;
    for (auto& m : got) {
      if (isClearBoundary(m) || (!startAfter.empty() && !rowIdAfter(m.id, startAfter))) return;
      if (!rows.empty() && isRepeatRow(rows.back(), m)) {
        repeats.back()++;
        continue;
      }
      if (rows.size() >= maxRows) return;
      rows.push_back(std::move(m));
      repeats.push_back(1);
      spillToPsram(rows.back().text);   // the kept rows ride PSRAM, the page is transient
      spillToPsram(rows.back().tags);
      spillToPsram(rows.back().blobPath);
    }
    if (!full) return;
  }
}
}  // namespace

void TurnEngine::setTrigger(const std::string& text, const std::string& src) {
  curUserRowId_.clear();   // a seedless turn has no owner row in flight
  triggerText_ = text;
  triggerTags_ = std::string(kTriggerTag) + "," + kSrcTag + tagSafe(src);
  triggerPending_ = histV2On();
}

namespace {
// The owner row's tags: the sender, then what the channel noted (H1).
std::string ownerRowTags(const std::string& fromTag, const history::Inbound& in) {
  std::string tags = fromTag;
  const auto add = [&tags](const std::string& item) { tags += (tags.empty() ? "" : ",") + item; };
  std::string refs;
  for (const auto& r : in.refs) refs += (refs.empty() ? "" : ".") + tagSafe(r);
  if (!refs.empty()) add(kAttTag + refs);
  if (!in.src.empty()) add(kSrcTag + tagSafe(in.src));
  if (in.sentUnix) add(kSentTag + std::to_string(in.sentUnix));
  return tags;
}

// H7: a source already in the chat's recent history (a re-served update, a
// redelivery after a restart).
bool sourceSeen(const TurnEngine::Deps& d, const std::string& chatId, const std::string& src) {
  for (const auto& m : d.historyRows(chatId, 2 * kHistoryPageRows, std::string()))   // 48 rows
    if (tagValue(m.tags, kSrcTag) == src) return true;
  return false;
}
}  // namespace

bool TurnEngine::captureOwnerRow(const std::string& chatId, const std::string& text,
                                 const std::string& fromTag) {
  curUserRowId_.clear();
  triggerPending_ = false;
  if (!histV2On()) {
    if (d_.episodicCaptureUser) d_.episodicCaptureUser(chatId, text, fromTag);
    return true;
  }
  history::Inbound in;
  const bool noted = history::takeInbound(chatId, text, in);
  if (noted && !in.src.empty() && sourceSeen(d_, chatId, in.src)) {
    hlog::logf("history: %s%s is already in this chat's history - delivery dropped", kSrcTag, in.src.c_str());
    return false;   // captured once, answered once
  }
  // Stored once: the media rows keep their records, the owner row what was sent.
  curUserRowId_ = d_.captureUserRow(chatId, noted && !in.said.empty() ? in.said : text,
                                    noted ? ownerRowTags(fromTag, in) : fromTag);
  return true;
}

std::vector<HistTurn> TurnEngine::loadTurns(const std::string& chatId, const HistoryOptions& o,
                                            size_t maxRows) const {
  std::vector<nimbus::orch::EpisodicMessage> rows;
  std::vector<uint32_t> repeats;
  readHistory(d_, chatId, o.startAfter, maxRows, rows, repeats);
  return historyTurns(rows, o, &repeats);
}

std::string TurnEngine::historyBlock(const std::string& chatId, size_t highBytes) {
  if (chatId.empty() || chatId == "system") return std::string();
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  HistoryBudget b;
  b.highBytes = highBytes;
  HistoryOptions o;
  o.inFlightId = curUserRowId_;
  o.startAfter = rowIdAfter(marks.softStart, marks.thru) ? marks.softStart : marks.thru;
  o.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  // Twice the row budget: enough to see past high water and shift once; after
  // the shift the whole history after the soft start fits one read.
  const HistoryWindow w = fitHistory(loadTurns(chatId, o, 2 * b.highRows), marks.softStart, b, o.utcOffsetMin);
  if (w.shifted) {
    hlog::logf("history(%s): overflow shift to %s (%u B kept)", chatId.c_str(), w.softStart.c_str(),
               (unsigned)w.bytes);
    if (d_.noteHistoryShift) d_.noteHistoryShift(chatId, w.softStart);
  }
  return w.block;
}

// R3 (G1): the same fitted turns as real messages (renderHistory), the summary
// pair first (the caller passes the RAW summary; rawFoldSummary).
std::vector<HistMsg> TurnEngine::historyMessages(const std::string& chatId, size_t highBytes,
                                                 const std::string& summary) {
  if (chatId.empty() || chatId == "system")
    return summary.empty() ? std::vector<HistMsg>() : renderHistory({}, summary, 0);
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  HistoryBudget b;
  b.highBytes = highBytes;
  HistoryOptions o;
  o.inFlightId = curUserRowId_;
  o.startAfter = rowIdAfter(marks.softStart, marks.thru) ? marks.softStart : marks.thru;
  o.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  const HistoryWindow w = fitHistory(loadTurns(chatId, o, 2 * b.highRows), marks.softStart, b, o.utcOffsetMin);
  if (w.shifted) {
    hlog::logf("history(%s): overflow shift to %s (%u B kept)", chatId.c_str(), w.softStart.c_str(),
               (unsigned)w.bytes);
    if (d_.noteHistoryShift) d_.noteHistoryShift(chatId, w.softStart);
  }
  return renderHistory(w.kept, summary, o.utcOffsetMin);
}

FoldSlice TurnEngine::historyFoldSlice(const std::string& chatId, size_t capBytes, bool manual) {
  if (!histV2On() || chatId.empty()) return FoldSlice();
  const std::string host = resolveTurnHost(d_);
  HistoryBudget b;
  b.highBytes = orch::deriveBudget(orch::modelCtxTokens(host, d_.cfg.provider.orchModel
                                                                  ? d_.cfg.provider.orchModel(host)
                                                                  : std::string()),
                                   {})
                    .recentConvBytes;
  const HistoryMarks marks = d_.historyMarks ? d_.historyMarks(chatId) : HistoryMarks();
  HistoryOptions o;
  o.startAfter = marks.thru;   // the shift's hidden rows are exactly what a fold must take
  o.utcOffsetMin = d_.utcOffsetMin ? d_.utcOffsetMin() : 0;
  return foldSlice(loadTurns(chatId, o, 400), b.lowBytes(), capBytes, manual, o.utcOffsetMin);
}

void TurnEngine::captureAssistantRow(const std::string& chatId, const std::string& text,
                                     const char* outKind) {
  if (!d_.apply.captureAssistant) return;
  if (!histV2On()) {
    d_.apply.captureAssistant(chatId, text);
    return;
  }
  flushTrigger(chatId);
  outKind_ = outKind;
  d_.apply.captureAssistant(chatId, text);
  outKind_.clear();
}

void TurnEngine::flushTrigger(const std::string& chatId) {
  if (triggerPending_ && d_.captureTrigger) d_.captureTrigger(chatId, triggerText_, triggerTags_);
  triggerPending_ = false;
}

void TurnEngine::clearChatConv(const std::string& chatId) {
  if (!d_.cfg.provider.setConvId) return;
  std::string raw = d_.cfg.provider.convId ? d_.cfg.provider.convId() : std::string();
  // convMapSet with an empty convId removes exactly this chat's record.
  d_.cfg.provider.setConvId(convMapSet(raw, chatId, "", ""));
}

TurnEngine::TurnFacts& TurnEngine::facts() {
  if (!facts_) facts_.reset(new TurnFacts());   // once per boot (PSRAM-sized on the device)
  return *facts_;
}

void TurnEngine::meterSpend(const orch::TurnSpend& spend, const std::string& tag) {
  if (spend.empty()) return;
  if (d_.cfg.budget.recordSpend) {
    d_.cfg.budget.recordSpend(spend, tag);
    return;
  }
  if (!d_.cfg.budget.recordTokens) return;
  for (size_t i = 0; i < spend.n; ++i) {
    const orch::SpendSlot& sl = spend.slot[i];
    const orch::TokenDelta dt = orch::ledgerDelta(sl);
    d_.cfg.budget.recordTokens(sl.host, (uint32_t)dt.in, (uint32_t)dt.out, (uint32_t)dt.cacheR,
                               (uint32_t)dt.cacheW, tag, sl.model);
  }
}

std::string TurnEngine::turnLabel() const {
  switch (turnSource_) {
    case TurnSource::Owner:     return "owner message";
    case TurnSource::Serial:    return "serial console message";
    case TurnSource::Synthesis: return "sub-agent results synthesis";
    case TurnSource::Loop:
      if (attribution_ == "loop:dream") return "nightly dream";
      return loopName_.empty() ? std::string("routine") : "routine '" + loopName_ + "'";
  }
  return "owner message";
}

bool TurnEngine::reapStuckTurn(uint32_t nowMs) {
  if (!turnInFlight_) return false;
  const uint32_t cap =
      (uint32_t)(d_.cfg.loop.deadlineS ? d_.cfg.loop.deadlineS() : 600) * 1000u + 120000u;
  if ((int32_t)(nowMs - turnStartMs_) < (int32_t)cap) return false;
  turnInFlight_ = false;
  emitHead((uint8_t)Status::Offline);   // collapse the blue head arc
  return true;
}

}  // namespace agent
