#include "nimbus/harness/jobs.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "nimbus/harness/log.h"
#include "nimbus/harness/skill_md.h"   // composeSkillInjection - per-spawn capsule
#include "nimbus/mem_cap.h"   // utf8CapLen - the fresh-result 3500-char UTF-8 cap
#include "nimbus/orch/gradient.h"   // foldLine - the overflow-stub one-liner
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

// ---- JobEngine --------------------------------------------------------------

JobEngine::JobEngine(Deps d, Tuning t)
    : d_(std::move(d)), t_(t), pollBackoffMs_(t.pollIntervalMs) {
  envSlot_.resize(1);   // the PSRAM-backed poll envelope slot
  // Reserve the (small, bounded) queues up front so enqueue never allocates on
  // the device - the replacement for the old malloc-while-pending tactic.
  pending_.reserve(orch::kAgentMaxJobs);
  fresh_.reserve(orch::kAgentMaxJobs);
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

// First key'd provider in subPriority order ("" if none configured).
std::string JobEngine::firstSubProvider() const {
  std::string prio = d_.subPriority ? d_.subPriority() : std::string();
  size_t start = 0;
  while (start < prio.length()) {
    size_t comma = prio.find(',', start);
    if (comma == std::string::npos) comma = prio.length();
    std::string p = prio.substr(start, comma - start);
    // trim (matches Arduino String::trim on the device)
    while (!p.empty() && isspace((unsigned char)p.front())) p.erase(p.begin());
    while (!p.empty() && isspace((unsigned char)p.back())) p.pop_back();
    if (p.length() && d_.providerHasKey && d_.providerHasKey(p)) return p;
    start = comma + 1;
  }
  return "";
}

// ---- fresh-result store -----------------------------------------------------

void JobEngine::addFreshResult(const char* tag, const char* model, const std::string& text,
                               const std::string& ns) {
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
  if ((int)fresh_.size() >= orch::kAgentMaxJobs) {
    // Over the block cap: a one-line STUB instead of the old SILENT DROP
    // (Context Fabric - bounded views over unbounded storage, never loss).
    fr.text = nimbus::orch::foldLine("", text, 160) + " [full " +
              std::to_string((unsigned)text.size()) + " B" +
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
      fr.text += " [full " + std::to_string((unsigned)text.size()) + " B: results.get(\"" +
                 ringTag + "\")]";
  } else {
    fr.text = text;
  }
  fresh_.push_back(std::move(fr));
}

std::string JobEngine::takeFreshResults() {
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
    const std::string head = "- " + f.tag + " (" + f.model + "): ";
    if (b.size() + head.size() + f.text.size() + 1 <= kBlockBudget) {
      b += head + f.text + "\n";
    } else {
      // Use the REAL ring tag spillResult() handed back (falls back to the
      // reconstruction only when a host rig ran without the spill closure).
      const std::string handle = !f.ringTag.empty() ? f.ringTag : ("sub:" + f.tag);
      b += head + nimbus::orch::foldLine("", f.text, 120) +
           " [full: results.get(\"" + handle + "\")]\n";
      stubbed++;
    }
  }
  if (stubbed)
    b += "(" + std::to_string(stubbed) +
         " result(s) summarized - fetch full text with results.get)\n";
  fresh_.clear();
  freshSinceMs_ = 0;   // single reset point: EVERY consumer (per-completion synthesis,
                       // batch consolidation, a user turn folding results in) clears the
                       // coalesce/fallback clock - a stale t0 made the next completion
                       // false-trip the 60 s raw fallback (review finding 2026-07-13)
  return b;
}

// ---- spawn enqueue + dispatch -----------------------------------------------

void JobEngine::enqueueSpawn(const orch::Spawn& s, const std::string& chatId, bool quiet) {
  if (s.task.length() == 0) return;
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
    return;
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
}

void JobEngine::dispatchSpawn(const PendingSpawn& p) {
  if (!d_.fabric) return;
  std::string chatId = p.chatId;
  char tag[24];
  snprintf(tag, sizeof(tag), "job%04lu", (unsigned long)(tagSeq_++));

  // Provider resolution: explicit hint wins; else connector-aware routing (a
  // connector task lands on the provider that hosts it); else the sub-priority
  // default. This is what lets a Notion/Drive/Gmail spawn run on lab compute
  // instead of struggling on-device.
  std::string provider = p.provider[0] ? std::string(p.provider) : std::string();
  if (provider.empty() && p.skill[0] && d_.connectorProvider) {
    std::string cp = d_.connectorProvider(p.skill);
    // Only accept the connector's provider if it's actually key'd - otherwise fall
    // through to the sub-priority default (preserves the keyless-provider refusal
    // instead of dispatching a doomed keyless request).
    if (!cp.empty() && (!d_.providerHasKey || d_.providerHasKey(cp))) provider = cp;
  }
  if (provider.empty()) provider = firstSubProvider();
  if (provider.length() == 0) { deliver(chatId, "No provider is configured to run that agent."); return; }
  ManagedAgentAdapter* a = d_.fabric->adapterFor(provider.c_str());
  if (!a) { deliver(chatId, std::string("Provider '") + provider + "' isn't configured."); return; }

  std::string model = p.model;
  if (!d_.modelIsValid || !d_.modelIsValid(provider, model)) {
    std::string coerced = d_.subModel ? d_.subModel(provider) : std::string();
    hlog::logf("orchestrator: spawn model coerced (%s -> %s on %s)",
               model.length() ? model.c_str() : "(none)", coerced.c_str(), provider.c_str());
    model = coerced;
  }

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

  Directive d;
  d.category    = p.category;
  d.instruction = instruction.c_str();
  d.chatId      = p.chatId;
  d.tag         = tag;
  d.model       = model.length() ? model.c_str() : nullptr;
  d.skill       = p.skill[0] ? p.skill : nullptr;

  char jobId[96] = {};
  FabricErr err = a->dispatch(d, jobId);
  if (err != FabricErr::Ok) {
    hlog::logf("orchestrator: spawn failed %d (prov=%s model=%s cat=%s)",
               (int)err, provider.c_str(), model.c_str(), p.category);
    // A Timeout usually means the sub STARTED and outran the 60 s wait (the old
    // "couldn't start" was a lie the owner caught) - but the same signal can be
    // a very slow connection that never delivered the request, so the honest
    // wording claims neither outcome as certain (prism v4.1 #4).
    if (err == FabricErr::Timeout)
      deliver(chatId, std::string("The agent on ") + provider +
              " didn't respond within 60s - it may have run without me getting "
              "its result. Try a smaller task or split it into steps.");
    else
      deliver(chatId, std::string("Couldn't start that agent on ") + provider + ".");
    return;
  }

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
  if (d_.journal) d_.journal->write(rec);

  hlog::logf("orchestrator: spawned %s -> %s (%s/%s)", tag, jobId, rec.backend, rec.model);
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
  if (hasFreshResults() && freshSinceMs_ && activeCount() == 0) {
    const uint32_t waited = nowMs() - freshSinceMs_;
    if (waited >= kSynthFallbackMs) {
      hlog::log("orchestrator: synthesis stuck - delivering raw results (fallback)");
      deliver(freshChatId_, takeFreshResults());
      freshSinceMs_ = 0;
    } else if (waited >= kSynthCoalesceMs && !(d_.turnInFlight && d_.turnInFlight())) {
      if (d_.synthesize) d_.synthesize(freshChatId_);
      if (!hasFreshResults()) freshSinceMs_ = 0;   // consumed by the turn
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
    hlog::logf("orchestrator: poll %s err=%d state=%d heap %u->%u", rec.tag, (int)err,
               (int)env_ref().state, (unsigned)hBefore, (unsigned)freeHeap());

    if (err == FabricErr::NotFound) {   // expired / unknown remote job: terminal
      d_.journal->update(rec.tag, orch::JobState::Error);
      deliver(rec.chatId, std::string("Job [") + jobLabel(rec) + "] expired or was not found.");
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
                         nimbus::orch::nsForChat(rec.chatId, /*admin=*/false));
          freshChatId_ = rec.chatId;
          if (!freshSinceMs_) freshSinceMs_ = nowMs();
        } else {
          deliver(rec.chatId, failText);
        }
        fireResult(rec.tag, orch::JobState::Error, rec.chatId, "lost contact");
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
    if (orch::isTerminal(env_ref().state) && d_.recordSpawnUsage &&
        (env_ref().promptTokens || env_ref().completionTokens))
      d_.recordSpawnUsage(rec.backend, env_ref().promptTokens, env_ref().completionTokens);
    fireResult(rec.tag, env_ref().state, rec.chatId,
               env_ref().reply[0] ? env_ref().reply : env_ref().error);   // Error: reason, not "" (prism B)
    if (env_ref().state == orch::JobState::Done) {
      std::string reply = env_ref().reply[0] ? std::string(env_ref().reply) : std::string("(done, no output)");
      // TOPOLOGY INVERSION (owner R5b): the owner talks ONLY to the orchestrator.
      // Raw sub-agent output no longer goes straight to Telegram - the FULL result
      // is parked as a fresh result and a per-completion SYNTHESIS turn (above, in
      // this same pump cadence after a short coalesce window) decides what the
      // owner actually sees. If the synthesis can't run (heap/provider down), the
      // 60 s fallback drains the raw results with their labels - never lost.
      emitJobState(rec.tag, rec.backend, orch::JobState::Done);
      d_.journal->markSeen(rec.tag);
      watchClear(rec.tag);
      scheduleReap(rec.tag);   // collapse the Done arc off the ring after a short grace
      // v4.0.0 auto-persist: a run-tagged job's FULL reply becomes a durable
      // doc in its project - how "sub-agents write documents" actually works
      // (they have no device tools; the head persists on their behalf). The
      // outcome line rides the fresh result so the planner KNOWS what saved.
      if (rec.prj[0] && d_.persistResult) {
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
                     nimbus::orch::nsForChat(rec.chatId, /*admin=*/false));
      freshChatId_ = rec.chatId;
      if (!freshSinceMs_) freshSinceMs_ = nowMs();   // oldest un-synthesized result
    } else if (env_ref().state == orch::JobState::Error) {
      // While OTHER work is still running, a per-sub failure joins the fresh
      // results (the final report says which subs failed) instead of pinging
      // the owner mid-run - 50 subs must not mean up to 50 failure messages.
      // A failure with nothing else live IS the final report: deliver as before.
      const std::string failText = std::string("Job [") + jobLabel(rec) + "] failed: " +
              (env_ref().error[0] ? env_ref().error : "unknown error");
      if (activeCount() > 1 || !pending_.empty()) {   // >1: this job still counts
        addFreshResult(rec.tag, jobLabel(rec).c_str(), std::string("FAILED: ") + failText,
                       nimbus::orch::nsForChat(rec.chatId, /*admin=*/false));
        freshChatId_ = rec.chatId;
        if (!freshSinceMs_) freshSinceMs_ = nowMs();
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
