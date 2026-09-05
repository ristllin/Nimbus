#pragma once
#include "nimbus/power/power_monitor.h"

// power_battery_adc - a concrete power::Monitor that reads the PACK voltage through a
// resistor divider on an ADC1 pin. This is the simple resistor-divider alternative to
// the I2C fuel gauge in power_fuelgauge.h (no extra chip; coarser SoC).
//
// Wiring (board hardware.md): tap BAT+ *before* the DC-DC (the regulated 5 V rail is
// flat and tells you nothing). e.g. a 2S pack (6.0-8.4 V):
//     BAT+ ── Rtop(220k) ──┬── Rbot(100k) ── GND
//                          └── GPIO4 (ADC1)          (÷3.2)
// ⚠ Use an ADC1 pin (GPIO1-10): WiFi owns ADC2 (GPIO11-20), so ADC2 reads garbage
// while WiFi is up. Free ADC1 spares on the Solide S3: 3, 4, 5, 6, 9.
//
// Compiled ONLY when NIMBUS_HAS_BATTERY_ADC is defined; the default build stays on
// power::NullMonitor (desk-powered, battery UI hidden).

#if defined(NIMBUS_HAS_BATTERY_ADC)
namespace nimbus::hw {

class AdcBatteryMonitor : public power::Monitor {
 public:
  // adcPin:      ADC1 GPIO (1-10) wired to the divider node.
  // dividerX100: (Rtop+Rbot)/Rbot * 100 - e.g. 320 for 220k+100k (÷3.2). >0.
  // cells:       series cell count (2 = 2S pack, 1 = single LiPo). >=1.
  // vbusPin:     digital pin HIGH when USB present, or -1 to infer from voltage.
  bool begin(int adcPin, int dividerX100, int cells, int vbusPin = -1);
  power::Sample sample() override;
  // The last computed PACK mV, latched in sample() BEFORE the plausibility gate -
  // so an open sense line (reads ~0) is distinguishable from a low pack (~7000)
  // over HTTP even though both return an invalid Sample. Diagnostics only.
  uint16_t lastRawPackMv() const override { return lastRawPackMv_; }

 private:
  int      adcPin_ = -1;
  int      dividerX100_ = 320;
  int      cells_ = 1;
  int      vbusPin_ = -1;
  bool     ready_ = false;
  uint16_t lastRawPackMv_ = 0;
};

}  // namespace nimbus::hw
#endif  // NIMBUS_HAS_BATTERY_ADC
