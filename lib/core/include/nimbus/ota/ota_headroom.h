#pragma once
#include <atomic>
#include <cstdint>

// OTA headroom - where an update check or install runs (lane L31 bench 2026-10-13,
// lane L32 design).
//
// The check and the install each run on ONE self-deleting task whose 16 KB stack must
// be a single contiguous block of internal SRAM. A loaded device (Telegram long-poll
// on, cloud link online) can sit just under that block: Lumi read intLargest 16372
// against the 16384 B stack, so every check (the daily one too) was refused. Lending
// the Telegram poll task's stack (pause it, run the OTA task, restart it) cannot be
// made safe: nothing guarantees the poll's 24 KB block re-forms after the OTA task,
// and then Telegram and every local turn stay down until a reboot (release gate P2J).
//
// The rule, pinned by test/test_ota_headroom:
//   1. The stack fits -> the dedicated OTA task, exactly as on an unloaded device.
//   2. It does not fit (or its create fails although the gate passed), the device is
//      in Orchestrator mode, the poll task is alive and the free heap carries the TLS
//      fetch -> hand the job to the POLL TASK ITSELF: stage it in a single-flight slot
//      and the poll loop runs it inline between its own work (never inside a turn or
//      a helper dispatch), on its own stack. No task is created, paused, deleted or
//      restarted, so there is nothing to restore.
//   3. Anything else (Notifier mode, no poll task, or too little free heap) -> refuse
//      with the same honest "low-heap" answer.
// No new task and no concurrency (AGENTS.md section 4): the work runs on a task that
// already exists, one TLS session at a time.
namespace nimbus {
namespace ota {

// The stack of the otacheck / otainstall task, in bytes (the Xtensa port counts
// stack in bytes). One contiguous internal block of at least this size must be free.
inline constexpr uint32_t kOtaTaskStackBytes = 16384;
// Free internal heap the work needs: the TCB, lwIP and the TLS session's internal
// transients stay above the TLS/lwIP danger zone. The same floor holds for a run on
// the poll task, which needs no stack block but the same TLS fetch.
inline constexpr uint32_t kSpawnFreeInternal = 24000;

// True when the dedicated OTA task's stack fits the heap as measured.
bool fitsOtaTask(uint32_t freeInternal, uint32_t largestInternal);
// True when the free heap carries the work inline on the poll task.
bool fitsOnPoll(uint32_t freeInternal);

struct HeadroomInput {
  uint32_t freeInternal = 0;      // free internal 8-bit heap
  uint32_t largestInternal = 0;   // largest free internal 8-bit block
  bool orchMode = false;          // Orchestrator mode (Notifier mode has no poll task)
  bool pollAlive = false;         // the Telegram poll task exists (token or not: it is
                                  // also the local turn processor)
  bool spawnFailed = false;       // the gate passed but the dedicated task create failed
};

enum class Headroom : uint8_t {
  Fits = 0,    // spawn the dedicated OTA task
  OnPoll,      // stage the job for the poll task
  LowMemory,   // refuse "low-heap"
};
Headroom decideHeadroom(const HeadroomInput& in);
const char* headroomStr(Headroom h);   // "fits" / "on-poll" / "low-memory"

enum class OtaJob : uint8_t { None = 0, Check, Install };
const char* otaJobName(OtaJob j);      // "otacheck" / "otainstall" / ""

// The single-flight hand-over to the poll task: at most one job is staged or running
// at a time, and a staged job is taken (and so run) exactly once. stage() runs on the
// requesting task (web, main loop, or the poll task itself for /update), take() and
// done() on the poll task; one atomic word orders them.
class OnPollSlot {
 public:
  // Stage a job. False, staging nothing, while a job is staged or running.
  bool stage(OtaJob j);
  // A job is staged and not yet taken (the poll loop's cheap probe, any task).
  bool pending() const;
  // A job is staged or running.
  bool busy() const;
  // The job staged or running (None when the slot is empty).
  OtaJob job() const;
  // Poll task: the staged job, now marked running. None when nothing is staged (a
  // running job is never handed out a second time).
  OtaJob take();
  // Poll task: the running job ended; the slot is empty again.
  void done();

 private:
  static constexpr uint8_t kRunning = 0x80;
  std::atomic<uint8_t> state_{0};   // 0 empty, else the job, | kRunning once taken
};

// The poll task's whole step, in the order that keeps single flight: take the staged
// job, run it, empty the slot, THEN release the OTA claim the requester took. A
// request racing the end of a run sees the claim still held ("busy") or an empty
// slot, never a half-finished hand-over. A run that restarts the device (a successful
// install) never returns, so it holds both through the restart, as the dedicated task
// holds the claim. Returns the job that ran (None: nothing was staged). `claim` is
// the device's std::atomic<bool> (anything with store(bool): a test spies the order).
template <class Claim, class Run>
OtaJob runStaged(OnPollSlot& slot, Claim& claim, Run&& run) {
  const OtaJob j = slot.take();
  if (j == OtaJob::None) return j;
  run(j);
  slot.done();
  claim.store(false);
  return j;
}

// The device seams routeOtaWork() runs against (the house hook pattern, like
// usbfw::CommitHooks): plain function pointers + an opaque context.
struct RouteHooks {
  void* ctx = nullptr;
  HeadroomInput (*snapshot)(void* ctx) = nullptr;   // live heap + mode + poll task
  bool (*spawn)(void* ctx) = nullptr;               // create the dedicated OTA task
};

enum class Route : uint8_t {
  Spawned = 0,   // the dedicated task runs; it releases the claim when it ends
  OnPoll,        // staged for the poll task; its run releases the claim
  LowMemory,     // refused before anything ran: "low-heap"
  SpawnFailed,   // the gate passed, the create failed, and the poll task cannot take it
  Busy,          // the slot already holds a job: refused, never staged twice
};

// Rules 1-3 for one request: at most one task create, at most one staging. The
// caller holds the OTA single-flight claim and releases it on any result but
// Spawned and OnPoll.
Route routeOtaWork(const RouteHooks& h, OnPollSlot& slot, OtaJob job);

}  // namespace ota
}  // namespace nimbus
