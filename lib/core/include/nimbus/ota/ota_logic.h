#pragma once
#include <cstddef>
#include <cstdint>

// OTA update - the PURE, host-tested core. Owns every decision the device glue
// (src/sys/ota_update.cpp) must not improvise: version compare, manifest
// parse+validation, the canonical signed message, the update state machine,
// install eligibility, and the boot-health/rollback + auto-install policies.
// NO Arduino, NO mbedtls - crypto (SHA-256 stream, ECDSA verify) and flash IO
// stay in the device glue; everything decidable from plain data lives here.

namespace nimbus {
namespace ota {

// --- version ----------------------------------------------------------------
// Accepts "v2.10.0" / "2.10.0"; anything after a '-' or '+' is ignored
// ("v2.10.0-rc1" parses as 2.10.0). Returns false on malformed input.
bool parseVersion(const char* s, int& maj, int& min, int& pat);

// <0 if a<b, 0 if equal, >0 if a>b. An unparseable side compares OLDER than a
// parseable one (a device with a garbage version should still accept updates);
// two unparseable versions compare equal.
int compareVersions(const char* a, const char* b);

// --- manifest ---------------------------------------------------------------
inline constexpr uint32_t kMinFwBytes = 512u * 1024u;       // sanity floor
inline constexpr uint32_t kMaxFwBytes = 0x640000u;          // one 6.5 MB app slot
inline constexpr uint8_t  kMaxSigLen  = 80;                 // DER ECDSA P-256 <= 72

struct VariantInfo {
  char     url[224]   = {0};      // https only, length-bounded
  uint32_t size       = 0;        // exact firmware.bin byte count
  uint8_t  sha256[32] = {0};      // decoded from manifest hex
  char     shaHex[65] = {0};      // lowercase hex (kept for the signed message)
  uint8_t  sig[kMaxSigLen] = {0}; // DER ECDSA signature (decoded from base64)
  uint8_t  sigLen     = 0;
};

struct ManifestInfo {
  char version[24]    = {0};      // e.g. "v2.11.0"
  char build[40]      = {0};
  char notes[96]      = {0};      // human line for web UI / Telegram
  char minVersion[24] = {0};      // "" = no floor; gates AUTO install only
  VariantInfo v;                  // the requested variant only
};

// Parse + validate manifest JSON for one variant. On failure returns false and
// (if errOut) points errOut at a static reason string ("schema", "version",
// "variant", "url", "size", "sha256", "sig"). Accepts only schema==2 (typed
// manifests; see the device-type slugs below), an https URL, size in
// [kMinFwBytes, kMaxFwBytes], 64-hex sha256, base64 DER sig. The "variant"
// argument is the device TYPE (e.g. "nimbus-tft", "freenove-28"); an untyped
// device passes "" and never matches, which the glue reports as "no update".
bool parseManifest(const char* json, size_t len, const char* variant,
                   ManifestInfo& out, const char** errOut = nullptr);

// --- device type ------------------------------------------------------------
// The four frozen OTA device-type slugs the typed manifest keys on. Every real
// device is exactly one of these; e-ink / untyped devices carry "" and get no
// updates.
inline constexpr const char* kTypeNimbusTft = "nimbus-tft";
inline constexpr const char* kTypeFreenove28 = "freenove-28";
inline constexpr const char* kTypeFreenove35 = "freenove-35";
inline constexpr const char* kTypeFreenove40 = "freenove-40";

// True when this firmware build may run under the given type slug. isFreenove =
// (SOLIDE_BOARD==freenove_s3). A Solide build accepts only "nimbus-tft"; a
// Freenove build accepts only the "freenove-*" sizes. Any unknown slug (incl.
// "", legacy build tags, or a mismatched family) returns false, so a misseeded
// otaType can never pull a wrong-pinout image. Legacy compile-time build tags
// ("test", "test-cyd") are handled by the glue's fallback, not here.
bool typeAllowedForBoard(const char* type, bool isFreenove);

// Derive the OTA device type from hardware identity during the transition boot
// (existing devices that carry no otaType yet). isFreenove = SOLIDE_BOARD==
// freenove_s3; screenIsTft = stored scrModel=="tft". Solide+tft -> "nimbus-tft";
// Solide+eink -> "" (frozen: e-ink gets no more updates); Freenove -> the base
// "freenove-28" size (the flasher seeds the exact size on fresh installs, so this
// default only ever applies to a Freenove reaching the typed scheme by transition
// without a seeded size - the smallest safe panel). Writes buf, returns its
// length (0 for the untyped e-ink case, which the glue leaves unset).
size_t deriveDeviceType(char* buf, size_t cap, bool isFreenove, bool screenIsTft);

// --- signed message ---------------------------------------------------------
// The canonical payload CI signs and the device verifies:
//   "nimbus-ota-v2\n<version>\n<type>\n<sha256-hex-lowercase>\n"
// Golden-tested against tools/make_manifest.py --print-message. Returns the
// message length, or 0 if buf is too small.
size_t buildSigMessage(char* buf, size_t cap, const char* version,
                       const char* variant, const char* shaHex);

// --- decoding helpers (exposed for tests + tooling) -------------------------
// Lowercase/uppercase hex -> bytes. Returns false unless src is exactly
// 2*dstLen hex chars.
bool decodeHex(const char* src, uint8_t* dst, size_t dstLen);
// Standard base64 (no url-safe, padding required) -> bytes. Returns decoded
// length, or 0 on any malformed input / overflow of dstCap.
size_t decodeB64(const char* src, uint8_t* dst, size_t dstCap);

// --- state machine ----------------------------------------------------------
enum class State : uint8_t {
  Idle = 0,       // nothing known yet
  Checking,       // manifest fetch in flight
  UpToDate,       // checked; nothing newer
  Available,      // checked; a newer (or forced-visible) release exists
  Downloading,    // install task streaming to the inactive slot
  Verifying,      // stream done; sha + signature check
  ReadyToReboot,  // committed; restart imminent
  Error,          // last operation failed (reason in device glue)
  Unsupported,    // build has no NIMBUS_OTA_VARIANT tag
};
const char* stateStr(State s);

// A check may start from any settled state; never while an install runs.
bool canCheck(State s);
// An install may start only from a settled post-check state. (The force path
// still requires a completed check so a manifest is loaded.)
bool canInstall(State s);

// --- eligibility ------------------------------------------------------------
enum class Eligibility : uint8_t {
  Newer = 0,            // manifest > current
  Same,                 // equal - install only with force
  Older,                // downgrade - install only with force
  BlockedMinVersion,    // current < manifest.minVersion - AUTO path must skip
};
Eligibility eligibility(const char* currentVersion, const ManifestInfo& m);
// True when the AUTO path may install this manifest (strictly newer AND the
// minVersion floor, if any, is satisfied). Manual installs consult force.
bool autoEligible(Eligibility e);

// --- boot-health / rollback policy ------------------------------------------
inline constexpr uint8_t kOtaMaxBootAttempts = 3;

// pending = the NVS otaPend flag; bootCount AFTER this boot's increment.
// True => flip back to the previous slot now.
bool shouldRollback(bool pending, uint8_t bootCount);

// When the running (freshly-updated) image may mark itself valid. With WiFi
// up once, 120 s crash-free suffices; a WiFi-dead site self-validates after
// 600 s so a fine image on a moved/renamed network never false-rollbacks.
bool bootHealthy(uint32_t uptimeS, bool wifiEverUp);

// --- battery / health install gate ------------------------------------------
// Both the manual and the auto install paths consult this gate before flashing:
// an interrupted write on a dying pack can leave a slot unbootable. Install is
// allowed when the pack is healthy enough to finish the write - level at or
// above kManualBattFloorPct AND estimated health at or above kManualHealthFloorPct
// - OR the device is on external power, OR battery monitoring is disabled (there
// is no pack to protect). Otherwise the caller shows the matching next step. The
// owner can still force past a NeedPower stop ("Install anyway, I am charging")
// because this board has no VBUS sense line to prove the charger on its own.
enum class InstallGate : uint8_t {
  Allowed = 0,       // healthy enough, on external power, or monitoring off
  NeedPower,         // pack below the level floor  -> "Connect power"
  NeedRecalibrate,   // health estimate below floor -> "Recalibrate to 100%"
};
inline constexpr int     kManualBattFloorPct   = 40;
inline constexpr uint8_t kManualHealthFloorPct = 60;

struct InstallGateInput {
  bool    battMonEnabled  = true;   // false => no pack, gate is a no-op
  bool    onExternalPower = false;  // BatteryEstimate.onExternalPower
  int     battPct         = 0;      // calibrated SoC
  uint8_t healthPct       = 100;    // estimated capacity health
};
InstallGate installGate(const InstallGateInput& in);
const char* installGateStr(InstallGate g);  // "allowed"/"need-power"/"need-recalibrate"

// --- auto-install idle window ------------------------------------------------
struct IdleSnapshot {
  bool     turnInFlight    = false;  // orchestrator turn / tool loop running
  bool     voiceActive     = false;  // hold-to-talk capture or STT in flight
  bool     audioPlaying    = false;  // SFX/TTS speaker output
  bool     onExternalPower = false;  // BatteryEstimate.onExternalPower
  bool     battMonEnabled  = true;   // battery monitoring on (false => no pack)
  int      battPct         = 0;      // calibrated SoC
  uint8_t  healthPct       = 100;    // estimated capacity health (gate parity)
  uint32_t internalFreeB   = 0;      // free INTERNAL heap
};
inline constexpr int      kAutoBattFloorPct  = 50;
inline constexpr uint32_t kAutoHeapFloorB    = 24u * 1024u;
bool autoInstallAllowed(const IdleSnapshot& s);

// --- definitive check result ------------------------------------------------
// A completed /api/ota/check ALWAYS resolves to one of these, so a caller that
// polls the check never hangs on "checking": UpToDate (reachable, nothing
// newer), NewVersion (reachable, a newer release with notes), Unreachable (the
// release feed could not be fetched at all), or Failed (reached the server but
// the manifest was rejected). Derived from the settled State plus whether the
// last fetch actually reached the server. Pending = still checking / never ran.
enum class CheckResult : uint8_t { Pending = 0, UpToDate, NewVersion, Unreachable, Failed };
CheckResult checkResult(State settled, bool reachedServer);
const char* checkResultStr(CheckResult r);  // "pending"/"up-to-date"/"new-version"/"unreachable"/"failed"

// Inverse of stateStr(): the one-word status ("idle"/"checking"/...) back to the
// State. Lets the device glue - which exposes the status only as a string - drive
// updateView() below without duplicating the vocabulary. Any unknown word (incl.
// nullptr) maps to Idle, the safe "nothing known" default.
State stateFromStr(const char* s);

// --- update UI view (CUM-193) -----------------------------------------------
// The single source of truth for how an update state is SHOWN to the owner, so
// the device screen and the web UI describe the same states in the same words.
// Pure: given the state plus the live strings it interpolates (`latest` version,
// `err` reason, `fwVersion` = the running version; "" / nullptr when N/A) and the
// download progress (`pct`, 0..100 while downloading else -1), it fills one
// printable-ASCII status line (the device screen is ~48 chars) and the affordance
// flags the UI needs: `busy` = show an in-progress affordance (a check or an
// install is running), `showInstall` = offer the Install action (a newer release
// is staged). `line` is empty ONLY for Idle, where the caller keeps the control's
// default help text.
struct UpdateView {
  bool busy = false;
  bool showInstall = false;
  int  pct = -1;
  char line[48] = {0};
};
UpdateView updateView(State s, int pct, const char* latest, const char* err,
                      const char* fwVersion);

}  // namespace ota
}  // namespace nimbus
