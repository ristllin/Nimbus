// Host tests for the OTA headroom rule (lib/core nimbus/ota/ota_headroom.h, lane L31).
//
// Bench 2026-10-13: Lumi (Telegram long-poll on, cloud link online) refused every
// update check with "low-heap", also its daily one: intLargest 16372 against the
// OTA task's 16384 B stack. The fix pauses the idle Telegram poll task (24 KB stack)
// for the length of the OTA task. These pin the CLASS (AGENTS.md section 3):
//   - the gate itself at its exact boundaries, and the two measured devices;
//   - the decision over EVERY input combination: pause only when the poll runs,
//     is idle, and pausing frees enough; never while a turn or job runs on it;
//   - the spawn sequence over EVERY scripted device outcome (128 paths): at most one
//     pause and two spawns, and a paused poll is owed its restart on every path;
//   - the restart over the whole OTA lifecycle: never while the OTA task runs,
//     always once it ends (success or failure) or never started, retried until it
//     lands, and exactly once after it lands (no thrash);
//   - the restart itself never runs two pollers.

#include <unity.h>

#include <atomic>
#include <cstdint>
#include <initializer_list>

#include "nimbus/ota/ota_headroom.h"

using namespace nimbus::ota;

void setUp() {}
void tearDown() {}

static constexpr uint32_t kPollStack = 24576;   // telegram.cpp POLL_STACK_BYTES

// ---- the gate ----------------------------------------------------------------

static void test_gate_boundaries() {
  TEST_ASSERT_TRUE(fitsOtaTask(kSpawnFreeInternal, kOtaTaskStackBytes));
  TEST_ASSERT_FALSE(fitsOtaTask(kSpawnFreeInternal - 1, kOtaTaskStackBytes));
  TEST_ASSERT_FALSE(fitsOtaTask(kSpawnFreeInternal, kOtaTaskStackBytes - 1));
  TEST_ASSERT_FALSE(fitsOtaTask(0, 0));
  // The floor is the real stack, not a rounder number below it: the old 16000 floor
  // passed Lumi's 16372 and the task create then failed on the 16384 B stack.
  TEST_ASSERT_EQUAL_UINT32(16384u, kOtaTaskStackBytes);
}

static void test_measured_devices() {
  // Lumi on v4.5.10: free 45236, largest 16372, Telegram + cloud on.
  HeadroomInput lumi;
  lumi.freeInternal = 45236;
  lumi.largestInternal = 16372;
  lumi.pollRunning = true;
  lumi.pollStackBytes = kPollStack;
  TEST_ASSERT_FALSE(fitsOtaTask(lumi.freeInternal, lumi.largestInternal));
  TEST_ASSERT_EQUAL_INT((int)Headroom::PausePoll, (int)decideHeadroom(lumi));
  lumi.pollBusy = true;   // a turn in flight: never pause, the honest refusal stands
  TEST_ASSERT_EQUAL_INT((int)Headroom::LowMemory, (int)decideHeadroom(lumi));
  // nimbus-light (no Telegram, no cloud): largest 23540 fits as it is.
  HeadroomInput light;
  light.freeInternal = 60000;
  light.largestInternal = 23540;
  TEST_ASSERT_EQUAL_INT((int)Headroom::Fits, (int)decideHeadroom(light));
}

// ---- the decision, every combination -------------------------------------------

struct HeapCase {
  uint32_t freeB, largestB;
};
// Fits; largest short; free short; both short; far too short even with the pause.
static const HeapCase kHeaps[] = {
    {60000, 30000}, {45236, 16372}, {20000, 30000}, {12000, 9000}, {1000, 1000},
};

// The rule restated from the spec (not from the code): fits now -> Fits; else the
// poll runs, is idle, and pausing it frees enough -> PausePoll; else LowMemory.
static Headroom spec(const HeadroomInput& in) {
  if (!in.spawnFailed && fitsOtaTask(in.freeInternal, in.largestInternal)) return Headroom::Fits;
  const uint32_t stack = in.pollStackBytes;
  const uint32_t largestAfter = in.largestInternal > stack ? in.largestInternal : stack;
  const bool helps = stack > 0 && fitsOtaTask(in.freeInternal + stack, largestAfter);
  const bool idleRunning = in.pollRunning && !in.pollBusy;
  return (idleRunning && helps) ? Headroom::PausePoll : Headroom::LowMemory;
}

// One combination: the decision matches the spec, and the two class rules hold on
// their own. Returns whether it paused.
static bool checkCombination(const HeadroomInput& in) {
  const Headroom d = decideHeadroom(in);
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)spec(in), (int)d, headroomStr(d));
  if (in.pollBusy || !in.pollRunning) TEST_ASSERT_NOT_EQUAL((int)Headroom::PausePoll, (int)d);
  return d == Headroom::PausePoll;
}

static void test_decision_every_combination() {
  int pauses = 0;
  for (const HeapCase& hc : kHeaps)
    for (int bits = 0; bits < 8; ++bits)
      for (uint32_t stack : {0u, kPollStack}) {
        HeadroomInput in;
        in.freeInternal = hc.freeB;
        in.largestInternal = hc.largestB;
        in.pollRunning = bits & 1;
        in.pollBusy = bits & 2;
        in.spawnFailed = bits & 4;
        in.pollStackBytes = stack;
        if (checkCombination(in)) ++pauses;
      }
  TEST_ASSERT_TRUE(pauses > 0);   // the pause path is reachable at all
}

static void test_headroom_str() {
  TEST_ASSERT_EQUAL_STRING("fits", headroomStr(Headroom::Fits));
  TEST_ASSERT_EQUAL_STRING("pause-poll", headroomStr(Headroom::PausePoll));
  TEST_ASSERT_EQUAL_STRING("low-memory", headroomStr(Headroom::LowMemory));
}

// ---- the spawn sequence, every scripted path -------------------------------------

// A scripted device: what the heap reads, whether the poll runs and is busy, and
// what each seam answers. Counts every call so a test can assert the absence of a
// second pause or a third spawn.
struct FakeDevice {
  bool fitsFirst = true;      // the first measurement passes the gate
  bool pollRunning = true;
  bool pollBusy = false;
  bool pauseOk = true;        // the quiesce joined the task in time
  bool headroomOk = true;     // the freed stack came back within the wait
  bool spawn1Ok = true;
  bool spawn2Ok = true;
  int snapshots = 0, pauses = 0, waits = 0, spawns = 0;
  bool pausedNow = false;     // the poll task is paused (the model's view)
};

static HeadroomInput fakeSnapshot(void* ctx) {
  FakeDevice* d = static_cast<FakeDevice*>(ctx);
  ++d->snapshots;
  HeadroomInput in;
  const bool fits = d->fitsFirst || (d->pausedNow && d->headroomOk);
  in.freeInternal = fits ? 60000 : 45236;
  in.largestInternal = fits ? 30000 : 16372;
  in.pollRunning = d->pollRunning && !d->pausedNow;
  in.pollBusy = d->pollBusy;
  in.pollStackBytes = kPollStack;
  return in;
}
static bool fakePause(void* ctx) {
  FakeDevice* d = static_cast<FakeDevice*>(ctx);
  ++d->pauses;
  if (d->pauseOk) d->pausedNow = true;
  return d->pauseOk;
}
static bool fakeWait(void* ctx) {
  FakeDevice* d = static_cast<FakeDevice*>(ctx);
  ++d->waits;
  return d->headroomOk;
}
static bool fakeSpawn(void* ctx) {
  FakeDevice* d = static_cast<FakeDevice*>(ctx);
  ++d->spawns;
  return d->spawns == 1 ? d->spawn1Ok : d->spawn2Ok;
}
static SpawnHooks hooksFor(FakeDevice& d) {
  SpawnHooks h;
  h.ctx = &d;
  h.snapshot = fakeSnapshot;
  h.pausePoll = fakePause;
  h.waitHeadroom = fakeWait;
  h.spawn = fakeSpawn;
  return h;
}

static void test_fits_spawns_without_touching_the_poll() {
  FakeDevice d;
  PollResumeLedger ledger;
  const SpawnOutcome o = spawnWithHeadroom(hooksFor(d), ledger);
  TEST_ASSERT_EQUAL_INT((int)SpawnResult::Spawned, (int)o.result);
  TEST_ASSERT_FALSE(o.paused);
  TEST_ASSERT_EQUAL_INT(0, d.pauses);
  TEST_ASSERT_EQUAL_INT(1, d.spawns);
  TEST_ASSERT_FALSE(ledger.owed());
}

static void test_loaded_device_pauses_then_spawns() {
  FakeDevice d;
  d.fitsFirst = false;
  PollResumeLedger ledger;
  const SpawnOutcome o = spawnWithHeadroom(hooksFor(d), ledger);
  TEST_ASSERT_EQUAL_INT((int)SpawnResult::Spawned, (int)o.result);
  TEST_ASSERT_TRUE(o.paused);
  TEST_ASSERT_EQUAL_INT(1, d.pauses);
  TEST_ASSERT_EQUAL_INT(1, d.waits);
  TEST_ASSERT_EQUAL_INT(1, d.spawns);
  TEST_ASSERT_TRUE(ledger.owed());
}

static void test_busy_poll_is_never_paused() {
  FakeDevice d;
  d.fitsFirst = false;
  d.pollBusy = true;
  PollResumeLedger ledger;
  const SpawnOutcome o = spawnWithHeadroom(hooksFor(d), ledger);
  TEST_ASSERT_EQUAL_INT((int)SpawnResult::LowMemory, (int)o.result);
  TEST_ASSERT_EQUAL_INT(0, d.pauses);
  TEST_ASSERT_EQUAL_INT(0, d.spawns);
  TEST_ASSERT_FALSE(ledger.owed());
}

static void test_pause_that_times_out_still_owes_the_restart() {
  FakeDevice d;
  d.fitsFirst = false;
  d.pauseOk = false;   // a turn began between the busy check and the quiesce
  PollResumeLedger ledger;
  const SpawnOutcome o = spawnWithHeadroom(hooksFor(d), ledger);
  TEST_ASSERT_EQUAL_INT((int)SpawnResult::LowMemory, (int)o.result);
  TEST_ASSERT_TRUE(o.paused);
  TEST_ASSERT_EQUAL_INT(0, d.spawns);
  TEST_ASSERT_TRUE(ledger.owed());   // its loop stopped all the same
}

static void test_gate_pass_but_spawn_failure_gets_the_pause() {
  FakeDevice d;
  d.spawn1Ok = false;   // the heap moved between the measurement and the create
  d.fitsFirst = true;
  PollResumeLedger ledger;
  // The re-measure after the failure must not read "fits" (spawnFailed forces it).
  const SpawnOutcome o = spawnWithHeadroom(hooksFor(d), ledger);
  TEST_ASSERT_EQUAL_INT((int)SpawnResult::Spawned, (int)o.result);
  TEST_ASSERT_TRUE(o.paused);
  TEST_ASSERT_EQUAL_INT(1, d.pauses);
  TEST_ASSERT_EQUAL_INT(2, d.spawns);
  TEST_ASSERT_TRUE(ledger.owed());
}

static void test_null_snapshot_never_guesses() {
  FakeDevice d;
  SpawnHooks h = hooksFor(d);
  h.snapshot = nullptr;
  PollResumeLedger ledger;
  const SpawnOutcome o = spawnWithHeadroom(h, ledger);
  TEST_ASSERT_EQUAL_INT((int)SpawnResult::LowMemory, (int)o.result);
  TEST_ASSERT_EQUAL_INT(0, d.spawns);
  TEST_ASSERT_EQUAL_INT(0, d.pauses);
}

// The device glue's lifecycle over one request: the claim is held while the OTA task
// runs (Spawned) and released at once otherwise; tick() runs the ledger step. A
// resume seam that fails `failuresLeft` times first, then lands.
struct ResumeSeam {
  int calls = 0;
  int failuresLeft = 0;
  bool operator()() {
    ++calls;
    if (failuresLeft > 0) { --failuresLeft; return false; }
    return true;
  }
};

static void test_every_path_restarts_a_paused_poll() {
  int paths = 0, pausedPaths = 0;
  for (int mask = 0; mask < 128; ++mask) {
    FakeDevice d;
    d.fitsFirst   = mask & 1;
    d.pollRunning = mask & 2;
    d.pollBusy    = mask & 4;
    d.pauseOk     = mask & 8;
    d.headroomOk  = mask & 16;
    d.spawn1Ok    = mask & 32;
    d.spawn2Ok    = mask & 64;
    PollResumeLedger ledger;
    const SpawnOutcome o = spawnWithHeadroom(hooksFor(d), ledger);
    ++paths;

    // Bounded: never two pauses, never three spawns.
    TEST_ASSERT_TRUE(d.pauses <= 1);
    TEST_ASSERT_TRUE(o.spawns <= 2);
    TEST_ASSERT_EQUAL_INT(d.spawns, o.spawns);
    // Only an idle, running poll is ever paused.
    if (d.pauses) TEST_ASSERT_TRUE(d.pollRunning && !d.pollBusy);
    // The ledger owes a restart exactly when the poll was paused.
    TEST_ASSERT_EQUAL(o.paused, ledger.owed());
    TEST_ASSERT_EQUAL(o.paused, d.pauses == 1);
    if (o.paused) ++pausedPaths;

    // Lifecycle: a spawned OTA task holds the claim while it runs. No restart then.
    ResumeSeam seam;
    seam.failuresLeft = mask % 3;   // 0..2 failed restarts before it lands
    std::atomic<bool> claim{o.result == SpawnResult::Spawned};   // the OTA task holds it
    if (claim.load()) {
      for (int t = 0; t < 5; ++t) ledger.tick(claim, [&] { return seam(); });
      TEST_ASSERT_EQUAL_INT(0, seam.calls);
      TEST_ASSERT_TRUE(claim.load());   // the tick never takes a held claim
      claim.store(false);               // the task ends
    }
    // The task ends (success of a check, or any failure of either task), or it never
    // started and the refusal released the claim at once: the restart comes, retries
    // until it lands, then stops (no thrash), and the claim is free again after.
    for (int t = 0; t < 10; ++t) ledger.tick(claim, [&] { return seam(); });
    TEST_ASSERT_FALSE(claim.load());
    if (o.paused) {
      TEST_ASSERT_EQUAL_INT(1 + mask % 3, seam.calls);
      TEST_ASSERT_FALSE(ledger.owed());
    } else {
      TEST_ASSERT_EQUAL_INT(0, seam.calls);   // never "restart" a poll nobody paused
    }
  }
  TEST_ASSERT_EQUAL_INT(128, paths);
  TEST_ASSERT_TRUE(pausedPaths > 0);
}

// A successful install restarts the device and holds the claim through it: the
// ledger never restarts the poll under the new image's feet.
static void test_install_success_holds_until_restart() {
  PollResumeLedger ledger;
  ledger.notePaused();
  ResumeSeam seam;
  std::atomic<bool> claim{true};   // held through esp_restart
  for (int t = 0; t < 100; ++t) ledger.tick(claim, [&] { return seam(); });
  TEST_ASSERT_EQUAL_INT(0, seam.calls);
  TEST_ASSERT_TRUE(ledger.owed());
}

// The restart holds the claim while it runs, so a web request that arrives in the
// gap between the restart and the debt clearing is refused "busy" instead of
// pausing the poll again (which the ledger would then forget: the poll would stay
// down until the next restart).
static void test_restart_holds_the_claim_against_a_racing_request() {
  PollResumeLedger ledger;
  ledger.notePaused();
  std::atomic<bool> claim{false};
  bool requesterGotIn = true;
  ledger.tick(claim, [&] {
    bool expected = false;   // a requester on another task tries to claim mid-restart
    requesterGotIn = claim.compare_exchange_strong(expected, true);
    return true;
  });
  TEST_ASSERT_FALSE(requesterGotIn);
  TEST_ASSERT_FALSE(ledger.owed());
  TEST_ASSERT_FALSE(claim.load());   // released after the restart
}

// ---- the restart never runs two pollers ------------------------------------------

static void test_restart_step_never_runs_two_pollers() {
  TEST_ASSERT_EQUAL_INT((int)PollRestart::Running, (int)pollRestartStep(true, true));
  TEST_ASSERT_EQUAL_INT((int)PollRestart::WaitExit, (int)pollRestartStep(true, false));
  TEST_ASSERT_EQUAL_INT((int)PollRestart::Start, (int)pollRestartStep(false, false));
  TEST_ASSERT_EQUAL_INT((int)PollRestart::Start, (int)pollRestartStep(false, true));
  // Class rule: a live task handle is never answered with Start.
  for (int run = 0; run < 2; ++run)
    TEST_ASSERT_NOT_EQUAL((int)PollRestart::Start, (int)pollRestartStep(true, run));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_gate_boundaries);
  RUN_TEST(test_measured_devices);
  RUN_TEST(test_decision_every_combination);
  RUN_TEST(test_headroom_str);
  RUN_TEST(test_fits_spawns_without_touching_the_poll);
  RUN_TEST(test_loaded_device_pauses_then_spawns);
  RUN_TEST(test_busy_poll_is_never_paused);
  RUN_TEST(test_pause_that_times_out_still_owes_the_restart);
  RUN_TEST(test_gate_pass_but_spawn_failure_gets_the_pause);
  RUN_TEST(test_null_snapshot_never_guesses);
  RUN_TEST(test_every_path_restarts_a_paused_poll);
  RUN_TEST(test_install_success_holds_until_restart);
  RUN_TEST(test_restart_holds_the_claim_against_a_racing_request);
  RUN_TEST(test_restart_step_never_runs_two_pollers);
  return UNITY_END();
}
