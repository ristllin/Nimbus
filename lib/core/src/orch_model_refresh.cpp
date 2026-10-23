#include "nimbus/orch/model_refresh.h"

#include <cstring>

#include "nimbus/orch/model_catalog.h"   // lane F: the model store header

namespace nimbus {
namespace orch {

int ModelRefreshQueue::indexOf(const char* slug) {
  if (!slug) return -1;
  for (size_t i = 0; i < kProviderSlotCount; ++i)
    if (std::strcmp(slug, kProviderSlots[i].slug) == 0) return (int)i;
  return -1;
}

RefreshAsk ModelRefreshQueue::request(const char* slug, bool keyed, uint32_t nowMs) {
  const int i = indexOf(slug);
  if (i < 0 || !keyed) return RefreshAsk::NoKey;
  Slot& s = slots_[i];
  if (s.wanted) return RefreshAsk::Pending;
  if (s.ran && (uint32_t)(nowMs - s.lastMs) < kMinGapMs) return RefreshAsk::Recent;
  s.wanted = true;
  return RefreshAsk::Queued;
}

const char* ModelRefreshQueue::due(bool inTurn, bool online, bool busy, bool restingOk) const {
  if (inTurn || !online || busy || !restingOk) return nullptr;
  for (size_t k = 0; k < kProviderSlotCount; ++k) {
    const size_t i = (next_ + k) % kProviderSlotCount;
    if (slots_[i].wanted) return kProviderSlots[i].slug;
  }
  return nullptr;
}

void ModelRefreshQueue::started(const char* slug, uint32_t nowMs) {
  const int i = indexOf(slug);
  if (i < 0) return;
  slots_[i].wanted = false;
  slots_[i].ran = true;
  slots_[i].lastMs = nowMs;
  next_ = ((size_t)i + 1) % kProviderSlotCount;
}

bool ModelRefreshQueue::queued(const char* slug) const {
  const int i = indexOf(slug);
  return i >= 0 && slots_[i].wanted;
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
  return s;
}

// ---- the passive refresh ---------------------------------------------------------

PassiveWhy passiveRefreshDue(const ModelsStamp& s, const std::string& runningFw, uint32_t nowEpoch) {
  if (!s.present) return PassiveWhy::NoStamp;
  if (s.fw != runningFw) return PassiveWhy::Firmware;
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
  }
  return "none";
}

}  // namespace orch
}  // namespace nimbus
