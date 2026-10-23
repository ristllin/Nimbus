#include "nimbus/harness/jobs.h"

#include <algorithm>   // remove_if - expire timed-out dispatch times
#include <cctype>
#include <cstdio>
#include <cstring>

#include "nimbus/harness/log.h"
#include "nimbus/harness/rate_limit.h" // rateLimitReply - a refused spawn names its window
#include "nimbus/harness/reply_overflow.h"  // the whole reply the envelope cut (L19-36)
#include "nimbus/harness/skill_md.h"   // composeSkillInjection - per-spawn capsule
#include "nimbus/mem_cap.h"   // utf8CapLen - the fresh-result 3500-char UTF-8 cap
#include "nimbus/orch/compact.h"   // kFoldMaxChats - the chat registry bound on spawn notes
#include "nimbus/orch/connectors_wire.h"  // connectorNotCalledNote (CUM-536), isFileSkill
#include "nimbus/orch/sub_provenance.h"   // liveTask, the unverified rule, [tools: n] (lane S)
#include "nimbus/orch/gradient.h"   // foldLine - the overflow-stub one-liner
#include "nimbus/orch/model_choices.h"  // modelInChoices, cheapestChoice - validated spawn-model coercion
#include "nimbus/orch/provider_park.h"  // the one per-provider park: a closed provider gets no spawn
#include "nimbus/orch/reply_markup.h"   // cleanReplyMarkup - the owner fallback is a reply seam
#include "nimbus/orch/router_route.h"  // resolveRouterRoute - a keyless spawn's router selector
#include "nimbus/orch/usage_ledger.h"   // cheapestByEffectivePrice - the ledger's price (CUM-708)
#include "nimbus/orch/vector_memory.h"  // nsForChat - the sub-agent result owner

// Lifted from src/agent/orchestrator.cpp (Stage F). Every owner-visible string,
// log line, timer value, and gate moved BYTE-IDENTICAL - grep-diff the literals
// against the pre-lift file when in doubt; test_harness_jobs pins the
// load-bearing ones (refusals, delivery messages, backoff, synthesis clock).

namespace agent {

namespace orch = nimbus::orch;
namespace attn = nimbus::attn;
using solide::ring::Status;

// ---- pure helpers (moved verbatim) ------------------------------------------

uint32_t keyFromTag(const char* tag) {
  uint32_t h = 2166136261u;
  for (const char* p = tag; p && *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
  return h ? h : 1u;   // never 0 (reserved "no key")
}

std::function<void(const std::string&, const std::string&)> capturingDelivery(
    std::function<bool(const std::string&, const std::string&)> send,
    std::function<void(const std::string&, const std::string&)> capture) {
  return [send, capture](const std::string& chatId, const std::string& text) {
    const bool delivered = send ? send(chatId, text) : true;
    if (delivered && capture) capture(chatId, text);
  };
}

const char kJobDeliveryTag[] = "src:jobs";

bool isJobDelivery(const std::string& rowTags) {
  return ("," + rowTags + ",").find(std::string(",") + kJobDeliveryTag + ",") !=
         std::string::npos;
}

// Map a journal JobState to a ring Status (the ring's attention vocabulary).
static Status ringStatusFor(orch::JobState s) {
  switch (s) {
    case orch::JobState::Queued:     return Status::Idle;      // accepted, not started
    case orch::JobState::Running:    return Status::Running;
    case orch::JobState::NeedsInput: return Status::WaitingInput;
    case orch::JobState::Done:       return Status::Done;
    case orch::JobState::Error:      return Status::Error;
    case orch::JobState::Cancelled:  return Status::Offline;   // frees the segment
    default:                         return Status::Running;   // Unknown: keep it live
  }
}

static void backendOf(const char* jobId, char* out, size_t n) {
  const char* colon = strchr(jobId, ':');
  size_t len = colon ? (size_t)(colon - jobId) : strlen(jobId);
  if (len > n - 1) len = n - 1;
  memcpy(out, jobId, len);
  out[len] = 0;
}

static const char* stateStr(orch::JobState s) {
  switch (s) {
    case orch::JobState::Queued:     return "queued";
    case orch::JobState::Running:    return "running";
    case orch::JobState::NeedsInput: return "needs_input";
    case orch::JobState::Done:       return "done";
    case orch::JobState::Error:      return "error";
    case orch::JobState::Cancelled:  return "cancelled";
    default:                         return "unknown";
  }
}

// Owner-facing label for a sub-agent. The raw "job0003" tag reads badly in Telegram
// (owner complaint: "[job0000x] responses look bad"), so lead with what the owner
// actually cares about - which model is doing what kind of work. The tag stays the
// internal correlation id (logs, the prompt's running-sessions digest, await
// matching), just not in owner-facing messages.
static std::string jobLabel(const orch::JobRecord& rec) {
  if (rec.name[0]) return std::string(rec.name);   // the agent's own chosen name (R5a)
  std::string mdl = rec.model[0] ? std::string(rec.model)
                                 : std::string(rec.backend[0] ? rec.backend : "sub-agent");
  return rec.category[0] ? (mdl + " (" + rec.category + ")") : mdl;
}

// The owner's reply when a spawn's dispatch fails: what happened, then the one
// next step (subagents F3). The switch names every FabricErr with no default,
// so a new error class fails the build's -Wswitch until it has its own words.
static std::string spawnRefusal(FabricErr err, const std::string& provider, RateLimit window) {
  const std::string refused = "Couldn't start that agent on " + provider;
  switch (err) {
    // A Timeout usually means the sub STARTED and outran the dispatch wait (the old
    // "couldn't start" was a lie the owner caught) - but the same signal can be
    // a very slow connection that never delivered the request, so the honest
    // wording claims neither outcome as certain (prism v4.1 #4). The wait is the
    // adapter's own (120 s OpenAI, Anthropic and Cumulo, 60 s Mistral, 50 s Z.ai,
    // 30 s custom), so no number.
    case FabricErr::Timeout:
      return "The agent on " + provider +
             " didn't respond in time - it may have run without me getting "
             "its result. Try a smaller task or split it into steps.";
    // The provider answered 200 but its result could not be read: it ran (and
    // charged), so "couldn't start" would be the same lie.
    case FabricErr::ParseFail:
      return "The agent on " + provider +
             " answered, but I couldn't read its result - it may have run. Try a smaller "
             "task or split it into steps.";
    // CUM-465: a 429 names the quota window that refused, in the head turn's own
    // copy (rateLimitReply), so "wait a minute" is never promised for a spent day.
    case FabricErr::RateLimited: return refused + ". " + rateLimitReply(window);
    case FabricErr::Auth:
      return refused + ": the " + provider +
             " key was refused. Check it under Providers & models.";
    case FabricErr::Network:
      return refused + ": " + provider + " could not be reached. Check the connection and try again.";
    case FabricErr::NotFound:
      return refused + ": " + provider + " does not offer that model or agent. Try another model.";
    case FabricErr::BadRequest:
      return refused + ": " + provider +
             " rejected the request (often a model it does not offer). Try another model or provider.";
    case FabricErr::Unsupported:
      return refused + ": " + provider + " cannot run sub-agents here. Try another provider.";
    case FabricErr::RemoteFail:
      return refused + ": " + provider + " had an error. Try again, or use another provider.";
    case FabricErr::Ok:
      break;
  }
  return refused + ".";
}

// ---- JobEngine --------------------------------------------------------------

JobEngine::JobEngine(Deps d, Tuning t)
    : d_(std::move(d)), t_(t), pollBackoffMs_(t.pollIntervalMs) {
  envSlot_.resize(1);   // the PSRAM-backed poll envelope slot
  // Reserve the (small, bounded) queues up front so enqueue never allocates on
  // the device - the replacement for the old malloc-while-pending tactic.
  pending_.reserve(orch::kAgentMaxJobs);
  fresh_.reserve(orch::kAgentMaxJobs);
  ledger_.reserve(kLedgerMax);
  nextPollAt_ = nowMs();   // was begin(): g_nextPollAt = millis()
}

void JobEngine::deliver(const std::string& chatId, const std::string& text) {
  if (d_.deliver) d_.deliver(chatId, text);
}

// Emit one JobState event with the backend accent hue applied.
void JobEngine::emitJobState(const char* tag, const char* backend, orch::JobState st) {
  if (!d_.event) return;
  attn::Event e;
  e.type = attn::Event::Type::JobState;
  e.key = keyFromTag(tag);
  e.status = (uint8_t)ringStatusFor(st);
  e.hasAccent = true;
  e.accentHue = d_.backendHue ? d_.backendHue(backend ? backend : "") : 255;
  d_.event(e);
}

// Stuck-job watchdog bookkeeping: exact tag match, else a free slot, else slot 0
// (the table is sized to the journal, so eviction is rare and only resets one
// generous ceiling clock - never anything user-visible).
JobEngine::JobWatch& JobEngine::watchFor(const char* tag) {
  int freeSlot = -1;
  for (int i = 0; i < nimbus::orch::kAgentMaxJobs; i++) {
    if (strncmp(watch_[i].tag, tag, sizeof(watch_[i].tag)) == 0) return watch_[i];
    if (freeSlot < 0 && !watch_[i].tag[0]) freeSlot = i;
  }
  if (freeSlot < 0) freeSlot = 0;
  watch_[freeSlot] = JobWatch{};
  strncpy(watch_[freeSlot].tag, tag, sizeof(watch_[freeSlot].tag) - 1);
  return watch_[freeSlot];
}

void JobEngine::watchClear(const char* tag) {
  for (int i = 0; i < nimbus::orch::kAgentMaxJobs; i++)
    if (strncmp(watch_[i].tag, tag, sizeof(watch_[i].tag)) == 0) watch_[i] = JobWatch{};
}

// Clear a job from the RING: an Offline status frees the router slot, which makes the
// Animator play its collapse ("teardown") animation on the next compose. Used to reap
// a completed sub-agent after a short grace so its Done arc doesn't linger forever.
void JobEngine::emitJobCleared(uint32_t key) {
  if (!d_.event) return;
  attn::Event e;
  e.type = attn::Event::Type::JobState;
  e.key = key;
  e.status = (uint8_t)Status::Offline;   // "Offline frees the slot"
  d_.event(e);
}

// Deferred ring teardown. A delivered terminal job (Done/Error) stays on the ring for
// kReapGraceMs so the user sees the result ripple, then collapses. The journal already
// drops it (markSeen -> excluded from count()/get()/sessionInfos), so this is purely
// the ring visual; gc() then compacts the delivered slot once its reap fires.
void JobEngine::scheduleReap(const char* tag, uint32_t graceMs) {
  reap_.push_back({keyFromTag(tag), nowMs() + graceMs});
}

void JobEngine::reapDone() {
  const uint32_t now = nowMs();
  bool reaped = false;
  for (size_t i = 0; i < reap_.size();) {
    if ((int32_t)(now - reap_[i].dueAt) >= 0) {
      emitJobCleared(reap_[i].key);
      reap_.erase(reap_.begin() + i);
      reaped = true;
    } else {
      ++i;
    }
  }
  if (reaped && d_.journal) d_.journal->gc();   // compact the just-collapsed delivered records
}

// The Cumulo router: one key that carries every upstream it lists (CUM-541).
static const char kRouterSlug[] = "cumulo";

// L19-8: the one per-provider park closes a provider for spawns too (out of credit
// or a refused key, learned on any seam, or the pause switch). The spawn seam has no
// key text at hand, so a key change reaches it through the seams that do.
static bool spawnSeamOpen(const std::string& provider, uint32_t nowMs, std::string* why = nullptr) {
  const orch::ParkVerdict v =
      orch::providerPark().available(provider.c_str(), orch::PaidSeam::Spawn, 0, nowMs);
  if (!v.open && why) *why = orch::parkWords(provider.c_str(), v);
  return v.open;
}

// First key'd provider in subPriority order, then the router keys this device
// holds (Cumulo, then Z.ai; the head's own fallback order, routerFallbackHost),
// so a one-key device runs a spawn that names no provider on that key instead of
// refusing it. A provider the park closed is skipped (L19-8). "" if none is open.
std::string JobEngine::firstSubProvider() const { return firstSubProvider(nullptr); }

// trim (matches Arduino String::trim on the device)
static std::string trimmedSlug(std::string p) {
  while (!p.empty() && isspace((unsigned char)p.front())) p.erase(p.begin());
  while (!p.empty() && isspace((unsigned char)p.back())) p.pop_back();
  return p;
}

// A keyed provider the park leaves open runs the spawn; a closed one keeps its words.
bool JobEngine::subProviderRuns(const std::string& p, std::string* closedWhy) const {
  if (p.empty() || !hasKey(p)) return false;
  std::string why;
  if (spawnSeamOpen(p, nowMs(), &why)) return true;
  if (closedWhy && closedWhy->empty()) *closedWhy = why;
  return false;
}

std::string JobEngine::firstSubProvider(std::string* closedWhy) const {
  std::string prio = d_.subPriority ? d_.subPriority() : std::string();
  size_t start = 0;
  while (start < prio.length()) {
    size_t comma = prio.find(',', start);
    if (comma == std::string::npos) comma = prio.length();
    const std::string p = trimmedSlug(prio.substr(start, comma - start));
    if (subProviderRuns(p, closedWhy)) return p;
    start = comma + 1;
  }
  for (const char* r : {kRouterSlug, "zai"})
    if (subProviderRuns(r, closedWhy)) return r;
  return "";
}

// "<provider>/<model>" (a model that already names that provider is kept).
static std::string routerSelector(const std::string& provider, const std::string& model) {
  const std::string prefix = provider + "/";
  return model.rfind(prefix, 0) == 0 ? model : prefix + model;
}

// The router selector a spawn on `provider` (no key of its own) runs as: its own
// model, else the model the coercion gives that provider (subModel), whichever the
// keyed router offers first (modelIsValid("cumulo", ...): the router's choice list
// or catalog). A bare id on the router is its default upstream
// (resolveRouterRoute), so a list of bare ids offers "openai/<id>" as "<id>", and
// that form is returned so the coercion keeps it. "" when the router is not keyed
// or offers neither.
std::string JobEngine::routerOffer(const std::string& provider, const std::string& model) const {
  if (!hasKey(kRouterSlug) || !d_.modelIsValid) return "";
  const std::string sub = subModelOf(provider);
  for (const std::string& m : {model, sub}) {
    if (m.empty()) continue;
    const std::string sel = routerSelector(provider, m);
    if (!orch::routableSelector(sel)) continue;
    if (d_.modelIsValid(kRouterSlug, sel)) return sel;
    const orch::RouterRoute r = orch::resolveRouterRoute(sel);
    if (orch::resolveRouterRoute(r.model).upstream == r.upstream &&
        d_.modelIsValid(kRouterSlug, r.model))
      return r.model;
  }
  return "";
}

// A spawn on a provider with no key that the router cannot carry: the real
// reason, to the owner and to the head, instead of a doomed keyless dispatch.
void JobEngine::refuseKeyless(const PendingSpawn& p, const std::string& provider,
                              const std::string& model) {
  const bool router = hasKey(kRouterSlug);
  const std::string m = !model.empty() ? model : subModelOf(provider);
  const std::string want = m.empty() ? std::string() : routerSelector(provider, m);
  // The router selector that was asked for. Never for custom: the router carries no
  // custom endpoint, so "the router does not offer custom/<model>" would mislead.
  const bool named = router && !want.empty() && provider != "custom";
  // The custom provider is a base URL the owner sets, not a key.
  const std::string what = provider == "custom" ? " endpoint" : " key";
  std::string owner = "Couldn't start that agent on " + provider + ". No " + provider + what + " is set";
  std::string note = provider + " has no" + what + " here";
  if (named) {
    owner += ", and the Cumulo router does not offer " + want;
    note += " and the Cumulo router does not offer " + want +
            "; spawn it on cumulo with a model its [AVAILABLE MODELS] entry lists";
  }
  owner += router ? ". Add the" + what + " under Providers & models, or ask for a model the router lists."
                  : ". Add the" + what + " under Providers & models.";
  hlog::logf("orchestrator: spawn refused (%s has no%s%s%s)", provider.c_str(), what.c_str(),
             named ? ", router does not offer " : "", named ? want.c_str() : "");
  deliver(p.chatId, owner);
  noteSpawnRefused(p.chatId, p.name, p.task, note);
}

// The provider a spawn's skill names as a configured connector's host ("any" for a
// connector any provider runs), or "" when the skill is no connector.
std::string JobEngine::connectorHost(const PendingSpawn& p) const {
  return p.skill[0] && d_.connectorProvider ? d_.connectorProvider(p.skill) : std::string();
}

// A connector task on a provider with no key, or on the Cumulo router: refused with
// the real reason instead of moving to (or running on) the router. The router never
// carries the owner's connectors, and its subs report no connector run, so its
// result could not be marked unverified (CUM-536).
void JobEngine::refuseConnector(const PendingSpawn& p, const std::string& provider,
                                const std::string& host) {
  const std::string skill = p.skill;
  std::string owner = "Couldn't start that agent on " + provider + ". The " + skill + " connector ";
  std::string note = skill + " is a ";
  if (host == "any") {
    owner += "needs a provider key of its own, not the Cumulo router. Add one under Providers & models.";
    note += "connector, and connectors never run on the Cumulo router; spawn it on a provider with a key";
  } else if (hasKey(host)) {
    owner += "runs on " + host + ", so ask for it there.";
    note += host + " connector; spawn it on " + host;
  } else {
    owner += "runs only on a " + host + " key. Add it under Providers & models.";
    note += host + " connector; it runs only on a " + host + " key, never on the Cumulo router";
  }
  hlog::logf("orchestrator: spawn refused (connector %s on %s, not %s)", skill.c_str(), host.c_str(),
             provider.c_str());
  deliver(p.chatId, owner);
  noteSpawnRefused(p.chatId, p.name, p.task, note);
}

// ---- fresh-result store -----------------------------------------------------

void JobEngine::addFreshResult(const char* tag, const char* model, const std::string& text,
                               const std::string& ns, bool failed, bool unverified) {
  // Spill the FULL text to the recent-results ring FIRST (nullable - host rigs
  // without the closure keep the legacy shape). The returned ring tag
  // ("sub:<jobtag>") is what overflow stubs and clip markers reference, so a
  // bounded view can always be widened with results.get.
  std::string ringTag;
  if (d_.spillResult) ringTag = d_.spillResult(tag ? tag : "", model ? model : "", text, ns);
  FreshResult fr;
  // Field caps preserved from the device struct: char tag[12] / char model[40].
  fr.tag   = tag   ? std::string(tag).substr(0, 11)   : std::string();
  fr.model = model ? std::string(model).substr(0, 39) : std::string();
  fr.ringTag = ringTag;   // the real ring handle, used verbatim by the overflow stub
  fr.failed = failed;
  fr.unverified = unverified;
  if ((int)fresh_.size() >= orch::kAgentMaxJobs) {
    // Over the block cap: a one-line STUB instead of the old SILENT DROP
    // (Context Fabric - bounded views over unbounded storage, never loss).
    fr.text = nimbus::orch::foldLine("", text, 160);
    fr.hint = " [full " + std::to_string((unsigned)text.size()) + " B" +
              (ringTag.empty() ? "" : ": results.get(\"" + ringTag + "\")") + "]";
    fresh_.push_back(std::move(fr));
    return;
  }
  // 3500 chars/result (was 220 -> 2000 -> 3500; Glass Box A6 closed most of the
  // asymmetry against the 4096 B ResultEnvelope the sub-agent actually sends).
  const int CAP = 3500;
  int keep = nimbus::utf8CapLen(text.c_str(), (int)text.length(), CAP);
  if (keep < (int)text.length()) {
    fr.text = text.substr(0, (size_t)keep) + "\xE2\x80\xA6";
    if (!ringTag.empty())
      fr.hint = " [full " + std::to_string((unsigned)text.size()) + " B: results.get(\"" +
                ringTag + "\")]";
  } else {
    fr.text = text;
  }
  fresh_.push_back(std::move(fr));
}

std::string JobEngine::takeFreshResults(bool* unverified, int* sourced, int* unsourced) {
  int s = 0, u = 0;
  for (const auto& f : fresh_) {
    if (f.failed) continue;
    (f.unverified ? u : s)++;
  }
  if (sourced) *sourced = s;
  if (unsourced) *unsourced = u;
  return takeFreshResults(unverified);
}

std::string JobEngine::takeFreshResults(bool* unverified) {
  if (unverified) {
    *unverified = false;
    for (const auto& f : fresh_) *unverified = *unverified || f.unverified;
  }
  if (fresh_.empty()) return "";
  std::string b = "[FRESH RESULTS] (sub-agents that just finished)\n";
  // Bound the SEED (Context Fabric, 2026-08-05): a fan-out synthesis inlined all
  // sub-results here - 6 x 3500 chars = ~21 KB in the pinned user turn that the
  // in-turn fold can never trim, which (measured) floored the request body near
  // the wire ceiling. Render full text until a block budget, then one-line stubs
  // whose full text stays fetchable with results.get (the sub-results are already
  // in the ring). This is what makes 6 vs 100 sub-agents cost the head the same.
  const size_t kBlockBudget = 8192;
  int stubbed = 0;
  for (const auto& f : fresh_) {
    // Provenance on every result (lane S): how many tools the sub ran, so the
    // head can tell a looked-up answer from the model's own guess.
    const std::string head = "- " + f.tag + " (" + f.model + ")" + f.provenance + ": ";
    const std::string full = f.text + f.hint;   // the seed keeps the widen handle
    if (b.size() + head.size() + full.size() + 1 <= kBlockBudget) {
      b += head + full + "\n";
    } else {
      // Use the REAL ring tag spillResult() handed back (falls back to the
      // reconstruction only when a host rig ran without the spill closure).
      const std::string handle = !f.ringTag.empty() ? f.ringTag : ("sub:" + f.tag);
      b += head + nimbus::orch::foldLine("", full, 120) +
           " [full: results.get(\"" + handle + "\")]\n";
      stubbed++;
    }
  }
  if (stubbed)
    b += "(" + std::to_string(stubbed) +
         " result(s) summarized - fetch full text with results.get)\n";
  fresh_.clear();
  drained_ = false;   // single reset point: EVERY consumer (batch consolidation, a user
                      // turn folding results in, the raw fallback) clears the stuck
                      // clock - a stale t0 made the next batch false-trip the 60 s raw
                      // fallback (review finding 2026-07-13)
  return b;
}

std::string JobEngine::ownerResultsText() const {
  if (fresh_.empty()) return "";
  // Copy style (AGENTS.md section 6): calm, what happened, no machinery. The
  // title is the agent's owner-facing label (its chosen name, else model and
  // category) - never the job tag, never a results.get handle.
  std::string b = "Here are the raw findings; the summary step did not complete.\n";
  const size_t kBudget = 8192;   // same bound as the seed: Telegram splits, the web
                                 // reply slot does not
  const std::string kFailed = "FAILED: ";
  for (const auto& f : fresh_) {
    // An engine failure line already names its sub ("Job [label] failed: why") -
    // the same line the owner gets when it fails alone - so it needs no title.
    // Decided by the out-of-band flag, never by the text: a sub-agent's own reply
    // that starts with "FAILED: " keeps its title (it cannot pose as the device).
    const bool prefixed = f.failed && f.text.compare(0, kFailed.size(), kFailed) == 0;
    // The raw fallback is a reply seam too: a sub-agent's markup never reaches the owner.
    const std::string body =
        nimbus::orch::cleanReplyMarkup(prefixed ? f.text.substr(kFailed.size()) : f.text);
    const std::string title = f.failed ? std::string() : (f.model.empty() ? "Result" : f.model);
    std::string block = title.empty() ? body : title + "\n" + body;
    if (b.size() + block.size() + 2 > kBudget) {
      const std::string line = nimbus::orch::foldLine("", body, 160);
      block = title.empty() ? line : title + "\n" + line;
    }
    b += "\n" + block + "\n";
  }
  return b;
}

void JobEngine::deliverOwnerFallback() {
  hlog::log("orchestrator: synthesis stuck - delivering raw results (fallback)");
  const std::string chatId = freshChatId_;
  const std::string text = ownerResultsText();
  (void)takeFreshResults();   // consumed: the clock resets, nothing is delivered twice
  deliver(chatId, text);
}

// ---- the spawn ledger (lane L, UNIFIED 3.17) ------------------------------------
// The head kept "6 subs placed ... VERIFY files_list" in its cross-turn notes on
// 17-18 Oct, so every later turn (the dream too) polled a dead project. The truth
// lives here instead: each chat's recent spawns from the job table, rendered into
// that chat's next turn.

namespace {
constexpr size_t kLedgerTaskMax = 120;
constexpr size_t kLedgerReasonMax = 160;

std::string capText(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  const int keep = nimbus::utf8CapLen(s.c_str(), (int)s.size(), (int)n);
  return s.substr(0, (size_t)keep) + "\xE2\x80\xA6";
}
}  // namespace

void JobEngine::ledgerQueued(const PendingSpawn& p) {
  if (ledger_.size() >= kLedgerMax) ledger_.erase(ledger_.begin());
  LedgerEntry e;
  e.chatId = p.chatId;
  e.name = p.name;
  e.task = capText(p.task, kLedgerTaskMax);
  e.provider = p.provider;
  e.asked = p.model;
  e.state = LedgerState::Queued;
  e.atMs = nowMs();
  ledger_.push_back(std::move(e));
}

JobEngine::LedgerEntry* JobEngine::queuedEntry(const PendingSpawn& p) {
  const std::string task = capText(p.task, kLedgerTaskMax);
  for (auto& e : ledger_)
    if (e.state == LedgerState::Queued && e.chatId == p.chatId && e.name == p.name && e.task == task)
      return &e;
  return nullptr;
}

void JobEngine::ledgerStarted(const PendingSpawn& p, const char* tag, const std::string& provider,
                              const std::string& model) {
  LedgerEntry* e = queuedEntry(p);
  if (!e) return;
  e->state = LedgerState::Running;
  e->tag = tag;
  e->provider = provider;
  e->model = model;
  e->atMs = nowMs();
}

void JobEngine::ledgerSwapped(const PendingSpawn& p, const std::string& why) {
  if (LedgerEntry* e = queuedEntry(p)) e->swapped = capText(why, kLedgerReasonMax);
}

void JobEngine::ledgerRefused(const std::string& chatId, const std::string& name,
                              const std::string& task, const std::string& reason) {
  const std::string t = capText(task, kLedgerTaskMax);
  for (auto& e : ledger_) {   // a queued spawn that failed at dispatch
    if (e.state != LedgerState::Queued || e.chatId != chatId || e.name != name || e.task != t) continue;
    e.state = LedgerState::Refused;
    e.reason = capText(reason, kLedgerReasonMax);
    e.atMs = nowMs();
    return;
  }
  if (ledger_.size() >= kLedgerMax) ledger_.erase(ledger_.begin());
  LedgerEntry e;
  e.chatId = chatId;
  e.name = name;
  e.task = t;
  e.reason = capText(reason, kLedgerReasonMax);
  e.state = LedgerState::Refused;
  e.atMs = nowMs();
  ledger_.push_back(std::move(e));
}

void JobEngine::ledgerFinished(const char* tag, LedgerState st, const std::string& reason) {
  for (auto& e : ledger_) {
    if (e.tag != tag) continue;
    e.state = st;
    e.reason = capText(reason, kLedgerReasonMax);
    e.atMs = nowMs();
    return;
  }
}

void JobEngine::noteSpawnIntentRefused(const std::string& chatId, const std::string& name,
                                       const std::string& task, const std::string& reason) {
  noteSpawnRefused(chatId, name.c_str(), task, reason);
}

// One ledger line: who, then what became of it.
std::string JobEngine::ledgerLine(const LedgerEntry& e, uint32_t now) {
  const unsigned mins = (unsigned)((now - e.atMs) / 60000u);
  const std::string ago = mins == 0 ? "just now" : std::to_string(mins) + " min ago";
  std::string who = !e.name.empty() ? e.name : nimbus::orch::foldLine("", e.task, 40);
  if (!e.tag.empty()) who += " (" + e.tag + ", " + e.provider + "/" + e.model + ")";
  std::string line = "- " + who + ": ";
  switch (e.state) {
    case LedgerState::Queued:    line += "queued, not started yet; it starts when a slot frees"; break;
    case LedgerState::Running:   line += "running, started " + ago; break;
    case LedgerState::Done:      line += "finished " + ago + "; its result came back as a fresh result"; break;
    case LedgerState::Failed:    line += "failed " + ago + (e.reason.empty() ? "" : " - " + e.reason); break;
    case LedgerState::Refused:   line += "did not start (" + ago + ") - " + e.reason; break;
    case LedgerState::Cancelled: line += "stopped " + ago; break;
  }
  if (!e.swapped.empty())
    line += " (" + e.swapped + ")";
  else if (!e.asked.empty() && !e.model.empty() && e.asked != e.model)
    line += " (asked for " + e.asked + ", which " + e.provider + " does not list)";
  return line + "\n";
}

std::string JobEngine::spawnLedgerText(const std::string& chatId) const {
  const uint32_t now = nowMs();
  std::string lines;
  int shown = 0;
  for (auto it = ledger_.rbegin(); it != ledger_.rend() && shown < 8; ++it) {
    if (it->chatId != chatId || now - it->atMs > kLedgerWindowMs) continue;
    lines += ledgerLine(*it, now);
    shown++;
  }
  if (lines.empty()) return "";
  return "[SPAWN LEDGER] (this chat's sub-agents from the device's own job table, newest first. "
         "It is the truth about them: never re-check it with file or session tools, and never "
         "copy it into your notes.)\n" + lines;
}

// ---- spawn enqueue + dispatch -----------------------------------------------

// CUM-538: one model-facing line per spawn that never started, so the head's next
// turn knows the real reason (and does not invent one). Kept per spawning chat,
// with a per-chat budget: past it that chat gets one fold line and nothing more,
// and no other chat is affected. At most kSpawnNoteChats chats hold notes (the
// chat registry bound); a further chat's first note drops the notes that have
// waited longest (their owner was told each refusal at once), but never a chat's
// "may have started" line while any entry holds none (see spawnNotesFor). The per-chat text
// grows past the small-allocation threshold at once (reserved), so on the device
// it lives in PSRAM, not internal SRAM.
static constexpr size_t kSpawnNotesMax = 640;
// A "may have started" line is never folded away while this much room is left:
// dropping it would let the head spawn a paid job twice. Timeouts are rare (each
// holds the single dispatch for its whole 30 to 60 s wait), so the extra room is too.
static constexpr size_t kSpawnNotesMaybeMax = 2 * kSpawnNotesMax;
static constexpr size_t kSpawnNoteChats = orch::kFoldMaxChats;
static const char kSpawnNotesFold[] = "(more refusals not listed)\n";

// A new chat takes the place of the oldest entry with no "may have started" line:
// dropping that line would let its chat's head spawn the paid job twice. When
// every entry holds one, only another "may have started" note displaces the
// oldest; a plain refusal is not kept (its owner was told it at once).
JobEngine::SpawnNotes* JobEngine::spawnNotesFor(const std::string& chatId, bool mayHaveRun) {
  for (auto& e : spawnNotes_)
    if (e.chatId == chatId) return &e;
  if (spawnNotes_.size() >= kSpawnNoteChats) {
    auto victim = std::find_if(spawnNotes_.begin(), spawnNotes_.end(),
                               [](const SpawnNotes& e) { return !e.mayHaveRun; });
    if (victim == spawnNotes_.end()) {
      if (!mayHaveRun) return nullptr;
      victim = spawnNotes_.begin();
    }
    spawnNotes_.erase(victim);
  }
  spawnNotes_.push_back(SpawnNotes{chatId, std::string(), false, false});
  spawnNotes_.back().text.reserve(kSpawnNotesMax + sizeof(kSpawnNotesFold));
  return &spawnNotes_.back();
}

void JobEngine::noteSpawnRefused(const std::string& chatId, const char* name,
                                 const std::string& task, const std::string& reason,
                                 bool mayHaveRun) {
  ledgerRefused(chatId, name ? name : "", task,
                (mayHaveRun ? "may have run, but its result will not arrive: " : "") + reason);
  SpawnNotes* np = spawnNotesFor(chatId, mayHaveRun);
  if (!np) return;
  SpawnNotes& n = *np;
  if (mayHaveRun) n.mayHaveRun = true;   // the framing warns even if the line cannot fit
  if (n.folded && !mayHaveRun) return;
  const std::string who = (name && name[0]) ? std::string(name)
                                            : nimbus::orch::foldLine("", task, 40);
  const std::string line = "- '" + who +
                           (mayHaveRun ? "' may have started: " : "' did not start: ") +
                           reason + "\n";
  const size_t budget = mayHaveRun ? kSpawnNotesMaybeMax : kSpawnNotesMax;
  if (n.text.size() + line.size() <= budget) {
    n.text += line;
  } else if (!n.folded) {
    n.text += kSpawnNotesFold;
    n.folded = true;
  }
}

std::string JobEngine::takeSpawnNotes(const std::string& chatId, bool* mayHaveRun) {
  if (mayHaveRun) *mayHaveRun = false;
  for (size_t i = 0; i < spawnNotes_.size(); ++i) {
    if (spawnNotes_[i].chatId != chatId) continue;
    if (mayHaveRun) *mayHaveRun = spawnNotes_[i].mayHaveRun;
    std::string out = std::move(spawnNotes_[i].text);
    spawnNotes_.erase(spawnNotes_.begin() + (long)i);
    return out;
  }
  return "";
}

// Expired entries are dropped on every read and write, so a stale time can never
// count again when millis() wraps (49.7 days).
void JobEngine::expireTimeouts() const {
  const uint32_t now = nowMs();
  timedOutAt_.erase(std::remove_if(timedOutAt_.begin(), timedOutAt_.end(),
                                   [now](uint32_t at) { return now - at >= kMaybeRunningMs; }),
                    timedOutAt_.end());
}

void JobEngine::noteDispatchTimeout() {
  expireTimeouts();
  if ((int)timedOutAt_.size() >= orch::kAgentMaxJobs) timedOutAt_.erase(timedOutAt_.begin());
  timedOutAt_.push_back(nowMs());
}

int JobEngine::maybeRunningCount() const {
  expireTimeouts();
  return (int)timedOutAt_.size();
}

void JobEngine::noteLiveTag(const char* tag, bool live) {
  constexpr size_t kLiveTagsMax = 16;
  if (liveTags_.size() >= kLiveTagsMax) liveTags_.erase(liveTags_.begin());
  LiveTag t{};
  strncpy(t.tag, tag ? tag : "", sizeof(t.tag) - 1);
  t.live = live;
  liveTags_.push_back(t);
}

bool JobEngine::liveFor(const char* tag) const {
  for (const LiveTag& t : liveTags_)
    if (tag && strcmp(t.tag, tag) == 0) return t.live;
  return false;
}

std::string JobEngine::deliveredReply(const orch::JobRecord& rec, bool& unverified) {
  const orch::ResultEnvelope& e = env_ref();
  std::string reply = e.reply[0] ? std::string(e.reply) : std::string("(done, no output)");
  // Provenance (CUM-536, then lane S): the result is the model's own guess when
  // the task asked for an attached connector that never ran, or asked for live
  // data (a status, today's figures) and the sub ran no tool at all - counted
  // from the provider's own record (ResultEnvelope.toolRuns). The marker is put
  // on BEFORE any copy is made (the result hook's trace row, the project
  // auto-persist, [FRESH RESULTS], the owner's raw fallback), and the flag makes
  // the turn that reads it refuse memory writes.
  const bool connectorGuess = e.connectorAsked[0] && e.connectorCalls == 0;
  unverified = nimbus::orch::subResultUnverified(e.connectorAsked[0] != 0, e.connectorCalls,
                                                 liveFor(rec.tag), e.toolRuns);
  if (!unverified) return reply;
  if (connectorGuess) {
    hlog::logf("orchestrator: %s asked for %s but no connector ran - result marked unverified",
               rec.tag, e.connectorAsked);
    return nimbus::orch::connectorNotCalledNote(e.connectorAsked) + "\n\n" + reply;
  }
  hlog::logf("orchestrator: %s asked for live data but ran no tool - result marked not checked",
             rec.tag);
  return nimbus::orch::liveDataNotCheckedNote() + "\n\n" + reply;
}

bool JobEngine::enqueueSpawn(const orch::Spawn& s, const std::string& chatId, bool quiet) {
  if (s.task.length() == 0) {   // never silent (lane L): the head hears why
    noteSpawnRefused(chatId, s.name.c_str(), s.task, "it was sent without a task");
    return false;
  }
  if ((int)s.task.length() > orch::kSpawnTaskMax - 1)   // was a SILENT strncpy clip
    hlog::logf("orchestrator: spawn task truncated %d -> %d chars",
               (int)s.task.length(), orch::kSpawnTaskMax - 1);
  // The QUEUE depth (how many a turn may enqueue) is decoupled from concurrency:
  // the pump drains one/cycle into the ≤kMaxActiveInflight window, so a deep wave
  // is accepted and runs SEQUENTIALLY (NOT concurrently). Refuse only when the
  // inbox itself is full. The refused spawn is DROPPED (nothing re-queues it) -
  // the message must say so, not promise a retry that never happens (prism #5).
  if ((int)pending_.size() >= orch::kMaxPendingSpawns) {
    deliver(chatId, "My spawn queue is full - I couldn't take that one. Ask "
                    "again once some agents finish.");
    noteSpawnRefused(chatId, s.name.c_str(), s.task,
                     "the spawn queue is full (" + std::to_string(orch::kMaxPendingSpawns) +
                         " waiting); start it again after some finish");
    return false;
  }
  if (d_.fire) d_.fire("spawn");   // "Right away, sir."
  pending_.emplace_back();
  PendingSpawn* pp = &pending_.back();
  // NO memset - PendingSpawn is non-POD (std::string task); its members carry
  // initializers. The old memset nulled the string's internals and a SHORT
  // (SSO) task then crashed the board (see the struct comment).
  // parseTurn already lowercased provider + defaulted category/note.
  strncpy(pp->provider, s.provider.c_str(), sizeof(pp->provider) - 1);
  strncpy(pp->model,    s.model.c_str(),    sizeof(pp->model)    - 1);
  strncpy(pp->category, s.category.c_str(), sizeof(pp->category) - 1);
  strncpy(pp->skill,    s.skill.c_str(),    sizeof(pp->skill)    - 1);
  strncpy(pp->name,     s.name.c_str(),     sizeof(pp->name)     - 1);
  strncpy(pp->note,     s.note.c_str(),     sizeof(pp->note)     - 1);
  strncpy(pp->project,  s.project.c_str(),  sizeof(pp->project)  - 1);
  pp->task   = s.task.substr(0, (size_t)orch::kSpawnTaskMax - 1);   // PSRAM string
  pp->attach = s.attach;
  strncpy(pp->chatId,   chatId.c_str(),     sizeof(pp->chatId)   - 1);
  pp->quiet = quiet;
  ledgerQueued(*pp);
  return true;
}

// Provider resolution: explicit hint wins; else connector-aware routing (a
// connector task lands on the provider that hosts it); else the sub-priority
// default. This is what lets a Notion/Drive/Gmail spawn run on lab compute
// instead of struggling on-device.
std::string JobEngine::spawnProvider(const PendingSpawn& p) const {
  if (p.provider[0]) return std::string(p.provider);
  // A file task runs where its file can come back (the device's PDF route), never
  // on the sub-priority default that may be Mistral (critique A7).
  // code_interpreter is the Code sandbox switch, not a connector, so a stored card's
  // provider never routes it; with no sandbox route the sub-priority default runs.
  if (p.skill[0] && nimbus::orch::isFileSkill(p.skill)) {
    const std::string fp = d_.fileSpawnProvider ? d_.fileSpawnProvider()
                                                : std::string(hasKey("openai") ? "openai" : "");
    return !fp.empty() && hasKey(fp) ? fp : firstSubProvider();
  }
  if (p.skill[0] && d_.connectorProvider) {
    std::string cp = d_.connectorProvider(p.skill);
    // Only accept the connector's provider if it's actually key'd - otherwise fall
    // through to the sub-priority default (preserves the keyless-provider refusal
    // instead of dispatching a doomed keyless request). "any" names no provider.
    if (!cp.empty() && cp != "any" && (!d_.providerHasKey || d_.providerHasKey(cp))) return cp;
  }
  return firstSubProvider();
}

// CUM-541 one-key spawns: a provider with no key of its own (the head named openai
// on a device whose only key is Cumulo) runs on the router when the router offers
// its model, instead of a doomed keyless dispatch. True when `provider`/`model` now
// name the router; false leaves them for refuseKeyless.
bool JobEngine::keylessToRouter(std::string& provider, std::string& model) const {
  const std::string offered = routerOffer(provider, model);
  if (offered.empty()) return false;
  hlog::logf("orchestrator: spawn on %s has no key - runs on the Cumulo router as %s (asked %s)",
             provider.c_str(), offered.c_str(), model.length() ? model.c_str() : "(none)");
  provider = kRouterSlug;
  model = offered;
  return true;
}

namespace {
// Why neither the asked model nor the provider's sub-agent default can run, for the
// ledger line and the head's note; names both, including when nothing was asked.
std::string unlistedWhy(const std::string& provider, const std::string& asked,
                        const std::string& sub) {
  const std::string s = asked.empty() ? "no model was named and " + provider
                                      : provider + " does not offer " + asked + ",";
  if (sub.empty()) return s + (asked.empty() ? " has no sub-agent default" : " and has no sub-agent default");
  return s + (asked.empty() ? " does not offer its sub-agent default " : " nor its sub-agent default ") + sub;
}
}  // namespace

// Validated coercion (subagents F5): a model the provider does not list never
// runs as an unchecked default. The asked model when listed, else the
// provider's sub default when listed, else the CHEAPEST model the list holds
// (the built-in price table): never list position 0, which is the priciest model
// on most providers' lists. Any substitution is named on the ledger line, also
// when no model was asked. When no listed model has a known price the spawn is
// refused with the reason. A provider with no list at all (custom: "" from
// modelChoices) runs as asked. Without the modelChoices dep the old fallback to
// the sub default stands.
bool JobEngine::coerceSpawnModel(const PendingSpawn& p, const std::string& provider,
                                 std::string& model) {
  if (d_.modelIsValid && d_.modelIsValid(provider, model)) return true;
  const std::string asked = model;
  const std::string sub = subModelOf(provider);
  if (!d_.modelChoices) {
    model = sub;
  } else {
    const std::string list = d_.modelChoices(provider);
    if (list.empty()) {   // nothing to check against
      if (model.empty()) model = sub;
      return true;
    }
    // The sub-agent default runs when the provider offers it: in the short list, or
    // accepted by the one check (a bare router id such as the owner's "gpt-5.6" is the
    // router's "openai/gpt-5.6"; a stored pick is never refused - lane F2).
    if (!sub.empty() && (nimbus::orch::modelInChoices(list, sub) ||
                         (d_.modelIsValid && d_.modelIsValid(provider, sub)))) {
      model = sub;
      if (!asked.empty())
        ledgerSwapped(p, provider + " does not offer " + asked + "; runs on its sub-agent default " + sub);
    } else {
      const std::string why = unlistedWhy(provider, asked, sub);
      model = nimbus::orch::cheapestByEffectivePrice(provider.c_str(), list);   // CUM-708: the ledger's price
      if (model.empty()) {
        refuseUnpricedModel(p, provider, asked, sub, why);
        return false;
      }
      ledgerSwapped(p, why + "; runs on " + model + ", the cheapest model it lists");
    }
  }
  hlog::logf("orchestrator: spawn model coerced (%s -> %s on %s)",
             asked.length() ? asked.c_str() : "(none)", model.c_str(), provider.c_str());
  return true;
}

// No listed model has a known price: nothing runs. The owner hears what was asked
// (or that nothing was) and which default is missing; the head gets the same.
void JobEngine::refuseUnpricedModel(const PendingSpawn& p, const std::string& provider,
                                    const std::string& asked, const std::string& sub,
                                    const std::string& why) {
  hlog::logf("orchestrator: spawn refused (%s: %s; no listed model has a known price)",
             provider.c_str(), why.c_str());
  std::string what = asked.empty() ? std::string("no model was named and ") : "it does not offer " + asked;
  if (sub.empty())
    what += asked.empty() ? "it has no default model" : " and has no default model";
  else
    what += asked.empty() ? "its default model " + sub + " is not on its list" : " or its default model " + sub;
  deliver(p.chatId, "Couldn't start that agent on " + provider + ": " + what +
                        ", and none of the models it offers has a known price to fall back on. "
                        "Pick a model from its list under Providers & models.");
  noteSpawnRefused(p.chatId, p.name, p.task,
                   why + ", and no listed model has a known price; spawn it with a model from "
                         "that provider's [AVAILABLE MODELS] entry");
}

// L19-8: a spawn whose provider the park closed (named by the head, or a connector's
// host) sends it nothing. It moves to the first open sub provider, its model left to
// the coercion there; a connector task cannot move (its connector lives on that
// provider) and nothing open is left either way: refused, with the park's words.
bool JobEngine::moveOffClosedProvider(const PendingSpawn& p, const std::string& host,
                                      std::string& provider, std::string& model) {
  std::string why;
  if (spawnSeamOpen(provider, nowMs(), &why)) return true;
  const std::string alt = host.empty() ? firstSubProvider() : std::string();
  if (alt.empty()) {
    hlog::logf("orchestrator: spawn refused (%s)", why.c_str());
    deliver(p.chatId, "Couldn't start that agent: " + why + ".");
    noteSpawnRefused(p.chatId, p.name, p.task, why);
    return false;
  }
  hlog::logf("orchestrator: spawn moved %s -> %s (%s)", provider.c_str(), alt.c_str(), why.c_str());
  ledgerSwapped(p, why + "; runs on " + alt);
  provider = alt;
  model.clear();
  return true;
}

bool JobEngine::routeSpawn(const PendingSpawn& p, std::string& provider, std::string& model,
                           ManagedAgentAdapter*& a) {
  const std::string chatId = p.chatId;
  provider = spawnProvider(p);
  if (provider.length() == 0) {
    std::string closedWhy;   // keyed providers exist, but the park closed every one (L19-8)
    firstSubProvider(&closedWhy);
    if (!closedWhy.empty()) {
      deliver(chatId, "Couldn't start that agent: " + closedWhy + ".");
      noteSpawnRefused(chatId, p.name, p.task, closedWhy);
      return false;
    }
    deliver(chatId, "No provider is configured to run that agent.");
    noteSpawnRefused(chatId, p.name, p.task, "no provider with a key can run it");
    return false;
  }
  model = p.model;
  // A connector task never moves to (or runs on) the router: refuseConnector.
  const std::string host = connectorHost(p);
  if (!moveOffClosedProvider(p, host, provider, model)) return false;
  const bool keyless = d_.providerHasKey && !d_.providerHasKey(provider);
  const bool routed = keyless && host.empty() && keylessToRouter(provider, model);
  a = d_.fabric->adapterFor(provider.c_str());
  if (!a) {
    deliver(chatId, std::string("Provider '") + provider + "' isn't configured.");
    noteSpawnRefused(chatId, p.name, p.task,
                     "provider '" + provider + "' isn't configured on this device");
    return false;
  }
  if (!host.empty() && (keyless || provider == kRouterSlug)) {
    refuseConnector(p, provider, host);
    return false;
  }
  if (keyless && !routed) {
    refuseKeyless(p, provider, model);
    return false;
  }
  return coerceSpawnModel(p, provider, model);
}

// L19-36: save the whole reply (`whole`, longer than the envelope) to the job's
// project on the card, or to kSpillProject when the spawn named none, then end the
// envelope with the spill marker. True when the card holds it; false keeps the
// envelope's honest "could not be saved" marker.
static const char kSpillProject[] = "results";
bool JobEngine::spillWholeReply(const orch::JobRecord& rec, const std::string& whole, size_t dropped) {
  std::string path, why = "no file store";
  if (d_.persistResult) {
    const char* prj = rec.prj[0] ? rec.prj : kSpillProject;
    std::string text = whole;
    if (dropped) text += "\n[the reply continued for " + std::to_string(dropped) + " more bytes that were not kept]";
    const std::string note = d_.persistResult(prj, rec.name, rec.tag, text, rec.chatId);
    const std::string saved = "[saved: ";
    if (note.rfind(saved, 0) == 0 && note.size() > saved.size() + 1)
      path = note.substr(saved.size(), note.size() - saved.size() - 1);
    else
      why = note;
  }
  orch::endWithSpillMarker(env_ref(), path, whole.size() + dropped, path.empty() ? why : std::string());
  hlog::logf("orchestrator: %s reply was %u B - %s", rec.tag, (unsigned)(whole.size() + dropped),
             path.empty() ? ("not saved: " + why).c_str() : ("saved whole as " + path).c_str());
  return !path.empty();
}

void JobEngine::dispatchSpawn(const PendingSpawn& p) {
  if (!d_.fabric) return;
  std::string chatId = p.chatId;
  char tag[24];
  snprintf(tag, sizeof(tag), "job%04lu", (unsigned long)(tagSeq_++));

  std::string provider, model;
  ManagedAgentAdapter* a = nullptr;
  if (!routeSpawn(p, provider, model, a)) return;

  // Dynamic-skill injection (roadmap P2): when the spawn names a skill and the
  // resolver returns a capsule, PREPEND it to the instruction -
  // "[SKILL: id]\n<capsule>\n\n---\n<task>" (capsule capped at 4 KB with a
  // visible truncation note). Empty resolve keeps today's provider-hint
  // passthrough EXACTLY; the skill string rides Directive.skill either way.
  std::string instruction = p.task;
  if (p.skill[0] && d_.resolveSkill)
    instruction = composeSkillInjection(p.skill, d_.resolveSkill(p.skill), p.task);
  // Temporal grounding for the sub-agent (see JobDeps.nowString): providers give
  // spawned agents no clock, so anchor "now" explicitly at the top of the brief.
  if (d_.nowString) {
    std::string now = d_.nowString();
    if (!now.empty())
      instruction = "[Current date-time: " + now +
                    " - trust this over any internal sense of the date.]\n" + instruction;
  }
  // Conversation context (Release B4): what the owner and head were just
  // discussing, so a spawn like "book the place we talked about" is actionable.
  // Appended AFTER the task so the task stays the lead instruction.
  if (d_.chatContext) {
    std::string cctx = d_.chatContext(p.chatId);
    if (!cctx.empty()) instruction += "\n\n" + cctx;
  }
  instruction += kSubReplyCapNote;   // L19-36: the sub knows the reply cap
  // v4.0.0 attachments: splice each referenced doc's CONTENT into the brief
  // (the model references by name - cheap output tokens; the firmware supplies
  // the bytes). Total budget 24 KB, UTF-8-safe clip, honest not-found notes.
  if (!p.attach.empty()) {
    size_t budget = 24 * 1024;
    for (const auto& ref : p.attach) {
      const size_t slash = ref.find('/');
      if (slash == std::string::npos || slash == 0 || slash + 1 >= ref.size()) continue;
      std::string doc = d_.readDoc
                            ? d_.readDoc(p.chatId, ref.substr(0, slash), ref.substr(slash + 1))
                            : std::string();
      if (doc.empty()) {
        instruction += "\n\n[ATTACHED " + ref + ": not found or unreadable]";
        continue;
      }
      if (doc.size() > budget) {
        int keep = nimbus::utf8CapLen(doc.c_str(), (int)doc.size(), (int)budget);
        doc = doc.substr(0, (size_t)keep) + "\n[attachment truncated at budget]";
      }
      budget -= doc.size() < budget ? doc.size() : budget;
      instruction += "\n\n[ATTACHED: " + ref + "]\n" + doc;
      if (!budget) break;
    }
  }

  RateLimit window = RateLimit::Unknown;   // a refusing 429's window, adapter-filled
  Directive d;
  d.category    = p.category;
  d.instruction = instruction.c_str();
  d.chatId      = p.chatId;
  d.tag         = tag;
  d.model       = model.length() ? model.c_str() : nullptr;
  d.skill       = p.skill[0] ? p.skill : nullptr;
  d.task        = p.task.c_str();   // the bare task: connector provenance (CUM-536)
  d.rateLimit   = &window;

  char jobId[96] = {};
  FabricErr err = a->dispatch(d, jobId);
  if (err != FabricErr::Ok) {
    hlog::logf("orchestrator: spawn failed %d (prov=%s model=%s cat=%s)",
               (int)err, provider.c_str(), model.c_str(), p.category);
    const std::string refusal = spawnRefusal(err, provider, window);
    deliver(chatId, refusal);
    if (err == FabricErr::Timeout || err == FabricErr::ParseFail) {
      // It most likely ran (a synchronous sub - Mistral, Z.ai, Cumulo, custom - runs
      // inside the POST that timed out or answered unreadably; an async one was
      // created) and no job record exists to wait on, so the head must not spawn it
      // again.
      const char* what = err == FabricErr::Timeout ? " did not answer in time"
                                                   : " answered but its result could not be read";
      noteSpawnRefused(chatId, p.name, p.task,
                       "the agent on " + provider + what + ", so it may have run, and its "
                       "result will not arrive. Do not spawn it again unless the owner asks",
                       /*mayHaveRun=*/true);
      noteDispatchTimeout();
    } else {
      noteSpawnRefused(chatId, p.name, p.task, refusal);
    }
    return;
  }

  noteLiveTag(tag, nimbus::orch::liveTask(p.task));   // read when the result lands
  orch::JobRecord rec;
  memset(&rec, 0, sizeof(rec));
  strncpy(rec.tag,      tag,   sizeof(rec.tag)   - 1);
  strncpy(rec.jobId,    jobId, sizeof(rec.jobId) - 1);
  backendOf(jobId, rec.backend, sizeof(rec.backend));
  strncpy(rec.category, p.category, sizeof(rec.category) - 1);
  strncpy(rec.prj,      p.project,  sizeof(rec.prj)      - 1);
  strncpy(rec.model,    model.length() ? model.c_str() : "default", sizeof(rec.model) - 1);
  strncpy(rec.name,     p.name, sizeof(rec.name) - 1);
  strncpy(rec.chatId,   p.chatId, sizeof(rec.chatId) - 1);
  rec.state        = orch::JobState::Queued;
  rec.resultSeen   = false;
  rec.dispatchedAt = nowMs();
  // A record the store refused (a full NVS) is still held in RAM, so the job is polled
  // this run and only a restart loses it; the refusal is in nvs.recentFails.
  if (d_.journal && !d_.journal->write(rec))
    hlog::logf("orchestrator: journal record %s not stored", tag);

  hlog::logf("orchestrator: spawned %s -> %s (%s/%s)", tag, jobId, rec.backend, rec.model);
  ledgerStarted(p, tag, rec.backend, rec.model);
  if (d_.hooks.onSpawn) {   // lifecycle observer (after the journal write - the job is real)
    SpawnEv ev;
    ev.tag = tag;
    ev.backend = rec.backend;
    ev.category = rec.category;
    ev.model = rec.model;
    ev.task = p.task;       // B5: the brief itself (bounded by the task buffer)
    ev.chatId = p.chatId;
    d_.hooks.onSpawn(ev);
  }
  // Scheduled/loop turns are single-deliverable: suppress the "On it." spawn ack so
  // a loop firing emits only its final synthesis, not lifecycle chatter. Interactive
  // turns keep the ack for the FIRST TWO live spawns (reassuring); past that a
  // fan-out's acks are thread spam (a 12-spawn wave = 12 messages) - the head's
  // own turn reply already told the owner what it started.
  if (!p.quiet && activeCount() <= 2)
    deliver(chatId, std::string(p.note) + "  [" + (p.name[0] ? std::string(p.name) : std::string(tag)) +
            " \xC2\xB7 " + rec.backend + "/" + rec.model + "]");
  emitJobState(tag, rec.backend, orch::JobState::Queued);
}

// ---- the pump (the JOB parts of the device's pollJobs) ----------------------

int JobEngine::pump() {
  if (!d_.fabric || !d_.journal) return 0;
  reapDone();     // collapse any delivered sub-agent arcs whose grace has elapsed

  // Synthesis fires only when ALL spawned work has drained (owner design,
  // 2026-08-10, superseding R5b's per-completion synthesis): "if you spin 50
  // this will spam the thread... the user only cares about the orchestrator's
  // FINAL report." Mid-run, results accumulate quietly - full text is already
  // durable (the results ring + per-project docs), fresh_ holds bounded
  // stubs - and the owner's chat stays silent until the last sub finishes,
  // when ONE synthesis turn writes the final report. The head can still start
  // a follow-up generation from that turn; each generation ends in exactly one
  // report. The stuck-synthesis raw fallback waits for the same drain (a raw
  // dump of 50 labeled results mid-run is the same spam, worse).
  //
  // CUM-480: the stuck clock starts at the DRAIN, not at the first result. Subs
  // routinely finish minutes apart; timing from the oldest result made the very
  // first post-drain pump fire the raw fallback with no synthesis ever tried.
  // Synthesis is attempted first - by the loop-closure below on the drain pump
  // itself, and again each pump while it defers (low memory leaves the results
  // parked) - and the raw fallback fires only after kSynthFallbackMs of that real
  // stuckness. New work starting re-arms the clock.
  if (!hasFreshResults() || activeCount() != 0) {
    drained_ = false;
  } else {
    if (!drained_) { drained_ = true; drainedAtMs_ = nowMs(); }
    const uint32_t waited = nowMs() - drainedAtMs_;
    if (waited >= kSynthFallbackMs) {
      deliverOwnerFallback();
    } else if (waited >= kSynthCoalesceMs && !(d_.turnInFlight && d_.turnInFlight())) {
      if (d_.synthesize) d_.synthesize(freshChatId_);
    }
  }

  // Dispatch at most ONE queued spawn per cycle, while in-flight sessions stay
  // under the inflight cap and heap is above the dispatch floor.
  if (!pending_.empty() && nowMs() >= nextDispatchAt_ &&
      d_.journal->count() < t_.maxActiveInflight &&
      freeHeap() >= t_.dispatchMinHeap) {
    PendingSpawn p = pending_.front();
    pending_.erase(pending_.begin());
    // A deep wave grew the queue's internal-SRAM backing past the baseline;
    // release it once the inbox drains so the larger allocation is transient, not
    // a permanent steady-state cost (nimbus-internal-heap-map).
    if (pending_.empty() && pending_.capacity() > (size_t)orch::kAgentMaxJobs) {
      std::vector<PendingSpawn>().swap(pending_);
      pending_.reserve(orch::kAgentMaxJobs);
    }
    // Bridge the dispatch window for activeCount(): the entry left pending_ and
    // the journal record lands only after dispatch returns (a synchronous
    // Mistral sub blocks HERE for its whole run) - without this flag the head
    // arc read zero children mid-run and the reconciler cleared it.
    dispatching_ = true;
    dispatchSpawn(p);
    dispatching_ = false;
    nextDispatchAt_ = nowMs() + 2500;
    nextPollAt_     = nowMs() + 1500;
    return d_.journal->count();
  }

  // Loop-closure: once every sub-agent has finished and nothing is queued, feed
  // the parked results back for ONE synthesis turn.
  if (pending_.empty() && d_.journal->count() == 0 && hasFreshResults()) {
    if (d_.synthesize) d_.synthesize(freshChatId_);
    return d_.journal->count();
  }

  if (nowMs() < nextPollAt_) return d_.journal->count();
  int n = d_.journal->count();
  if (n == 0) return 0;

  // Lifecycle observer: one event per polled state below (observer-only).
  auto fireResult = [this](const char* tag, orch::JobState st,
                           const char* chatId = "", const char* reply = "") {
    if (!d_.hooks.onResult) return;
    ResultEv ev;
    ev.tag = tag;
    ev.state = (uint8_t)st;
    ev.terminal = orch::isTerminal(st);
    ev.chatId = chatId ? chatId : "";
    if (ev.terminal && reply) ev.reply = reply;   // B5: only terminal states carry text
    d_.hooks.onResult(ev);
  };

  for (int k = 0; k < n; k++) {
    int i = (rrIndex_ + k) % n;
    orch::JobRecord rec;
    if (!d_.journal->get(i, rec)) continue;
    if (rec.resultSeen || orch::isTerminal(rec.state)) continue;

    rrIndex_    = (i + 1) % n;
    nextPollAt_ = nowMs() + pollBackoffMs_;

    memset(&env_ref(), 0, sizeof(nimbus::orch::ResultEnvelope));
    uint32_t hBefore = freeHeap();
    FabricErr err = d_.fabric->poll(rec.jobId, env_ref());
    // L19-36: the whole reply when the 16 KB envelope cut it ("" when it fit); taken
    // after every poll so a Running poll's copy never outlives it.
    size_t overflowDropped = 0;
    const std::string overflow = providers::takeReplyOverflow(&overflowDropped);
    hlog::logf("orchestrator: poll %s err=%d state=%d heap %u->%u", rec.tag, (int)err,
               (int)env_ref().state, (unsigned)hBefore, (unsigned)freeHeap());

    if (err == FabricErr::NotFound) {   // expired / unknown remote job: terminal
      d_.journal->update(rec.tag, orch::JobState::Error);
      deliver(rec.chatId, std::string("Job [") + jobLabel(rec) + "] expired or was not found.");
      ledgerFinished(rec.tag, LedgerState::Failed, "it expired or was not found on its provider");
      fireResult(rec.tag, orch::JobState::Error, rec.chatId, "expired");
      emitJobState(rec.tag, rec.backend, orch::JobState::Error);
      d_.journal->markSeen(rec.tag);
      watchClear(rec.tag);
      scheduleReap(rec.tag, attnHoldMs_);  // red is a CTA: hold, then clear - never forever
      return d_.journal->count();
    }
    if (err != FabricErr::Ok) {   // transient: exponential backoff, cap 120 s
      // Stuck-job watchdog rule 1 (2026-08-12 "orange breathing ring for hours"):
      // a job whose polls return ONLY errors has no path to a terminal state -
      // the backoff loop retried forever and its Running arc breathed its accent
      // indefinitely (Running is deliberately exempt from the attention expiry).
      // After kJobPollGiveUpMs of consecutive failures, give up HONESTLY: the
      // outcome rides the normal fresh-result/synthesis path, the arc reaps.
      JobWatch& w = watchFor(rec.tag);
      if (!w.firstErrMs) w.firstErrMs = nowMs() ? nowMs() : 1;
      if ((int32_t)(nowMs() - w.firstErrMs) > (int32_t)nimbus::orch::kJobPollGiveUpMs) {
        hlog::logf("orchestrator: %s gave up - %u min of failed polls (last err %d)",
                   rec.tag, (unsigned)(nimbus::orch::kJobPollGiveUpMs / 60000u), (int)err);
        const std::string failText = std::string("Job [") + jobLabel(rec) +
            "] lost: no response from " + rec.backend +
            " for 15 minutes - giving up. Its work may still finish remotely, "
            "but the result won't reach this device.";
        d_.journal->update(rec.tag, orch::JobState::Error);
        // Same owner-delivery discipline as a normal sub failure (W19): while
        // other work runs, the failure joins the fresh results so the final
        // synthesis reports it; alone, it IS the report.
        if (activeCount() > 1 || !pending_.empty()) {
          addFreshResult(rec.tag, jobLabel(rec).c_str(), std::string("FAILED: ") + failText,
                         nimbus::orch::nsForChat(rec.chatId, /*admin=*/false), /*failed=*/true);
          freshChatId_ = rec.chatId;
        } else {
          deliver(rec.chatId, failText);
        }
        fireResult(rec.tag, orch::JobState::Error, rec.chatId, "lost contact");
        ledgerFinished(rec.tag, LedgerState::Failed, "its provider stopped answering about it");
        emitJobState(rec.tag, rec.backend, orch::JobState::Error);
        d_.journal->markSeen(rec.tag);
        watchClear(rec.tag);
        scheduleReap(rec.tag, attnHoldMs_);
        return d_.journal->count();
      }
      pollBackoffMs_ = pollBackoffMs_ < 120000 ? pollBackoffMs_ * 2 : 120000;
      nextPollAt_ = nowMs() + pollBackoffMs_;
      hlog::logf("orchestrator: poll %s transient err %d, backoff %ums", rec.tag, (int)err,
                 (unsigned)pollBackoffMs_);
      return d_.journal->count();
    }
    pollBackoffMs_ = t_.pollIntervalMs;   // healthy response → reset cadence
    {
      // Healthy answer: the provider is reachable again - reset the give-up
      // clock. Rule 2: a job the provider still reports non-terminal past
      // kJobArcParkMs keeps polling, but its ring arc PARKS to a dim static
      // segment (Idle) so a long remote run can't breathe for hours (the
      // ambient grammar; completion still fires the normal Done/Error cues).
      JobWatch& w = watchFor(rec.tag);
      w.firstErrMs = 0;
      if (!orch::isTerminal(env_ref().state) && !w.parked &&
          (int32_t)(nowMs() - rec.dispatchedAt) > (int32_t)nimbus::orch::kJobArcParkMs) {
        w.parked = true;
        hlog::logf("orchestrator: %s still running after %u min - ring arc parked (dim)",
                   rec.tag, (unsigned)(nimbus::orch::kJobArcParkMs / 60000u));
        emitJobState(rec.tag, rec.backend, orch::JobState::Queued);  // -> Status::Idle (dim static)
      }
    }

    d_.journal->update(rec.tag, env_ref().state);
    // Spend attribution (spawn:<backend>): terminal polls only, so each job
    // records at most once (markSeen removes it from the poll set after this).
    if (orch::isTerminal(env_ref().state) && d_.recordSpawnUsage) {
      orch::TokenUsage u;
      u.promptTokens = env_ref().promptTokens;
      u.completionTokens = env_ref().completionTokens;
      u.cacheReadTokens = env_ref().cacheReadTokens;
      u.cacheWriteTokens = env_ref().cacheWriteTokens;
      u.responses = 1;
      if (!u.empty() || u.cacheReadTokens || u.cacheWriteTokens)
        d_.recordSpawnUsage(rec.backend, rec.model, u);
    }
    const bool done = env_ref().state == orch::JobState::Done;
    // L19-36: a reply longer than the envelope is saved whole to the card first, and
    // the envelope ends with a marker naming that file and its real size.
    const bool spilled = done && !overflow.empty() && spillWholeReply(rec, overflow, overflowDropped);
    bool unverified = false;
    std::string reply = done ? deliveredReply(rec, unverified) : std::string();
    fireResult(rec.tag, env_ref().state, rec.chatId,
               done ? reply.c_str()
                    : (env_ref().reply[0] ? env_ref().reply : env_ref().error));   // Error: reason, not "" (prism B)
    if (done) {
      // TOPOLOGY INVERSION (owner R5b): the owner talks ONLY to the orchestrator.
      // Raw sub-agent output no longer goes straight to Telegram - the FULL result
      // is parked as a fresh result and a per-completion SYNTHESIS turn (above, in
      // this same pump cadence after a short coalesce window) decides what the
      // owner actually sees. If the synthesis can't run (heap/provider down), the
      // fallback (60 s after the batch drains) delivers the raw results - never lost.
      emitJobState(rec.tag, rec.backend, orch::JobState::Done);
      ledgerFinished(rec.tag, LedgerState::Done);
      d_.journal->markSeen(rec.tag);
      watchClear(rec.tag);
      scheduleReap(rec.tag);   // collapse the Done arc off the ring after a short grace
      // v4.0.0 auto-persist: a run-tagged job's FULL reply becomes a durable
      // doc in its project - how "sub-agents write documents" actually works
      // (they have no device tools; the head persists on their behalf). The
      // outcome line rides the fresh result so the planner KNOWS what saved.
      if (rec.prj[0] && d_.persistResult && !spilled) {   // a spilled reply is saved already
        std::string note = d_.persistResult(rec.prj, rec.name, rec.tag, reply, rec.chatId);
        if (!note.empty()) reply += "\n" + note;
      }
      // v4.1 provider file capture: a run that produced a binary FILE (e.g. a
      // Mistral code_interpreter PDF) carries file references in artifacts[] (the
      // bytes never ride env.reply). Fetch each to SD + register it so the head
      // can files.send it; the outcome line rides the fresh result so the planner
      // KNOWS a file landed. Runs HERE on tg_poll - the poll above is cache-local,
      // so no TLS is held; the fetch takes the work slot for one download.
      if (d_.fetchArtifact) {
        const int na = env_ref().artifactCount;
        for (int ai = 0; ai < na && ai < nimbus::orch::kMaxArtifacts; ai++) {
          const nimbus::orch::Artifact& art = env_ref().artifacts[ai];
          if (strcmp(art.type, "file") != 0) continue;
          std::string note = d_.fetchArtifact(rec.backend, art.url, art.label,
                                              rec.prj, rec.name, rec.tag, rec.chatId);
          if (!note.empty()) reply += "\n" + note;
        }
      }
      // Owning tenant = the SPAWNING chat's namespace, so a sub-agent result is
      // readable only by that chat (and admins) - same boundary as the memory tools.
      addFreshResult(rec.tag, jobLabel(rec).c_str(), reply,
                     nimbus::orch::nsForChat(rec.chatId, /*admin=*/false),
                     /*failed=*/false, unverified);
      // How many tools the sub ran, from the provider's own record (lane S).
      fresh_.back().provenance = nimbus::orch::freshProvenanceTag(env_ref().toolRuns, env_ref().webRuns);
      freshChatId_ = rec.chatId;
    } else if (env_ref().state == orch::JobState::Error) {
      // While OTHER work is still running, a per-sub failure joins the fresh
      // results (the final report says which subs failed) instead of pinging
      // the owner mid-run - 50 subs must not mean up to 50 failure messages.
      // A failure with nothing else live IS the final report: deliver as before.
      const std::string failText = std::string("Job [") + jobLabel(rec) + "] failed: " +
              (env_ref().error[0] ? env_ref().error : "unknown error");
      ledgerFinished(rec.tag, LedgerState::Failed, env_ref().error[0] ? env_ref().error : "unknown error");
      if (activeCount() > 1 || !pending_.empty()) {   // >1: this job still counts
        addFreshResult(rec.tag, jobLabel(rec).c_str(), std::string("FAILED: ") + failText,
                       nimbus::orch::nsForChat(rec.chatId, /*admin=*/false), /*failed=*/true);
        freshChatId_ = rec.chatId;
      } else {
        deliver(rec.chatId, failText);
      }
      emitJobState(rec.tag, rec.backend, orch::JobState::Error);
      d_.journal->markSeen(rec.tag);
      watchClear(rec.tag);
      scheduleReap(rec.tag, attnHoldMs_);  // red is a CTA: hold the attention window, then clear
    } else {
      // Running / NeedsInput update - but a PARKED job must not re-emit Running:
      // this fires every healthy poll (~15 s), so it would instantly un-park the
      // dim arc and the breathe would be back (the exact hours-long symptom).
      // NeedsInput is an attention state and always gets through.
      if (env_ref().state == orch::JobState::NeedsInput || !watchFor(rec.tag).parked)
        emitJobState(rec.tag, rec.backend, env_ref().state);
    }
    return d_.journal->count();
  }
  return d_.journal->count();
}

// ---- cancel / await / queries -----------------------------------------------

bool JobEngine::cancel(const char* tagOrJobId) {
  if (!d_.fabric || !d_.journal) return false;
  int n = d_.journal->count();
  for (int i = 0; i < n; i++) {
    orch::JobRecord rec;
    if (!d_.journal->get(i, rec)) continue;
    if (strcmp(rec.tag, tagOrJobId) == 0 || strcmp(rec.jobId, tagOrJobId) == 0) {
      d_.fabric->cancel(rec.jobId);
      d_.journal->update(rec.tag, orch::JobState::Cancelled);
      ledgerFinished(rec.tag, LedgerState::Cancelled);
      emitJobState(rec.tag, rec.backend, orch::JobState::Cancelled);
      d_.journal->markSeen(rec.tag);
      watchClear(rec.tag);
      return true;
    }
  }
  return false;
}

void JobEngine::awaitTag(const std::string& firstTag) {
  if (!d_.journal) return;
  int n = d_.journal->count();
  for (int i = 0; i < n; i++) {
    orch::JobRecord r;
    if (d_.journal->get(i, r) && firstTag.length() && firstTag == r.tag) { rrIndex_ = i; break; }
  }
  nextPollAt_ = nowMs();
}

int JobEngine::activeJobCount() const { return d_.journal ? d_.journal->count() : 0; }

std::vector<nimbus::orch::SessionInfo> JobEngine::sessionInfos() const {
  std::vector<nimbus::orch::SessionInfo> out;
  if (!d_.journal) return out;
  int n = d_.journal->count();
  for (int i = 0; i < n; i++) {
    orch::JobRecord rec;
    if (!d_.journal->get(i, rec)) continue;
    nimbus::orch::SessionInfo s;
    s.id = rec.tag;
    s.provider = rec.backend;
    s.model = rec.model;
    s.title = rec.category;   // the journal keeps a category, not a task string
    s.state = stateStr(rec.state);
    s.hue = d_.backendHue ? d_.backendHue(rec.backend) : 255;   // ring accent for the session cursor
    out.push_back(s);
  }
  return out;
}

}  // namespace agent
