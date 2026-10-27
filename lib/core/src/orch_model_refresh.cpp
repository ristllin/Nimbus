#include "nimbus/orch/model_refresh.h"

#include <algorithm>
#include <cstring>

#include "nimbus/heap_watch.h"           // kSpawnMarginBytes - the worker spawn gate's room
#include "nimbus/orch/model_catalog.h"   // lane F: the model store header

namespace nimbus {
namespace orch {

int ModelRefreshQueue::indexOf(const char* slug) {
  if (!slug) return -1;
  for (size_t i = 0; i < kProviderSlotCount; ++i)
    if (std::strcmp(slug, kProviderSlots[i].slug) == 0) return (int)i;
  return -1;
}

RefreshAsk ModelRefreshQueue::request(const char* slug, bool keyed, uint32_t nowMs, bool force) {
  const int i = indexOf(slug);
  if (i < 0 || !keyed) return RefreshAsk::NoKey;
  Slot& s = slots_[i];
  if (s.wanted) {
    if (force) s.retry = false;   // due at the next pass
    return RefreshAsk::Pending;
  }
  if (!force && s.ran && (uint32_t)(nowMs - s.lastMs) < kMinGapMs) return RefreshAsk::Recent;
  s.wanted = true;
  return RefreshAsk::Queued;
}

const char* ModelRefreshQueue::due(bool inTurn, bool online, bool busy, bool restingOk,
                                   uint32_t nowMs) const {
  if (inTurn || !online || busy || !restingOk) return nullptr;
  for (size_t k = 0; k < kProviderSlotCount; ++k) {
    const size_t i = (next_ + k) % kProviderSlotCount;
    const Slot& s = slots_[i];
    if (s.wanted && !(s.retry && (int32_t)(nowMs - s.retryAt) < 0)) return kProviderSlots[i].slug;
  }
  return nullptr;
}

void ModelRefreshQueue::started(const char* slug, uint32_t nowMs) {
  const int i = indexOf(slug);
  if (i < 0) return;
  slots_[i].wanted = false;
  slots_[i].retry = false;
  slots_[i].ran = true;
  slots_[i].lastMs = nowMs;
  next_ = ((size_t)i + 1) % kProviderSlotCount;
}

uint32_t ModelRefreshQueue::finished(const char* slug, bool failed, uint32_t nowMs) {
  const int i = indexOf(slug);
  if (i < 0) return 0;
  Slot& s = slots_[i];
  if (!failed) {
    s.fails = 0;
    return 0;
  }
  if (s.fails >= kRetryTries) {   // given up: the next passive pass asks again
    s.fails = 0;
    return 0;
  }
  ++s.fails;
  const uint32_t delay = std::min(kRetryMaxMs, kRetryFirstMs << (s.fails - 1));   // 2 .. 32, then 60 min
  s.wanted = true;
  s.retry = true;
  s.retryAt = nowMs + delay;
  return delay;
}

void ModelRefreshQueue::yielded(const char* slug, uint32_t nowMs) {
  const int i = indexOf(slug);
  if (i < 0) return;
  Slot& s = slots_[i];
  s.wanted = true;   // no try counted: `fails` and the retry delay are for runs that failed
  s.retry = true;
  s.retryAt = nowMs + kYieldHoldMs;
}

bool LocalTurnWait::waiting(uint32_t nowMs) const {
  return queued_.load() != served_.load() && (uint32_t)(nowMs - lastMs_.load()) < kStaleMs;
}

bool ModelRefreshQueue::queued(const char* slug) const {
  const int i = indexOf(slug);
  return i >= 0 && slots_[i].wanted;
}

bool ModelRefreshQueue::retryWaiting(const char* slug, uint32_t nowMs) const {
  const int i = indexOf(slug);
  return i >= 0 && slots_[i].wanted && slots_[i].retry && (int32_t)(nowMs - slots_[i].retryAt) < 0;
}

RefreshRoute refreshRoute(bool inTurn, bool pollAlive, uint32_t intFree, uint32_t liveLargest,
                          uint32_t restingLargest) {
  if (inTurn || !refreshHeapOk(restingLargest)) return RefreshRoute::Wait;
  // The worker spawn gate's room (heap_watch.h spawnVerdict: the stack plus its margin, the turn
  // checked above) and one block for the stack itself.
  const bool taskFits = intFree >= kRefreshTaskStackBytes + heapwatch::kSpawnMarginBytes &&
                        liveLargest >= kRefreshTaskStackBytes + kRefreshTaskSlackBytes;
  if (taskFits) return RefreshRoute::Task;
  return pollAlive && intFree >= kRefreshPollMinFree ? RefreshRoute::Poll : RefreshRoute::Wait;
}

const char* refreshRouteToken(RefreshRoute r) {
  switch (r) {
    case RefreshRoute::Task: return "task";
    case RefreshRoute::Poll: return "poll";
    case RefreshRoute::Wait: return "wait";
  }
  return "wait";
}

bool ModelRefreshQueue::lastStart(const char* slug, uint32_t& atMs) const {
  const int i = indexOf(slug);
  atMs = i >= 0 ? slots_[i].lastMs : 0;
  return i >= 0 && slots_[i].ran;
}

// ---- the stamp -----------------------------------------------------------------

ModelsStamp stampFromStore(const std::string& storeText) {
  ModelsStamp s;
  ModelStoreMeta meta;
  if (storeText.empty() || !parseModelStoreHeader(storeText, meta)) return s;
  s.present = true;
  s.fetchedAt = meta.fetchedAt;
  s.fw = meta.fw;
  s.src = meta.src;
  return s;
}

// ---- the passive refresh ---------------------------------------------------------

PassiveWhy passiveRefreshDue(const ModelsStamp& s, const std::string& runningFw, uint32_t nowEpoch) {
  if (!s.present) return PassiveWhy::NoStamp;
  if (s.fw != runningFw) return PassiveWhy::Firmware;
  if (s.src == "legacy") return PassiveWhy::Legacy;   // merged at boot, never fetched
  if (!nowEpoch) return PassiveWhy::None;   // no clock: no age can be judged
  if (!s.fetchedAt) return PassiveWhy::UnknownAge;
  if (nowEpoch >= s.fetchedAt && nowEpoch - s.fetchedAt >= kPassiveMaxAgeS) return PassiveWhy::Stale;
  return PassiveWhy::None;
}

const char* passiveWhyToken(PassiveWhy w) {
  switch (w) {
    case PassiveWhy::None:       return "none";
    case PassiveWhy::NoStamp:    return "no-stamp";
    case PassiveWhy::Firmware:   return "firmware";
    case PassiveWhy::Stale:      return "stale";
    case PassiveWhy::UnknownAge: return "unknown-age";
    case PassiveWhy::Legacy:     return "legacy";
  }
  return "none";
}

bool passiveWants(bool keyed, int8_t verifyVerdict, const ModelsStamp& s, const std::string& runningFw,
                  uint32_t nowEpoch) {
  return keyed && verifyVerdict != 0 && passiveRefreshDue(s, runningFw, nowEpoch) != PassiveWhy::None;
}

}  // namespace orch
}  // namespace nimbus
