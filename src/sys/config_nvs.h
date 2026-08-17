#pragma once
#include <Arduino.h>   // String - the deviceName accessors below

#include "nimbus/profile.h"

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

// Persist the active mode. Returns false if the store is unavailable.
bool saveMode(Mode m);

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
// value actually stored ("" when cleared).
String saveDeviceName(const String& raw);

}  // namespace nimbus::sys
