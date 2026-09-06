#pragma once
#include <cstdint>

// panel_controller - honest liveness for the colour panel's ILI9341 controller.
//
// The health/status "Display (color touch): ok, up" was hardwired to the boot
// begin() result and LIED while the controller was off the SPI bus: the owner's
// nimbus-light showed a black glass while every readback failed, yet the row
// still read "ok" with zero live measurement. "panel up" from a begin() that
// succeeded at boot says nothing about a controller that stopped answering
// afterwards.
//
// This is the nimbus-side detector, and it is driven by the driver's own
// solide::display_tft::healthy() read (RDDST, register 0x09) - the reliable
// liveness signal the driver already uses everywhere else. RDDST's status byte
// mirrors the MADCTL we wrote, so a controller that still answers and holds its
// mode reads healthy, while a disconnected or silently-reset one does not. It is
// deliberately debounced so a live panel never reads as dead and a single
// glitched read never trips.
//
// Why NOT the controller id (RDDID, register 0x04)? Because RDDID is unreliable
// across ILI9341 variants: a healthy Freenove / CYD panel returns id 0x000000
// while it is fully working and visibly rendering the UI (verified on hardware).
// Keying liveness on that id reported a working Freenove panel as "not
// responding". The driver's healthy() deliberately reads RDDST instead of RDDID
// for exactly this reason (see display_tft.cpp), and this detector follows the
// same reliable signal.
//
// Pure + host-tested (no Arduino). The device feeds one healthy() reading per poll.

namespace nimbus::display {

// Debounced colour-panel controller liveness. Driven one healthy() reading per
// poll: healthy==true (the controller answered and still holds the mode we wrote)
// is a sign of LIFE and clears the streak immediately; healthy==false across the
// whole debounce window is a controller that is not answering.
class PanelControllerLiveness {
 public:
  // Default 3 consecutive unhealthy reads: enough to ignore a lone read glitched
  // by bus contention, short at the device's ~2 s liveness cadence (~6 s of
  // confirmed-silent comms) so a genuinely absent panel is caught fast.
  static constexpr uint16_t kDefaultThreshold = 3;

  PanelControllerLiveness() = default;
  explicit PanelControllerLiveness(uint16_t threshold)
      : threshold_(threshold ? threshold : 1) {}

  // Fold in one healthy() reading and return the current verdict.
  //   didRead - the caller actually performed a read this poll. Pass false when
  //             the read was skipped (the render bus was busy, so a register read
  //             would return noise, not a verdict): that is NO NEW EVIDENCE, so it
  //             neither trips nor clears - the last verdict simply holds. A busy
  //             render bus can therefore never look dead.
  //   healthy - solide::display_tft::healthy(): true = the controller answered its
  //             RDDST status register and it still matches the mode we wrote (a
  //             LIVE panel, including a Freenove whose RDDID reads 0x000000);
  //             false = it did not answer or has lost its configuration.
  bool update(bool didRead, bool healthy) {
    if (!didRead) return notResponding_;  // no evidence this poll; hold the verdict
    if (healthy) {
      unhealthyStreak_ = 0;  // the controller answered: it is alive, clear at once
    } else if (unhealthyStreak_ != 0xFFFF) {
      unhealthyStreak_++;
    }
    notResponding_ = unhealthyStreak_ >= threshold_;
    return notResponding_;
  }

  bool     notResponding() const { return notResponding_; }
  uint16_t unhealthyStreak() const { return unhealthyStreak_; }
  uint16_t threshold() const { return threshold_; }

 private:
  uint16_t threshold_ = kDefaultThreshold;
  uint16_t unhealthyStreak_ = 0;
  bool     notResponding_ = false;
};

// The single honest verdict for the display, mapped from the two independent
// signals the device can gather. Kept here as ONE pure function so the health
// report (agent::health) and /api/state cannot drift into disagreeing verdicts.
//
//   NotResponding - the controller's RDDST health read fails across the debounce
//                   window. A fault, and the owner's exact case. Independent of
//                   the pixel probe, so it is caught even in the shipped default
//                   (probe off).
//   Unverified    - the controller answers (or has not been read as dead) but the
//                   pixel-content probe is off, so the image on the glass is NOT
//                   confirmed. Present, but never reported as a healthy "true":
//                   "not measured" must not render as "ok".
//   Ok            - the pixel-content probe ran and the panel's own pixels matched
//                   the frame that was pushed: confirmed content, the strongest
//                   evidence short of a human looking at the glass.
enum class PanelStatus : uint8_t { NotResponding, Unverified, Ok };

// Map the live signals to the honest status.
//   notResponding - debounced PanelControllerLiveness verdict (healthy()==false
//                   across the debounce window).
//   probed        - the pixel-content probe is enabled (it actually measures).
//   contentOk     - that probe's content-match result (only meaningful if probed).
constexpr PanelStatus panelStatus(bool notResponding, bool probed, bool contentOk) {
  if (notResponding) return PanelStatus::NotResponding;
  if (probed) return contentOk ? PanelStatus::Ok : PanelStatus::NotResponding;
  return PanelStatus::Unverified;
}

}  // namespace nimbus::display
