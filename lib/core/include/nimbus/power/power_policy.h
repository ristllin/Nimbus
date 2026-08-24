#pragma once
#include <cstdint>

#include "nimbus/power/power_monitor.h"

// power_policy - the two-threshold battery policy as a pure, host-tested FSM.
//
//   T1 (default 20%): force the Battery Saver profile + warn (attention badge,
//       optional Telegram ping). Exits at T1 + hysteresis or on external power.
//   T2 (default 8%):  request shutdown (flush journals + clean deep sleep).
//       Latched until external power appears.
//   VBUS: debounced presence drives the auto-Desk profile switch.
//
// The policy only decides; the caller wires decisions to Selector::setForced /
// setVbus and executes warnings/shutdown. Invalid samples change nothing.

namespace nimbus::power {

struct PolicyConfig {
  uint8_t  t1Percent = 20;
  uint8_t  t2Percent = 8;
  uint8_t  hystPercent = 5;
  uint32_t vbusDebounceMs = 1500;
  // ---- voltage-grounded T2 (owner feature 2026-07-17: the pack BMS's own
  // undervoltage cut is POOR - measured, it let the pack fall to 5574 mV live).
  // When t2PackMv > 0 it REPLACES the percent test for T2: the study proved the
  // percent scale unreliable at the bottom (the gauge said 0% while the pack
  // really held 33%), so the protection triggers on the measured quantity.
  // Default 6000 mV = ~10% REAL SoC from the measured curve (resting 10% ≈
  // 6034 mV pack; ~30-40 mV idle-load sag → ~6000 live) and sits 430 mV above
  // the observed BMS cut. 0 = the low-batt sleep is OFF entirely.
  uint16_t t2PackMv = 6000;
  // Consecutive below-threshold samples required (an LED burst sags the pack
  // transiently - one dipped reading must not sleep the device).
  uint8_t  t2ConsecN = 3;
  // Owner/AI override: skip the T2 shutdown entirely (drain experiments, or the
  // owner accepting deep-discharge risk). Clears a latched T2.
  bool     t2Override = false;
};

struct PolicyEvents {  // edges fired by exactly one update() call
  bool enterT1 = false;
  bool exitT1 = false;
  bool enterT2 = false;
  bool vbusConnected = false;
  bool vbusDisconnected = false;
};

class Policy {
 public:
  explicit Policy(const PolicyConfig& cfg = {}) : cfg_(cfg) {}

  PolicyEvents update(const Sample& s, uint32_t nowMs);

  bool forcedBatterySaver() const { return inT1_; }
  bool shutdownRequested() const { return inT2_; }
  bool onVbus() const { return vbusStable_; }

  // Live-adjustable (web/AI config): threshold + override without rebuilding.
  void setT2PackMv(uint16_t mv) { cfg_.t2PackMv = mv; belowN_ = 0; }
  void setT2Override(bool on)   { cfg_.t2Override = on; if (on) { inT2_ = false; belowN_ = 0; } }
  uint16_t t2PackMv() const     { return cfg_.t2PackMv; }
  // Post-sleep grace decided "genuinely recovered": drop a latched T2 so the
  // execution side doesn't re-fire on stale state. Re-enters normally if the
  // pack sags below the threshold again.
  void clearT2()                { inT2_ = false; belowN_ = 0; }
  bool t2Override() const       { return cfg_.t2Override; }

 private:
  PolicyConfig cfg_;
  bool inT1_ = false;
  bool inT2_ = false;
  bool vbusStable_ = false;   // debounced state
  bool vbusRaw_ = false;      // last raw reading
  uint32_t vbusRawSince_ = 0; // when the raw reading last changed
  bool vbusInit_ = false;
  uint8_t belowN_ = 0;        // consecutive samples below t2PackMv
};

// ── low-battery owner-alert gate (field bug 2026-08-11) ─────────────────────
// The Policy's T1 edge legitimately RE-FIRES: on hysteresis dither (a high-IR
// near-empty pack rests up past exit and sags back under load), on any charger
// blip, and - worst - on every reboot, including the T2 sleep's 5-minute
// charger-sniff wakes, each of which is a full boot with fresh RAM state. The
// Telegram ping rode the raw edge with no memory, so a parked low board pinged
// the owner "battery low" every wake cycle indefinitely. This gate gives the
// PING its own memory: at most one ping per cooldown window, persisted across
// reboots by the caller (a single epoch), re-armed only by external power.
class AlertGate {
 public:
  static constexpr uint32_t kDefaultCooldownS = 6 * 3600;
  // Epochs below this are "clock not set" (pre-SNTP boot): cooldown math is
  // impossible, so a PRIOR ping (persisted epoch != 0) suppresses, and a
  // never-pinged gate allows exactly one (the genuine first alert).
  static constexpr uint32_t kSaneEpoch = 1767225600;  // 2026-01-01
  // thresholdPct is on the CALIBRATED scale (owner 2026-08-12: alert at 20%
  // REAL charge - the raw T1 edge fires around ~45% real, and at raw-zero the
  // pack still holds ~33%).
  static constexpr uint8_t kDefaultThresholdPct = 20;
  explicit AlertGate(uint32_t persistedLastPingEpoch = 0,
                     uint32_t cooldownS = kDefaultCooldownS,
                     uint8_t thresholdPct = kDefaultThresholdPct)
      : lastPingEpoch_(persistedLastPingEpoch), cooldownS_(cooldownS),
        thresholdPct_(thresholdPct) {}

  // External power re-arms: the discharge episode ended, so the NEXT descent
  // through the threshold is news again and may ping immediately.
  void noteExternalPower() { lastPingEpoch_ = 0; bootPinged_ = false; }

  // Call every battery-telemetry tick (NOT on a policy edge - at the raw T1
  // edge the calibrated SoC is still ~45%, and no later edge exists when it
  // reaches the threshold). True => send the ping now (state recorded).
  bool shouldPing(uint8_t calibratedPct, bool discharging, uint32_t nowEpoch);

  // Persist this whenever it changes; feed it back to the constructor at boot.
  uint32_t persistEpoch() const { return lastPingEpoch_; }

 private:
  uint32_t lastPingEpoch_;      // 0 = armed (never pinged / re-armed by charge)
  uint8_t  thresholdPct_ = kDefaultThresholdPct;
  uint32_t cooldownS_;
  bool     bootPinged_ = false; // unsynced-clock fallback: at most one per boot
};

// ── post-sleep wake decision (owner refinement 2026-07-17) ──────────────────────
// A pack RESTS UPWARD after the load drops (measured: a pack at the 6000 sleep
// floor rests to 6918-6992 mV), so "am I above the sleep threshold?" oscillates:
// sleep -> rest -> wake "recovered" -> drain -> sleep, forever. The wake bar is
// therefore SEPARATE and higher: stay awake only above wakeMv, or when charging
// is positively inferred. A KNOB wake is a human - they get their grace window
// regardless; this decides what happens AFTER it.
// Default 6500 (OWNER'S CALL 2026-07-17, eyes open): this sits BELOW the measured
// rested-empty band (6918-6992 mV), so a slept pack rests back over the bar and
// wakes into further ~1-2 h drain cycles, parking nearer ~3-4% real than 10% -
// more usable bottom-end runtime traded for deep-discharge wear. 7200 (above the
// rested-empty band) is the strict no-rewake setting; per-board tunable (wakeMv).
constexpr uint16_t kWakeMvDefault = 6500;

inline bool stayAwakeAfterSleep(uint16_t packMv, uint16_t sleepMv, uint16_t wakeMv,
                                bool charging) {
  if (charging) return true;                  // a charger is the definitive signal
  if (packMv == 0) return false;              // no reading: assume still empty
  uint16_t bar = wakeMv;
  if (bar <= sleepMv) bar = sleepMv;          // misconfig guard: never below sleep
  return packMv >= bar;
}

}  // namespace nimbus::power
