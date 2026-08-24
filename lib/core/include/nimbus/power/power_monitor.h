#pragma once
#include <cstdint>

// power_monitor - the seam between battery hardware (which does not exist yet)
// and Nimbus's power policy. Firmware ships against this interface today:
//   NullPowerMonitor  - no hardware: reports desk-powered, battery UI hidden.
//   SimPowerMonitor   - host tests + serial debug: scriptable samples.
//   Max17048Monitor   - later, in src/hw/, when a fuel gauge + VBUS sense land
//                       (possibly delivered by solide-drivers; see plan Q1).

namespace nimbus::power {

struct Sample {
  bool     valid = false;          // false: no battery telemetry available
  uint8_t  percent = 0;            // 0-100, meaningful only when valid
  uint16_t millivolts = 0;         // meaningful only when valid
  bool     onExternalPower = true; // VBUS / USB present
  bool     charging = false;
};

class Monitor {
 public:
  virtual ~Monitor() = default;
  virtual Sample sample() = 0;
};

// No battery hardware: behaves as permanently desk-powered.
class NullMonitor : public Monitor {
 public:
  Sample sample() override {
    Sample s;
    s.valid = false;
    s.onExternalPower = true;
    return s;
  }
};

// Scriptable monitor for host tests and the serial debug command: the caller
// sets the sample; sample() returns it verbatim.
class SimMonitor : public Monitor {
 public:
  void set(const Sample& s) { current_ = s; }
  void setBattery(uint8_t percent, bool external = false, bool charging = false) {
    current_.valid = true;
    current_.percent = percent;
    current_.millivolts = uint16_t(3300 + (uint32_t(percent) * 900) / 100);
    current_.onExternalPower = external;
    current_.charging = charging;
  }
  Sample sample() override { return current_; }

 private:
  Sample current_;
};

// Reject an absent/floating divider before it becomes battery telemetry. A real
// Li-ion pack is physically plausible only in this broad per-cell window; the
// normal operating/SoC range (3.2-4.2 V) sits comfortably inside it. This guard
// is deliberately wider than the low-battery sleep floor: a genuinely depleted
// 2S pack at 5.5 V remains valid and protected, while a USB-only board's floating
// ADC reading (live-observed as a false 3.02 V *pack*) is treated like no gauge.
inline bool plausibleLiIonPackMv(uint32_t packMv, int cells) {
  if (cells <= 0) return false;
  const uint32_t cellMv = packMv / uint32_t(cells);
  return cellMv >= 2500 && cellMv <= 4500;
}

// Rough per-cell Li-ion state-of-charge from a RESTING cell voltage (mV). Piecewise-
// linear over the flat Li-ion/LiPo discharge curve (3.20-4.20 V); approximate under
// load (voltage sags), so treat it as a coarse gauge, not a coulomb counter. Clamped
// 0-100. Portable + host-tested; the device's ADC monitor divides the pack voltage by
// the series cell count and calls this. (An I2C fuel gauge like the MAX17048 does this
// modelling in hardware - this is the resistor-divider approximation.)
// The resting per-cell voltage→SoC curve, high-mV first. Both liIonPercent() and its
// inverse read this ONE table.
// ⚠ battery-measurement TODO: these are the generic Li-ion approximation. The
// full-charge→cutoff drain campaign (the Battery Lab tooling) produces a MEASURED
// ADC-reported resting curve for the actual LiitoKala INR18650-35E 2S packs - replace
// the points below with aggregate.py's generated array (then a truly-full pack the S3
// ADC reads ~3950 mV/cell maps to ~100% natively, fixing "reads 73% when full").
struct LiIonCurvePoint { uint16_t mv; uint8_t pct; };
static const LiIonCurvePoint kLiIonCurve[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3900, 68}, {3800, 55},
    {3700, 42},  {3600, 28}, {3500, 15}, {3400, 6},  {3300, 2}, {3200, 0}};
static constexpr int kLiIonCurveN = int(sizeof(kLiIonCurve) / sizeof(kLiIonCurve[0]));

// LiFePO4 (lithium iron phosphate) resting per-cell curve. The chemistry is much
// flatter than Li-ion/LiPo: nominal 3.2 V, resting-full ~3.35 V, cutoff ~2.5 V, so
// its whole usable band sits BELOW the Li-ion ADC-compression knee (3.8 V) - the
// top-band stretch is naturally inert and this curve is used directly. Coarse (a
// resistor-divider gauge on a flat chemistry is approximate by nature); documented
// as such in docs/reference/battery-estimation.md.
static const LiIonCurvePoint kLiFePO4Curve[] = {
    {3400, 100}, {3350, 95}, {3330, 90}, {3320, 80}, {3300, 60}, {3280, 40},
    {3260, 25},  {3230, 15}, {3130, 8},  {3000, 3},  {2500, 0}};
static constexpr int kLiFePO4CurveN = int(sizeof(kLiFePO4Curve) / sizeof(kLiFePO4Curve[0]));

// Battery chemistry an owner can select. The series-cell count (1S/2S) is a
// SEPARATE, orthogonal setting (pack mV / cells = per-cell mV); this picks the
// per-cell voltage->SoC curve + calibration behaviour.
enum class Chemistry : uint8_t { LiIonLipo = 0, LiFePO4 = 1 };
inline const char* chemistrySlug(Chemistry c) {
  return c == Chemistry::LiFePO4 ? "lifepo4" : "liion";
}
// Parse a stored slug; anything unrecognized falls back to Li-ion (the default,
// = current behaviour). Machine slug is frozen once shipped.
inline Chemistry chemistryFromSlug(const char* s) {
  if (s && (s[0] == 'l' || s[0] == 'L') && (s[1] == 'i' || s[1] == 'I') &&
      (s[2] == 'f' || s[2] == 'F'))
    return Chemistry::LiFePO4;
  return Chemistry::LiIonLipo;
}

// Generic piecewise-linear SoC from a RESTING per-cell voltage over any high-mV-
// first curve. liIonPercent() is exactly this over kLiIonCurve; percentFor() for a
// non-Li-ion / custom curve calls it directly. Clamped 0..100.
inline uint8_t socForCurve(uint16_t cellMv, const LiIonCurvePoint* k, int n) {
  if (n <= 0) return 0;
  if (cellMv >= k[0].mv) return k[0].pct;   // at/above the top anchor (100 on standard curves)
  if (cellMv <= k[n - 1].mv) return k[n - 1].pct;
  for (int i = 1; i < n; i++) {
    if (cellMv >= k[i].mv) {  // interp between k[i] (lower mV) and k[i-1] (higher mV)
      long span = k[i - 1].mv - k[i].mv;
      long p = k[i].pct + (long)(cellMv - k[i].mv) * (k[i - 1].pct - k[i].pct) / span;
      return uint8_t(p);
    }
  }
  return 0;
}

// Inverse: the RESTING per-cell voltage (mV) for a target SoC % over any curve.
inline uint16_t mvForPctCurve(uint8_t pct, const LiIonCurvePoint* k, int n) {
  if (n <= 0) return 0;
  if (pct >= k[0].pct) return k[0].mv;
  if (pct <= k[n - 1].pct) return k[n - 1].mv;
  for (int i = 1; i < n; i++) {
    if (pct >= k[i].pct) {  // interp between k[i] (lower pct) and k[i-1] (higher pct)
      long span = k[i - 1].pct - k[i].pct;
      long mv = k[i].mv + (long)(pct - k[i].pct) * (k[i - 1].mv - k[i].mv) / span;
      return uint16_t(mv);
    }
  }
  return k[n - 1].mv;
}

// Curve table for a chemistry (the fallback when no custom curve is configured).
inline void curveFor(Chemistry c, const LiIonCurvePoint*& pts, int& n) {
  if (c == Chemistry::LiFePO4) { pts = kLiFePO4Curve; n = kLiFePO4CurveN; }
  else                         { pts = kLiIonCurve;   n = kLiIonCurveN; }
}

inline uint8_t liIonPercent(uint16_t cellMv) { return socForCurve(cellMv, kLiIonCurve, kLiIonCurveN); }

// Inverse of liIonPercent: the RESTING per-cell voltage (mV) for a target SoC %.
// Used by STORAGE mode to target ~70% (~3.80 V/cell) on a Li-ion pack.
inline uint16_t liIonCellMvForPct(uint8_t pct) { return mvForPctCurve(pct, kLiIonCurve, kLiIonCurveN); }

// Max custom-curve points an owner may configure (bounds the model's storage).
inline constexpr int kMaxCurvePoints = 12;
// Parse an owner custom curve from "mv:pct,mv:pct,..." (high-mV first). Writes at
// most kMaxCurvePoints into out and returns the count, or 0 if the string is
// malformed or not strictly descending in mV with pct monotonic non-increasing
// and within 0..100. Portable + host-tested; no <string>, no allocation.
int parseCurveCsv(const char* csv, LiIonCurvePoint* out, int maxN);

}  // namespace nimbus::power
