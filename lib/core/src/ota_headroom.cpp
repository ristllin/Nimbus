#include "nimbus/ota/ota_headroom.h"

namespace nimbus {
namespace ota {

bool fitsOtaTask(uint32_t freeInternal, uint32_t largestInternal) {
  return freeInternal >= kSpawnFreeInternal && largestInternal >= kOtaTaskStackBytes;
}

Headroom decideHeadroom(const HeadroomInput& in) {
  if (!in.spawnFailed && fitsOtaTask(in.freeInternal, in.largestInternal)) return Headroom::Fits;
  if (!in.pollRunning || in.pollBusy || in.pollStackBytes == 0) return Headroom::LowMemory;
  // Pausing returns the poll stack as one free block of its size, on top of what
  // is free now. Pause only when that is enough: a pause that cannot help would
  // stop Telegram for nothing.
  const uint32_t freeAfter = in.freeInternal + in.pollStackBytes;
  const uint32_t largestAfter =
      in.largestInternal > in.pollStackBytes ? in.largestInternal : in.pollStackBytes;
  return fitsOtaTask(freeAfter, largestAfter) ? Headroom::PausePoll : Headroom::LowMemory;
}

const char* headroomStr(Headroom h) {
  switch (h) {
    case Headroom::Fits:      return "fits";
    case Headroom::PausePoll: return "pause-poll";
    case Headroom::LowMemory: return "low-memory";
  }
  return "low-memory";
}

// Pause the poll task and wait for its stack to come back. The ledger owes the
// restart from the first instruction: a quiesce that times out has still stopped
// the poll loop (it exits after its current cycle), so every path must restart it.
static bool pauseForHeadroom(const SpawnHooks& h, PollResumeLedger& ledger, SpawnOutcome& out) {
  out.paused = true;
  ledger.notePaused();
  if (!h.pausePoll || !h.pausePoll(h.ctx)) return false;   // a turn began after all
  return h.waitHeadroom && h.waitHeadroom(h.ctx);
}

static bool trySpawn(const SpawnHooks& h, SpawnOutcome& out) {
  ++out.spawns;
  const bool ok = h.spawn && h.spawn(h.ctx);
  out.result = ok ? SpawnResult::Spawned : SpawnResult::SpawnFailed;
  return ok;
}

// The gate passed but the stack still did not fit (the heap moved between the
// measurement and the create): the same memory refusal, so the pause gets its
// one chance here too. A pause that does not free enough leaves SpawnFailed.
static void retryWithPause(const SpawnHooks& h, PollResumeLedger& ledger, SpawnOutcome& out) {
  HeadroomInput again = h.snapshot(h.ctx);
  again.spawnFailed = true;
  if (decideHeadroom(again) != Headroom::PausePoll) return;
  if (pauseForHeadroom(h, ledger, out)) trySpawn(h, out);
}

SpawnOutcome spawnWithHeadroom(const SpawnHooks& h, PollResumeLedger& ledger) {
  SpawnOutcome out;              // LowMemory until a spawn is tried
  if (!h.snapshot) return out;   // no way to measure: never guess
  const Headroom first = decideHeadroom(h.snapshot(h.ctx));
  if (first == Headroom::LowMemory) return out;
  if (first == Headroom::PausePoll && !pauseForHeadroom(h, ledger, out)) return out;
  if (trySpawn(h, out)) return out;
  if (!out.paused) retryWithPause(h, ledger, out);   // at most one pause in all
  return out;
}

}  // namespace ota
}  // namespace nimbus
