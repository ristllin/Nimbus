#pragma once
#include <Arduino.h>   // String - the deviceName accessors below

#include <solide/memory.h>

#include "nimbus/profile.h"
#include "nimbus/store/persist.h"   // PersistLedger + NvsUsage (CUM-479, host-tested)
#include "nimbus/store/nvs_diag.h"  // RecentFails + the failure log line (host-tested)
#include "sys/nvs_guard.h"          // the reserve: nvsAdmit before a write that grows NVS

// config_nvs - device persistence glue for the portable Config and the active
// operating Mode (plan §3.1 profiles, §3.6 mode_manager: "NVS-persisted,
// switchable via menu/web").
//
// This is the thin device edge: the versioned, host-tested serialization lives
// in nimbus/config_store (serialize/deserialize); here we only move bytes to and
// from solide::memory. The Config blob rides the SD-backed blob store; the Mode
// is a single int in the NVS namespace. Everything degrades gracefully - with no
// backing store (SD absent / NVS closed) loads return false/leave defaults and
// saves return false, never crashing the caller.

namespace nimbus::sys {

// Operating mode (plan §3.6). Persisted as an int so it survives reboots.
enum class Mode : uint8_t { Notifier = 0, Orchestrator = 1 };

// Fill out from persisted config. Returns false (out untouched) when nothing is
// stored yet or the blob is corrupt - the caller keeps its defaults.
bool loadConfig(Config& out);

// Serialize and persist cfg. Returns false if the store is unavailable or the
// write fails.
bool saveConfig(const Config& cfg);

// Load the persisted mode, or def when none is stored.
// True when NVS was unavailable or only opened on a retry. When this is set the
// device may be running on DEFAULTS rather than the owner's saved settings - which
// silently changed a board's operating mode in the field - so it is surfaced in
// STATUS / /api/state instead of being invisible.
bool nvsDegraded();

Mode loadMode(Mode def = Mode::Notifier);

// Persist the active mode. Returns false if the store is unavailable or the write
// did not land (a full NVS).
bool saveMode(Mode m);

// Persist the battery-mode (profile) mirror int on its own - the same value
// saveConfig() mirrors. POST /api/config writes it synchronously so a profile pick
// that cannot be stored answers 507 instead of a staged "ok" (CUM-479).
bool saveProfile(ProfileId p);

// BLE advertising enable (Notifier-mode nsn transport). Persisted so the user's
// choice survives reboot; defaults ON (the LED broker needs it). Runtime-applied
// with no reboot - see net::ble::setEnabled.
bool loadBleEnabled(bool def = true);
bool saveBleEnabled(bool enabled);

// Device identity (nimbus::identity, P2): the ONE user-visible name that drives
// the setup-AP SSID ("<name>-setup"), the mDNS hostname, the BLE advertised
// name, and the orchestrator's prompt identity. "" = not yet assigned (first
// boot auto-numbers against visible siblings - wifi_portal resolves + persists).
// Renames are reboot-to-apply (no live softAP/mDNS/BLE rename).
String deviceName();
// Sanitizes (identity::sanitizeName) and persists. An empty/unusable name
// CLEARS the stored value so the next boot re-runs auto-numbering. Returns the
// sanitized value (what was asked to be stored, "" when cleared); a write that did
// not land is reported through the persist ledger below, so a SaveScope sees it.
String saveDeviceName(const String& raw);

// The ONE string read of the device's NVS settings: an absent key is a normal
// state and returns `def` quietly. Preferences::getString() (under
// solide::memory::getString) logs "[E] ... nvs_get_str len fail: <key> NOT_FOUND"
// for every read of a key that was never written; on a factory-fresh device in
// setup mode (no Wi-Fi saved yet) that was one error line every 5 s from the
// Wi-Fi slot reads alone (lane L31). has() probes the value types silently, so an
// absent key never enters getString's error path. Values are unchanged; only the
// noise goes. test/test_nvs_quiet_reads fails on any other solide::memory string
// read in src/.
inline String nvsGetStr(const char* key, const char* def = "") {
  return solide::memory::has(key) ? solide::memory::getString(key, def) : String(def);
}

// ---- owner-config persistence honesty (CUM-479) -----------------------------
// The solide::memory (NVS) seam the portable write primitives in nimbus/store/
// persist.h run against. NvsKvRaw writes unconditionally: its caller has already
// asked the reserve (agent::store's helpers gate the whole write, including the
// erase-then-write step of writeShrinkSafe). NvsKv asks the reserve on every string
// write (nvs_guard.h), so a caller outside the store (the Wi-Fi list, the device
// name) cannot spend the entries kept for critical writes; never pair NvsKv with
// writeShrinkSafe, whose rewrite after its erase must not be refused.
struct NvsKvRaw {
  bool   setString(const char* k, const String& v) { return solide::memory::setString(k, v); }
  String getString(const char* k, const char* def) { return nvsGetStr(k, def); }
  bool   has(const char* k) { return solide::memory::has(k); }
  bool   erase(const char* k) { return solide::memory::eraseKey(k); }
};
struct NvsKv : NvsKvRaw {
  bool setString(const char* k, const String& v) {
    return nvsAdmit(kNvsNsSolide, k, nimbus::store::NvsValKind::Str, v.length()) &&
           NvsKvRaw::setString(k, v);
  }
};

// Every NVS write made through the store (agent::store setters) and the NVS saves
// above (mode, profile mirror, BLE, device name) reports its verdict here. A failure
// is recorded against the calling FreeRTOS task and in the recent-failure ring, it
// raises the once-per-boot owner notice, and it is logged ("nvs: write FAILED [<k>]
// ...", one line per failing key per minute). The verdict is returned unchanged so a
// setter can `return notePersist(ok, key);`.
bool notePersist(bool ok, const char* key);

// A save route opens one of these BEFORE its first write and asks failed() before
// it answers: true when any NVS write this task made since construction did not land
// (including writes nested inside a setter). Routes then send the 507 storage_full
// reply instead of {"ok":true}. Web routes run one at a time on the AsyncTCP task, so
// the scope sees exactly its own request's writes.
class SaveScope {
 public:
  SaveScope();
  bool failed() const;

 private:
  uint32_t  since_;
  uintptr_t task_;
};

// NVS partition usage (nvs_get_stats over the default partition, every namespace).
// Returns false (out zeroed) when the stats call fails.
bool nvsUsage(nimbus::store::NvsUsage& out);
// Writes that did not land since boot, and the most recent failing key ("" none).
uint32_t persistFailures();
String   lastPersistFailKey();
// The newest failed writes (key, sequence number, time), newest first, for /api/state
// nvs.recentFails. Returns how many entries were copied (at most `max`).
size_t recentPersistFails(nimbus::store::RecentFails::Entry* out, size_t max);
// The once-per-boot "settings storage is almost full" owner notice. A failed write
// raises it; the Telegram poll task takes it (true exactly once) and sends the notice.
bool takeStorageNotice();

}  // namespace nimbus::sys
