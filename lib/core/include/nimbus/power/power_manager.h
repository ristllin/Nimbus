#pragma once
#include <cstdint>

#include "nimbus/power/power_monitor.h"
#include "nimbus/power/power_policy.h"
#include "nimbus/profile.h"  // Selector

// power_manager - the portable coordinator that ties a Monitor, the two-
// threshold Policy, and the profile Selector together, and tells the device
// what to do each tick. Host-tested via SimMonitor; the device just supplies a
// concrete Monitor and executes the returned actions (warn/badge, telemetry
// refresh, clean deep sleep) and re-applies the resolved profile.
//
// With a NullMonitor (no battery hardware) every sample is invalid: the policy
// never fires, no action is ever raised, and the device runs as desk-powered.

namespace nimbus::power {

struct ManagerActions {
  bool profileChanged = false;  // Selector forced/VBUS changed -> re-apply config
  bool warnT1 = false;          // entered T1: show low-battery badge + optional ping
  bool clearedT1 = false;       // left T1 (recovered / external power)
  bool shutdownT2 = false;      // entered T2: flush journals + clean deep sleep
  bool telemetryDue = false;    // time to refresh on-screen/web telemetry
};

class Manager {
 public:
  Manager(Monitor* mon, Selector* sel, const PolicyConfig& pc = {})
      : mon_(mon), sel_(sel), policy_(pc) {}

  // Telemetry cadence in ms (0 = never emit telemetryDue). Driven from the
  // effective TelemetryPeriodS config on device.
  void setTelemetryPeriodMs(uint32_t ms) { telemetryMs_ = ms; }

  // Whether VBUS presence should auto-force the Desk profile. TRUE for a real
  // battery device (plugged in -> Desk). MUST be FALSE when there is no battery
  // hardware (NullMonitor always reports external power, which would otherwise
  // pin the profile to Desk forever and silently ignore the user's pick). T1
  // Battery-Saver forcing is unaffected (it needs valid telemetry, which a
  // NullMonitor never has). Default true preserves battery-device behaviour.
  void setVbusAutoProfile(bool on) { vbusAuto_ = on; }

  // Owner control over the T1 battery-mode switch. When a low-battery warning
  // latches, the selector is FORCED to the Battery Saver mode (Dark, ring
  // brightness 10) so a nearly-flat pack is not spent on lights. That has always
  // been unconditional; this makes it a setting.
  // ⚠ Default TRUE deliberately: this is SHIPPED behaviour, so defaulting it off
  // here would silently remove a power saving from every existing device. The
  // owner-facing default lives in the device layer, not in the core.
  // Turning it off does NOT weaken protection: the warning badge, the Telegram
  // ping, the sound event and the T2 deep sleep are all untouched.
  void setAutoSaverOnLow(bool on) { autoSaver_ = on; }

  ManagerActions tick(uint32_t nowMs);

  Sample last() const { return last_; }
  bool onVbus() const { return policy_.onVbus(); }
  // Live access for runtime-configurable protection (sleep threshold/override).
  Policy& policyRef() { return policy_; }
  bool forced() const { return policy_.forcedBatterySaver(); }

 private:
  Monitor* mon_;
  Selector* sel_;
  Policy    policy_;
  Sample    last_;
  uint32_t  telemetryMs_ = 120000;
  uint32_t  lastTelemetryMs_ = 0;
  bool      telemetryInit_ = false;
  bool      vbusAuto_ = true;   // VBUS auto-Desk (off without battery hardware)
  bool      autoSaver_ = true;  // T1 forces the Battery Saver mode (shipped default)
};

}  // namespace nimbus::power
