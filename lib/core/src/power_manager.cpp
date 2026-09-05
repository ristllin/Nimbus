#include "nimbus/power/power_manager.h"

namespace nimbus::power {

ManagerActions Manager::tick(uint32_t nowMs) {
  ManagerActions a;
  last_ = mon_->sample();
  const PolicyEvents ev = policy_.update(last_, nowMs);

  // Drive the profile selector: T1 forces Battery Saver, VBUS auto-picks Desk.
  // The selector's own precedence (forced > VBUS > user) resolves conflicts.
  if (sel_) {
    // autoSaver_ gates ONLY the mode switch. Every other T1 consequence below
    // (warn badge, Telegram ping, sound) and the T2 deep sleep are unconditional
    // - protection is not opt-out-able, only the cosmetic/mode part is.
    sel_->setForced(autoSaver_ && policy_.forcedBatterySaver());
    // VBUS auto-Desk only when enabled (a real battery device). Without battery
    // hardware the monitor always reports external power, so honouring it would
    // pin the profile to Desk forever and ignore the user's chosen profile.
    sel_->setVbus(vbusAuto_ ? policy_.onVbus() : false);
  }

  a.warnT1 = ev.enterT1;
  a.clearedT1 = ev.exitT1;
  a.shutdownT2 = ev.enterT2;
  a.profileChanged = ev.enterT1 || ev.exitT1 || ev.vbusConnected ||
                     ev.vbusDisconnected;

  // Telemetry cadence - only while we actually have battery data.
  if (telemetryMs_ != 0 && last_.valid) {
    if (!telemetryInit_) {
      telemetryInit_ = true;
      lastTelemetryMs_ = nowMs;
      a.telemetryDue = true;  // first valid sample refreshes immediately
    } else if (uint32_t(nowMs - lastTelemetryMs_) >= telemetryMs_) {
      lastTelemetryMs_ = nowMs;
      a.telemetryDue = true;
    }
  }

  // Open-sense-line detector cadence - deliberately NOT behind last_.valid and
  // NOT tied to the (owner-configurable, 60-300 s) telemetry period: the
  // detector counts invalid samples, and a fault must not be silenced by a
  // UI-refresh setting. Fixed 30 s => the detector's 3-check debounce claims an
  // open sense line on the third check, about a minute after boot (battery_sense.h).
  if (!senseInit_) {
    senseInit_ = true;
    lastSenseMs_ = nowMs;
    a.senseTelemetryDue = true;
  } else if (uint32_t(nowMs - lastSenseMs_) >= kSenseCadenceMs) {
    lastSenseMs_ = nowMs;
    a.senseTelemetryDue = true;
  }
  return a;
}

}  // namespace nimbus::power
