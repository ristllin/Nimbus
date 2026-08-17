#pragma once
#include "nimbus/power/power_monitor.h"

// power_fuelgauge - a concrete power::Monitor for a MAX17048-class I2C fuel
// gauge plus a VBUS sense pin. This targets hardware that does NOT yet exist on
// the Solide S3 board (solide-drivers v0.1.0 ships no battery hardware), so it
// is compiled ONLY when NIMBUS_HAS_FUEL_GAUGE is defined; the default build uses
// power::NullMonitor and behaves as desk-powered. When a gauge is fitted, define
// the flag and verify the pins in nimbus_config.h against the final board.
//
// MAX17048 (I2C addr 0x36): VCELL @0x02 (78.125 µV/LSB), SOC @0x04 (1/256 %/LSB),
// CRATE @0x16 (signed, 0.208 %/hr per LSB - sign = charging/discharging).

#if defined(NIMBUS_HAS_FUEL_GAUGE)

namespace nimbus::hw {

class FuelGaugeMonitor : public power::Monitor {
 public:
  // sdaPin/sclPin: I2C bus for the gauge. vbusPin: reads HIGH when USB present.
  bool begin(int sdaPin, int sclPin, int vbusPin);
  power::Sample sample() override;

 private:
  bool readReg(uint8_t reg, uint16_t& out);
  int  vbusPin_ = -1;
  bool ready_ = false;
};

}  // namespace nimbus::hw

#endif  // NIMBUS_HAS_FUEL_GAUGE
