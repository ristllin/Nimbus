#include "power_battery_adc.h"

#if defined(NIMBUS_HAS_BATTERY_ADC)
#include <Arduino.h>

namespace nimbus::hw {

bool AdcBatteryMonitor::begin(int adcPin, int dividerX100, int cells, int vbusPin) {
  adcPin_ = adcPin;
  dividerX100_ = dividerX100 > 0 ? dividerX100 : 320;
  cells_ = cells > 0 ? cells : 1;
  vbusPin_ = vbusPin;
  analogReadResolution(12);
  // 11 dB (≈12 dB on S3) → full-scale ~3.1 V, covering the ÷N divider node. The
  // eFuse calibration is applied by analogReadMilliVolts() so raw counts don't matter.
  analogSetPinAttenuation(adcPin_, ADC_11db);
  if (vbusPin_ >= 0) pinMode(vbusPin_, INPUT);
  ready_ = true;
  return true;
}

power::Sample AdcBatteryMonitor::sample() {
  power::Sample s;
  if (!ready_) return s;  // invalid: policy treats it as desk-powered
  // Average a few reads to knock down ADC noise; analogReadMilliVolts applies the
  // per-chip eFuse calibration, so this is already in millivolts at the pin.
  uint32_t acc = 0;
  const int kReads = 8;
  for (int i = 0; i < kReads; i++) acc += analogReadMilliVolts(adcPin_);
  const uint32_t nodeMv = acc / kReads;
  const uint32_t packMv = nodeMv * uint32_t(dividerX100_) / 100;  // undo the divider
  // Latch the computed pack mV for HTTP diagnostics BEFORE the plausibility gate
  // rejects it (an open sense line reads ~0, a real pack ~7000). This is never a
  // policy input - the gate below still owns whether the Sample is valid.
  lastRawPackMv_ = uint16_t(packMv > 65535 ? 65535 : packMv);
  // A carrier without the optional divider leaves GPIO4 floating. It produced a
  // repeatable-looking 3020 mV *pack* reading on a fresh USB-powered TFT board,
  // which the policy correctly interpreted as a dead 2S pack and deep-slept nine
  // seconds into onboarding. Range-check before setting valid: an absent gauge
  // must behave like NullMonitor, while a real low 2S pack (>=5.0 V) still reaches
  // the 6.0 V protection threshold.
  if (!power::plausibleLiIonPackMv(packMv, cells_)) return s;
  const uint16_t cellMv = uint16_t(packMv / uint32_t(cells_));
  s.valid = true;
  s.millivolts = uint16_t(packMv > 65535 ? 65535 : packMv);
  s.percent = power::liIonPercent(cellMv);
  // Charge state: only a WIRED VBUS pin is authoritative here. Without one we do
  // NOT guess from absolute voltage (the S3 ADC under-reads a full pack, so a
  // "cell >= 4.15 V" test both never fires when full AND fabricates a discharge
  // estimate the rest of the time). The BatteryModel infers charging/full/
  // discharging from the voltage TREND instead - see power_battery_model.cpp.
  if (vbusPin_ >= 0) {
    s.onExternalPower = digitalRead(vbusPin_) == HIGH;
    s.charging = s.onExternalPower && s.percent < 99;
  } else {
    s.onExternalPower = false;
    s.charging = false;
  }
  return s;
}

}  // namespace nimbus::hw
#endif  // NIMBUS_HAS_BATTERY_ADC
