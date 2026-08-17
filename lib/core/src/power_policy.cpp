#include "nimbus/power/power_policy.h"

namespace nimbus::power {

PolicyEvents Policy::update(const Sample& s, uint32_t nowMs) {
  PolicyEvents ev;

  // ---- VBUS debounce (valid even without battery telemetry) ----
  if (!vbusInit_) {
    vbusInit_ = true;
    vbusRaw_ = s.onExternalPower;
    vbusRawSince_ = nowMs;
    vbusStable_ = s.onExternalPower;  // first reading seeds the stable state
  } else if (s.onExternalPower != vbusRaw_) {
    vbusRaw_ = s.onExternalPower;
    vbusRawSince_ = nowMs;
  } else if (vbusRaw_ != vbusStable_ && nowMs - vbusRawSince_ >= cfg_.vbusDebounceMs) {
    vbusStable_ = vbusRaw_;
    if (vbusStable_) ev.vbusConnected = true; else ev.vbusDisconnected = true;
  }

  // ---- Battery thresholds (need valid telemetry) ----
  if (s.valid) {
    if (vbusStable_ || s.charging) {
      // External power clears both latches.
      if (inT1_) { inT1_ = false; ev.exitT1 = true; }
      inT2_ = false;
      belowN_ = 0;
    } else {
      // T2 criterion: measured pack VOLTAGE when configured (the percent scale is
      // curve-shaped and was measured reading 0% with a third of the pack left);
      // debounced over t2ConsecN consecutive samples so a transient LED-load sag
      // can't sleep the device. The override skips T2 entirely - the drain
      // harness and an informed owner/AI both need to go below the floor.
      bool t2Hit = false;
      if (cfg_.t2PackMv > 0) {
        if (s.millivolts > 0 && s.millivolts <= cfg_.t2PackMv) {
          if (belowN_ < 255) belowN_++;
        } else {
          belowN_ = 0;
        }
        t2Hit = belowN_ >= cfg_.t2ConsecN;
      } else {
        // t2PackMv == 0 means the low-batt sleep is OFF - full stop. (It used to
        // fall back to the legacy percent-T2, so "off" was secretly a MORE
        // aggressive, undebounced percent trigger on the scale the study proved
        // wrong - review finding 2026-07-17.)
        t2Hit = false;
      }
      if (cfg_.t2Override) { t2Hit = false; inT2_ = false; }
      if (!inT2_ && t2Hit) {
        inT2_ = true;
        ev.enterT2 = true;
        if (!inT1_) { inT1_ = true; ev.enterT1 = true; }  // T2 implies T1
      }
      if (!inT1_ && s.percent <= cfg_.t1Percent) {
        inT1_ = true;
        ev.enterT1 = true;
      } else if (inT1_ && !inT2_ && s.percent >= uint8_t(cfg_.t1Percent + cfg_.hystPercent)) {
        inT1_ = false;
        ev.exitT1 = true;
      }
    }
  }

  return ev;
}

bool AlertGate::shouldPing(uint8_t calibratedPct, bool discharging, uint32_t nowEpoch) {
  // Only a DISCHARGING pack at or under the threshold is alert-worthy: a
  // charging pack passing 20% on the way up is good news, and an invalid /
  // external-power sample must never ping.
  if (!discharging || calibratedPct > thresholdPct_) return false;
  if (nowEpoch < kSaneEpoch) {
    // Clock not set: without real time no cooldown can be computed. A prior
    // ping (persisted from an earlier boot) suppresses - this is exactly the
    // 5-minute wake-sniff loop, where every wake is a fresh pre-SNTP boot.
    if (lastPingEpoch_ != 0 || bootPinged_) return false;
    bootPinged_ = true;
    return true;
  }
  if (lastPingEpoch_ != 0 && nowEpoch - lastPingEpoch_ < cooldownS_) return false;
  lastPingEpoch_ = nowEpoch;
  bootPinged_ = true;
  return true;
}

}  // namespace nimbus::power
