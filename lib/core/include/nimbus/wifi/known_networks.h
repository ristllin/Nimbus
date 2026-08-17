#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <ArduinoJson.h>

// Known networks - the PURE, host-tested store for the Wi-Fi credentials the device
// is willing to join. NO Arduino, NO WiFi.h, NO Preferences: the device glue
// (src/net/wifi_store.*) does the NVS IO and hands the blob in and out.
//
// Why a list instead of the historical single slot: the device kept exactly ONE
// {ssid, pass} pair, so saving a new network DESTROYED the previous one. Combined
// with an unbounded reconnect loop that made a real trap - a device carried to a
// second location scanned forever for a network that no longer existed, and the
// endless scan starved the setup AP's beacons on the shared 2.4 GHz radio, so the
// config AP (the only way to fix the credentials) could not be seen or joined.
//
// Order is MEANINGFUL: index 0 is the most recently joined network. The device
// mirrors the head back to the legacy single slot so an OTA rollback to an older
// image still finds the network most likely to work.
//
// See docs/wifi-resilience.md.

namespace nimbus {
namespace wifi {

inline constexpr int    kMaxKnownNetworks = 5;
inline constexpr size_t kSsidMax = 32;   // 802.11 SSID limit
// 64, not 63. A WPA2 PASSPHRASE is 8-63 characters, but a 64-character hex
// string is also a valid credential - it is the raw 256-bit PMK, and both
// ESP-IDF and the Arduino core branch on exactly strlen==64 to use it directly.
// Capping at 63 silently truncated such a key on load, on upsert AND during
// legacy migration, and the damaged copy was then written back over the legacy
// slot - destroying the original with no owner action, on the first boot of
// this firmware, in a way an OTA rollback could not recover.
inline constexpr size_t kPassMax = 64;
inline constexpr int    kSchemaVersion = 1;

// One saved network. `pass` empty = an open network (a legitimate value, NOT
// "unset" - an open network is joinable and must round-trip).
struct KnownNet {
  std::string ssid;
  std::string pass;
  bool        autoJoin  = true;   // false = remembered, but never joined automatically
  uint32_t    lastOkDay = 0;      // epoch-day of the last successful join; 0 = never
};

// One entry from a radio scan (the device fills these from WiFi.SSID(i)/RSSI(i)).
struct ScanHit {
  std::string ssid;
  int8_t      rssi   = -127;
  bool        locked = true;      // encrypted (WiFi.encryptionType != OPEN)
};

// --- (de)serialization -------------------------------------------------------
// Wire format (one NVS string, key "staNets"):
//   {"v":1,"n":[{"s":"<ssid>","p":"<psk>","a":1,"d":20661}, ...]}
std::string dumpNetworks(const std::vector<KnownNet>& nets);

// Tolerant by design, mirroring loadLoops(): an EMPTY blob is "no networks yet",
// which is success - a virgin device has no key. Malformed JSON returns false and
// leaves `out` empty rather than half-populated. Over-long fields are clamped and
// entries with an empty SSID are dropped, so a hand-edited or corrupted blob can
// never feed a garbage SSID to the radio.
bool loadNetworks(const std::string& json, std::vector<KnownNet>& out,
                  int maxCount = kMaxKnownNetworks);

// --- list operations ---------------------------------------------------------
// SSIDs are compared EXACTLY. 802.11 SSIDs are case-sensitive octet strings, and a
// case-folding match here would join the wrong network or overwrite the wrong entry.
int findNetwork(const std::vector<KnownNet>& nets, const std::string& ssid);  // -1 = absent

enum class UpsertResult : uint8_t { Added = 0, Updated, Evicted, Rejected };

// Add or update, newest-first. An existing SSID has its password/autoJoin replaced
// and MOVES TO THE FRONT. At capacity the least-recently-joined entry is evicted -
// except `protectSsid` (the network currently in use), which is never dropped to
// make room for a network that has never worked. An empty or over-long SSID is
// Rejected and the list is left untouched.
UpsertResult upsertNetwork(std::vector<KnownNet>& nets, const KnownNet& add, int maxCount,
                           const std::string& protectSsid, KnownNet* evicted = nullptr);

bool forgetNetwork(std::vector<KnownNet>& nets, const std::string& ssid);

// Record a successful join: promote to the head and stamp the day. Returns true ONLY
// if the list actually changed, so the caller can skip the NVS write when a device
// reconnects to the network already at the head (NVS has finite erase cycles and this
// runs on every reconnect).
bool touchNetwork(std::vector<KnownNet>& nets, const std::string& ssid, uint32_t day);

// --- candidate selection -----------------------------------------------------
struct Candidate {
  int    knownIndex = -1;   // index into the known list
  int8_t rssi       = -127; // as seen in the scan
};

// Intersect the known list with what the radio can actually SEE, strongest first.
// This is the core of scan-then-match: instead of retrying one stored SSID forever,
// the device only attempts networks that are present, in the order most likely to work.
// Entries with autoJoin=false are excluded.
std::vector<Candidate> rankCandidates(const std::vector<KnownNet>& nets,
                                      const std::vector<ScanHit>& scan);

// Convenience: the known-list index of the strongest visible network, or -1 if the
// scan contains nothing we know (which is what sends the policy to Unreachable).
int pickBest(const std::vector<KnownNet>& nets, const std::vector<ScanHit>& scan);

// --- migration ---------------------------------------------------------------
// Fold the legacy single slot (NVS staSsid/staPass) into the list. Idempotent: an
// upgraded device runs this on every boot, and once the SSID is present nothing
// changes. Returns true if the list was modified (caller persists only then).
bool migrateLegacySlot(std::vector<KnownNet>& nets, const std::string& legacySsid,
                       const std::string& legacyPass, int maxCount = kMaxKnownNetworks);

}  // namespace wifi
}  // namespace nimbus
