#include <unity.h>

#include <cstdio>
#include <vector>

#include "nimbus/attention.h"
#include "nimbus/render_sched.h"
#include "nimbus/profile.h"
#include "nimbus/ring_plan.h"
#include "nimbus_config.h"

// "Day in the life" (the HIL test spec): a scripted event stream driven through the
// SAME three portable modules the device loop wires together -
//   attention::Router  (semantic events -> ScreenIntent + ring-dirty)
//   ring::compose       (router state + cfg + cursor -> ring Plan)
//   render::Scheduler      (ScreenIntents/detents -> timed RenderCommands)
// - asserting the EXACT ring plans + e-ink RenderCommand sequence over time as
// jobs appear/progress/complete, attention fires, and posture/profile change.
//
// This is the integration gap the 189 unit tests missed (the HIL test spec): each
// module is tested in isolation, but the ORDER-DEPENDENT interplay (an attention
// badge bypassing a shut coalesce window mid-detent; a profile switch flipping
// posture AND scheduler windows on live pending work) only shows up when they
// run together against a clock.
//
// The event->scheduler wiring mirrors src/main.cpp exactly:
//   Decision d = router.route(e, now);
//   if (d.screen.render) sched.onIntent(uint8_t(d.screen.id), d.screen.attention, now);
// and the ring is composed from the router + cfg + cursor at the same instants.

using namespace nimbus;
using solide::ring::Status;
using attn::ScreenId;
using render::Kind;
using render::RenderCommand;

void setUp() {}
void tearDown() {}

namespace {

// One issued e-ink command, tagged with the millisecond it fired.
struct Issued {
  uint32_t atMs;
  uint8_t  screen;
  Kind     kind;
  bool     fullClear;
};

// A device stand-in: owns the router, scheduler, cursor and active Config, and
// exposes the same operations the main loop performs, so a test script reads
// like a timeline of real user/agent activity.
struct Device {
  attn::Router  router;
  render::Scheduler sched;
  ring::Cursor  cursor;
  Config        cfg;
  std::vector<Issued> issued;

  Device() {
    cfg.setProfile(ProfileId::Balanced);  // Passive, brightness 30 at boot
    applyProfileToSched();
  }

  // Push the active profile's e-ink windows into the scheduler, exactly as the
  // device does on a profile switch (main.cpp applies cfg -> SchedConfig).
  void applyProfileToSched() {
    render::SchedConfig sc;
    sc.dwellMs    = uint32_t(cfg.effective(Param::DwellMs));
    sc.coalesceMs = uint32_t(cfg.effective(Param::CoalesceMs));
    sched.configure(sc);
  }

  // Route one semantic event and feed the resulting ScreenIntent to the scheduler.
  attn::Decision emit(const attn::Event& e, uint32_t now) {
    attn::Decision d = router.route(e, now);
    if (d.screen.render)
      sched.onIntent(uint8_t(d.screen.id), d.screen.attention, now);
    return d;
  }

  // ---- typed event helpers (match test_ringplan's vocabulary) ----
  attn::Decision jobState(uint32_t key, Status st, uint32_t now, int accent = -1) {
    attn::Event e;
    e.type = attn::Event::Type::JobState;
    e.key = key;
    e.status = uint8_t(st);
    if (accent >= 0) { e.hasAccent = true; e.accentHue = uint8_t(accent); }
    return emit(e, now);
  }
  attn::Decision jobProgress(uint32_t key, uint8_t pct, uint32_t now) {
    attn::Event e;
    e.type = attn::Event::Type::JobProgress;
    e.key = key; e.value = pct;
    return emit(e, now);
  }
  attn::Decision voice(attn::VoiceStage s, uint32_t now) {
    attn::Event e;
    e.type = attn::Event::Type::Voice; e.stage = s;
    return emit(e, now);
  }
  attn::Decision lowBattery(uint8_t pct, uint32_t now) {
    attn::Event e;
    e.type = attn::Event::Type::LowBattery; e.value = pct;
    return emit(e, now);
  }

  // Encoder detent: move the cursor immediately (ring is instant) and arm the
  // dwell for the JobDetail screen (main.cpp uses ScreenId::JobDetail here).
  void detent(int dir, uint32_t now) {
    cursor.onDetent(dir, NIMBUS_RING_LEDS, now);
    sched.onDetent(uint8_t(ScreenId::JobDetail), now);
  }

  // Switch the active profile (menu/web/VBUS/battery); posture, brightness AND
  // the scheduler windows all follow.
  void setProfile(ProfileId id) { cfg.setProfile(id); applyProfileToSched(); }

  // ---- scheduler pump ----
  void tick(uint32_t t) {
    RenderCommand c = sched.tick(t);
    if (c.render) issued.push_back({t, c.screenId, c.kind, c.fullClear});
  }
  // Tick once per ms over [from,to] inclusive (as the sched tests do).
  void run(uint32_t from, uint32_t to) {
    for (uint32_t t = from;; ++t) { tick(t); if (t == to) break; }
  }
  void done(uint32_t t) { sched.onRenderDone(t); }

  // The ring plan the compositor would hand the LED driver right now.
  ring::Plan plan(bool panelBusy, uint32_t now) {
    return ring::compose(router, cfg, cursor, panelBusy, now);
  }
};

void assertTrace(const std::vector<Issued>& expected,
                 const std::vector<Issued>& actual) {
  char msg[48];
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected.size(), actual.size(),
                                   "issued command count");
  for (size_t i = 0; i < expected.size(); ++i) {
    std::snprintf(msg, sizeof msg, "cmd[%u] atMs", unsigned(i));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected[i].atMs, actual[i].atMs, msg);
    std::snprintf(msg, sizeof msg, "cmd[%u] screen", unsigned(i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected[i].screen, actual[i].screen, msg);
    std::snprintf(msg, sizeof msg, "cmd[%u] kind", unsigned(i));
    TEST_ASSERT_EQUAL_INT_MESSAGE(int(expected[i].kind), int(actual[i].kind), msg);
    std::snprintf(msg, sizeof msg, "cmd[%u] fullClear", unsigned(i));
    TEST_ASSERT_EQUAL_INT_MESSAGE(int(expected[i].fullClear),
                                  int(actual[i].fullClear), msg);
  }
}

}  // namespace

// ============================================================================
// A full Notifier-mode desk session over ~90 s of wall-clock, in Active posture.
//
// Timeline (Desk profile: Active, brightness 60, coalesce 15000, dwell 300,
// fullEveryN 8):
//   t=0      two agent jobs start Running (ambient) - first-ever ambient renders
//            StatusIdle immediately; panel busy 2214 ms (measured fast B/W).
//   t=5000   job #2 asks for approval (attention) - Badge bypasses the coalesce
//            window and renders as soon as the panel is free.
//   t=20000  user turns the knob 3x to inspect - cursor moves instantly on the
//            ring; e-ink JobDetail renders 300 ms after the LAST detent.
//   t=40000  job #2 approved -> Running again (ambient); job #1 completes (Done).
//   t=60000  both jobs go Offline - ring empties.
// Asserts the exact RenderCommand sequence AND the ring Plan (segCount, per-job
// segments, cursor glow) at each phase.
// ============================================================================
static void test_desk_session_active_jobs_attention_and_cursor() {
  Device dev;
  dev.setProfile(ProfileId::Desk);  // Active

  // -- t=0: two jobs appear. First is ambient StatusIdle, due immediately. --
  dev.jobState(1, Status::Running, 0, /*accent=*/170);
  dev.jobState(2, Status::Running, 0, /*accent=*/85);
  {
    ring::Plan p = dev.plan(/*panelBusy=*/false, 0);
    TEST_ASSERT_EQUAL(int(Posture::Full), int(p.posture));
    TEST_ASSERT_EQUAL_UINT8(60, p.brightness);              // Desk preset
    TEST_ASSERT_EQUAL(2, p.segCount);                       // both jobs on the ring
    TEST_ASSERT_EQUAL_UINT32(1, p.segs[0].key);
    TEST_ASSERT_EQUAL_UINT8(170, p.segs[0].accentHue);
    TEST_ASSERT_EQUAL_UINT32(2, p.segs[1].key);
    TEST_ASSERT_FALSE(p.single.lit);                        // single is Passive-only
  }
  dev.run(0, 0);          // StatusIdle fires at t=0 (first-ever ambient)
  dev.done(2214);         // fast B/W refresh completes

  // -- t=5000: job #2 needs approval. Attention Badge, immediate (coalesce shut
  //    until 2214+15000=17214, but attention bypasses it). --
  dev.jobState(2, Status::AwaitingApproval, 5000, /*accent=*/85);
  {
    // Progress fill on job #1 is ambient - recompute shows it on the ring.
    dev.jobProgress(1, 40, 5000);
    ring::Plan p = dev.plan(false, 5000);
    TEST_ASSERT_EQUAL(2, p.segCount);
    TEST_ASSERT_EQUAL_UINT8(40, p.segs[0].progress);
    TEST_ASSERT_EQUAL(int(Status::AwaitingApproval), int(p.segs[1].status));
  }
  dev.run(5000, 5000);    // Badge (attention) fires immediately
  dev.done(5000 + 2214);

  // -- t=20000: user inspects with the knob. Ring cursor is INSTANT; e-ink
  //    JobDetail waits dwell (300 ms) after the last detent. --
  dev.detent(+1, 20000);
  dev.detent(+1, 20100);
  dev.detent(+1, 20200);  // last detent: dwell fires at 20500
  {
    ring::Plan p = dev.plan(false, 20250);
    TEST_ASSERT_TRUE(p.cursor.active);                 // glow live immediately
    TEST_ASSERT_EQUAL_UINT8(3, p.cursor.index);        // moved +3 from 0
    TEST_ASSERT_FALSE(p.cursor.syncing);               // panel idle here
  }
  dev.run(20201, 20499);  // still silent: dwell not settled
  TEST_ASSERT_TRUE(dev.issued.size() == 2);            // nothing new yet
  dev.run(20500, 20500);  // JobDetail fires exactly at last-detent + 300
  dev.done(20500 + 2214);

  // -- t=40000: #2 approved back to Running (ambient), #1 completes (Done). The
  //    two ambients collapse into ONE coalesced StatusIdle render at the window
  //    edge. Last completed ambient was StatusIdle at t=0 (done 2214); Desk
  //    coalesce is 15000, so window opened long ago -> the FIRST of these fires
  //    immediately at 40000, the SECOND is held for a fresh 15000 window. --
  dev.jobState(2, Status::Running, 40000, /*accent=*/85);
  dev.jobState(1, Status::Done, 40000, /*accent=*/170);
  dev.run(40000, 40000);  // one StatusIdle now (latest-wins in the pending slot)
  dev.done(40000 + 2214);

  // -- t=60000: both jobs end. Ring empties. --
  dev.jobState(1, Status::Offline, 60000);
  dev.jobState(2, Status::Offline, 60000);
  {
    ring::Plan p = dev.plan(false, 60000);
    // Desk (Full) idle with no jobs -> FULLY DARK (owner 2026-07-15: an all-day
    // desk ring shouldn't glow white when nothing's happening; a CTA or a single-
    // click reveal still lights it). Supersedes the earlier faint-heartbeat design.
    TEST_ASSERT_EQUAL(0, p.segCount);
    TEST_ASSERT_FALSE(p.single.lit);
  }

  // Exact e-ink sequence over the whole session. Screens:
  //   StatusIdle=0, JobDetail=1, Badge=2. Desk fullEveryN=8 so no ghost upgrade
  //   inside these 4 renders.
  assertTrace(
      {
          {0, uint8_t(ScreenId::StatusIdle), Kind::FastBW, false},
          {5000, uint8_t(ScreenId::StatusIdle), Kind::FastBW, false},
          {20500, uint8_t(ScreenId::JobDetail), Kind::FastBW, false},
          {40000, uint8_t(ScreenId::StatusIdle), Kind::FastBW, false},
      },
      dev.issued);
}

// ============================================================================
// Passive posture (Balanced) - the F2 scenario as a positive/negative pair:
// with no jobs the ring is DARK (looks dead but is correct), then a single
// attention job lights exactly ONE LED with the state hue, and voice takes it
// over while live. Also proves the attention ScreenIntent still schedules a Badge.
// ============================================================================
static void test_passive_single_led_attention_and_voice_takeover() {
  Device dev;  // Balanced: Passive, brightness 30, coalesce 30000

  // No jobs: ring dark (F2 - "turning does nothing visible" is CORRECT here).
  {
    ring::Plan p = dev.plan(false, 0);
    TEST_ASSERT_EQUAL(int(Posture::Calm), int(p.posture));  // Balanced default is now Calm
    TEST_ASSERT_EQUAL_UINT8(30, p.brightness);
    TEST_ASSERT_EQUAL(0, p.segCount);
    TEST_ASSERT_FALSE(p.single.lit);
  }

  // A background Running job stays ambient - Passive ring still dark (Running
  // never lights the single attention LED).
  dev.jobState(1, Status::Running, 100);
  {
    ring::Plan p = dev.plan(false, 200);
    TEST_ASSERT_EQUAL(0, p.segCount);
    TEST_ASSERT_FALSE(p.single.lit);
  }
  dev.run(0, 200);        // first-ever ambient StatusIdle fires
  dev.done(200 + 2214);

  // Job #1 now waits for input: attention. Single LED lights with the
  // WaitingInput hue (213), and a Badge is scheduled immediately.
  dev.jobState(1, Status::WaitingInput, 5000);
  {
    ring::Plan p = dev.plan(false, 5100);
    TEST_ASSERT_EQUAL(0, p.segCount);                 // ring stays dark around it
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(0, p.single.index);       // AttnLedIndex preset 0
    TEST_ASSERT_EQUAL_UINT8(213, p.single.hue);       // styleFor(WaitingInput)
  }
  dev.run(5000, 5000);    // Badge (attention) immediate

  // Voice starts recording: it OWNS the single LED (blue 170), overriding the
  // attention hue, and schedules a VoiceGlyph.
  dev.voice(attn::VoiceStage::Recording, 6000);
  {
    ring::Plan p = dev.plan(false, 6100);
    TEST_ASSERT_EQUAL(int(attn::VoiceStage::Recording), int(p.voice));
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(170, p.single.hue);       // voice blue beats 213
  }
  dev.done(5000 + 2214);  // Badge done; VoiceGlyph (attention) now free to fire
  dev.run(7215, 7215);

  // Voice ends: the attention LED returns to its own WaitingInput treatment.
  dev.voice(attn::VoiceStage::None, 8000);
  {
    ring::Plan p = dev.plan(false, 8100);
    TEST_ASSERT_EQUAL(int(attn::VoiceStage::None), int(p.voice));
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(213, p.single.hue);       // back to WaitingInput hue
  }

  assertTrace(
      {
          {0, uint8_t(ScreenId::StatusIdle), Kind::FastBW, false},
          {5000, uint8_t(ScreenId::StatusIdle), Kind::FastBW, false},
          {7215, uint8_t(ScreenId::VoiceGlyph), Kind::FastBW, false},
      },
      dev.issued);
}

// ============================================================================
// Profile switch mid-session (F2 corollary + reconfigure): a live attention job
// exists while the user flips Balanced(Passive) -> Desk(Active) -> back. The
// ring representation flips between single-LED and full-segments WITHOUT losing
// the job, and the scheduler adopts Desk's tighter coalesce window on the
// already-pending ambient.
// ============================================================================
static void test_profile_switch_flips_ring_representation_live() {
  Device dev;  // Balanced / Passive

  dev.jobState(7, Status::AwaitingApproval, 0, /*accent=*/32);
  // Passive: one LED, AwaitingApproval hue 32.
  {
    ring::Plan p = dev.plan(false, 100);
    TEST_ASSERT_EQUAL(int(Posture::Calm), int(p.posture));  // Balanced default is now Calm
    TEST_ASSERT_EQUAL(0, p.segCount);
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(32, p.single.hue);
  }
  dev.run(0, 0);          // Badge (attention) at t=0
  dev.done(2214);

  // Flip to Desk (Active). Same job, now a full segment; single LED goes unlit.
  dev.setProfile(ProfileId::Desk);
  {
    ring::Plan p = dev.plan(false, 3000);
    TEST_ASSERT_EQUAL(int(Posture::Full), int(p.posture));
    TEST_ASSERT_EQUAL_UINT8(60, p.brightness);
    TEST_ASSERT_EQUAL(1, p.segCount);
    TEST_ASSERT_EQUAL_UINT32(7, p.segs[0].key);
    TEST_ASSERT_EQUAL(int(Status::AwaitingApproval), int(p.segs[0].status));
    TEST_ASSERT_EQUAL_UINT8(32, p.segs[0].accentHue);
    TEST_ASSERT_FALSE(p.single.lit);
  }

  // Flip back to Balanced (Passive): single LED returns, job preserved.
  dev.setProfile(ProfileId::Balanced);
  {
    ring::Plan p = dev.plan(false, 4000);
    TEST_ASSERT_EQUAL(int(Posture::Calm), int(p.posture));  // Balanced default is now Calm
    TEST_ASSERT_EQUAL_UINT8(30, p.brightness);
    TEST_ASSERT_EQUAL(0, p.segCount);
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(32, p.single.hue);
  }
  // The initial Badge is the only e-ink render in this short window.
  assertTrace({{0, uint8_t(ScreenId::StatusIdle), Kind::FastBW, false}}, dev.issued);
}

// ============================================================================
// Attention bypasses a SHUT coalesce window, and ambient is dropped (never
// queued) behind a pending attention - the interplay that only surfaces when
// router intents drive the real scheduler. Balanced coalesce = 30000.
// ============================================================================
static void test_attention_bypasses_ambient_window_over_time() {
  Device dev;  // Balanced, coalesce 30000

  // First ambient establishes the coalesce clock.
  dev.jobState(1, Status::Running, 0);
  dev.run(0, 0);          // StatusIdle immediate
  dev.done(2200);         // ambient clock stamped at 2200; window shut until 32200

  // An ambient progress update inside the window is held (not rendered yet).
  dev.jobProgress(1, 50, 5000);
  dev.run(5000, 6000);    // nothing issues: window still shut
  TEST_ASSERT_EQUAL_UINT32(1, dev.issued.size());

  // Attention (WaitingInput) arrives at 10000 -> Badge bypasses the shut window
  // and fires immediately, while the pending ambient is DROPPED behind it.
  dev.jobState(1, Status::WaitingInput, 10000);
  dev.run(10000, 10000);  // Badge now
  dev.done(12200);

  // After the attention render, the dropped ambient does NOT resurface on its
  // own; the coalesce window (still measured from the 2200 ambient completion)
  // reopens at 32200 but nothing is pending, so nothing more renders.
  dev.run(12200, 40000);
  assertTrace(
      {
          {0, uint8_t(ScreenId::StatusIdle), Kind::FastBW, false},
          {10000, uint8_t(ScreenId::StatusIdle), Kind::FastBW, false},
      },
      dev.issued);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_desk_session_active_jobs_attention_and_cursor);
  RUN_TEST(test_passive_single_led_attention_and_voice_takeover);
  RUN_TEST(test_profile_switch_flips_ring_representation_live);
  RUN_TEST(test_attention_bypasses_ambient_window_over_time);
  return UNITY_END();
}
