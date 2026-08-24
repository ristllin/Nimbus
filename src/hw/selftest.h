#pragma once
#include <Arduino.h>
#include <vector>

#include "nimbus/power/power_monitor.h"
#include "nimbus/power/battery_model.h"

// hw::selftest - a device HEALTH CHECK that aggregates every existing probe seam
// (boot HAL, fault mask, SD, heap/PSRAM, battery, WiFi/BLE, SFX tier, and - only
// when audible is allowed - the acoustic speaker+mic loopback and mic level) into
// one structured result. Surfaced three ways: the settings-menu Self-test screen,
// the token-gated /api/selftest endpoint, and the orchestrator's device.selftest
// tool. SILENT items are always safe; AUDIBLE items are gated by the caller
// (sfx::isSilent() + store::allowHwTests()) so a silent device never blares.
//
// Device layer (Arduino + solide::): the caller (main.cpp / webui) fills the bits
// it owns (HAL flags, battery sample, radio state) and the engine probes the
// globally-accessible seams itself.

namespace nimbus::hw {

struct SelfTestItem {
  const char* name;
  enum Status : uint8_t { Pass = 0, Fail, Skip } status = Skip;
  String detail;
};

// Caller-owned state the engine can't reach on its own.
struct SelfTestInputs {
  // solide::begin() BeginResult (captured in main.cpp g_hal).
  bool halDisplay = true, halLeds = true, halStorage = true, halMemory = true;
  bool halTouch = true;   // color touch panel (the only input device)
  nimbus::power::Sample battery;   // g_power.last()
  nimbus::power::BatteryEstimate batteryEst;   // analytics (time-left / health)
  bool wifiConnected = false;  int wifiRssi = 0;
  bool bleAdvertising = false; int bleBonds = 0;
  bool orchMode = false;           // BLE checks are Notifier-only
};

// Run the check. audible=true runs the speaker/mic acoustic tests (the caller
// must have already confirmed they're allowed). Bounded + returns quickly for
// the silent set; audible adds ~1-2 s for the loopback.
std::vector<SelfTestItem> runSelfTest(const SelfTestInputs& in, bool audible);

// Compact renderers. json() = {"items":[{"name","status","detail"}...],
// "pass":n,"fail":n,"skip":n}; text() = one "name: STATUS detail" line each.
String selfTestJson(const std::vector<SelfTestItem>& items);
String selfTestText(const std::vector<SelfTestItem>& items);

// One-line summary for STATUS ("selftest=3P/0F/2S").
String selfTestSummary(const std::vector<SelfTestItem>& items);

// ---- shared seam: main.cpp owns the live device state (g_hal, g_power, radios),
// so it installs a provider and every surface (menu / /api/selftest / device.*
// tools / console SELFTEST) calls runNow()/deviceStatusJson() without reaching
// into main's globals. Safe before the provider is set (returns defaults).
using InputsProvider = SelfTestInputs (*)();
void setInputsProvider(InputsProvider p);

// Gather inputs (via the provider) and run. audible must already be permitted.
std::vector<SelfTestItem> runNow(bool audible);

// Compact SILENT device snapshot for the orchestrator's device.status tool +
// the web/console - heap, psram, SD free/total, battery, WiFi, BLE, faults, sfx.
// Never makes a sound; safe on any task.
String deviceStatusJson();

// Local wall-clock "YYYY-MM-DD Day HH:MM TZ", or "" while SNTP has never
// synced. Shared temporal grounding for device.status, the World prompt, and
// sub-agent task briefs (owner 2026-07-24 - the model had no clock at all).
String localTimeStr();

}  // namespace nimbus::hw
