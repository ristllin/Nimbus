#include "nimbus/status_style.h"

namespace nimbus {

using solide::ring::Anim;
using solide::ring::Status;

StatusStyle statusStyle(Status s) {
  // role = theme-family palette index (0 primary .. 3 detail); alert = the theme's
  // dedicated alert hue. Anim/brightness give each status a distinct feel.
  switch (s) {
    // AMBIENT GRAMMAR (owner 2026-07-16, supersedes the earlier "errors blink"
    // call): this device sits in your peripheral vision ALL DAY - nothing that
    // PERSISTS may strobe. Every needs-you state is a smooth breathe at the same
    // calm tempo; HUE alone carries the meaning (input = cool role1, approval =
    // amber role3, error = the theme's reserved alert red). Red stays unmissable
    // precisely because nothing else on the ring is ever red. Fast motion is for
    // one-shot transition cues only, never a steady state.
    case Status::Running:          return {0, false, Anim::Comet,   100};  // slide = working
    case Status::WaitingInput:     return {1, false, Anim::Breathe, 100};  // needs YOU
    case Status::AwaitingApproval: return {3, false, Anim::Breathe, 100};  // decision gate (amber)
    case Status::Done:             return {2, false, Anim::Fade,     85};  // settling
    case Status::Error:            return {0, true,  Anim::Breathe, 100};  // "breathing red" -
      // was a 300 ms hard square blink (3.3 red flashes/s at full brightness,
      // held minutes) - an alarm, not signage. The reserved alert hue does the
      // shouting; the motion stays calm.
    case Status::Idle:             return {0, false, Anim::Solid,    20};  // STATIC dull glow -
      // motion is reserved for needs-you + processing (owner rule); an idle ring
      // that breathes reads as blinking/anxious. Dim solid, nothing more.
    case Status::Offline:          return {0, false, Anim::Off,       0};  // freed
  }
  return {0, false, Anim::Solid, 100};
}

}  // namespace nimbus
