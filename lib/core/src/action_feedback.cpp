#include "nimbus/action_feedback.h"

#include <cstddef>

namespace nimbus::action {

Cues cuesFor(Outcome o) {
  switch (o) {
    case Outcome::Ok:
      return {nimbus::sfx::Ev::AgentDone, RingCue::Success};
    case Outcome::Acknowledged:
      return {nimbus::sfx::Ev::AgentSpawn, RingCue::Ack};
    case Outcome::Failed:
      return {nimbus::sfx::Ev::Error, RingCue::Failure};
  }
  // Defensive: an unmapped outcome is treated as a failure, never a silent
  // success - a bug must be loud (the honesty rule), not quietly cheerful.
  return {nimbus::sfx::Ev::Error, RingCue::Failure};
}

bool screenLineOk(const char* s) {
  if (!s || !s[0]) return false;
  int n = 0;
  for (const char* p = s; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 || c > 0x7e) return false;  // control chars, newline, non-ASCII
    if (++n > kMaxActionLineLen) return false;
  }
  return true;
}

namespace {
// One catalog, one place to read every user-facing action line. Sentence case,
// verb-led; failures state what happened then the one next step (AGENTS.md
// section 6). A nullptr line means that action already shows the outcome on its
// own row / status band / Ask screen, so no extra line is drawn. Order MUST match
// enum MenuAction.
constexpr MenuActionCopy kCopy[] = {
    // ForgetBonds
    {"Bluetooth pairings cleared", "Couldn't clear pairings. Try again.", nullptr},
    // RescanSd
    {"SD card mounted", "No SD card found. Reseat it and rescan.", nullptr},
    // CloudPair (result lands later on the Pairing screen)
    {nullptr, nullptr, "Pairing started. Watch for the code."},
    // PublishAp (fire-and-forget; the AP status row follows)
    {nullptr, nullptr, "Setup network is publishing."},
    // WifiJoin (the real join result arrives later as a Wi-Fi cue)
    {nullptr, "Network not saved. Choose it again.", "Joining the network."},
    // WifiForget
    {"Network forgotten.", "Couldn't forget it. Try again.", nullptr},
    // Reset
    {"Settings reset to defaults.", nullptr, nullptr},
    // BluetoothOn
    {"Bluetooth on.", nullptr, nullptr},
    // BluetoothOff
    {"Bluetooth off.", nullptr, nullptr},
    // Restart (the panel also shows a "Restarting..." screen)
    {nullptr, nullptr, "Restarting now."},
    // PowerOff (the panel also shows its shutdown screen)
    {nullptr, nullptr, "Powering off."},
    // Calibrate (the guided full-screen flow takes over next)
    {nullptr, nullptr, "Starting touch calibration."},
    // UpdateCheck (the update status band carries the words)
    {nullptr, nullptr, nullptr},
    // UpdateInstall (the update band / Ask screen carries the words)
    {nullptr, nullptr, nullptr},
};
static_assert(sizeof(kCopy) / sizeof(kCopy[0]) == static_cast<size_t>(MenuAction::COUNT),
              "one copy row per MenuAction, in enum order");
constexpr MenuActionCopy kNullCopy{nullptr, nullptr, nullptr};
}  // namespace

const MenuActionCopy& copyFor(MenuAction a) {
  const auto i = static_cast<unsigned>(a);
  if (i >= static_cast<unsigned>(MenuAction::COUNT)) return kNullCopy;
  return kCopy[i];
}

const char* lineFor(const MenuActionCopy& c, Outcome o) {
  switch (o) {
    case Outcome::Ok:
      return c.ok;
    case Outcome::Acknowledged:
      return c.ack;
    case Outcome::Failed:
      return c.fail;
  }
  return nullptr;
}

}  // namespace nimbus::action
