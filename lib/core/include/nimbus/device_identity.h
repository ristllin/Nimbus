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
