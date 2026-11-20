#include "orchestrator_common.h"

#include <atomic>

#include "../sys/agent_log.h"
#include "store.h"
#include "telegram.h"                   // telegram::enabled - telegramDeliverable
#include "adapters/moderation.h"        // CUM-69 device classifier (behind the gate decision core)
#include "safety_activity_store.h"       // CUM-215 Safety activity log + scoped allowlist (record/allow)
#include "nimbus/orch/moderation.h"     // portable gate decision core (fail-open/closed, admin-exempt)
#include "nimbus/harness/config.h"      // telegramDeliverableFor (host-tested)
#include "nimbus/harness/head_arc.h"    // portable head-arc reconciler (W6)
#include "nimbus/orch/voice_notes.h"     // cutForSpeech (ttsAnnounce)

// orchestrator_common - the engine-neutral half of the device seam (lane RB-SEAM1a, no
// behaviour change): job glue, ring arcs, voice, the job status getters and the reply
// sinks' allowlist and moderation gate, moved here from orchestrator.cpp so a second
// engine can be bound beside the old one. orchestrator.cpp keeps the old engine's half
// (and, for now, what a host test pins there by path or what would cost image bytes to
// move: lanes/RB-SEAM1a/PR_BODY.md lists both); what the two share is declared in
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

// ---- job glue and its status getters -------------------------------------------

bool cancel(const char* tagOrJobId) {
  return g_jobs && g_jobs->cancel(tagOrJobId);
}

int  activeJobCount() { return g_journal.count(); }
int  pendingSpawnCount() { return g_jobs ? g_jobs->pendingCount() : 0; }   // queued, not yet dispatched

// ---- voice -----------------------------------------------------------------------
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

// ---- reply sinks: who a reply may reach -----------------------------------------

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

// ---- ring arcs ---------------------------------------------------------------------

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

// ---- moderation gates (CUM-69) ---------------------------------------------
// The portable decision core (lib/core/orch_moderation) owns the fail-open/closed
// policy + admin exemption; the device adapter runs the classifier. Every gate is
// owner opt-in (default off) so with the switches off NO classifier call is made
// and the turn path is byte-identical to before.

static nimbus::orch::ModConfig modConfig() {
  nimbus::orch::ModConfig c;
  c.inbound   = store::modInbound();
  c.outbound  = store::modOutbound();
  c.injection = store::modInjection();
  return c;
}

// Run one gate: apply the role/switch gate; if it applies, classify + decide. The
// outbound gate can run while a turn holds the single TLS slot, so it uses a SHORT
// acquire timeout (skips = fail-open if busy) rather than stalling the reply; the
// inbound gate runs pre-turn with the slot free, so it waits the full window.
nimbus::orch::ModAction moderateGate(nimbus::orch::ModGate gate, const String& chatId,
                                     const String& text) {
  using namespace nimbus::orch;
  ModConfig cfg = modConfig();
  Role role = roleOfChat(chatId);
  if (!gateApplies(gate, role, cfg)) return ModAction::Allow;
  const uint32_t acquireMs = (gate == ModGate::OutboundReply) ? 250u : 15000u;
  ClassifierVerdict v = agent::moderation::classify(std::string(text.c_str()), gate, acquireMs);
  ModAction a = decide(gate, v);
  // Safety ACTIVITY surface (CUM-215): a text gate that blocks is a scanner verdict
  // the owner can review. Consult the owner's SCOPED allowlist first: an approved
  // sender/pattern turns the block into a real unblock (never a global off switch);
  // otherwise the verdict is recorded to the bounded activity log for the Safety tab.
  if (a == ModAction::Block) {
    const std::string s(chatId.c_str()), tx(text.c_str());
    const std::string src = modGateName(gate);   // inbound | outbound: the gate binding
    // The classifier returns no sub-category, so the rule is the coarse moderation
    // class (kCoarseModerationRule); the granular gate name goes to `source`. The
    // owner can unblock this sender or this exact content, but not "this type" (that
    // would be the whole gate - forbidden by contentClassApprovable). The allow-rule is
    // bound to this gate's source, so an inbound approval never quiets the outbound gate.
    if (agent::safety::allowed(kCoarseModerationRule, s, src, tx)) return ModAction::Allow;
    // Record ONLY a genuine classifier flag. A fail-closed Error (no key / outage) also
    // returns Block here, but it is the absence of a verdict, not flagged content: do not
    // persist unscreened guest text as "blocked" or make it reportable (CUM-215).
    if (blockIsScannerVerdict(v))
      agent::safety::record(SafetyVerdict::Blocked, kCoarseModerationRule, "telegram", s, src, tx);
  }
  return a;
}

}  // namespace orchestrator
}  // namespace agent
