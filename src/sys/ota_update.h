#pragma once
#include <Arduino.h>

// OTA firmware update - device glue over the portable nimbus::ota core.
// Downloads a signed release from GitHub into the INACTIVE app slot, verifies
// SHA-256 + ECDSA (include/ota_pubkey.h) BEFORE the otadata flip, and reboots
// with the app-level rollback guard armed (NVS pend/boots/prev - the arduino
// sdkconfig has no bootloader rollback, so bootGuard() enforces "3 failed
// boots => flip back to the previous slot" itself).
//
// Lifecycle: setup() calls bootGuard() right after solide::begin() (NVS up,
// BEFORE any risky bring-up so a crash-loop anywhere later still burns an
// attempt); loop() calls tick() (internally rate-limited) which handles
// mark-valid, the daily check cadence, and the auto-install idle window.

#include "nimbus/ota/ota_logic.h"   // IdleSnapshot (portable, host-tested)

namespace otaupd {

// Events fired from the OTA tasks (hook runs in TASK context - keep it cheap;
// main.cpp wires Telegram notify + telegram-stop here, loops-subsystem style).
enum Event : int {
  EvAvailable = 1,     // a="version", b="notes" - new release seen (once per version)
  EvInstallStart,      // a="dry"|"real"
  EvInstallFail,       // a=reason, b=version
  EvRebooting,         // a=version - commit done, restart in ~3.5 s
  EvValidated,         // a=running version - post-update boot marked valid
};
using EventHook = void (*)(int ev, const char* a, const char* b);
void setEventHook(EventHook h);

// Auto-install idle snapshot provider (main.cpp fills turn/voice/audio/battery
// state). Return false when the state is unknown - auto-install then defers.
using IdleProvider = bool (*)(nimbus::ota::IdleSnapshot&);
void setIdleProvider(IdleProvider p);

// Rollback/validation guard - ms-scale, runs before the watchdog is armed.
void bootGuard();

// Cheap periodic driver (mark-valid, scheduled checks, auto-install). Call
// every loop() pass; it self-limits to ~1 Hz.
void tick();

// Spawn the on-demand check task (manifest fetch + compare). False when refused;
// *whyOut (optional) gets the machine reason ("busy"/"unsupported"/"no-wifi"/
// "low-heap"/"in-progress") so the caller can name the real cause instead of
// blaming the network (CUM-197). Map it with nimbus::ota::checkRefusalCopy().
bool requestCheck(const char** whyOut = nullptr);

// Spawn the install task. dryRun = download + verify only, never flips.
// force = allow Same/Older (explicit web-UI downgrade). False when refused;
// *whyOut (optional) gets a static reason string.
bool requestInstall(bool dryRun, bool force, const char** whyOut = nullptr);

// True while the install task runs - main loop refuses voice capture / new
// turns and paints the ring progress arc from progressPct().
bool installing();

// One-word state for STATUS + /api/state ("idle"/"checking"/"available"/...).
const char* statusStr();
// Definitive outcome of the last completed check for /api/state + /api/ota/check
// pollers: "pending"/"up-to-date"/"new-version"/"unreachable"/"failed". Unlike
// statusStr() this never rests on a non-terminal "checking" once a check settles,
// and it distinguishes a transport failure ("unreachable") from a bad manifest.
const char* checkResultStr();
// Download progress 0..100, or -1 when no download is running.
int progressPct();
// Version of the newest release seen by the last successful check ("" = none),
// and its human notes line.
String latestSeen();
String latestNotes();
// Last OTA outcome from NVS ("ok vX" / "rollback vX" / "sha-fail vX" / ...).
String lastResult();
// Short reason for state "error" ("fetch"/"sha-fail"/"sig-fail"/...).
const char* lastError();
// The running app slot label ("app0"/"app1") - proves an install flipped slots.
const char* runningSlot();

#ifdef NIMBUS_TEST
// HIL seams. simArm(prevLabel): arm the pend/boots/prev guard as if an update
// to the CURRENT image had just flipped from <prevLabel>. simCrash(on): make
// every boot abort() early while the guard is pending - a synthetic bad image;
// bootGuard clears it when the rollback fires. simClear(): drop all sim state.
// setManifestUrl(""): RAM-only manifest URL override (OTAURL console cmd).
void simArm(const String& prevLabel);
void simCrash(bool on);
void simClear();
void setManifestUrl(const String& url);
#endif

}  // namespace otaupd
