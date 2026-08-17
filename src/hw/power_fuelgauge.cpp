#include "hw/power_fuelgauge.h"

#if defined(NIMBUS_HAS_FUEL_GAUGE)

#include <Arduino.h>
#include <Wire.h>

namespace nimbus::hw {

namespace {
constexpr uint8_t kAddr = 0x36;
constexpr uint8_t kRegVCell = 0x02;
constexpr uint8_t kRegSoc = 0x04;
constexpr uint8_t kRegCrate = 0x16;
}  // namespace

bool FuelGaugeMonitor::readReg(uint8_t reg, uint16_t& out) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(int(kAddr), 2) != 2) return false;
  const uint8_t hi = Wire.read();
  const uint8_t lo = Wire.read();
  out = (uint16_t(hi) << 8) | lo;  // MAX17048 is big-endian
  return true;
}

bool FuelGaugeMonitor::begin(int sdaPin, int sclPin, int vbusPin) {
  vbusPin_ = vbusPin;
  if (vbusPin_ >= 0) pinMode(vbusPin_, INPUT);
  Wire.begin(sdaPin, sclPin);
  uint16_t v = 0;
  ready_ = readReg(kRegVCell, v);  // probe presence
  return ready_;
}

power::Sample FuelGaugeMonitor::sample() {
  power::Sample s;
  if (!ready_) return s;  // valid=false -> treated as no telemetry

  uint16_t vcell = 0, soc = 0, crate = 0;
  const bool okV = readReg(kRegVCell, vcell);
  const bool okS = readReg(kRegSoc, soc);
  const bool okC = readReg(kRegCrate, crate);
  // Any of the three failing is a transient bus error -> invalid this tick. CRATE
  // is included because on a no-VBUS-pin board onExternalPower is derived from
  // s.charging (below); a dropped CRATE read would otherwise report a charging
  // device as {valid, onExternalPower=false, charging=false} and let the policy
  // fire a false low-battery warning / T2 shutdown while externally powered.
  if (!okV || !okS || !okC) return s;

  // Plausibility guard against a stuck/floating bus that still clocks in a word
  // (classically all-ones). VCELL 0xFFFF is 5.12 V, impossible for a single LiPo
  // (~4.35 V max), and a SOC integer part above 100% is likewise impossible.
  // Without this a glitched 0xFFFF SOC read would report a full battery
  // (percent=100, valid=true) and mask a genuine near-empty T1/T2 condition.
  if (vcell == 0xFFFF || (soc >> 8) > 100) return s;

  s.valid = true;
  // VCELL LSB = 78.125 µV = 5/64 mV, so mV = raw * 5 / 64 exactly. Computing it
  // this way keeps the intermediate in range: raw*5 <= 65535*5 = 327675 fits in
  // uint32_t, whereas raw*78125 overflows uint32_t for raw > 54975 (~4.295 V) and
  // wrapped to a garbage-low millivolt reading while valid stayed true.
  s.millivolts = uint16_t((uint32_t(vcell) * 5u) >> 6);
  // SOC LSB = 1/256 % ; high byte is the integer percent. Clamp to 100.
  uint16_t pct = soc >> 8;
  s.percent = uint8_t(pct > 100 ? 100 : pct);
  // CRATE is signed; positive => charging.
  s.charging = int16_t(crate) > 0;
  s.onExternalPower = (vbusPin_ >= 0) ? (digitalRead(vbusPin_) == HIGH)
                                      : s.charging;
  return s;
}

}  // namespace nimbus::hw

#endif  // NIMBUS_HAS_FUEL_GAUGE
