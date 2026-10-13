#include "nimbus/ota/ota_headroom.h"

namespace nimbus {
namespace ota {

bool fitsOtaTask(uint32_t freeInternal, uint32_t largestInternal) {
  return freeInternal >= kSpawnFreeInternal && largestInternal >= kOtaTaskStackBytes;
}

bool fitsOnPoll(uint32_t freeInternal) { return freeInternal >= kSpawnFreeInternal; }

Headroom decideHeadroom(const HeadroomInput& in) {
  if (!in.spawnFailed && fitsOtaTask(in.freeInternal, in.largestInternal)) return Headroom::Fits;
  if (in.orchMode && in.pollAlive && fitsOnPoll(in.freeInternal)) return Headroom::OnPoll;
  return Headroom::LowMemory;
}

const char* headroomStr(Headroom h) {
  switch (h) {
    case Headroom::Fits:      return "fits";
    case Headroom::OnPoll:    return "on-poll";
    case Headroom::LowMemory: return "low-memory";
  }
  return "low-memory";
}

const char* otaJobName(OtaJob j) {
  switch (j) {
    case OtaJob::Check:   return "otacheck";
    case OtaJob::Install: return "otainstall";
    case OtaJob::None:    break;
  }
  return "";
}

bool OnPollSlot::stage(OtaJob j) {
  if (j == OtaJob::None) return false;
  uint8_t expected = 0;
  return state_.compare_exchange_strong(expected, static_cast<uint8_t>(j));
}

bool OnPollSlot::pending() const {
  const uint8_t s = state_.load();
  return s != 0 && !(s & kRunning);
}

OtaJob OnPollSlot::job() const { return static_cast<OtaJob>(state_.load() & ~kRunning); }

OtaJob OnPollSlot::take() {
  uint8_t s = state_.load();
  if (s == 0 || (s & kRunning)) return OtaJob::None;
  if (!state_.compare_exchange_strong(s, static_cast<uint8_t>(s | kRunning))) return OtaJob::None;
  return static_cast<OtaJob>(s);
}

OtaJob OnPollSlot::cancel() {
  uint8_t s = state_.load();
  if (s == 0 || (s & kRunning)) return OtaJob::None;
  if (!state_.compare_exchange_strong(s, 0)) return OtaJob::None;
  return static_cast<OtaJob>(s);
}

void OnPollSlot::done() {
  uint8_t s = state_.load();
  if (s & kRunning) state_.compare_exchange_strong(s, 0);
}

Route routeOtaWork(const RouteHooks& h, OnPollSlot& slot, OtaJob job) {
  if (!h.snapshot || job == OtaJob::None) return Route::LowMemory;   // never guess
  HeadroomInput in = h.snapshot(h.ctx);
  Headroom d = decideHeadroom(in);
  if (d == Headroom::Fits) {
    if (h.spawn && h.spawn(h.ctx)) return Route::Spawned;
    // The gate passed but the create failed (the heap moved between the two): the
    // same memory refusal, so the poll task gets its chance here too.
    in = h.snapshot(h.ctx);
    in.spawnFailed = true;
    d = decideHeadroom(in);
    if (d != Headroom::OnPoll) return Route::SpawnFailed;
  }
  if (d == Headroom::LowMemory) return Route::LowMemory;
  return slot.stage(job) ? Route::OnPoll : Route::Busy;
}

}  // namespace ota
}  // namespace nimbus
