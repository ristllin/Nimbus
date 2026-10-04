#pragma once
#include <cstdint>
#include <string>
#include <vector>

// device_identity - portable helpers for the user-visible device identity.
//
// One name drives every surface: the setup-AP SSID ("<name>-setup"), the mDNS
// hostname (lowercased label), the BLE advertised name, and the orchestrator's
// prompt identity. The name is either user-chosen (web UI, NVS `nimbus_name`)
// or auto-assigned on first boot by NUMBERING against sibling Nimbus devices
// whose APs are currently visible ("Nimbus", "Nimbus-2", "Nimbus-3", ...).
//
// Pure string logic - host-tested (test_identity); the device glue (NVS read/
// write + the WiFi scan) lives in src/sys/config_nvs.* and src/net/wifi_portal.
namespace nimbus::identity {

// Base name auto-assignment counts from ("Nimbus" -> "Nimbus-2" -> ...).
inline const char* kBaseName = "Nimbus";
// Suffix appended to the name for the setup AP's SSID.
inline const char* kApSuffix = "-setup";

// Sanitize a user-supplied name into an SSID/BLE-safe display name: printable
// ASCII letters/digits/space/dash/underscore only, trimmed, runs of blanks
// collapsed, capped at 24 chars (24 + "-setup" = 30 <= the 32-byte SSID limit).
// Returns "" when nothing usable survives (caller falls back to auto naming).
std::string sanitizeName(const std::string& raw);

// Lowercase RFC-1123-ish mDNS label from a name: [a-z0-9-] only, other chars
// become '-', runs collapsed, leading/trailing '-' trimmed, capped at 24.
// mdnsLabel("Nimbus") == "nimbus" (the historical default hostname).
std::string mdnsLabel(const std::string& name);

// Pick the first free auto name given the SSIDs visible in a WiFi scan.
// A sibling occupies index 1 when "<base>" or "<base>-setup" is visible, and
// index N when "<base>-N" or "<base>-N-setup" (N >= 2) is visible. Returns the
// lowest free index as "<base>" (1) or "<base>-N".
std::string pickSiblingName(const std::string& base,
                            const std::vector<std::string>& ssids);

// First-boot sibling scan retry (CUM-468). The scan feeding pickSiblingName()
// runs right after the radio comes up, and the driver can refuse it for a beat.
// The device used to read any refusal as "no siblings": on the 2026-10-04 bench
// every fresh unit finished the scan in under 200 ms (a real 2.4 GHz scan takes
// ~2 s), and two adjacent units both came up as "Nimbus-setup" / nimbus.local.
//
// A refusal shows up in one of two shapes, and both are retried:
//   - a negative result (WIFI_SCAN_FAILED / WIFI_SCAN_RUNNING): the scan never
//     started;
//   - 0 networks, faster than kSiblingScanMinRealMs: the scan started but the
//     driver aborted it. The esp32-arduino core ignores the scan-done status and
//     reports an aborted scan as 0, while a COMPLETED sweep dwells >= 100 ms on
//     each of 11-13 channels, so it cannot come back empty in under ~1.1 s.
// A retry waits kSiblingScanRetryGapMs, and no attempt STARTS later than
// kSiblingScanWindowMs after the first. One real scan takes ~2 s, so the worst
// case stays near the ~3 s first-boot budget (it runs before the loop watchdog is
// armed). Any other result is final - a slow 0 included: an empty neighborhood
// genuinely means the plain base name.
inline constexpr uint32_t kSiblingScanRetryGapMs = 200;
inline constexpr uint32_t kSiblingScanWindowMs   = 1000;
inline constexpr uint32_t kSiblingScanMinRealMs  = 500;

// True when the device should wait kSiblingScanRetryGapMs and scan again.
// `scanResult` is what the attempt that just ended returned (a network count, or
// a negative driver code), `attemptMs` how long that attempt took, and
// `msSinceFirstAttempt` the time from the start of the FIRST attempt to the end
// of this one.
bool retrySiblingScan(int scanResult, uint32_t attemptMs, uint32_t msSinceFirstAttempt);

// What the first-boot scan loop saw, for the device's one-line boot log.
struct SiblingScanOutcome {
  int      result   = -2;  // final result: a network count, or the last negative code
  int      firstRc  = -2;  // what the FIRST attempt returned (the CUM-468 diagnostic)
  int      attempts = 0;
  uint32_t ms       = 0;   // from the start of the first attempt to the end of the last
};

// The retry loop itself, kept here so the loop that ships is the loop the host
// suite drives with a fake clock (test_identity). The device passes the real
// seams: scan() = a blocking WiFi.scanNetworks, now() = millis, pause() = delay.
template <class Scan, class Now, class Pause>
SiblingScanOutcome runSiblingScan(Scan scan, Now now, Pause pause) {
  SiblingScanOutcome o;
  const uint32_t t0 = now();
  for (;;) {
    if (o.attempts > 0) pause(kSiblingScanRetryGapMs);
    const uint32_t started = now();
    o.result = scan();
    if (o.attempts++ == 0) o.firstRc = o.result;
    const uint32_t ended = now();
    o.ms = ended - t0;
    if (!retrySiblingScan(o.result, ended - started, o.ms)) return o;
  }
}

// Setup-AP passphrase length (WPA2 needs >= 8; 10 x 5 bits = 50 bits entropy).
inline constexpr int kSetupPassLen = 10;

// Mint a setup-AP passphrase: kSetupPassLen chars drawn from a 32-symbol
// lowercase+digit alphabet with the ambiguous 0/o/1/l removed (the value is
// read off a small screen and typed on a phone). 32 symbols make `rnd() % 32`
// bias-free. `rnd` is the entropy source (esp_random on-device; a stub in
// host tests).
std::string makeSetupPass(uint32_t (*rnd)());

// Wi-Fi network QR payload ("WIFI:S:<ssid>;T:WPA;P:<pass>;;") - the format
// phone cameras recognize to auto-join a network. Special characters
// (\ ; , : ") are backslash-escaped per the de-facto spec. Empty pass emits
// T:nopass with no P field (an open AP). Returns "" when ssid is empty (no
// network to name -> caller falls back to a URL QR).
std::string wifiQrPayload(const std::string& ssid, const std::string& pass);

}  // namespace nimbus::identity
