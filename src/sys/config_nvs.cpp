#include "sys/config_nvs.h"

#include <solide/memory.h>

#include "nimbus/config_store.h"
#include "nimbus/device_identity.h"

namespace nimbus::sys {

namespace {
constexpr char kCfgBlob[] = "nimbus_cfg";   // SD blob: versioned Config
constexpr char kModeKey[] = "nimbus_mode";  // NVS int: active Mode (<=15 chars)
constexpr char kProfKey[] = "nimbus_prof";  // NVS int: battery mode (profile), a
                                            // standalone mirror of Config.profile so
                                            // it survives a config-blob version bump
constexpr char kBleKey[]  = "nimbus_ble";   // NVS int: BLE advertising enable
constexpr char kNameKey[] = "nimbus_name";  // NVS str: device identity ("" = auto)

// solide::memory::begin() latches its ok flag ONCE. If that single open fails -
// observed live on a board that came up in the WRONG operating mode - the flag stays
// false for the entire run: every getter silently returns its default and every setter
// silently fails. The device then boots with all settings at defaults AND cannot
// persist a correction, signalled only by one Serial line that a production
// (silent-serial) build never prints.
//
// So retry the open before believing a failure, and remember that it happened so the
// degraded state is reportable instead of invisible.
bool s_nvsRetried = false;

bool nvsReady() {
  if (solide::memory::ok()) return true;
  // One re-open attempt. NVS is a flash driver, not a network: if this fails too the
  // partition is genuinely unavailable and defaults really are all we have.
  s_nvsRetried = true;
  return solide::memory::begin("solide");
}
}  // namespace

// True when NVS is unavailable, or was only reachable after a retry. Either way the
// device may be running on defaults rather than the owner's saved settings, which is
// worth surfacing rather than hiding.
bool nvsDegraded() { return !solide::memory::ok() || s_nvsRetried; }

bool loadConfig(Config& out) {
  if (!nvsReady()) return false;
  uint8_t buf[kConfigMaxBytes];
  size_t n = solide::memory::getBlob(kCfgBlob, buf, sizeof(buf));
  const bool ok = (n != 0) && deserializeConfig(buf, n, out);
  // Overlay the standalone battery-mode int. The profile lives inside the
  // versioned Config blob, which deserializeConfig rejects wholesale on a version
  // bump or corruption - that silently reverted the owner's battery mode to the
  // compiled default (Balanced) on the next firmware update, while the operating
  // Mode (its own NVS int) survived. Mirroring the profile the same way keeps the
  // two in step. Applied even when the blob is absent/rejected so the battery mode
  // is restored regardless.
  const int32_t pv = solide::memory::getInt(kProfKey, -1);
  if (pv >= int32_t(ProfileId::BatterySaver) && pv <= int32_t(ProfileId::Desk))
    out.setProfile(ProfileId(pv));
  return ok;
}

bool saveConfig(const Config& cfg) {
  if (!nvsReady()) return false;
  uint8_t buf[kConfigMaxBytes];
  size_t n = serializeConfig(cfg, buf, sizeof(buf));
  if (n == 0) return false;
  const bool wrote = solide::memory::putBlob(kCfgBlob, buf, n);
  // Best-effort standalone mirror (see loadConfig); the blob write is the source
  // of truth for everything else, so its result is what we return.
  solide::memory::setInt(kProfKey, int32_t(cfg.profile()));
  return wrote;
}

Mode loadMode(Mode def) {
  if (!nvsReady()) return def;
  int32_t v = solide::memory::getInt(kModeKey, int32_t(def));
  return (v == int32_t(Mode::Orchestrator)) ? Mode::Orchestrator : Mode::Notifier;
}

bool saveMode(Mode m) {
  if (!nvsReady()) return false;
  return solide::memory::setInt(kModeKey, int32_t(m));
}

bool loadBleEnabled(bool def) {
  if (!nvsReady()) return def;
  return solide::memory::getInt(kBleKey, def ? 1 : 0) != 0;
}

bool saveBleEnabled(bool enabled) {
  if (!nvsReady()) return false;
  return solide::memory::setInt(kBleKey, enabled ? 1 : 0);
}

String deviceName() {
  if (!nvsReady()) return String("");
  return solide::memory::getString(kNameKey, "");
}

String saveDeviceName(const String& raw) {
  const std::string clean = identity::sanitizeName(std::string(raw.c_str()));
  if (nvsReady())
    solide::memory::setString(kNameKey, String(clean.c_str()));
  return String(clean.c_str());
}

}  // namespace nimbus::sys
