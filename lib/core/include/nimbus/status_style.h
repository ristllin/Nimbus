#pragma once
#include <cstdint>

#include "solide/ring.h"   // solide::ring::Status / Anim

// status_style - the SINGLE SOURCE of the Notifier/Orchestrator ring "status
// language" (owner ask: one documented status->behaviour map, so presentation
// isn't rewired in many places). Each canonical session Status maps to:
//   * a THEME ROLE  - an index into the active theme's colour family
//     (nimbus::themeRoleHue), so the ring is painted in the user's theme, keyed
//     by status, instead of the raw wire/provider hue ("all green" bug); Error
//     uses the theme's dedicated ALERT hue (nimbus::themeAlertHue) so it stays
//     alarming without a jarring pure-red clash on a cool theme.
//   * an ANIMATION  - a distinct motion per status (owner ask: "apply different
//     patterns not just colours"), under the ambient-grammar law that persistent
//     states never strobe: Running slides (Comet); WaitingInput, AwaitingApproval
//     and Error all breathe (~2.6 s - Error is "breathing red", not a blip); Done
//     fades to a settled ember; Idle is a static dim glow.
//   * a BRIGHTNESS  - so ambient/idle states sit back and active ones stand out.
//
// Pure + host-tested (test_status_style); the device (ring_plan/ring_out) and the
// mapping doc (docs/notifier-status-language.md) both consume this, and the web
// theme-family view mirrors it - none can drift.

namespace nimbus {

struct StatusStyle {
  uint8_t roleIdx;             // theme-family palette index (0..3) for the hue
  bool    alert;               // true (Error): use themeAlertHue() instead of roleIdx
  solide::ring::Anim anim;     // motion pattern for this status
  uint8_t brightPct;           // 0..100 relative brightness (ambient states sit back)
};

StatusStyle statusStyle(solide::ring::Status s);

}  // namespace nimbus
