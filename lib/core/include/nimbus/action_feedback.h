#pragma once
#include <cstdint>

#include "nimbus/sfx_map.h"

// action_feedback - the PORTABLE, host-tested mapping from a device-menu action's
// OUTCOME to its feedback cues, plus the one catalog of user-facing lines those
// actions show. Every actionable menu control (forget paired devices, rescan for
// SD, reset to defaults, re-pair, ...) routes its result through here so the
// sound + ring cue are consistent and no button press is silent. The device seam
// (src/main.cpp emitMenuActionFeedback) turns a Cues value into a real sound, a
// brief ring swell, and an on-screen line; this layer carries no Arduino, audio,
// or LED dependency so it is unit-tested under `pio test -e native`.
//
// Two deliberate design rules live here:
//  1. The cue selection is a function of the OUTCOME only, never of the specific
//     action. Success everywhere sounds and looks the same; failure everywhere
//     sounds and looks the same. That is what makes the feedback learnable.
//  2. The action-specific WORDS live in one catalog (copyFor) so the host test
//     validates every shipped line against the screen budget - a new over-long or
//     non-ASCII line fails the suite, not the panel on a customer's desk.
namespace nimbus::action {

// The result of running an actionable control. Acknowledged is the honest answer
// for a genuine fire-and-forget begin() whose true result only lands later and
// out of band (a pairing request, an AP publish): it is NOT a faked success.
enum class Outcome : uint8_t {
  Ok,            // it ran and the result is known good
  Acknowledged,  // it was started/requested; the true result is not knowable here
  Failed,        // it ran and the result is known bad
};

// Which ring swell the device paints. The concrete RGB lives in the device seam
// (LED code is not portable); this enum keeps the outcome -> cue mapping host-
// testable. All three are a brief Pattern::Pulse SWELL, never a hard Flash strobe
// (the frozen ring-attention grammar: even one-shot confirms swell).
enum class RingCue : uint8_t {
  Success,  // brief green swell
  Ack,      // brief neutral (theme-accent) swell - started, result unknown
  Failure,  // brief red swell
};

// The feedback cues for one outcome: which sound event to voice and which ring
// swell to paint. The on-screen line is supplied separately (copyFor) because it
// carries action-specific words.
struct Cues {
  nimbus::sfx::Ev sfx;
  RingCue         ring;
};

// The one, consistent outcome -> cues mapping. Pure; host-tested.
//
// Sound choices reuse existing tones (no new sound assets): Ok voices AgentDone
// (a settled completion tone), Acknowledged voices AgentSpawn (a "something
// started" tone), Failed voices Error (the distinct fault tone). All three have
// an embedded clip, so they are audible on a device with no SD card. The device
// seam plays them bypassing the per-event verbosity rank (a button press is a
// direct interaction, not the ambient job-status chatter that rank throttles)
// while still honouring the hard mute - sound level Off, a speaker fault, or a
// live voice-capture mute all keep it silent.
Cues cuesFor(Outcome o);

// Device screen line budget: printable ASCII, single line, at most this many
// chars (the ~48-char panel/serial contract in AGENTS.md section 6).
constexpr int kMaxActionLineLen = 48;

// True iff `s` is a non-empty, single-line, printable-ASCII string within the
// budget. A call site may legitimately pass nullptr for a line (meaning "the
// action's own row/band already shows the result, add no extra line"), but a
// nullptr/empty string is NOT a valid on-screen line, so this returns false for
// it - the device seam skips the screen channel when the catalog line is null.
bool screenLineOk(const char* s);

// Every actionable device-menu control, for the one-line call sites and for the
// host test to iterate the whole catalog.
enum class MenuAction : uint8_t {
  ForgetBonds = 0,  // Connectivity > Forget paired devices
  RescanSd,         // Connectivity > Rescan SD card, and Main > SD card
  CloudPair,        // Connectivity > Cloud link code (re-pair; Orchestrator only)
  PublishAp,        // Connectivity > Wi-Fi > Publish setup network
  WifiJoin,         // Connectivity > Wi-Fi > Choose network (join a saved one)
  WifiForget,       // Connectivity > Wi-Fi > Forget network
  Reset,            // Main > Reset to defaults
  BluetoothOn,      // Connectivity > Bluetooth -> On
  BluetoothOff,     // Connectivity > Bluetooth -> Off
  Restart,          // Main > Restart
  PowerOff,         // Main > Power off
  Calibrate,        // Display > Calibrate touch
  UpdateCheck,      // Software update > Check for updates
  UpdateInstall,    // Software update > Install
  COUNT
};

// The user-facing lines for one action's outcomes. Any field may be nullptr when
// that action has no on-screen line for that outcome (its own row, status band,
// or Ask screen already names the result, or the outcome cannot arise). Non-null
// lines are sentence case, verb-led, and failures name the next step; the host
// test asserts every non-null line passes screenLineOk().
struct MenuActionCopy {
  const char* ok;
  const char* fail;
  const char* ack;
};

// The single copy catalog. Returns a stable reference for a valid action; for an
// out-of-range value it returns an all-null entry (no words, still safe).
const MenuActionCopy& copyFor(MenuAction a);

// Pick the line for an outcome out of a copy entry (Ok->ok, Acknowledged->ack,
// Failed->fail). May be nullptr. A convenience so the device seam and the test
// resolve the line the same way.
const char* lineFor(const MenuActionCopy& c, Outcome o);

}  // namespace nimbus::action
