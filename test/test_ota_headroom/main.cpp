// Host tests for where an OTA check or install runs (lib/core nimbus/ota/ota_headroom.h,
// lanes L31/L32).
//
// Bench 2026-10-13: Lumi (Telegram long-poll on, cloud link online) refused every
// update check with "low-heap", also its daily one: intLargest 16372 against the
// OTA task's 16384 B stack. Lane L31 paused the Telegram poll to lend its stack, and
// the release gate (P2J) showed the poll could then never come back. The design now:
// the dedicated task when it fits, else the job is staged for the poll task, which
// runs it inline between its own work; nothing is paused or restarted. These pin the
// CLASS (AGENTS.md section 3):
//   - the gates at their exact boundaries, and the two measured devices;
//   - the decision over EVERY input combination (free, largest, mode, poll alive,
//     spawn failed): the dedicated task first whenever it fits, the poll task only in
//     Orchestrator mode with the poll alive, otherwise the honest refusal;
//   - single-flight staging: a second request while one is staged or running is
//     refused, a staged job is taken (run) exactly once, and the claim and the slot
//     are released only after the run, in an order no racing request can split;
//   - the routing over EVERY scripted device outcome (128 paths): at most one task
//     create and one staging, a job is staged only where the decision allows it, and
//     an unloaded device never touches the poll task.
// The class rule "no path pauses, deletes or restarts the poll task for OTA" is the
// source guard in test/test_ota_poll_guard.

#include <unity.h>

#include <atomic>
#include <cstdint>

#include "nimbus/ota/ota_headroom.h"

using namespace nimbus::ota;

void setUp() {}
void tearDown() {}

// ---- the gates -----------------------------------------------------------------

static void test_gate_boundaries() {
  TEST_ASSERT_TRUE(fitsOtaTask(kSpawnFreeInternal, kOtaTaskStackBytes));
  TEST_ASSERT_FALSE(fitsOtaTask(kSpawnFreeInternal - 1, kOtaTaskStackBytes));
  TEST_ASSERT_FALSE(fitsOtaTask(kSpawnFreeInternal, kOtaTaskStackBytes - 1));
  TEST_ASSERT_FALSE(fitsOtaTask(0, 0));
  // The floor is the real stack, not a rounder number below it: the old 16000 floor
  // passed Lumi's 16372 and the task create then failed on the 16384 B stack.
  TEST_ASSERT_EQUAL_UINT32(16384u, kOtaTaskStackBytes);
  // The poll task needs no stack block, but the same free heap for the TLS fetch.
  TEST_ASSERT_TRUE(fitsOnPoll(kSpawnFreeInternal));
  TEST_ASSERT_FALSE(fitsOnPoll(kSpawnFreeInternal - 1));
  TEST_ASSERT_FALSE(fitsOnPoll(0));
}

static HeadroomInput lumiInput() {
  // Lumi on v4.5.10: free 45236, largest 16372, Telegram + cloud on.
  HeadroomInput in;
  in.freeInternal = 45236;
  in.largestInternal = 16372;
  in.orchMode = true;
  in.pollAlive = true;
  return in;
}

static void test_measured_devices() {
  HeadroomInput lumi = lumiInput();
  TEST_ASSERT_FALSE(fitsOtaTask(lumi.freeInternal, lumi.largestInternal));
  TEST_ASSERT_EQUAL_INT((int)Headroom::OnPoll, (int)decideHeadroom(lumi));
  // The same heap with no poll task to take it, or in Notifier mode: the honest refusal.
  HeadroomInput noPoll = lumi;
  noPoll.pollAlive = false;
  TEST_ASSERT_EQUAL_INT((int)Headroom::LowMemory, (int)decideHeadroom(noPoll));
  HeadroomInput notifier = lumi;
  notifier.orchMode = false;
  TEST_ASSERT_EQUAL_INT((int)Headroom::LowMemory, (int)decideHeadroom(notifier));
  // nimbus-light (no Telegram, no cloud): largest 23540 fits, and the dedicated task
  // stays first even with a live poll task (no change for an unloaded device).
  HeadroomInput light;
  light.freeInternal = 60000;
  light.largestInternal = 23540;
  light.orchMode = true;
  light.pollAlive = true;
  TEST_ASSERT_EQUAL_INT((int)Headroom::Fits, (int)decideHeadroom(light));
}

// ---- the decision, every combination -----------------------------------------------

struct HeapCase {
  uint32_t freeB, largestB;
};
// Fits; Lumi (largest short); free short with a big block; free just at the floor with
// a short block; free just under it; both far too short.
static const HeapCase kHeaps[] = {
    {60000, 30000}, {45236, 16372}, {20000, 30000},
    {kSpawnFreeInternal, 9000}, {kSpawnFreeInternal - 1, 9000}, {1000, 1000},
};

// The rule restated from the spec (not from the code): the task fits and its create did
// not fail -> Fits; else Orchestrator mode, a live poll task and enough free heap ->
// OnPoll; else LowMemory.
static Headroom spec(const HeadroomInput& in) {
  const bool taskFits = in.freeInternal >= 24000 && in.largestInternal >= 16384;
  if (taskFits && !in.spawnFailed) return Headroom::Fits;
  if (in.orchMode && in.pollAlive && in.freeInternal >= 24000) return Headroom::OnPoll;
  return Headroom::LowMemory;
}

// One combination: the decision matches the spec, and the class rules hold on their
// own. Returns whether it chose the poll task.
static bool checkCombination(const HeadroomInput& in) {
  const Headroom d = decideHeadroom(in);
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)spec(in), (int)d, headroomStr(d));
  // Never the poll task without a poll task, or in Notifier mode.
  if (!in.orchMode || !in.pollAlive) TEST_ASSERT_NOT_EQUAL((int)Headroom::OnPoll, (int)d);
  // The dedicated task first whenever it fits: mode and poll state never change that.
  if (fitsOtaTask(in.freeInternal, in.largestInternal) && !in.spawnFailed)
    TEST_ASSERT_EQUAL_INT((int)Headroom::Fits, (int)d);
  // A failed create is never answered with another create.
  if (in.spawnFailed) TEST_ASSERT_NOT_EQUAL((int)Headroom::Fits, (int)d);
  return d == Headroom::OnPoll;
}

static void test_decision_every_combination() {
  int onPoll = 0, combos = 0;
  for (const HeapCase& hc : kHeaps)
    for (int bits = 0; bits < 8; ++bits) {
      HeadroomInput in;
      in.freeInternal = hc.freeB;
      in.largestInternal = hc.largestB;
      in.orchMode = bits & 1;
      in.pollAlive = bits & 2;
      in.spawnFailed = bits & 4;
      if (checkCombination(in)) ++onPoll;
      ++combos;
    }
  TEST_ASSERT_EQUAL_INT(48, combos);
  TEST_ASSERT_TRUE(onPoll > 0);   // the poll path is reachable at all
}

static void test_names() {
  TEST_ASSERT_EQUAL_STRING("fits", headroomStr(Headroom::Fits));
  TEST_ASSERT_EQUAL_STRING("on-poll", headroomStr(Headroom::OnPoll));
  TEST_ASSERT_EQUAL_STRING("low-memory", headroomStr(Headroom::LowMemory));
  // The job names are the task names the device logs: the bench check reads them.
  TEST_ASSERT_EQUAL_STRING("otacheck", otaJobName(OtaJob::Check));
  TEST_ASSERT_EQUAL_STRING("otainstall", otaJobName(OtaJob::Install));
  TEST_ASSERT_EQUAL_STRING("", otaJobName(OtaJob::None));
}

// ---- single-flight staging ---------------------------------------------------------

static void test_slot_stages_one_job_at_a_time() {
  OnPollSlot slot;
  TEST_ASSERT_FALSE(slot.pending());
  TEST_ASSERT_FALSE(slot.busy());
  TEST_ASSERT_EQUAL_INT((int)OtaJob::None, (int)slot.job());
  TEST_ASSERT_FALSE(slot.stage(OtaJob::None));   // nothing to stage
  TEST_ASSERT_TRUE(slot.stage(OtaJob::Check));
  TEST_ASSERT_TRUE(slot.pending());
  TEST_ASSERT_EQUAL_INT((int)OtaJob::Check, (int)slot.job());
  // A second request while one is staged is refused, whatever it asks for, and the
  // staged job is unchanged (never merged into something else, never overwritten).
  TEST_ASSERT_FALSE(slot.stage(OtaJob::Check));
  TEST_ASSERT_FALSE(slot.stage(OtaJob::Install));
  TEST_ASSERT_EQUAL_INT((int)OtaJob::Check, (int)slot.job());
  // done() before take() frees nothing: only a running job ends.
  slot.done();
  TEST_ASSERT_TRUE(slot.pending());
}

static void test_slot_runs_a_job_exactly_once() {
  OnPollSlot slot;
  TEST_ASSERT_EQUAL_INT((int)OtaJob::None, (int)slot.take());   // nothing staged
  TEST_ASSERT_TRUE(slot.stage(OtaJob::Install));
  TEST_ASSERT_EQUAL_INT((int)OtaJob::Install, (int)slot.take());
  // Running: no longer pending (the poll loop's probe goes quiet), still busy, and a
  // request while it runs is refused.
  TEST_ASSERT_FALSE(slot.pending());
  TEST_ASSERT_TRUE(slot.busy());
  TEST_ASSERT_EQUAL_INT((int)OtaJob::Install, (int)slot.job());
  TEST_ASSERT_FALSE(slot.stage(OtaJob::Check));
  // Never handed out a second time.
  TEST_ASSERT_EQUAL_INT((int)OtaJob::None, (int)slot.take());
  TEST_ASSERT_EQUAL_INT((int)OtaJob::None, (int)slot.take());
  slot.done();
  TEST_ASSERT_FALSE(slot.busy());
  TEST_ASSERT_EQUAL_INT((int)OtaJob::None, (int)slot.take());
  // Empty again: the next request stages normally.
  TEST_ASSERT_TRUE(slot.stage(OtaJob::Check));
}

// The device's request skeleton (requestCheck / requestInstall): take the single-flight
// claim, route, release the claim unless the work now runs somewhere. Returns the
// route, with Busy for a claim already held.
static Route request(std::atomic<bool>& claim, OnPollSlot& slot, const RouteHooks& h,
                     OtaJob job) {
  bool expected = false;
  if (!claim.compare_exchange_strong(expected, true)) return Route::Busy;
  const Route r = routeOtaWork(h, slot, job);
  if (r != Route::Spawned && r != Route::OnPoll) claim.store(false);
  return r;
}

static HeadroomInput lumiSnapshot(void*) { return lumiInput(); }
static int g_spawns = 0;
static bool countingSpawn(void*) { ++g_spawns; return true; }

static RouteHooks lumiHooks() {
  RouteHooks h;
  h.snapshot = lumiSnapshot;
  h.spawn = countingSpawn;
  return h;
}

static void test_second_request_is_refused_until_the_run_ends() {
  std::atomic<bool> claim{false};
  OnPollSlot slot;
  const RouteHooks h = lumiHooks();
  g_spawns = 0;
  TEST_ASSERT_EQUAL_INT((int)Route::OnPoll, (int)request(claim, slot, h, OtaJob::Check));
  TEST_ASSERT_TRUE(claim.load());   // held while staged
  // A second check or an install while it is staged: refused "busy", nothing staged.
  TEST_ASSERT_EQUAL_INT((int)Route::Busy, (int)request(claim, slot, h, OtaJob::Check));
  TEST_ASSERT_EQUAL_INT((int)Route::Busy, (int)request(claim, slot, h, OtaJob::Install));
  TEST_ASSERT_EQUAL_INT((int)OtaJob::Check, (int)slot.job());

  int runs = 0;
  const OtaJob ran = runStaged(slot, claim, [&](OtaJob j) {
    ++runs;
    TEST_ASSERT_EQUAL_INT((int)OtaJob::Check, (int)j);
    // While it runs, the claim and the slot are both still held: a request racing the
    // run is refused, and even one that skipped the claim could not stage a second job.
    TEST_ASSERT_TRUE(claim.load());
    TEST_ASSERT_TRUE(slot.busy());
    TEST_ASSERT_EQUAL_INT((int)Route::Busy, (int)request(claim, slot, h, OtaJob::Install));
    TEST_ASSERT_EQUAL_INT((int)Route::Busy, (int)routeOtaWork(h, slot, OtaJob::Install));
  });
  TEST_ASSERT_EQUAL_INT((int)OtaJob::Check, (int)ran);
  TEST_ASSERT_EQUAL_INT(1, runs);
  // Ended: both released, and a second step runs nothing (never twice).
  TEST_ASSERT_FALSE(claim.load());
  TEST_ASSERT_FALSE(slot.busy());
  TEST_ASSERT_EQUAL_INT((int)OtaJob::None, (int)runStaged(slot, claim, [&](OtaJob) { ++runs; }));
  TEST_ASSERT_EQUAL_INT(1, runs);
  TEST_ASSERT_EQUAL_INT(0, g_spawns);   // Lumi never even tried the dedicated task
  // The next request after the run is accepted again.
  TEST_ASSERT_EQUAL_INT((int)Route::OnPoll, (int)request(claim, slot, h, OtaJob::Install));
}

// The release order is what keeps single flight across tasks: the slot is emptied
// BEFORE the claim is released, so the first request that wins the claim after a run
// always finds an empty slot (never "busy" from a half-finished hand-over). A spy
// claim records what the slot looked like at the instant the claim was let go.
struct SpyClaim {
  const OnPollSlot* slot = nullptr;
  bool held = true;
  int releases = 0;
  bool slotBusyAtRelease = true;
  void store(bool v) {
    if (!v) { ++releases; slotBusyAtRelease = slot->busy(); }
    held = v;
  }
};

static void test_release_order_never_strands_a_request() {
  OnPollSlot slot;
  SpyClaim claim;
  claim.slot = &slot;
  TEST_ASSERT_TRUE(slot.stage(OtaJob::Check));
  runStaged(slot, claim, [&](OtaJob) {
    TEST_ASSERT_TRUE(claim.held);           // held through the whole run
    TEST_ASSERT_EQUAL_INT(0, claim.releases);
  });
  TEST_ASSERT_EQUAL_INT(1, claim.releases);  // released exactly once, at the end
  TEST_ASSERT_FALSE(claim.slotBusyAtRelease);
  TEST_ASSERT_FALSE(claim.held);
  // Nothing staged: the claim is not touched (it belongs to whoever holds it).
  SpyClaim idle;
  idle.slot = &slot;
  TEST_ASSERT_EQUAL_INT((int)OtaJob::None, (int)runStaged(slot, idle, [](OtaJob) {}));
  TEST_ASSERT_EQUAL_INT(0, idle.releases);
  TEST_ASSERT_TRUE(idle.held);
}

// ---- the routing, every scripted path ---------------------------------------------

// A scripted device: what the heap reads before and after a failed create, the mode,
// whether the poll task lives, what the create answers, and whether the slot already
// holds a job. Counts every call so a test can assert the absence of a second create.
struct FakeDevice {
  bool fitsFirst = true;       // the first measurement passes the task gate
  bool spawnOk = true;         // the task create succeeds
  bool freeForPoll = true;     // the free heap carries the TLS fetch on the poll task
  bool orchMode = true;
  bool pollAlive = true;
  int snapshots = 0, spawns = 0;
};

static HeadroomInput fakeSnapshot(void* ctx) {
  FakeDevice* d = static_cast<FakeDevice*>(ctx);
  ++d->snapshots;
  HeadroomInput in;
  in.freeInternal = d->freeForPoll ? 45236 : 20000;
  in.largestInternal = d->fitsFirst ? 30000 : 16372;
  in.orchMode = d->orchMode;
  in.pollAlive = d->pollAlive;
  return in;
}
static bool fakeSpawn(void* ctx) {
  FakeDevice* d = static_cast<FakeDevice*>(ctx);
  ++d->spawns;
  return d->spawnOk;
}
static RouteHooks hooksFor(FakeDevice& d) {
  RouteHooks h;
  h.ctx = &d;
  h.snapshot = fakeSnapshot;
  h.spawn = fakeSpawn;
  return h;
}

// The route restated from the spec: the task first; a failed create or a short heap
// falls to the poll task when it can take the job; a full slot is Busy, never a
// second job.
static Route expectedRoute(bool fitsTask, bool spawnOk, bool pollCan, bool preoccupied) {
  if (fitsTask && spawnOk) return Route::Spawned;
  if (!pollCan) return fitsTask ? Route::SpawnFailed : Route::LowMemory;
  return preoccupied ? Route::Busy : Route::OnPoll;
}

// What the slot holds after the route: the requested job when it was staged, else
// exactly what it held before (a refusal or a task start never stages or replaces).
static void checkSlotAfter(const OnPollSlot& slot, Route r, OtaJob job, bool preoccupied) {
  if (r == Route::OnPoll) {
    TEST_ASSERT_TRUE(slot.pending());
    TEST_ASSERT_EQUAL_INT((int)job, (int)slot.job());
    return;
  }
  TEST_ASSERT_EQUAL(preoccupied, slot.busy());
  if (preoccupied) TEST_ASSERT_EQUAL_INT((int)OtaJob::Check, (int)slot.job());
}

static void checkPath(int mask) {
  FakeDevice d;
  d.fitsFirst   = mask & 1;
  d.spawnOk     = mask & 2;
  d.freeForPoll = mask & 4;
  d.orchMode    = mask & 8;
  d.pollAlive   = mask & 16;
  const bool preoccupied = mask & 32;   // a job already staged (the claim was bypassed)
  const OtaJob job = (mask & 64) ? OtaJob::Install : OtaJob::Check;
  const bool fitsTask = d.fitsFirst && d.freeForPoll;   // free 20000 never fits the task
  const bool pollCan = d.orchMode && d.pollAlive && d.freeForPoll;
  OnPollSlot slot;
  if (preoccupied) TEST_ASSERT_TRUE(slot.stage(OtaJob::Check));
  const Route r = routeOtaWork(hooksFor(d), slot, job);

  TEST_ASSERT_EQUAL_INT((int)expectedRoute(fitsTask, d.spawnOk, pollCan, preoccupied), (int)r);
  // Bounded: never two creates, and a create only when the task fits.
  TEST_ASSERT_TRUE(d.spawns <= 1);
  TEST_ASSERT_EQUAL(fitsTask, d.spawns == 1);
  // The poll task takes a job only in Orchestrator mode, alive, with the free heap;
  // an unloaded device (the task fits and starts) never touches it.
  if (r == Route::OnPoll) TEST_ASSERT_TRUE(pollCan && !preoccupied);
  if (fitsTask && d.spawnOk) TEST_ASSERT_EQUAL_INT((int)Route::Spawned, (int)r);
  checkSlotAfter(slot, r, job, preoccupied);
}

static void test_every_path_routes_once_and_honestly() {
  for (int mask = 0; mask < 128; ++mask) checkPath(mask);
}

static void test_null_hooks_never_guess() {
  OnPollSlot slot;
  RouteHooks none;
  TEST_ASSERT_EQUAL_INT((int)Route::LowMemory, (int)routeOtaWork(none, slot, OtaJob::Check));
  FakeDevice d;
  d.fitsFirst = false;   // would go to the poll task
  TEST_ASSERT_EQUAL_INT((int)Route::LowMemory, (int)routeOtaWork(hooksFor(d), slot, OtaJob::None));
  TEST_ASSERT_FALSE(slot.busy());
  TEST_ASSERT_EQUAL_INT(0, d.spawns);
  // No create hook on a device where the task fits: the poll task still takes it.
  FakeDevice fits;
  RouteHooks noSpawn = hooksFor(fits);
  noSpawn.spawn = nullptr;
  TEST_ASSERT_EQUAL_INT((int)Route::OnPoll, (int)routeOtaWork(noSpawn, slot, OtaJob::Check));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_gate_boundaries);
  RUN_TEST(test_measured_devices);
  RUN_TEST(test_decision_every_combination);
  RUN_TEST(test_names);
  RUN_TEST(test_slot_stages_one_job_at_a_time);
  RUN_TEST(test_slot_runs_a_job_exactly_once);
  RUN_TEST(test_second_request_is_refused_until_the_run_ends);
  RUN_TEST(test_release_order_never_strands_a_request);
  RUN_TEST(test_every_path_routes_once_and_honestly);
  RUN_TEST(test_null_hooks_never_guess);
  return UNITY_END();
}
