#pragma once
#include <cstdint>

// panel_controller - honest liveness for the colour panel's ILI9341 controller.
//
// The health/status "Display (color touch): ok, up" was hardwired to the boot
// begin() result and LIED while the controller was off the SPI bus: the owner's
// nimbus-light showed a black glass while every readback pegged at the all-ones
// idle level (TFTID id=0xFFFFFF, healthy()==0), yet the row still read "ok" with
// zero live measurement. "panel up" from a begin() that succeeded at boot says
// nothing about a controller that stopped answering afterwards.
//
// This is the nimbus-side detector: it reads the controller id register over the
// shared MISO line (solide::display_tft::readReg(0x04, 3), the same RDDID the
// TFTID? console command uses - no pinned-driver change) and trips only on the
// persistent not-answering signature. It is deliberately narrow so a live panel
// never reads as dead:
//   - a plausible, mixed-bit id (a real manufacturer/driver code) is a sign of
//     LIFE and clears the streak;
//   - a single glitched read (bus contention with the render blit walks the value
//     around) does not trip, because the streak needs a debounce window;
//   - ONLY every bit pegged one way across that window (all-ones = MISO stuck
//     high / disconnected, or all-zeros = nothing driving MISO) is treated as a
//     controller that is not on the bus.
//
// Pure + host-tested (no Arduino). The device feeds one raw readback per poll.

namespace nimbus::display {

// A single controller-id readback matches the not-answering signature when every
// bit of the register width is pegged one way: all-ones (0xFFFFFF on the owner's
// board, MISO idle-high / disconnected) or all-zeros (nothing driving the line).
// A live controller returns a plausible non-zero id with mixed bits, so requiring
// the whole width to be one value rules a real answer out. `nbytes` is the width
// read (RDDID is 3); widths >= 4 clamp to the 32 bits readReg() returns, and a
// width < 1 (nothing read) is treated as no answer.
constexpr bool idLooksDead(uint32_t id, int nbytes) {
  if (nbytes < 1) return true;
  const uint32_t mask =
      (nbytes >= 4) ? 0xFFFFFFFFu : ((1u << (static_cast<unsigned>(nbytes) * 8u)) - 1u);
  const uint32_t v = id & mask;
  return v == 0u || v == mask;
}

// Debounced colour-panel controller liveness. Driven one id readback per poll.
class PanelControllerLiveness {
 public:
  // Default 3 consecutive not-answering reads: enough to ignore a lone read
  // glitched by bus contention, short at the device's ~2 s liveness cadence
  // (~6 s of confirmed-silent comms) so a genuinely absent panel is caught fast.
  static constexpr uint16_t kDefaultThreshold = 3;

  PanelControllerLiveness() = default;
  explicit PanelControllerLiveness(uint16_t threshold)
      : threshold_(threshold ? threshold : 1) {}

  // Fold in one controller-id readback and return the current verdict.
  //   didRead - the caller actually performed a read this poll. Pass false when
  //             the read was skipped (the render bus was busy, so a register read
  //             would return noise, not a verdict): that is NO NEW EVIDENCE, so it
  //             neither trips nor clears - the last verdict simply holds. A busy
  //             render bus can therefore never look dead.
  //   id, nbytes - the register readback (readReg(0x04, 3)) and its width.
  bool update(bool didRead, uint32_t id, int nbytes) {
    if (!didRead) return notResponding_;  // no evidence this poll; hold the verdict
    if (idLooksDead(id, nbytes)) {
      if (deadStreak_ != 0xFFFF) deadStreak_++;
    } else {
      deadStreak_ = 0;  // a plausible id: the controller answered, it is alive
    }
    notResponding_ = deadStreak_ >= threshold_;
    return notResponding_;
  }

  bool     notResponding() const { return notResponding_; }
  uint16_t deadStreak() const { return deadStreak_; }
  uint16_t threshold() const { return threshold_; }

 private:
  uint16_t threshold_ = kDefaultThreshold;
  uint16_t deadStreak_ = 0;
  bool     notResponding_ = false;
};

// The single honest verdict for the display, mapped from the two independent
// signals the device can gather. Kept here as ONE pure function so the health
// report (agent::health) and /api/state cannot drift into disagreeing verdicts.
//
//   NotResponding - the controller readback is the not-answering signature. A
//                   fault, and the owner's exact case. Independent of the pixel
//                   probe, so it is caught even in the shipped default (probe off).
//   Unverified    - the controller answers (or has not been read as dead) but the
//                   pixel-content probe is off, so the image on the glass is NOT
//                   confirmed. Present, but never reported as a healthy "true":
//                   "not measured" must not render as "ok".
//   Ok            - the pixel-content probe ran and the panel's own pixels matched
//                   the frame that was pushed: confirmed content, the strongest
//                   evidence short of a human looking at the glass.
enum class PanelStatus : uint8_t { NotResponding, Unverified, Ok };

// Map the live signals to the honest status.
//   notResponding - debounced PanelControllerLiveness verdict (all-ones id).
//   probed        - the pixel-content probe is enabled (it actually measures).
//   contentOk     - that probe's content-match result (only meaningful if probed).
constexpr PanelStatus panelStatus(bool notResponding, bool probed, bool contentOk) {
  if (notResponding) return PanelStatus::NotResponding;
  if (probed) return contentOk ? PanelStatus::Ok : PanelStatus::NotResponding;
  return PanelStatus::Unverified;
}

}  // namespace nimbus::display
