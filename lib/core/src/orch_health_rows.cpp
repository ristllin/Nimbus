#include "nimbus/orch/health_rows.h"

namespace nimbus {
namespace orch {

bool settingsStorageRow(const SettingsStorageInputs& in, HealthRow& out) {
  const bool low = in.freeEntries < in.reserveEntries;
  if (!low && in.failsSinceBoot == 0 && !in.starved) return false;
  out.key = "nvs";
  out.label = "Settings storage";
  out.state = "degraded";
  std::string d;
  if (in.failsSinceBoot) {
    d = std::to_string(in.failsSinceBoot) + (in.failsSinceBoot == 1 ? " save" : " saves") +
        " failed since start";
    if (!in.lastFailKey.empty()) d += ", last " + in.lastFailKey;
    d += ", ";
  }
  d += std::to_string(in.freeEntries) + (in.freeEntries == 1 ? " entry" : " entries") + " free";
  if (low) d += " (under the reserve of " + std::to_string(in.reserveEntries) + ")";
  if (in.starved) d += "; the reserve cannot be restored";
  d += ". Remove connectors you don't use to free space.";
  out.detail = d;
  return true;
}

HealthRow providersRow(ProviderPark& park, uint32_t nowMs) {
  HealthRow r;
  r.key = "providers";
  r.label = "Providers";
  ParkEntry e[ProviderPark::kMaxProviders];
  const size_t n = park.snapshot(e, ProviderPark::kMaxProviders, nowMs);
  std::string d;
  const uint8_t all = park.pausedBy(kPauseAllProviders);
  if (all) {
    d = "paid requests to every provider are paused";
    if (all & kPauseSpendCeiling) d += ": the monthly spend limit is reached";
  }
  for (size_t i = 0; i < n && !all; i++) {
    ParkVerdict v;
    v.open = e[i].kind == ParkKind::None && !e[i].paused;
    if (v.open) continue;
    v.kind = e[i].kind;
    v.paused = e[i].paused;
    v.httpStatus = e[i].httpStatus;
    v.recheckInMs = e[i].recheckInMs;
    if (!d.empty()) d += "; ";
    d += parkWords(e[i].provider, v);
  }
  r.state = d.empty() ? "ok" : "degraded";
  r.detail = d.empty() ? "every provider open" : d;
  return r;
}

std::string healthRowsText(const std::vector<HealthRow>& rows) {
  std::string out;
  for (const HealthRow& r : rows) out += r.label + " | " + r.state + " | " + r.detail + "\n";
  return out;
}

}  // namespace orch
}  // namespace nimbus
