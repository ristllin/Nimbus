#include "sys/config_nvs.h"

#include <nvs.h>   // nvs_get_stats - partition usage for /api/state + STATUS (CUM-479)
#include <solide/memory.h>

#include "nimbus/config_store.h"
#include "nimbus/device_identity.h"
#include "sys/agent_log.h"   // alogcf + errlog::cat::kNvs - the one persist-failure line

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

// ---- persist ledger (CUM-479) -------------------------------------------------
// Only failures touch it; the spinlock guards a fixed ring (no heap, no logging
// inside the critical section). Logging happens after the lock is released.
nimbus::store::PersistLedger s_ledger;
portMUX_TYPE s_ledgerMux = portMUX_INITIALIZER_UNLOCKED;
constexpr uint32_t kPersistLogQuietMs = 60000;   // one line per failing key per minute

// The last log time of the few keys that failed most recently. Two writers failing in
// turn (the usage ledger and the Telegram offset on a full device) each keep their own
// quiet window instead of resetting one shared slot on every alternation.
struct LoggedKey {
  char     key[nimbus::store::PersistLedger::kKeyMax];
  uint32_t ms;
};
LoggedKey s_logged[4] = {};
size_t    s_loggedNext = 0;

uintptr_t currentTask() { return (uintptr_t)xTaskGetCurrentTaskHandle(); }

// True when this failure should produce a log line: a key not logged within the quiet
// window. Decided under the lock (fixed table, no heap).
bool shouldLogLocked(const char* key, uint32_t now) {
  for (LoggedKey& e : s_logged) {
    if (strncmp(key, e.key, sizeof(e.key)) != 0) continue;
    if ((uint32_t)(now - e.ms) < kPersistLogQuietMs) return false;
    e.ms = now;
    return true;
  }
  LoggedKey& slot = s_logged[s_loggedNext++ % (sizeof(s_logged) / sizeof(s_logged[0]))];
  strncpy(slot.key, key, sizeof(slot.key) - 1);
  slot.key[sizeof(slot.key) - 1] = 0;
  slot.ms = now;
  return true;
}
}  // namespace

// One NVS int save, reported: never written when NVS is not open (the short-circuit),
// and a new key never spends the reserve (a rewrite of a stored int never grows).
static bool putIntNoted(const char* key, int32_t v) {
  const bool admitted =
      nvsReady() && nvsAdmit(kNvsNsSolide, key, nimbus::store::NvsValKind::Int, 0);
  return notePersist(admitted && solide::memory::setInt(key, v), key);
}

// Every failure, for /api/state nvs.recentFails (under s_ledgerMux), and the owner
// notice it raises (atomic; the Telegram poll task sends it on its own outbound path,
// never the failing writer's task).
static nimbus::store::RecentFails s_recentFails;
static nimbus::store::OnceNotice  s_storageNotice;

bool notePersist(bool ok, const char* key) {
  if (ok) return true;
  const char* k = key ? key : "?";
  const uint32_t now = millis();
  const uintptr_t task = currentTask();   // no FreeRTOS call inside the critical section
  portENTER_CRITICAL(&s_ledgerMux);
  s_ledger.noteFailure(task, k);
  s_recentFails.note(k, now);
  const bool log = shouldLogLocked(k, now);
  portEXIT_CRITICAL(&s_ledgerMux);
  s_storageNotice.raise();
  if (log) {
    nimbus::store::NvsUsage u;
    nvsUsage(u);
    // The key in brackets, not after "key=": the log redactor masks a key= value.
    agent::alogcf(nimbus::errlog::cat::kNvs, nimbus::store::kNvsWriteFailedFmt, k,
                  (unsigned)u.freeBytes, (unsigned)u.totalBytes);
  }
  return false;
}

size_t recentPersistFails(nimbus::store::RecentFails::Entry* out, size_t max) {
  portENTER_CRITICAL(&s_ledgerMux);
  const size_t n = s_recentFails.snapshot(out, max);
  portEXIT_CRITICAL(&s_ledgerMux);
  return n;
}

bool takeStorageNotice() { return s_storageNotice.take(); }

SaveScope::SaveScope() : since_(0), task_(currentTask()) {
  portENTER_CRITICAL(&s_ledgerMux);
  since_ = s_ledger.seq();
  portEXIT_CRITICAL(&s_ledgerMux);
}

bool SaveScope::failed() const {
  portENTER_CRITICAL(&s_ledgerMux);
  const bool f = s_ledger.failedSince(since_, task_);
  portEXIT_CRITICAL(&s_ledgerMux);
  return f;
}

bool nvsUsage(nimbus::store::NvsUsage& out) {
  out = nimbus::store::NvsUsage{};
  nvs_stats_t st = {};
  if (nvs_get_stats(nullptr, &st) != ESP_OK) return false;
  out = nimbus::store::nvsUsageFromEntries(st.used_entries, st.available_entries,
                                           st.total_entries);
  return true;
}

uint32_t persistFailures() {
  portENTER_CRITICAL(&s_ledgerMux);
  const uint32_t n = s_ledger.seq();
  portEXIT_CRITICAL(&s_ledgerMux);
  return n;
}

String lastPersistFailKey() {
  char k[nimbus::store::PersistLedger::kKeyMax];
  portENTER_CRITICAL(&s_ledgerMux);
  strncpy(k, s_ledger.lastKey(), sizeof(k) - 1);
  portEXIT_CRITICAL(&s_ledgerMux);
  k[sizeof(k) - 1] = 0;
  return String(k);
}

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
  // of truth for everything else, so its result is what we return. A mirror write
  // that did not land is still logged by name (CUM-479).
  saveProfile(cfg.profile());
  return wrote;
}

bool saveProfile(ProfileId p) { return putIntNoted(kProfKey, int32_t(p)); }

Mode loadMode(Mode def) {
  if (!nvsReady()) return def;
  int32_t v = solide::memory::getInt(kModeKey, int32_t(def));
  return (v == int32_t(Mode::Orchestrator)) ? Mode::Orchestrator : Mode::Notifier;
}

bool saveMode(Mode m) { return putIntNoted(kModeKey, int32_t(m)); }

bool loadBleEnabled(bool def) {
  if (!nvsReady()) return def;
  return solide::memory::getInt(kBleKey, def ? 1 : 0) != 0;
}

bool saveBleEnabled(bool enabled) { return putIntNoted(kBleKey, enabled ? 1 : 0); }

String deviceName() {
  if (!nvsReady()) return String("");
  return nvsGetStr(kNameKey);   // unset on a fresh device: a normal state
}

String saveDeviceName(const String& raw) {
  const String clean(identity::sanitizeName(std::string(raw.c_str())).c_str());
  // "" means "not assigned": stored as an erase, which only frees NVS entries, so
  // clearing the name lands even on a full partition (nimbus::store::writeOrErase).
  NvsKv kv;
  notePersist(nvsReady() && nimbus::store::writeOrErase(kv, kNameKey, clean), kNameKey);
  return clean;
}

}  // namespace nimbus::sys
