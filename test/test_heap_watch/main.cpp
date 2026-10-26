#include <unity.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "nimbus/heap_watch.h"

using namespace nimbus::heapwatch;

// ---- counting allocator shim -------------------------------------------------
// Every operator new in this test binary is counted, so a test can prove a code path
// allocated nothing. The failed-allocation hook runs inside the heap's own failure
// path, on any task or in an interrupt, so its push must never allocate.
static std::atomic<long> g_news{0};
void* operator new(std::size_t n) {
  g_news.fetch_add(1);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) {
  g_news.fetch_add(1);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void setUp() {}
void tearDown() {}

// ---- ring ---------------------------------------------------------------------

static void test_ring_reset_is_valid_and_empty() {
  FailRing r;
  std::memset(&r, 0xA5, sizeof(r));   // power-on garbage
  TEST_ASSERT_FALSE(ringValid(r));
  ringReset(r);
  TEST_ASSERT_TRUE(ringValid(r));
  TEST_ASSERT_EQUAL_UINT32(0, r.total);
  FailRecord out[kRingSize];
  uint32_t lost = 99;
  TEST_ASSERT_EQUAL(0, (int)ringSince(r, 0, out, lost));
  TEST_ASSERT_EQUAL_UINT32(0, lost);
}

// Garbage that happens to carry the magic still fails the CRC, and any single bit
// flip in a valid ring is caught.
static void test_ring_rejects_garbage_and_bit_flips() {
  FailRing r;
  ringReset(r);
  ringPush(r, {1000, 1600, (1u << 3) | (1u << 11), 15204, 7156}, "tg_poll");
  TEST_ASSERT_TRUE(ringValid(r));
  uint8_t* p = reinterpret_cast<uint8_t*>(&r);
  for (size_t byte = 0; byte < sizeof(FailRing); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      p[byte] ^= uint8_t(1u << bit);
      TEST_ASSERT_FALSE_MESSAGE(ringValid(r), "a flipped bit must invalidate the ring");
      p[byte] ^= uint8_t(1u << bit);
    }
  }
  TEST_ASSERT_TRUE(ringValid(r));
  FailRing g;
  std::memset(&g, 0x3C, sizeof(g));
  g.magic = kRingMagic;   // right magic, wrong CRC
  TEST_ASSERT_FALSE(ringValid(g));
}

// The ring keeps the newest kRingSize records, oldest first, and counts the ones it
// has overwritten, for every number of pushes and every drain point.
static void test_ring_wraps_and_drains_newest() {
  for (uint32_t pushes = 0; pushes <= 3 * kRingSize; ++pushes) {
    for (uint32_t since = 0; since <= pushes; ++since) {
      FailRing r;
      ringReset(r);
      for (uint32_t i = 0; i < pushes; ++i) ringPush(r, {i * 10, 100 + i, 0, 0, 0}, "t");
      TEST_ASSERT_TRUE(ringValid(r));
      FailRecord out[kRingSize];
      uint32_t lost = 0;
      const size_t n = ringSince(r, since, out, lost);
      const uint32_t fresh = pushes - since;
      const uint32_t want = fresh < kRingSize ? fresh : (uint32_t)kRingSize;
      TEST_ASSERT_EQUAL_UINT32(want, (uint32_t)n);
      TEST_ASSERT_EQUAL_UINT32(fresh - want, lost);
      for (size_t k = 0; k < n; ++k)   // oldest first, ending at the newest push
        TEST_ASSERT_EQUAL_UINT32(100 + (pushes - n + k), out[k].size);
    }
  }
}

static void test_ring_task_name_is_bounded() {
  FailRing r;
  ringReset(r);
  ringPush(r, {0, 1, 0, 0, 0}, "a_very_long_task_name");
  TEST_ASSERT_EQUAL_STRING("a_very_", r.rec[0].task);
  ringPush(r, {0, 1, 0, 0, 0}, nullptr);
  TEST_ASSERT_EQUAL_STRING("", r.rec[1].task);
  ringPush(r, {0, 1, 0, 0, 0}, "isr");
  TEST_ASSERT_EQUAL_STRING("isr", r.rec[2].task);
}

// The class guard for the hook: pushing (and validating) allocates nothing, for any
// number of pushes, including the wrap.
static void test_push_path_never_allocates() {
  static FailRing r;   // static: no allocation for the ring itself either
  ringReset(r);
  const long before = g_news.load();
  for (int i = 0; i < 1000; ++i) {
    ringPush(r, {(uint32_t)i, (uint32_t)i * 3, 0x808, 1234, 567}, (i & 1) ? "tg_poll" : "isr");
    (void)ringValid(r);
    (void)ringCrc(r);
  }
  ringReset(r);
  TEST_ASSERT_EQUAL_MESSAGE(before, g_news.load(), "the failed-allocation push path allocated");
  // Counter-test: the shim does count (a std::string over the SSO limit allocates).
  std::string s(100, 'x');
  TEST_ASSERT_TRUE(g_news.load() > before);
  (void)s;
}

static void test_fail_line_names_size_caps_task() {
  FailRing r;
  ringReset(r);
  ringPush(r, {42660000, 1600, (1u << 3) | (1u << 11) | (1u << 2), 15204, 7156}, "tg_poll");
  const std::string line = failLine(r.rec[0]);
  TEST_ASSERT_EQUAL_STRING(
      "heap: alloc FAILED size=1600 caps=DMA|INTERNAL|8BIT task=tg_poll free=15204 largest=7156 "
      "up=42660s",
      line.c_str());
  TEST_ASSERT_EQUAL_STRING("0", capsString(0).c_str());
  // A garbage task name (an RTC record from an unknown state) still prints safely.
  FailRecord e{};
  e.task[0] = 1;
  e.task[1] = ' ';
  TEST_ASSERT_NOT_NULL(std::strstr(failLine(e).c_str(), "task=??"));
}

// ---- ladder -------------------------------------------------------------------

static void test_ladder_logs_each_kilobyte_step_below_the_ceiling() {
  Ladder l;
  // Boot walks the minimum down from 200 KB: no lines above the ceiling.
  for (uint32_t m = 200000; m >= kLadderLogBelow; m -= 4096)
    TEST_ASSERT_FALSE(ladderNote(l, m, 0, 0).log);
  LadderStep s = ladderNote(l, 20000, 1u << kRelayBulk, 5000);
  TEST_ASSERT_TRUE(s.log);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.was);
  TEST_ASSERT_FALSE(ladderNote(l, 19500, 0, 6000).log);   // under 1 KB down: no line
  TEST_ASSERT_EQUAL_UINT32(19500, l.low);                 // ...but the mark moved
  s = ladderNote(l, 18976, 1u << kTurn, 7000);            // exactly 1 KB below 20000
  TEST_ASSERT_TRUE(s.log);
  TEST_ASSERT_EQUAL_UINT32(20000, s.was);
  TEST_ASSERT_FALSE(ladderNote(l, 25000, 0, 8000).newLow);   // a monitor window reads higher
  TEST_ASSERT_EQUAL_UINT32(18976, l.low);
  const std::string line = ladderLine(l, s.was, 21000, 9716);
  TEST_ASSERT_EQUAL_STRING(
      "heap: new low-water 18976 (was 20000) free=21000 largest=9716 tags=turn up=7s",
      line.c_str());
}

// Property over random minimum streams (a monitor window can read higher than the
// all-time minimum, so the stream is not monotonic): the logged marks are strictly
// decreasing in steps of at least 1 KB, every one is below the ceiling, the ladder's
// mark always equals the true minimum so far, and no qualifying low is missed: after
// every sample, the true minimum is within 1 KB of the last logged mark (or above
// the ceiling with nothing logged).
static void test_ladder_property_strictly_decreasing_none_missed() {
  std::mt19937 rng(0xC0FFEE);
  for (int run = 0; run < 400; ++run) {
    Ladder l;
    uint32_t trueMin = UINT32_MAX;
    std::vector<uint32_t> marks;
    uint32_t v = 60000;
    const int samples = 50 + (int)(rng() % 400);
    for (int i = 0; i < samples; ++i) {
      const int32_t step = (int32_t)(rng() % 5000) - 2600;   // drifts down, with spikes up
      v = (uint32_t)std::max<int32_t>(0, std::min<int32_t>(80000, (int32_t)v + step));
      if (v < trueMin) trueMin = v;
      const LadderStep s = ladderNote(l, v, rng(), (uint32_t)i * 1000);
      TEST_ASSERT_EQUAL_UINT32(trueMin, l.low);
      if (s.log) {
        TEST_ASSERT_TRUE(v < kLadderLogBelow);
        if (!marks.empty()) {
          TEST_ASSERT_EQUAL_UINT32(marks.back(), s.was);
          TEST_ASSERT_TRUE(v + kLadderStepBytes <= marks.back());
        }
        marks.push_back(v);
      }
      if (trueMin < kLadderLogBelow) {
        TEST_ASSERT_FALSE_MESSAGE(marks.empty(), "a low under the ceiling was never logged");
        TEST_ASSERT_TRUE_MESSAGE(trueMin + kLadderStepBytes > marks.back(),
                                 "a low 1 KB under the last mark was missed");
      } else {
        TEST_ASSERT_TRUE(marks.empty());
      }
    }
  }
}

// ---- activity tags ------------------------------------------------------------

static void test_activity_counts_and_sticky_bursts() {
  Activity a;
  TEST_ASSERT_EQUAL_UINT32(0, a.sample());
  a.enter(kWorker);
  a.enter(kWorker);   // two workers alive
  a.leave(kWorker);
  TEST_ASSERT_EQUAL_UINT32(1u << kWorker, a.active());
  a.leave(kWorker);
  TEST_ASSERT_EQUAL_UINT32(0, a.active());
  a.leave(kWorker);   // unmatched leave never wraps
  TEST_ASSERT_EQUAL_UINT32(0, a.active());
  // A burst between samples still counts once, then is gone.
  a.enter(kRelayBulk);
  a.leave(kRelayBulk);
  a.touch(kLanBig);
  TEST_ASSERT_EQUAL_UINT32((1u << kRelayBulk) | (1u << kLanBig) | (1u << kWorker), a.sample());
  TEST_ASSERT_EQUAL_UINT32(0, a.sample());
  // A live tag stays in every sample.
  a.enter(kVision);
  TEST_ASSERT_EQUAL_UINT32(1u << kVision, a.sample());
  TEST_ASSERT_EQUAL_UINT32(1u << kVision, a.sample());
  a.leave(kVision);
  // It was live during this interval, so the next sample still names it; then gone.
  TEST_ASSERT_EQUAL_UINT32(1u << kVision, a.sample());
  TEST_ASSERT_EQUAL_UINT32(0, a.sample());
}

static void test_every_tag_has_a_distinct_printable_name() {
  for (uint8_t t = 0; t < kTagCount; ++t) {
    const std::string n = tagName(Tag(t));
    TEST_ASSERT_TRUE(n.size() > 1 && n != "?");
    TEST_ASSERT_EQUAL(std::string::npos, n.find_first_of("| =,"));
    for (uint8_t u = 0; u < t; ++u) TEST_ASSERT_NOT_EQUAL(0, n.compare(tagName(Tag(u))));
  }
  TEST_ASSERT_EQUAL_STRING("-", tagsString(0).c_str());
  TEST_ASSERT_EQUAL_STRING("turn|relay.bulk",
                           tagsString((1u << kTurn) | (1u << kRelayBulk)).c_str());
}

// ---- rate limit ---------------------------------------------------------------

static void test_line_budget_caps_per_minute_and_counts_drops() {
  LineBudget b;
  int ok = 0;
  for (int i = 0; i < 20; ++i) ok += b.take(1000 + (uint32_t)i) ? 1 : 0;
  TEST_ASSERT_EQUAL(LineBudget::kPerWindow, ok);
  TEST_ASSERT_EQUAL_UINT32(20 - LineBudget::kPerWindow, b.takeDropped());
  TEST_ASSERT_EQUAL_UINT32(0, b.takeDropped());
  TEST_ASSERT_TRUE(b.take(1000 + LineBudget::kWindowMs));   // a new window
  // Wraps with millis(): a window straddling the 32-bit wrap still resets.
  LineBudget w;
  for (int i = 0; i < 10; ++i) w.take(0xFFFFFF00u);
  TEST_ASSERT_TRUE(w.take(0xFFFFFF00u + LineBudget::kWindowMs));
}

// ---- resting largest ----------------------------------------------------------

static void test_resting_largest_is_min_of_recent_idle_samples() {
  RestingLargest r;
  TEST_ASSERT_EQUAL_UINT32(7156, r.resting(7156));   // no idle sample yet: the fallback
  r.note(15860);
  TEST_ASSERT_EQUAL_UINT32(15860, r.resting(7156));  // the transient fallback is ignored
  r.note(15348);
  r.note(16372);
  r.note(15860);
  TEST_ASSERT_EQUAL_UINT32(15348, r.resting(0));
  r.note(16372);   // pushes out the oldest (15860); the 15348 is still in the window
  TEST_ASSERT_EQUAL_UINT32(15348, r.resting(0));
  r.note(16372);   // now the 15348 ages out
  TEST_ASSERT_EQUAL_UINT32(15860, r.resting(0));
}

// ---- spawn gate ---------------------------------------------------------------

// The table the four worker sites share (pverify 8 KB, cloudmint 8 KB, voices 6 KB,
// safetyrep 8 KB): never during a turn; otherwise only with the stack plus 16 KB free.
static void test_spawn_gate_table() {
  const uint32_t stacks[] = {8192, 8192, 6144, 8192};
  for (uint32_t st : stacks) {
    for (uint32_t freeB = 0; freeB < 64 * 1024; freeB += 512) {
      for (int turn = 0; turn < 2; ++turn) {
        const SpawnVerdict v = spawnVerdict(st, freeB, turn != 0);
        if (turn) TEST_ASSERT_EQUAL(SpawnVerdict::TurnInFlight, v);
        else if (freeB >= st + kSpawnMarginBytes) TEST_ASSERT_EQUAL(SpawnVerdict::Ok, v);
        else TEST_ASSERT_EQUAL(SpawnVerdict::LowMemory, v);
      }
    }
  }
  // The field case: pverify spawned at 22 KB free on a turn, carving its 8 KB stack.
  TEST_ASSERT_EQUAL(SpawnVerdict::LowMemory, spawnVerdict(8192, 22152, false));
  TEST_ASSERT_EQUAL(SpawnVerdict::Ok, spawnVerdict(8192, 8192 + 16384, false));
  // No wrap on a garbage stack size.
  TEST_ASSERT_EQUAL(SpawnVerdict::LowMemory, spawnVerdict(0xFFFFFFFFu, 0xFFFFFFFFu, false));
  TEST_ASSERT_EQUAL_STRING("turn in flight", spawnVerdictName(SpawnVerdict::TurnInFlight));
}

// ---- the boot's loopTask stack floor (release-v451x bisect) --------------------

// The floor fails the states that crashed the release gate and passes a healthy
// boot: the last-good main finished with 88 B of loopTask headroom and the
// crashing build measured 56 B; a boot with room to spare sits well above 1 KB.
// Exactly at the floor passes ("drops under" the floor faults).
static void test_loop_stack_floor_catches_the_eroded_boots() {
  TEST_ASSERT_TRUE(loopStackUnderFloor(0));
  TEST_ASSERT_TRUE(loopStackUnderFloor(56));    // the crashing build's journal segment
  TEST_ASSERT_TRUE(loopStackUnderFloor(88));    // the last-good main's whole boot
  TEST_ASSERT_TRUE(loopStackUnderFloor(kLoopStackFloorBytes - 1));
  TEST_ASSERT_FALSE(loopStackUnderFloor(kLoopStackFloorBytes));
  TEST_ASSERT_FALSE(loopStackUnderFloor(2108));   // the measured free at orch: begin
  TEST_ASSERT_FALSE(loopStackUnderFloor(UINT32_MAX));
}

// The boot ladder's one line, both ways: the bench leg greps the healthy form,
// and the fault form names the floor and the boot path, LOUD.
static void test_loop_stack_floor_line_healthy_and_fault() {
  const std::string ok = loopStackFloorLine(2890, true);
  TEST_ASSERT_TRUE(ok.find("boot stack: loopTask min free 2890 B, floor 1024 B - ok") != std::string::npos);
  const std::string fault = loopStackFloorLine(88, true);
  TEST_ASSERT_TRUE(fault.find("BOOT FAULT") != std::string::npos);
  TEST_ASSERT_TRUE(fault.find("88 B is under the 1024 B floor") != std::string::npos);
  TEST_ASSERT_TRUE(fault.find("grew past the loopTask stack") != std::string::npos);
  // A missing task handle reads as a fault too, never a clean pass.
  const std::string noTask = loopStackFloorLine(4096, false);
  TEST_ASSERT_TRUE(noTask.find("BOOT FAULT") != std::string::npos);
  TEST_ASSERT_TRUE(noTask.find("no task handle") != std::string::npos);
}

// A 12 KB stack under the gate the four worker sites share: granted at boot-like
// free (plenty, no turn), refused under the 16 KB margin, and always refused once a
// turn is in flight. (The orchestrator's boot task asks the boot form, below.)
static void test_spawn_gate_answers_the_boot_task_shape() {
  TEST_ASSERT_EQUAL(SpawnVerdict::Ok, spawnVerdict(12288, 80 * 1024, false));
  TEST_ASSERT_EQUAL(SpawnVerdict::LowMemory, spawnVerdict(12288, 12288 + 16384 - 1, false));
  TEST_ASSERT_EQUAL(SpawnVerdict::Ok, spawnVerdict(12288, 12288 + 16384, false));
  TEST_ASSERT_EQUAL(SpawnVerdict::TurnInFlight, spawnVerdict(12288, 80 * 1024, true));
}

// The boot task asks before any turn exists, so it needs its stack plus the boot
// margin only (never "turn in flight"), with no wrap on a garbage size.
static void test_boot_spawn_verdict_needs_the_stack_plus_the_boot_margin() {
  for (uint32_t freeB = 0; freeB < 64 * 1024; freeB += 256) {
    const SpawnVerdict v = spawnVerdictAtBoot(12288, freeB);
    if (freeB >= 12288 + kBootSpawnMarginBytes) TEST_ASSERT_EQUAL(SpawnVerdict::Ok, v);
    else TEST_ASSERT_EQUAL(SpawnVerdict::LowMemory, v);
  }
  TEST_ASSERT_EQUAL(SpawnVerdict::LowMemory, spawnVerdictAtBoot(0xFFFFFFFFu, 0xFFFFFFFFu));
  // The margin carries the Telegram poll task's 24 KB stack the chain creates next.
  TEST_ASSERT_TRUE(kBootSpawnMarginBytes >= 24u * 1024u);
}

// The fallback when the boot task cannot start, over every case: a trial image always
// rolls back (whatever the count or reset reason: waiting would keep the update's
// guard from ever counting it out); a crash boot never restarts (the restart would hide
// the crash notice); otherwise restart up to the cap, then boot on degraded - never a
// restart loop, never a wait in place. Power-on garbage in the RTC record reads 0.
static void test_orchboot_fallback_over_every_case() {
  using F = OrchBootFallback;
  TEST_ASSERT_EQUAL_UINT32(0, orchBootRestartsInARow(0xDEADBEEFu, 1));   // power-on garbage
  TEST_ASSERT_EQUAL_UINT32(0, orchBootRestartsInARow(0, 0));
  TEST_ASSERT_EQUAL_UINT32(1, orchBootRestartsInARow(kOrchBootRestartMagic, 1));
  for (uint32_t n = 0; n < 6; ++n) {
    for (int crash = 0; crash < 2; ++crash) {
      TEST_ASSERT_TRUE(orchBootFallback(true, crash, n) == F::RollBack);
      const F f = orchBootFallback(false, crash, n);
      if (crash) TEST_ASSERT_TRUE(f == F::Degraded);
      else if (n < kOrchBootMaxRestarts) TEST_ASSERT_TRUE(f == F::Restart);
      else TEST_ASSERT_TRUE(f == F::Degraded);
    }
  }
  TEST_ASSERT_TRUE(orchBootFallback(false, false, 0xFFFFFFFFu) == F::Degraded);   // a corrupt count
  TEST_ASSERT_TRUE(kOrchBootMaxRestarts >= 1 && kOrchBootMaxRestarts <= 3);   // a cap, never a loop
  TEST_ASSERT_EQUAL_STRING("restarting", orchBootFallbackName(F::Restart));
}

// The orchboot floor: the chain's own watermark gets the same 1 KB floor, both lines
// loud, and a task that never reported reads as a fault, never a pass.
static void test_orchboot_stack_floor_line_healthy_fault_and_unmeasured() {
  TEST_ASSERT_TRUE(orchBootStackUnderFloor(0));
  TEST_ASSERT_TRUE(orchBootStackUnderFloor(kOrchBootStackFloorBytes - 1));
  TEST_ASSERT_FALSE(orchBootStackUnderFloor(kOrchBootStackFloorBytes));
  const std::string ok = orchBootStackFloorLine(4210, 12288, true);
  TEST_ASSERT_TRUE(ok.find("boot stack: orchboot min free 4210 B of 12288 B, floor 1024 B - ok") != std::string::npos);
  const std::string fault = orchBootStackFloorLine(600, 12288, true);
  TEST_ASSERT_TRUE(fault.find("BOOT FAULT") != std::string::npos);
  TEST_ASSERT_TRUE(fault.find("600 B of 12288 B is under the 1024 B floor") != std::string::npos);
  const std::string none = orchBootStackFloorLine(0, 0, false);
  TEST_ASSERT_TRUE(none.find("BOOT FAULT") != std::string::npos);
  TEST_ASSERT_TRUE(none.find("no stack watermark") != std::string::npos);
}

// The device seams: the boot task reports its watermark, the floor check reads it,
// and the chain is never run inline on loopTask (the 88 B crash path).
static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static void test_orchboot_reports_its_watermark_and_never_runs_inline() {
  const std::string main = readFile("src/main.cpp");
  const size_t a = main.find("static void orchBootTask(void* pv)");
  const size_t b = main.find("// Live device state for the self-test engine");
  TEST_ASSERT_TRUE(a != std::string::npos && b != std::string::npos && a < b);
  const std::string region = main.substr(a, b - a);
  TEST_ASSERT_TRUE(region.find("noteOrchBootStack(") != std::string::npos);
  TEST_ASSERT_TRUE(region.find("spawnGate(kOrchBootStackBytes, who, /*atBoot=*/true)") != std::string::npos);
  // orchestratorBegin() runs only inside the boot task: one call in the WHOLE file
  // (setup() calls orchestratorBeginOffLoopTask), and it is in that task's body.
  size_t calls = 0;
  for (size_t p = main.find("orchestratorBegin();"); p != std::string::npos;
       p = main.find("orchestratorBegin();", p + 1))
    ++calls;
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)calls, "the orchestrator boot chain must never run inline on loopTask");
  TEST_ASSERT_TRUE(region.find("orchestratorBegin();") != std::string::npos);
  // The honest refusal: the policy decides, a trial image rolls back, a restart is
  // counted in RTC memory, and the last resort boots on degraded (loop() alive), never
  // a wait in place.
  TEST_ASSERT_TRUE(region.find("heapwatch::orchBootFallback(!otaupd::imageValidAtBoot(), crashBoot, inARow)") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(region.find("otaupd::rollbackNow()") != std::string::npos);
  TEST_ASSERT_TRUE(region.find("esp_restart()") != std::string::npos);
  TEST_ASSERT_TRUE(main.find("RTC_NOINIT_ATTR static OrchBootRestarts") != std::string::npos);
  TEST_ASSERT_TRUE(region.find("g_orchBootDegraded = true") != std::string::npos);
  TEST_ASSERT_TRUE(region.find("orchBootWait(kOrchBootRetryMs, kOrchBootWaitMs)") != std::string::npos);
  TEST_ASSERT_TRUE(main.find("kOrchBootHoldRetryMs") == std::string::npos);   // no wait in place
  // The degraded boot is visible: the screen notice, the web-chat row from loop(), the
  // state flag.
  TEST_ASSERT_TRUE(main.find("g_askOverride = kOrchBootDegradedScreen;") != std::string::npos);
  TEST_ASSERT_TRUE(main.find("if (g_orchMode) orchBootDegradedTick();") != std::string::npos);
  const std::string hw = readFile("src/sys/heap_watch.cpp");
  TEST_ASSERT_TRUE(hw.find("mem[\"orchBootDegraded\"]") != std::string::npos);
  TEST_ASSERT_TRUE(hw.find("orchBootStackFloorLine(") != std::string::npos);
  TEST_ASSERT_TRUE(hw.find("mem[\"orchBootStackMin\"]") != std::string::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ring_reset_is_valid_and_empty);
  RUN_TEST(test_ring_rejects_garbage_and_bit_flips);
  RUN_TEST(test_ring_wraps_and_drains_newest);
  RUN_TEST(test_ring_task_name_is_bounded);
  RUN_TEST(test_push_path_never_allocates);
  RUN_TEST(test_fail_line_names_size_caps_task);
  RUN_TEST(test_ladder_logs_each_kilobyte_step_below_the_ceiling);
  RUN_TEST(test_ladder_property_strictly_decreasing_none_missed);
  RUN_TEST(test_activity_counts_and_sticky_bursts);
  RUN_TEST(test_every_tag_has_a_distinct_printable_name);
  RUN_TEST(test_line_budget_caps_per_minute_and_counts_drops);
  RUN_TEST(test_resting_largest_is_min_of_recent_idle_samples);
  RUN_TEST(test_spawn_gate_table);
  RUN_TEST(test_loop_stack_floor_catches_the_eroded_boots);
  RUN_TEST(test_loop_stack_floor_line_healthy_and_fault);
  RUN_TEST(test_spawn_gate_answers_the_boot_task_shape);
  RUN_TEST(test_boot_spawn_verdict_needs_the_stack_plus_the_boot_margin);
  RUN_TEST(test_orchboot_fallback_over_every_case);
  RUN_TEST(test_orchboot_stack_floor_line_healthy_fault_and_unmeasured);
  RUN_TEST(test_orchboot_reports_its_watermark_and_never_runs_inline);
  return UNITY_END();
}
