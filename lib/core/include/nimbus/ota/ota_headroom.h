#pragma once
#include <atomic>
#include <cstdint>

// OTA headroom - the pure decision behind "an update check or install that would
// be refused only for low internal memory" (lane L31, bench 2026-10-13).
//
// The OTA check and install each run on ONE self-deleting task whose stack must be
// a single contiguous block of internal SRAM. A loaded device (Telegram long-poll
// on, cloud link online) can sit just under that block: Lumi read intLargest 16372
// against a 16384 B stack, so every check (also the daily one) was refused and the
// device could never update itself over the air. The Telegram poll task holds a
// 24 KB internal stack and already has a bounded, non-destructive stop (quiesce,
// the seam the factory reset uses): it joins the task and keeps its queues, so the
// same task can be started again on the same queues afterwards.
//
// The rule, pinned by test/test_ota_headroom:
//   1. The stack fits      -> spawn the OTA task, nothing else happens.
//   2. It does not fit, the poll task is running and idle, and pausing it frees
//      enough -> pause it (bounded), wait for its stack to come back, re-check,
//      spawn. A spawn that fails although the gate passed (the heap moved) gets
//      the same one chance.
//   3. Anything else (no poll task, a turn or background job in flight, or still
//      short after the pause) -> refuse with the same honest "low memory" answer.
//   4. A paused poll task is owed a restart from the moment the pause begins, on
//      EVERY path: a refusal, a spawn failure, an OTA task that ends in success or
//      in any failure. The restart waits until no OTA task holds the memory, and it
//      is retried until it lands.
// No new task and no concurrency: the OTA task already exists, and pausing the
// poll task only ever reduces what runs at once (AGENTS.md section 4).
namespace nimbus {
namespace ota {

// The stack of the otacheck / otainstall task, in bytes (the Xtensa port counts
// stack in bytes). One contiguous internal block of at least this size must be free.
inline constexpr uint32_t kOtaTaskStackBytes = 16384;
// Free internal heap the spawn needs beyond the stack itself: the TCB, lwIP and
// the TLS session's internal transients stay above the TLS/lwIP danger zone.
inline constexpr uint32_t kSpawnFreeInternal = 24000;

// True when the OTA task's stack fits the heap as measured.
bool fitsOtaTask(uint32_t freeInternal, uint32_t largestInternal);

struct HeadroomInput {
  uint32_t freeInternal = 0;      // free internal 8-bit heap
  uint32_t largestInternal = 0;   // largest free internal 8-bit block
  bool pollRunning = false;       // the Telegram poll task exists (Orchestrator mode)
  bool pollBusy = false;          // a turn or background job runs on it, or the
                                  // caller IS it: never pause it then
  uint32_t pollStackBytes = 0;    // what pausing it frees (its stack, one block)
  bool spawnFailed = false;       // the gate passed but the task create failed
};

enum class Headroom : uint8_t {
  Fits = 0,    // spawn the OTA task now
  PausePoll,   // pause the idle poll task first: that frees enough
  LowMemory,   // refuse "low-heap": nothing safe to free
};
Headroom decideHeadroom(const HeadroomInput& in);
const char* headroomStr(Headroom h);   // "fits" / "pause-poll" / "low-memory"

// Rule 4 as a ledger the device glue drives. notePaused() as the pause begins
// (a pause that times out still stopped the poll loop, so it is owed too);
// shouldResume() on every tick; noteResumed() with the restart's verdict.
// Shared between the requesting task (web or main loop) and the main-loop tick,
// so the flag is atomic; the OTA single-flight claim orders the two.
class PollResumeLedger {
 public:
  void notePaused() { owed_.store(true); }
  bool owed() const { return owed_.load(); }
  // Restart now? Only while owed and only when no OTA task holds the memory.
  bool shouldResume(bool otaTaskRunning) const { return owed_.load() && !otaTaskRunning; }
  // A failed restart (the old task still exiting, or no room for its stack yet)
  // stays owed and is retried on the next tick.
  void noteResumed(bool ok) {
    if (ok) owed_.store(false);
  }
  // One tick of rule 4, the exact step the device glue runs (otaupd::tick):
  // restart when owed and no OTA task runs. Returns true when a restart was tried.
  template <class Resume>
  bool tick(bool otaTaskRunning, Resume&& resume) {
    if (!shouldResume(otaTaskRunning)) return false;
    noteResumed(resume());
    return true;
  }

 private:
  std::atomic<bool> owed_{false};
};

// The restart itself (telegram::resume) never runs two pollers. `taskAlive` = the
// poll task handle is live, `runFlag` = its run flag is set.
enum class PollRestart : uint8_t {
  Running = 0,   // alive and running: nothing to do, the restart is done
  WaitExit,      // alive but stopping (a quiesce that timed out): it exits after
                 // its current cycle; starting another now would run two pollers
  Start,         // gone: start a fresh task on the live queues
};
constexpr PollRestart pollRestartStep(bool taskAlive, bool runFlag) {
  return taskAlive ? (runFlag ? PollRestart::Running : PollRestart::WaitExit)
                   : PollRestart::Start;
}

// The device seams spawnWithHeadroom() runs against (the house hook pattern, like
// usbfw::CommitHooks): plain function pointers + an opaque context.
struct SpawnHooks {
  void* ctx = nullptr;
  HeadroomInput (*snapshot)(void* ctx) = nullptr;   // live heap + poll state
  bool (*pausePoll)(void* ctx) = nullptr;           // bounded quiesce: true when gone
  bool (*waitHeadroom)(void* ctx) = nullptr;        // after a pause: true once it fits
  bool (*spawn)(void* ctx) = nullptr;               // create the OTA task
};

enum class SpawnResult : uint8_t {
  Spawned = 0,   // the OTA task runs (it clears the claim when it ends)
  LowMemory,     // refused before any spawn: "low-heap"
  SpawnFailed,   // the gate passed but the task create failed, even after a pause
};

struct SpawnOutcome {
  SpawnResult result = SpawnResult::LowMemory;
  bool paused = false;   // the poll task was paused (the ledger owes its restart)
  int spawns = 0;        // task-create attempts (at most 2)
};

// Rules 1-3, with rule 4's ledger noted at the pause. The caller holds the OTA
// single-flight claim and releases it on any result but Spawned.
SpawnOutcome spawnWithHeadroom(const SpawnHooks& h, PollResumeLedger& ledger);

}  // namespace ota
}  // namespace nimbus
