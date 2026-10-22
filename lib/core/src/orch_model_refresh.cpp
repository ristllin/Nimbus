#include "nimbus/orch/model_refresh.h"

#include <cstdlib>
#include <cstring>

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

namespace {
// A firmware version kept in a space-separated header: anything outside
// [A-Za-z0-9._+-] becomes '_', and it is bounded.
std::string stampSafe(const std::string& v) {
  std::string out;
  for (char c : v) {
    if (out.size() >= 48) break;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '+' || c == '-';
    out += ok ? c : '_';
  }
  return out;
}
}  // namespace

std::string formatModelsStamp(uint32_t fetchedAt, const std::string& fw) {
  return "#models v1 fetchedAt=" + std::to_string(fetchedAt) + " fw=" + stampSafe(fw) + "\n";
}

bool parseModelsStamp(const std::string& text, ModelsStamp& out) {
  out = ModelsStamp();
  static const char kTag[] = "#models v1";
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    const std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (line.compare(0, sizeof(kTag) - 1, kTag) != 0) continue;
    out.present = true;
    size_t t = sizeof(kTag) - 1;
    while (t < line.size()) {
      while (t < line.size() && (line[t] == ' ' || line[t] == '\r')) ++t;
      size_t e = line.find(' ', t);
      if (e == std::string::npos) e = line.size();
      const std::string kv = line.substr(t, e - t);
      t = e;
      const size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      if (k == "fetchedAt") out.fetchedAt = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
      else if (k == "fw") out.fw = stampSafe(v);
    }
    return true;
  }
  return false;
}

// ---- the passive refresh ---------------------------------------------------------

PassiveWhy passiveRefreshDue(const ModelsStamp& s, const std::string& runningFw, uint32_t nowEpoch) {
  if (!s.present) return PassiveWhy::NoStamp;
  if (s.fw != stampSafe(runningFw)) return PassiveWhy::Firmware;
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
