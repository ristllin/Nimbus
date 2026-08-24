#include <unity.h>

#include <cstdio>
#include <vector>

#include "nimbus/render_sched.h"

// Trace-driven tests for the render scheduler: each test scripts a sequence of
// (time, action) steps - detents, intents, render completions, reconfigures,
// and 1 ms-granular tick sweeps - and asserts the EXACT sequence of issued
// RenderCommands (time, screen, kind, fullClear). Anything issued that the
// expectation doesn't list, or issued at the wrong millisecond, fails.

using namespace nimbus::render;

void setUp() {}
void tearDown() {}

namespace {

struct Issued {
  uint32_t atMs;
  uint8_t screen;
  Kind kind;
  bool fullClear;
};

struct Trace {
  Scheduler sched;
  std::vector<Issued> issued;

  explicit Trace(const SchedConfig& cfg) { sched.configure(cfg); }

  void tick(uint32_t t) {
    RenderCommand c = sched.tick(t);
    if (c.render) issued.push_back({t, c.screenId, c.kind, c.fullClear});
  }
  // Tick once per ms over [fromMs, toMs] inclusive. Wrap-tolerant: fromMs may
  // be numerically greater than toMs across the uint32 boundary.
  void run(uint32_t fromMs, uint32_t toMs) {
    for (uint32_t t = fromMs;; ++t) {
      tick(t);
      if (t == toMs) break;
    }
  }
  void detent(uint32_t t, uint8_t screen) { sched.onDetent(screen, t); }
  void intent(uint32_t t, uint8_t screen, bool attention,
              Kind k = Kind::FastBW) {
    sched.onIntent(screen, attention, t, k);
  }
  void done(uint32_t t) { sched.onRenderDone(t); }
  void reconfig(const SchedConfig& cfg) { sched.configure(cfg); }
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
    TEST_ASSERT_EQUAL_INT_MESSAGE(int(expected[i].kind), int(actual[i].kind),
                                  msg);
    std::snprintf(msg, sizeof msg, "cmd[%u] fullClear", unsigned(i));
    TEST_ASSERT_EQUAL_INT_MESSAGE(int(expected[i].fullClear),
                                  int(actual[i].fullClear), msg);
  }
}

constexpr SchedConfig kCfg{/*dwellMs=*/300, /*coalesceMs=*/30000,
                           /*fullEveryN=*/6};

}  // namespace

// Three detents 100 ms apart: no render per detent, exactly one render at
// lastDetent + dwellMs (500), nothing after completion.
static void test_dwell_settle_three_detents() {
  Trace tr(kCfg);
  tr.detent(0, 7);
  tr.run(0, 99);
  tr.detent(100, 7);
  tr.run(100, 199);
  tr.detent(200, 7);
  tr.run(200, 600);  // 499 must stay silent; 500 fires
  TEST_ASSERT_TRUE(tr.sched.panelBusy());
  tr.done(2700);
  TEST_ASSERT_FALSE(tr.sched.panelBusy());
  tr.run(2700, 3500);
  assertTrace({{500, 7, Kind::FastBW, false}}, tr.issued);
}

// Detents during a busy panel re-arm the dwell; the render fires only after
// onRenderDone (dwell was due at 900 but the panel was busy until 2500).
static void test_detent_during_busy_rearms_and_fires_after_done() {
  Trace tr(kCfg);
  tr.detent(0, 3);
  tr.run(0, 300);  // dwell render at 300, panel busy
  tr.detent(400, 3);
  tr.detent(600, 3);  // re-arm again: last detent wins
  tr.run(601, 2499);  // busy: nothing may issue
  tr.done(2500);
  tr.run(2500, 2600);  // 2500-600 >= 300: fires at first free tick
  tr.done(4800);
  tr.run(4800, 5200);
  assertTrace({{300, 3, Kind::FastBW, false}, {2500, 3, Kind::FastBW, false}},
              tr.issued);
}

// Attention renders immediately inside the coalesce window, and its completion
// must NOT stamp the ambient clock: the next ambient still fires at
// firstAmbientDone + coalesceMs (32200), not attentionDone + coalesceMs.
static void test_attention_bypasses_coalesce() {
  Trace tr(kCfg);
  tr.intent(0, 1, false);
  tr.run(0, 0);  // first-ever ambient: immediate
  tr.done(2200);
  tr.intent(3000, 9, true);
  tr.run(3000, 3000);  // fires despite window being shut until 32200
  tr.done(5200);
  tr.intent(6000, 2, false);
  tr.run(6000, 32300);  // held through 32199, fires exactly at 32200
  assertTrace({{0, 1, Kind::FastBW, false},
               {3000, 9, Kind::FastBW, false},
               {32200, 2, Kind::FastBW, false}},
              tr.issued);
}

// Two ambient intents inside one coalesce window collapse to a single render
// carrying the latest screen.
static void test_two_ambients_in_window_one_render_latest_wins() {
  Trace tr(kCfg);
  tr.intent(0, 1, false);
  tr.run(0, 0);
  tr.done(2200);
  tr.intent(5000, 4, false);
  tr.run(5000, 5999);      // in-window: held
  tr.intent(6000, 5, false);  // replaces screen 4 in the single slot
  tr.run(6000, 32300);        // exactly one render, screen 5, at 32200
  tr.done(33000);
  tr.run(33000, 40000);  // slot is empty: nothing further
  assertTrace({{0, 1, Kind::FastBW, false}, {32200, 5, Kind::FastBW, false}},
              tr.issued);
}

// An ambient intent arriving while an attention intent is pending is dropped
// outright - it neither replaces the attention nor renders later.
static void test_ambient_never_clobbers_pending_attention() {
  Trace tr(kCfg);
  tr.intent(0, 1, false);
  tr.run(0, 0);  // occupy the panel so the attention stays pending
  tr.intent(100, 9, true);
  tr.intent(200, 2, false);  // dropped
  tr.run(201, 2199);         // busy: nothing
  tr.done(2200);
  tr.run(2200, 2200);  // pending attention fires
  tr.done(4400);
  tr.run(4400, 40000);  // window reopens at 32200 but nothing is pending
  assertTrace({{0, 1, Kind::FastBW, false}, {2200, 9, Kind::FastBW, false}},
              tr.issued);
}

// Before any ambient render has ever completed, an ambient intent is due
// immediately - no 30 s wait from boot.
static void test_first_ever_ambient_immediate() {
  Trace tr(kCfg);
  tr.run(0, 4);
  tr.intent(5, 8, false);
  tr.run(5, 5);
  assertTrace({{5, 8, Kind::FastBW, false}}, tr.issued);
}

// fullEveryN=3: exactly every 3rd issued FastBW/Partial command carries
// fullClear=true and resets the counter. Dwell-issued renders count too, and
// Partial can carry the upgrade.
static void test_ghosting_upgrade_every_third_issued() {
  Trace tr({300, 30000, /*fullEveryN=*/3});
  auto attn = [&](uint32_t at, uint8_t screen, Kind k) {
    tr.intent(at, screen, true, k);
    tr.tick(at);
    tr.done(at + 100);
  };
  attn(0, 1, Kind::FastBW);  // #1
  tr.detent(200, 2);
  tr.run(200, 500);  // #2: dwell render counts toward ghosting
  tr.done(600);
  attn(700, 3, Kind::Partial);   // #3: upgraded, counter resets
  attn(900, 4, Kind::Partial);   // #1 after reset
  attn(1100, 5, Kind::FastBW);   // #2
  attn(1300, 6, Kind::FastBW);   // #3: upgraded again (reset really happened)
  attn(1500, 7, Kind::FastBW);   // #1
  assertTrace({{0, 1, Kind::FastBW, false},
               {500, 2, Kind::FastBW, false},
               {700, 3, Kind::Partial, true},
               {900, 4, Kind::Partial, false},
               {1100, 5, Kind::FastBW, false},
               {1300, 6, Kind::FastBW, true},
               {1500, 7, Kind::FastBW, false}},
              tr.issued);
}

// Color renders are never upgraded and never advance the ghost counter: the
// upgrade lands on the 3rd counting (non-Color) command, not the 3rd issued.
static void test_color_neither_counts_nor_upgrades() {
  Trace tr({300, 30000, /*fullEveryN=*/3});
  auto attn = [&](uint32_t at, uint8_t screen, Kind k) {
    tr.intent(at, screen, true, k);
    tr.tick(at);
    tr.done(at + 100);
  };
  attn(0, 1, Kind::FastBW);     // count 1
  attn(200, 2, Kind::Partial);  // count 2
  attn(400, 3, Kind::Color);    // 3rd issued: exempt, no upgrade, no count
  attn(600, 4, Kind::FastBW);   // count 3: upgraded, reset
  attn(800, 5, Kind::Color);    // exempt, counter stays 0
  attn(1000, 6, Kind::FastBW);  // count 1
  attn(1200, 7, Kind::FastBW);  // count 2
  attn(1400, 8, Kind::Partial); // count 3: upgraded
  assertTrace({{0, 1, Kind::FastBW, false},
               {200, 2, Kind::Partial, false},
               {400, 3, Kind::Color, false},
               {600, 4, Kind::FastBW, true},
               {800, 5, Kind::Color, false},
               {1000, 6, Kind::FastBW, false},
               {1200, 7, Kind::FastBW, false},
               {1400, 8, Kind::Partial, true}},
              tr.issued);
}

// configure() mid-stream keeps the pending ambient and the armed dwell; the
// new windows apply to that in-flight work: dwell fires at detent + NEW
// dwellMs (4100, not 3400), the held ambient at lastAmbientDone + NEW
// coalesceMs (12200). Also shows dwell bypassing the (still shut) coalesce
// window.
static void test_reconfigure_midstream_preserves_pending_and_dwell() {
  Trace tr(kCfg);  // dwell 300, coalesce 30000
  tr.intent(0, 1, false);
  tr.run(0, 0);
  tr.done(2200);              // ambient clock stamped at 2200
  tr.intent(3000, 4, false);  // pending, held by the window
  tr.detent(3100, 7);         // armed dwell
  tr.reconfig({/*dwellMs=*/1000, /*coalesceMs=*/10000, /*fullEveryN=*/6});
  tr.run(3200, 4200);  // dwell at 3100+1000; old dwellMs would fire at 3400
  tr.done(6300);
  tr.run(6300, 12300);  // ambient at 2200+10000
  assertTrace({{0, 1, Kind::FastBW, false},
               {4100, 7, Kind::FastBW, false},
               {12200, 4, Kind::FastBW, false}},
              tr.issued);
}

// Timer math must survive the uint32 millisecond wrap (subtraction form):
// a dwell armed 200 ms before the wrap fires 100 ms after it, and a coalesce
// window stamped before the wrap opens on time after it.
static void test_uint32_wrap_safe_timing() {
  Trace tr(kCfg);
  tr.intent(4294950000u, 1, false);
  tr.run(4294950000u, 4294950000u);
  tr.done(4294960000u);  // ambient clock stamped 7296 ms before wrap
  tr.intent(4294965000u, 5, false);         // pending, window still shut
  tr.run(4294965000u, 4294967095u);         // held
  tr.detent(4294967096u, 6);                // 200 ms before wrap
  // dwell: (100 - 4294967096) mod 2^32 = 300 → fires at 100
  tr.run(4294967096u, 2299u);               // sweeps across the wrap
  tr.done(2300u);
  // ambient: (22704 - 4294960000) mod 2^32 = 30000 → fires at 22704
  tr.run(2300u, 23000u);
  assertTrace({{4294950000u, 1, Kind::FastBW, false},
               {100u, 6, Kind::FastBW, false},
               {22704u, 5, Kind::FastBW, false}},
              tr.issued);
}

// A detent stamped slightly AFTER the tick's nowMs (ISR-timestamped event
// drained by a loop that sampled millis() earlier) must NOT underflow and fire
// instantly: it reads as 0 ms elapsed and keeps waiting until dwellMs of real
// silence.
static void test_dwell_out_of_order_timestamp_does_not_fire_early() {
  Trace tr(kCfg);
  tr.detent(5001, 3);
  tr.tick(5000);              // now is 1 ms BEFORE the detent: must stay silent
  TEST_ASSERT_FALSE(tr.sched.panelBusy());
  tr.run(5001, 5300);        // 5001 + 300 = 5301 is the earliest legit fire
  TEST_ASSERT_TRUE(tr.issued.empty());
  tr.run(5301, 5301);
  assertTrace({{5301, 3, Kind::FastBW, false}}, tr.issued);
}


// ---- F28: clearPendingAmbient drops a queued ambient, keeps attention --------
static void test_clear_pending_ambient_drops_only_ambient() {
  SchedConfig cfg; cfg.dwellMs = 100; cfg.coalesceMs = 100;
  Trace t(cfg);
  // First ambient renders immediately (ambientEverDone_ false) + settles.
  t.intent(0, 1, /*attention*/false); t.run(0, 5); t.done(5);
  size_t base = t.issued.size();
  // A NEW ambient queues (coalesce window) but hasn't rendered yet.
  t.intent(10, 7, false);
  // Saver arms -> clear the pending ambient. tick() must now render NOTHING.
  TEST_ASSERT_TRUE(t.sched.clearPendingAmbient());
  t.run(11, 300);
  TEST_ASSERT_EQUAL_INT((int)base, (int)t.issued.size());   // nothing flushed over the logo
  // A pending ATTENTION intent is NEVER dropped.
  t.intent(310, 9, /*attention*/true);
  TEST_ASSERT_FALSE(t.sched.clearPendingAmbient());          // refused
  t.run(311, 320);
  TEST_ASSERT_EQUAL_INT((int)base + 1, (int)t.issued.size()); // the CTA still painted
  TEST_ASSERT_EQUAL_UINT8(9, t.issued.back().screen);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_clear_pending_ambient_drops_only_ambient);
  RUN_TEST(test_dwell_settle_three_detents);
  RUN_TEST(test_dwell_out_of_order_timestamp_does_not_fire_early);
  RUN_TEST(test_detent_during_busy_rearms_and_fires_after_done);
  RUN_TEST(test_attention_bypasses_coalesce);
  RUN_TEST(test_two_ambients_in_window_one_render_latest_wins);
  RUN_TEST(test_ambient_never_clobbers_pending_attention);
  RUN_TEST(test_first_ever_ambient_immediate);
  RUN_TEST(test_ghosting_upgrade_every_third_issued);
  RUN_TEST(test_color_neither_counts_nor_upgrades);
  RUN_TEST(test_reconfigure_midstream_preserves_pending_and_dwell);
  RUN_TEST(test_uint32_wrap_safe_timing);
  return UNITY_END();
}
