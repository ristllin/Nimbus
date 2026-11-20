#include "orchestrator_common.h"

#include <atomic>

#include "../sys/agent_log.h"
#include "store.h"
#include "telegram.h"                   // telegram::enabled - telegramDeliverable
#include "nimbus/harness/config.h"      // telegramDeliverableFor (host-tested)
#include "nimbus/harness/head_arc.h"    // portable head-arc reconciler (W6)
#include "nimbus/orch/voice_notes.h"     // cutForSpeech (ttsAnnounce)

// orchestrator_common - the engine-neutral half of the device seam (lane RB-SEAM1a, no
// behaviour change): tenants, job glue, ring arcs, voice, status getters and reply sinks,
// moved here from orchestrator.cpp so a second engine can be bound beside the old one.
// orchestrator.cpp keeps the old engine's half; what the two share is declared in
// orchestrator_common.h. The public surface is orchestrator.h, unchanged.

namespace agent {
namespace orchestrator {

namespace orch = nimbus::orch;
namespace attn = nimbus::attn;

// ---- module state shared with orchestrator.cpp ---------------------------------
HeavyFabric* g_fabric = nullptr;
Sinks        g_sinks;
// The portable sub-agent job machinery (spawn queue / dispatch gate / round-robin
// poll / fresh results / reap scheduler) - constructed in begin(), Stage F lift.
JobEngine*   g_jobs = nullptr;


// ---- sub-agent journal: cross-task snapshot + staged terminate (prism F24) ---
// The sub-agent journal (g_journal) is SINGLE-WRITER on the tg_poll task - pump()
// update/markSeen/gc it every cycle. External MCP clients reach session.list and
// session.terminate on the AsyncTCP web task, which must NEVER touch the journal
// concurrently (a poll firing mid-request could tear a record / corrupt job state).
// So, mirroring loops:: web-staging and the g_memEcho model:
//   * READS  (session.list, sessionsJson) go through a snapshot rebuilt on tg_poll
//     after each pump(), copied out under a short mutex - never the journal;
//   * WRITES (session.terminate) are STAGED from the web task and drained on
//     tg_poll (terminate becomes async - the remote cancel is best-effort anyway).
// The turn's OWN session_ops[] terminate stays synchronous: it runs on tg_poll via
// d.cancelSession -> cancel(), already the journal's writer task, so it's untouched.
SemaphoreHandle_t        g_sessMux = nullptr;   // guards g_sessSnapshot
// MuxHold is in orchestrator_common.h (the results ring in orchestrator.cpp holds it too).
static std::vector<nimbus::orch::SessionInfo> g_sessSnapshot;
static portMUX_TYPE             g_termSpin = portMUX_INITIALIZER_UNLOCKED;
static std::vector<std::string> g_termQueue;           // guarded by g_termSpin

// Rebuild the AsyncTCP-readable snapshot from the journal. Called ONLY on tg_poll
// (same task as pump), so the sessionInfos() journal read is race-free; the vector
// is built outside the lock and swapped in (O(1), no alloc while the mutex is held).
void refreshSessionSnapshot() {
  std::vector<nimbus::orch::SessionInfo> fresh =
      g_jobs ? g_jobs->sessionInfos() : std::vector<nimbus::orch::SessionInfo>{};
  MuxHold hold(g_sessMux);
  g_sessSnapshot.swap(fresh);
}

// Apply web-staged terminates on tg_poll (the journal's single writer). Swap the
// queue out under the spinlock (O(1)), then cancel() each outside it. A bad/stale
// id is a harmless no-op (the job already finished between staging and draining).
void drainTerminates() {
  std::vector<std::string> work;
  portENTER_CRITICAL(&g_termSpin);
  work.swap(g_termQueue);
  portEXIT_CRITICAL(&g_termSpin);
  for (const std::string& id : work) {
    bool ok = g_jobs && g_jobs->cancel(id.c_str());
    alogf("orchestrator: staged terminate %s -> %s", id.c_str(), ok ? "cancelled" : "not found");
  }
}


bool cancel(const char* tagOrJobId) {
  return g_jobs && g_jobs->cancel(tagOrJobId);
}

int  activeJobCount() { return g_journal.count(); }
int  pendingSpawnCount() { return g_jobs ? g_jobs->pendingCount() : 0; }   // queued, not yet dispatched

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

String jobsSummary() {
  int n = g_journal.count();
  if (n == 0) return "No active jobs.";
  String s;
  for (int i = 0; i < n; i++) {
    orch::JobRecord rec;
    if (!g_journal.get(i, rec)) continue;
    s += String(rec.tag) + " (" + rec.backend + "/" + rec.model + ") " +
         rec.category + " [" + stateStr(rec.state) + "]\n";
  }
  return s;
}


// tg_poll ONLY (the journal's writer task) - used by composeInputs on the turn.
// AsyncTCP/MCP callers must use sessionInfosSnapshot() instead.
std::vector<nimbus::orch::SessionInfo> sessionInfos() {
  return g_jobs ? g_jobs->sessionInfos() : std::vector<nimbus::orch::SessionInfo>{};
}

// Cross-task-safe read of the session snapshot (see refreshSessionSnapshot).
std::vector<nimbus::orch::SessionInfo> sessionInfosSnapshot() {
  std::vector<nimbus::orch::SessionInfo> out;
  MuxHold hold(g_sessMux);   // the copy allocates: given back on every path out (L24)
  out = g_sessSnapshot;
  return out;
}

bool sessionKnown(const std::string& id) {
  MuxHold hold(g_sessMux);
  for (const auto& s : g_sessSnapshot)
    if (s.id == id) return true;
  return false;
}


void stageTerminate(const std::string& id) {
  portENTER_CRITICAL(&g_termSpin);
  g_termQueue.push_back(id);   // bounded (kAgentMaxJobs); drained on tg_poll
  portEXIT_CRITICAL(&g_termSpin);
}

// Hold-to-talk message lifecycle (CUM-456) - see voiceMessages() in the header.
std::atomic<uint32_t> s_voiceHandled{0};
std::atomic<bool>     s_voiceOk{false};
bool                  s_inVoiceMessage = false;   // tg_poll only

// ---- attention-event helpers (the plan §3.6 integration seam) ---------------
// keyFromTag / ringStatusFor / emitJobState / emitJobCleared live in the portable
// JobEngine; the head-turn Running/Offline arc + the stuck-turn reaper live in
// the portable TurnEngine. emitAsk stays: it's turn-application, not job
// machinery, and rides the device ApplyDeps.

void emitAsk() {
  if (!g_sinks.event) return;
  attn::Event e; e.type = attn::Event::Type::IncomingAsk;
  g_sinks.event(e);
}

// Clear a pending "needs you" ask. Nothing used to fire this on the normal path, so
// an ask latched the Dark/Calm attention LED (and masked every other attention
// source) until the router's ~6-min backstop aged it out. The owner engaging - a
// message or a voice reply - IS the answer, so clear it then. (A scheduled loop
// turn is NOT the owner answering, so it deliberately doesn't call this.)
void clearAsk() {
  if (!g_sinks.event) return;
  attn::Event e; e.type = attn::Event::Type::AskCleared;
  g_sinks.event(e);
}

// Error arcs are calls-to-action: hold them for the tunable attention window
// (Param::AttnHoldMs, fed by main.cpp's applyConfig). Latched here because
// setAttnHoldMs can fire from applyConfig BEFORE begin() constructs the engine.
uint32_t g_attnHoldMs = 300000;  // 5 min default, mirrors the Param preset


// Speak a short confirmation of a job result aloud (opt-in via store::ttsEnabled).
// LIVE-GATED + BENCH-BROKEN: the audio path is unverified here (no key, broken
// bench audio). Compile-clean; the sink is nullable and no-ops when unset.
static void ttsAnnounce(const String& text) {
  if (!g_sinks.speak || !store::ttsEnabled() || text.length() == 0) return;
  int end = -1;
  for (int i = 0; i < (int)text.length() && i < 180; i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?' || c == '\n') { end = i + 1; break; }
  }
  // No sentence end in reach: the speech cut's rule, never in the middle of a character.
  String snippet = (end > 0) ? text.substring(0, end)
                             : String(nimbus::orch::cutForSpeech(std::string(text.c_str()), 180).text.c_str());
  g_sinks.speak(String(), snippet);   // no chat: the owner chat if the speaker cannot play it
}


// CUM-523: HarnessConfig::telegramDeliverable on this device - the portable
// agent::telegramDeliverableFor rule (config.h, host-tested) over the live bot,
// allowlist and public-mode facts.
bool telegramDeliverable(const std::string& chatId) {
  return agent::telegramDeliverableFor(agent::telegram::enabled(),
                                       store::telegramAllowlist().length() > 0,
                                       store::telegramPublic(), chatId);
}

// Is `cid` in the persisted Telegram allowlist? Read from NVS (thread-safe) - NOT
// telegram's poll-task-owned g_allowlist, since this runs on the turn task.
bool chatAllowed(const String& cid) {
  String al = store::telegramAllowlist();
  int s = 0;
  while (s < (int)al.length()) {
    int e = al.indexOf(',', s); if (e < 0) e = al.length();
    String t = al.substring(s, e); t.trim();
    if (t.length() && t == cid) return true;
    s = e + 1;
  }
  return false;
}


String firstAllowedChat() {
  String al = store::telegramAllowlist();
  int e = al.indexOf(','); if (e < 0) e = al.length();
  String t = al.substring(0, e); t.trim();
  return t;
}
bool isChatAllowed(const String& cid) { return chatAllowed(cid); }

uint32_t headJobKey() { return keyFromTag("head"); }

// CUM-11 backstop metric: every time a belt-and-braces path (not the primary edge)
// has to clear a stuck ring arc, this counts it. The 24 h wake-up soak acceptance is
// this == 0 - the backstops are insurance, never the mechanism. Surfaced in
// /api/state as ring.backstopFires so the soak can assert it stayed flat.
static uint32_t s_ringBackstopFires = 0;
uint32_t ringBackstopFires() { return s_ringBackstopFires; }
void noteRingBackstopFired() { ++s_ringBackstopFires; }

// Stuck-turn reaper - the decision + the arc-freeing event live in the portable
// TurnEngine (loop budget + 2 min margin). Called from the always-alive loop().
bool reapStuckTurn(uint32_t nowMs) {
  const bool fired = engineReapStuckTurn(nowMs);
  if (fired) ++s_ringBackstopFires;   // a dead turn slipped past the primary clear
  return fired;
}

// W6: keep the orchestrator's own "head" ring arc lit while its sub-agents run.
// A fan-out turn returns (and the TurnGuard would clear the arc) the instant it
// enqueues the children - long before they finish - so the head arc collapsed
// before the child arcs appeared. This runs on the always-alive main loop (the
// same watchdog block as reapStuckTurn), asks the pure tracker whether to light,
// refresh, or clear, and drives the arc through the engine. Returns true when it
// changed the arc (so the caller can refreshRing()).
bool reconcileHeadArc(uint32_t nowMs) {
  if (!engineRunning()) return false;
  static nimbus::harness::HeadArcTracker s_tracker;
  static uint32_t s_lastTurnCount = 0;
  // Completion EDGE: turnCount() bumps once per finished turn. A short wake-up turn
  // can start AND finish between two watchdog ticks, so turnInFlight below never
  // samples true - the edge is what tells the tracker a (possibly arc-leaving) turn
  // happened, so the primary clear path fires instead of a backstop (CUM-11).
  const uint32_t tc = turnCount();
  const bool turnEnded = (tc != s_lastTurnCount);
  s_lastTurnCount = tc;
  const bool inTurn = turnInFlight();
  const int children = g_jobs ? g_jobs->activeCount() : 0;
  const auto action = s_tracker.reconcile(inTurn, children, nowMs, turnEnded);
  // Fold the tracker's own frozen-children backstop into the aggregate metric.
  static uint32_t s_lastTrackerBackstops = 0;
  const uint32_t tb = s_tracker.backstopFires();
  if (tb != s_lastTrackerBackstops) { s_ringBackstopFires += tb - s_lastTrackerBackstops; s_lastTrackerBackstops = tb; }
  switch (action) {
    case nimbus::harness::HeadArcTracker::Action::Light:
      engineSetHeadArc(true);
      return true;
    case nimbus::harness::HeadArcTracker::Action::Clear:
      engineSetHeadArc(false);
      return true;
    case nimbus::harness::HeadArcTracker::Action::None:
    default:
      return false;
  }
}

void setAttnHoldMs(uint32_t ms) {   // fed from Param::AttnHoldMs
  g_attnHoldMs = ms;                // latched: applyConfig can fire before begin()
  if (g_jobs) g_jobs->setAttnHoldMs(ms);
}


// Hold-to-talk (CUM-456): the count is read first (acquire), so a reader that sees
// a finished message also sees its ok.
void voiceMessages(uint32_t* handled, bool* lastOk) {
  const uint32_t n = s_voiceHandled.load(std::memory_order_acquire);
  if (handled) *handled = n;
  if (lastOk) *lastOk = s_voiceOk.load(std::memory_order_relaxed);
}

}  // namespace orchestrator
}  // namespace agent
