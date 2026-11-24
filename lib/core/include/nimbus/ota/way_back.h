#pragma once
// The way back (lane E2-P-REL): the owner's `/update back` on an engine-2 device installs the
// stable release when it is older than the running firmware, which is how an engine-2 device
// (the v5.9.x line) goes back to a release of the earlier engine (v5.2.x) over the air.
//
// The rule: never automatic, never a version trick. The owner asks; the device leaves the
// candidate channel (the glue sets the stable channel first, fail closed, nimbus/ota/
// ota_channel.h), runs a fresh check of the stable manifest, says what it found, and on
// `/update back yes` installs it as a forced manual install (requestInstall force: the
// same path as the web page's force=1 and the cable, signature and type still checked).
// A forced install writes only the other app slot: the card, the files, the notes and the
// conversation stay; the earlier engine reads its own stores, which engine 2 never wrote
// over (src/agent/migrate_device.h).
//
// Header-only and portable: the decision and its owner lines, host-tested in
// test/test_ota_way_back; the glue is src/agent/commands_plain.cpp (engine 2 only).

#include <cstdint>
#include <cstring>
#include <string>

#include "nimbus/ota/ota_logic.h"   // compareVersions, parseVersion

namespace nimbus {
namespace ota {

enum class BackStep : uint8_t {
  Busy,          // an install runs
  Unsupported,   // this build cannot update itself
  StartCheck,    // no check of the stable manifest since the ask: start one
  Waiting,       // that check is still running
  CheckFailed,   // that check failed: the next ask starts another
  Offer,         // the stable release is older: ask for `/update back yes`
  Install,       // confirmed and older: forced install of the stable release
  NothingOlder,  // the stable release is not older (or unreadable): nothing to go back to
  NeedPower,     // confirmed and older, but the battery gate a normal install applies refuses (E2-REP4)
};

struct BackAsk {
  bool confirm = false;          // the owner sent `/update back yes`
  bool installing = false;       // an install runs or waits on the poll task
  bool checkedOnStable = false;  // a check started by `/update back` on the stable channel
  const char* state = "";        // otaupd::statusStr()
  const char* running = "";      // NIMBUS_FW_VERSION
  const char* latest = "";       // otaupd::latestSeen(): that check's stable version
  bool needPower = false;        // the battery gate of a normal install refuses now (installGate NeedPower)
};

inline BackStep backStep(const BackAsk& a) {
  const char* st = a.state ? a.state : "";
  if (a.installing) return BackStep::Busy;
  if (strcmp(st, "unsupported") == 0) return BackStep::Unsupported;
  if (!a.checkedOnStable || strcmp(st, "idle") == 0) return BackStep::StartCheck;
  if (strcmp(st, "checking") == 0) return BackStep::Waiting;
  if (strcmp(st, "up-to-date") != 0 && strcmp(st, "available") != 0) return BackStep::CheckFailed;
  int ma = 0, mi = 0, pa = 0;
  const char* latest = a.latest ? a.latest : "";
  if (!parseVersion(latest, ma, mi, pa) || compareVersions(latest, a.running) >= 0) return BackStep::NothingOlder;
  if (!a.confirm) return BackStep::Offer;
  return a.needPower ? BackStep::NeedPower : BackStep::Install;   // E2-REP4: the forced install checks the battery too
}

// The owner's line for a step (Telegram copy: plain, no '"', names the next step).
inline std::string backLine(BackStep s, const char* latest, const char* running) {
  const std::string l = latest ? latest : "", r = running ? running : "";
  switch (s) {
    case BackStep::Busy: return "An update is already installing. Send /update back again when it is done.";
    case BackStep::Unsupported: return "This build can't update itself. Go back over USB instead.";
    case BackStep::StartCheck:
      return "Switched to the stable channel and checking the stable release. Send /update back again in a minute.";
    case BackStep::Waiting: return "Still checking the stable release. Send /update back again in a minute.";
    case BackStep::CheckFailed: return "Couldn't check the stable release. Send /update back to try again.";
    case BackStep::Offer:
      return "The stable release is " + l + ", older than this firmware (" + r +
             "). Send /update back yes to install it. Files, notes and the conversation stay on the device.";
    case BackStep::Install:
      return "Installing " + l + ". The device downloads, verifies and restarts in about two minutes. Keep it "
             "powered on. Files, notes and the conversation stay on the device.";
    case BackStep::NothingOlder:
      return "The stable release (" + (l.empty() ? std::string("none found") : l) + ") is not older than this firmware (" +
             r + "), so there is nothing to go back to.";
    case BackStep::NeedPower:
      return "The battery is too low to install " + l + ". Connect power, then send /update back yes again.";
  }
  return std::string();
}

}  // namespace ota
}  // namespace nimbus
