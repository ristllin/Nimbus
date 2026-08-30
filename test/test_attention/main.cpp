#include <unity.h>

#include "nimbus/attention.h"

using namespace nimbus::attn;
using solide::ring::Status;

void setUp() {}
void tearDown() {}

// ---- helpers ----------------------------------------------------------------

// accent < 0 => no accent (hasAccent stays false); accent 0..255 => a provider
// accent, including 255 (white / unknown provider).
static Event jobState(uint32_t key, Status st, int accent = -1) {
  Event e;
  e.type = Event::Type::JobState;
  e.key = key;
  e.status = uint8_t(st);
  if (accent >= 0) {
    e.hasAccent = true;
    e.accentHue = uint8_t(accent);
  }
  return e;
}

static Event jobProgress(uint32_t key, uint8_t pct) {
  Event e;
  e.type = Event::Type::JobProgress;
  e.key = key;
  e.value = pct;
  return e;
}

static Event voice(VoiceStage st) {
  Event e;
  e.type = Event::Type::Voice;
  e.stage = st;
  return e;
}

static Event simple(Event::Type t) {
  Event e;
  e.type = t;
  return e;
}

// Fetch the snapshot slot for a key (used==false if absent).
static solide::ring::Slot slotFor(const Router& r, uint32_t key) {
  solide::ring::Slot snap[RING_MAX_SEGMENTS];
  const int n = r.jobs().snapshot(snap, RING_MAX_SEGMENTS);
  for (int i = 0; i < n; ++i)
    if (snap[i].key == key) return snap[i];
  return {};
}

// Every route() sets epd.render, so a Decision is fully pinned by four values.
// Macro (not function) so Unity failure lines point at the call site.
#define ASSERT_DECISION(d, wantScreen, wantAttn, wantNotify, wantRingDirty) \
  do {                                                                      \
    TEST_ASSERT_TRUE((d).screen.render);                                       \
    TEST_ASSERT_EQUAL(int(wantScreen), int((d).screen.id));                \
    TEST_ASSERT_EQUAL(int(wantAttn), int((d).screen.attention));               \
    TEST_ASSERT_EQUAL(int(wantNotify), int((d).notify.notify));             \
    TEST_ASSERT_EQUAL(int(wantRingDirty), int((d).ringDirty));              \
  } while (0)

#define ASSERT_TOP(top, wantActive, wantStatus, wantHue)      \
  do {                                                        \
    TEST_ASSERT_EQUAL(int(wantActive), int((top).active));    \
    TEST_ASSERT_EQUAL(int(wantStatus), int((top).status));    \
    TEST_ASSERT_EQUAL(int(wantHue), int((top).hue));          \
  } while (0)

// ---- classification ----------------------------------------------------------

static void test_status_classification_all_seven() {
  TEST_ASSERT_FALSE(isAttentionStatus(Status::Idle));
  TEST_ASSERT_FALSE(isAttentionStatus(Status::Running));
  TEST_ASSERT_TRUE(isAttentionStatus(Status::WaitingInput));
  TEST_ASSERT_TRUE(isAttentionStatus(Status::AwaitingApproval));
  TEST_ASSERT_FALSE(isAttentionStatus(Status::Done));
  TEST_ASSERT_TRUE(isAttentionStatus(Status::Error));
  TEST_ASSERT_FALSE(isAttentionStatus(Status::Offline));
}

// ---- per-event full Decisions --------------------------------------------------

static void test_jobstate_ambient_full_decision() {
  Router r;
  // Non-attention states: coalesced StatusIdle, ring recompute, no ping.
  Decision d = r.route(jobState(1, Status::Running), 100);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  d = r.route(jobState(2, Status::Idle), 200);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  d = r.route(jobState(1, Status::Done), 300);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  TEST_ASSERT_EQUAL(2, r.jobs().count());
  TEST_ASSERT_EQUAL(int(Status::Done), int(slotFor(r, 1).status));
}

static void test_jobstate_attention_full_decision_each_status() {
  Router r;
  const Status attn[] = {Status::WaitingInput, Status::AwaitingApproval,
                         Status::Error};
  uint32_t key = 10;
  for (Status st : attn) {
    Decision d = r.route(jobState(key++, st), 500);
    // Immediate badge + Telegram-able notify, ring recompute.
    ASSERT_DECISION(d, ScreenId::StatusIdle, true, true, true);
    TEST_ASSERT_EQUAL(int(Event::Type::JobState), int(d.notify.reason));
  }
  TEST_ASSERT_EQUAL(3, r.jobs().count());
}

static void test_jobprogress_full_decision() {
  Router r;
  r.route(jobState(7, Status::Running), 0);
  Decision d = r.route(jobProgress(7, 42), 100);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  TEST_ASSERT_EQUAL(42, slotFor(r, 7).progress);
  // Progress for an unknown key routes the same but registers nothing.
  d = r.route(jobProgress(99, 10), 200);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  TEST_ASSERT_EQUAL(1, r.jobs().count());
}

static void test_offline_frees_slot() {
  Router r;
  r.route(jobState(1, Status::Running), 0);
  r.route(jobState(2, Status::WaitingInput), 10);
  TEST_ASSERT_EQUAL(2, r.jobs().count());

  // Offline is non-attention: ambient decision, but the slot is gone.
  Decision d = r.route(jobState(2, Status::Offline), 20);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  TEST_ASSERT_EQUAL(1, r.jobs().count());
  TEST_ASSERT_FALSE(slotFor(r, 2).used);
  TEST_ASSERT_TRUE(slotFor(r, 1).used);
}

static void test_incoming_ask_and_cleared() {
  Router r;
  // The ask already reached the user's channel, so no NotifyIntent.
  Decision d = r.route(simple(Event::Type::IncomingAsk), 0);
  ASSERT_DECISION(d, ScreenId::Ask, true, false, true);
  TEST_ASSERT_TRUE(r.askPending());

  d = r.route(simple(Event::Type::AskCleared), 100);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  TEST_ASSERT_FALSE(r.askPending());
}

static void test_force_expire_attention_fallback() {
  Router r;
  // A stuck Error arc + a stuck ask, plus a healthy Running job that must survive.
  r.route(jobState(1, Status::Error), 1000);          // enteredAt = 1000
  r.route(jobState(2, Status::Running), 1000);        // ambient, never expired
  r.route(simple(Event::Type::IncomingAsk), 2000);    // askSince_ = 2000
  TEST_ASSERT_TRUE(r.topAttention().active);          // something red/attention up

  const uint32_t cap = 300000;  // AttnHoldMs
  // Before the cap: nothing expires (the reap window hasn't elapsed).
  TEST_ASSERT_FALSE(r.forceExpireAttention(1000 + cap, cap));   // Error age == cap, not > cap
  TEST_ASSERT_TRUE(r.askPending());

  // Past the cap: the Error arc (age from 1000) AND the ask (from 2000) clear;
  // the Running job is untouched, so the ring falls back to ambient.
  TEST_ASSERT_TRUE(r.forceExpireAttention(2000 + cap + 1, cap));
  TEST_ASSERT_FALSE(r.askPending());
  TEST_ASSERT_FALSE(r.topAttention().active);         // no attention source left
  // The healthy Running job is still tracked (ambient, not an attention state).
  solide::ring::Slot snap[RING_MAX_SEGMENTS];
  int n = r.jobs().snapshot(snap, RING_MAX_SEGMENTS);
  bool runningAlive = false;
  for (int i = 0; i < n; i++)
    if (snap[i].key == 2 && snap[i].status == Status::Running) runningAlive = true;
  TEST_ASSERT_TRUE(runningAlive);
  // Idempotent: nothing left to expire.
  TEST_ASSERT_FALSE(r.forceExpireAttention(9999999, cap));
}

// Regression: a re-ask while an ask is ALREADY pending must not reset the dwell
// clock, or the absolute cap is defeated and the ring breathes "needs you" forever
// (a routine that asks each run, with scheduled turns never clearing it).
static void test_ask_reask_does_not_reset_dwell_cap() {
  Router r;
  const uint32_t cap = 300000;  // AttnHoldMs
  r.route(simple(Event::Type::IncomingAsk), 2000);            // NEW ask -> askSince_ = 2000
  TEST_ASSERT_TRUE(r.askPending());
  // Re-ask just before the cap while still pending: the clock must NOT restart.
  r.route(simple(Event::Type::IncomingAsk), 2000 + cap - 1);
  TEST_ASSERT_TRUE(r.askPending());
  // Measured from the FIRST ask (2000), the latch is now past the cap and clears.
  TEST_ASSERT_TRUE(r.forceExpireAttention(2000 + cap + 1, cap));
  TEST_ASSERT_FALSE(r.askPending());
  // After a genuine clear a fresh ask restamps normally (a new dwell window opens).
  r.route(simple(Event::Type::IncomingAsk), 10000000);
  TEST_ASSERT_TRUE(r.askPending());
  TEST_ASSERT_FALSE(r.forceExpireAttention(10000000 + cap, cap));   // fresh window, not yet due
}

// CUM-221 (the 5th-recurrence stuck-ring CLASS): a delivered sub-agent's Done arc
// is collapsed only by JobEngine::reapDone on tg_poll; forceExpireAttention skips
// it (Done is not attention). This main-loop backstop guarantees a Done ember can
// never strand lit when that reap stalls - while leaving live Running/Idle arcs.
static void test_force_expire_done_arcs_collapses_stranded_ember() {
  Router r;
  r.route(jobState(1, Status::Done), 1000);       // a delivered sub-agent, ember lit
  r.route(jobState(2, Status::Running), 1000);    // a genuinely live sub-agent
  r.route(jobState(3, Status::Idle), 1000);       // an open, idle session
  TEST_ASSERT_EQUAL(3, r.jobs().count());

  const uint32_t cap = 300000;  // AttnHoldMs + grace on the device
  // At exactly the cap: not yet (> cap, strictly, mirrors forceExpireAttention).
  TEST_ASSERT_FALSE(r.forceExpireDoneArcs(1000 + cap, cap));
  TEST_ASSERT_EQUAL(3, r.jobs().count());
  // Past the cap: the Done ember collapses (Offline frees its slot); the live
  // Running and Idle arcs are untouched (they are not terminal - reaping them
  // would erase real work, the accepted long-sub-agent trade).
  TEST_ASSERT_TRUE(r.forceExpireDoneArcs(1000 + cap + 1, cap));
  TEST_ASSERT_EQUAL(2, r.jobs().count());
  solide::ring::Slot snap[RING_MAX_SEGMENTS];
  int n = r.jobs().snapshot(snap, RING_MAX_SEGMENTS);
  bool doneGone = true, runningAlive = false, idleAlive = false;
  for (int i = 0; i < n; i++) {
    if (snap[i].key == 1) doneGone = false;
    if (snap[i].key == 2 && snap[i].status == Status::Running) runningAlive = true;
    if (snap[i].key == 3 && snap[i].status == Status::Idle) idleAlive = true;
  }
  TEST_ASSERT_TRUE(doneGone);
  TEST_ASSERT_TRUE(runningAlive);
  TEST_ASSERT_TRUE(idleAlive);
  // Idempotent: nothing terminal left to expire.
  TEST_ASSERT_FALSE(r.forceExpireDoneArcs(9999999, cap));
}

// The happy path (reapDone fires on tg_poll before the backstop's cap) is
// unaffected: a Done arc cleared to Offline in time leaves the backstop nothing.
static void test_force_expire_done_arcs_no_op_when_reap_ran() {
  Router r;
  r.route(jobState(1, Status::Done), 1000);
  r.route(jobState(1, Status::Offline), 1200);   // tg_poll reapDone collapsed it in time
  const uint32_t cap = 300000;
  TEST_ASSERT_FALSE(r.forceExpireDoneArcs(1000 + cap + 1, cap));
  TEST_ASSERT_EQUAL(0, r.jobs().count());
}

static void test_voice_glyph_attention_and_none_ambient() {
  Router r;
  // Every live stage takes the immediate path (glyph can't wait 30 s)...
  const VoiceStage live[] = {VoiceStage::Recording, VoiceStage::Processing,
                             VoiceStage::Speaking};
  for (VoiceStage st : live) {
    Decision d = r.route(voice(st), 0);
    ASSERT_DECISION(d, ScreenId::VoiceGlyph, true, false, true);
    TEST_ASSERT_EQUAL(int(st), int(r.voiceStage()));
  }
  // ...and None settles back to coalesced ambient, still marking the ring.
  Decision d = r.route(voice(VoiceStage::None), 100);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  TEST_ASSERT_EQUAL(int(VoiceStage::None), int(r.voiceStage()));
}

static void test_low_battery_and_ok() {
  Router r;
  // T1 warning is a compact badge over the status display, not the full
  // Battery telemetry screen.
  Decision d = r.route(simple(Event::Type::LowBattery), 0);
  ASSERT_DECISION(d, ScreenId::StatusIdle, true, true, true);
  TEST_ASSERT_EQUAL(int(Event::Type::LowBattery), int(d.notify.reason));
  TEST_ASSERT_TRUE(r.lowBattery());

  d = r.route(simple(Event::Type::BatteryOk), 100);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  TEST_ASSERT_FALSE(r.lowBattery());
}

static void test_network_events_flag_only() {
  Router r;
  // Status-screen material only: ring untouched (ringDirty stays false).
  Decision d = r.route(simple(Event::Type::NetworkDegraded), 0);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, false);
  TEST_ASSERT_TRUE(r.networkDegraded());

  d = r.route(simple(Event::Type::NetworkOk), 100);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, false);
  TEST_ASSERT_FALSE(r.networkDegraded());
}

// ---- topAttention -------------------------------------------------------------

static void test_top_attention_ignores_non_attention_jobs() {
  Router r;
  // Running/Done/Idle occupy slots (Running even has priority > Idle) but must
  // never light the Passive attention LED.
  r.route(jobState(1, Status::Running), 0);
  r.route(jobState(2, Status::Done), 10);
  r.route(jobState(3, Status::Idle), 20);
  TEST_ASSERT_EQUAL(3, r.jobs().count());
  TEST_ASSERT_FALSE(r.topAttention().active);
}

// Full precedence chain: ask > approve-job > input-job > error-job > low-batt
// > none. Built up, then peeled back down.
static void test_top_attention_precedence_chain() {
  Router r;
  TEST_ASSERT_FALSE(r.topAttention().active);

  // error job: Error style is hue 0 (red solid).
  r.route(jobState(1, Status::Error), 0);
  ASSERT_TOP(r.topAttention(), true, Status::Error, 0);

  // input job outranks error (priority 3 > 2); WaitingInput style hue 213.
  r.route(jobState(2, Status::WaitingInput), 10);
  ASSERT_TOP(r.topAttention(), true, Status::WaitingInput, 213);

  // approval job outranks both (priority 4); AwaitingApproval style hue 32.
  r.route(jobState(3, Status::AwaitingApproval), 20);
  ASSERT_TOP(r.topAttention(), true, Status::AwaitingApproval, 32);

  // low battery never outranks an attention job.
  r.route(simple(Event::Type::LowBattery), 30);
  ASSERT_TOP(r.topAttention(), true, Status::AwaitingApproval, 32);

  // an ask outranks everything (WaitingInput style, hue 213).
  r.route(simple(Event::Type::IncomingAsk), 40);
  ASSERT_TOP(r.topAttention(), true, Status::WaitingInput, 213);

  // peel: clear the ask -> approval job resurfaces.
  r.route(simple(Event::Type::AskCleared), 50);
  ASSERT_TOP(r.topAttention(), true, Status::AwaitingApproval, 32);

  // peel: approval job ends -> input job.
  r.route(jobState(3, Status::Offline), 60);
  ASSERT_TOP(r.topAttention(), true, Status::WaitingInput, 213);

  // peel: input job ends -> error job.
  r.route(jobState(2, Status::Offline), 70);
  ASSERT_TOP(r.topAttention(), true, Status::Error, 0);

  // peel: error job ends -> low battery (reported as Error style, hue 0).
  r.route(jobState(1, Status::Offline), 80);
  TEST_ASSERT_EQUAL(0, r.jobs().count());
  ASSERT_TOP(r.topAttention(), true, Status::Error, 0);

  // peel: battery recovers -> nothing needs attention.
  r.route(simple(Event::Type::BatteryOk), 90);
  TEST_ASSERT_FALSE(r.topAttention().active);
}

// ---- accent passthrough ---------------------------------------------------------

static void test_accent_passthrough_in_snapshot() {
  Router r;
  // No accent supplied: slot stays accent-free (renders status style).
  r.route(jobState(1, Status::Running), 0);
  TEST_ASSERT_FALSE(slotFor(r, 1).hasAccent);

  // Any hue 0..254 is a valid provider accent.
  r.route(jobState(2, Status::Running, /*accent=*/42), 10);
  TEST_ASSERT_TRUE(slotFor(r, 2).hasAccent);
  TEST_ASSERT_EQUAL(42, slotFor(r, 2).accentHue);

  r.route(jobState(3, Status::Running, /*accent=*/0), 20);
  TEST_ASSERT_TRUE(slotFor(r, 3).hasAccent);
  TEST_ASSERT_EQUAL(0, slotFor(r, 3).accentHue);

  // 255 (white) is a real accent too - the "unknown provider" colour - not a
  // "no accent" sentinel.
  r.route(jobState(4, Status::Running, /*accent=*/255), 25);
  TEST_ASSERT_TRUE(slotFor(r, 4).hasAccent);
  TEST_ASSERT_EQUAL(255, slotFor(r, 4).accentHue);

  // A later state change may update the accent...
  r.route(jobState(2, Status::WaitingInput, /*accent=*/99), 30);
  TEST_ASSERT_EQUAL(99, slotFor(r, 2).accentHue);
  // ...while an event with no accent leaves the existing accent untouched.
  r.route(jobState(2, Status::Error), 40);
  TEST_ASSERT_TRUE(slotFor(r, 2).hasAccent);
  TEST_ASSERT_EQUAL(99, slotFor(r, 2).accentHue);
}

// Out-of-range status bytes are dropped, not turned into phantom slots.
static void test_out_of_range_status_is_dropped() {
  Router r;
  Event e;
  e.type = Event::Type::JobState;
  e.key = 5;
  e.status = 9;  // > Offline (6)
  Decision d = r.route(e, 0);
  TEST_ASSERT_FALSE(d.screen.render);
  TEST_ASSERT_FALSE(d.ringDirty);
  TEST_ASSERT_EQUAL(0, r.jobs().count());
  // A valid status for the same key still works afterwards.
  d = r.route(jobState(5, Status::Running), 10);
  ASSERT_DECISION(d, ScreenId::StatusIdle, false, false, true);
  TEST_ASSERT_EQUAL(1, r.jobs().count());
}

// THE RED-RING FLAP-BACK GUARD (owner root-cause round 2026-07-13). The broker
// re-sends full snapshots on every event, so a force-expired stale Error used to be
// re-added with a fresh enteredAt and red flapped back every backstop cycle. The
// tombstone must suppress the identical re-add...
static void test_tombstone_blocks_stale_error_flapback() {
  Router r;
  const uint32_t cap = 300000;
  r.route(jobState(1, Status::Error), 1000);
  TEST_ASSERT_TRUE(r.forceExpireAttention(1000 + cap + 1, cap));   // expire -> tombstone
  TEST_ASSERT_FALSE(r.topAttention().active);
  // The broker's next full snapshot re-asserts the SAME stale Error -> suppressed.
  Decision d = r.route(jobState(1, Status::Error), 1000 + cap + 5000);
  TEST_ASSERT_FALSE(d.ringDirty);                                   // no-op decision
  TEST_ASSERT_EQUAL(0, r.jobs().count());                           // nothing re-added
  TEST_ASSERT_FALSE(r.topAttention().active);                       // red stays gone
}

// ...but a REAL state change must revive the key instantly, and the TTL safety
// valve must guarantee suppression can never outlive kTombstoneTtlMs.
static void test_tombstone_cleared_by_state_change_and_ttl() {
  Router r;
  const uint32_t cap = 300000;
  // (a) different status clears the tombstone, then a NEW error is honored.
  r.route(jobState(1, Status::Error), 1000);
  r.forceExpireAttention(1000 + cap + 1, cap);
  r.route(jobState(1, Status::Running), 1000 + cap + 2000);   // real change -> clears
  Decision d = r.route(jobState(1, Status::Error), 1000 + cap + 3000);
  TEST_ASSERT_TRUE(d.ringDirty);                              // fresh error accepted
  TEST_ASSERT_TRUE(r.topAttention().active);
  // (b) TTL valve: expire again, then re-assert AFTER kTombstoneTtlMs -> accepted.
  const uint32_t t2 = 1000 + cap + 3000;
  r.forceExpireAttention(t2 + cap + 1, cap);
  const uint32_t late = t2 + cap + 1 + Router::kTombstoneTtlMs + 1;
  d = r.route(jobState(1, Status::Error), late);
  TEST_ASSERT_TRUE(d.ringDirty);                              // never suppressed forever
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tombstone_blocks_stale_error_flapback);
  RUN_TEST(test_tombstone_cleared_by_state_change_and_ttl);
  RUN_TEST(test_ask_reask_does_not_reset_dwell_cap);
  RUN_TEST(test_status_classification_all_seven);
  RUN_TEST(test_out_of_range_status_is_dropped);
  RUN_TEST(test_jobstate_ambient_full_decision);
  RUN_TEST(test_jobstate_attention_full_decision_each_status);
  RUN_TEST(test_jobprogress_full_decision);
  RUN_TEST(test_offline_frees_slot);
  RUN_TEST(test_incoming_ask_and_cleared);
  RUN_TEST(test_force_expire_attention_fallback);
  RUN_TEST(test_force_expire_done_arcs_collapses_stranded_ember);
  RUN_TEST(test_force_expire_done_arcs_no_op_when_reap_ran);
  RUN_TEST(test_voice_glyph_attention_and_none_ambient);
  RUN_TEST(test_low_battery_and_ok);
  RUN_TEST(test_network_events_flag_only);
  RUN_TEST(test_top_attention_ignores_non_attention_jobs);
  RUN_TEST(test_top_attention_precedence_chain);
  RUN_TEST(test_accent_passthrough_in_snapshot);
  return UNITY_END();
}
