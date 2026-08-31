#include "nimbus/wifi/known_networks.h"

#include <algorithm>

namespace nimbus {
namespace wifi {

namespace {

// Clamp on LOAD as well as on insert: a hand-edited or corrupted blob must never be
// able to hand an over-long SSID/PSK to the radio driver.
std::string clampTo(const std::string& s, size_t cap) {
  return s.size() <= cap ? s : s.substr(0, cap);
}

bool ssidUsable(const std::string& s) { return !s.empty() && s.size() <= kSsidMax; }

}  // namespace

// --- (de)serialization -------------------------------------------------------

std::string dumpNetworks(const std::vector<KnownNet>& nets) {
  ArduinoJson::JsonDocument d;
  d["v"] = kSchemaVersion;
  ArduinoJson::JsonArray arr = d["n"].to<ArduinoJson::JsonArray>();
  for (const KnownNet& n : nets) {
    if (!ssidUsable(n.ssid)) continue;   // never persist an unusable entry
    ArduinoJson::JsonObject o = arr.add<ArduinoJson::JsonObject>();
    o["s"] = n.ssid;
    o["p"] = n.pass;
    o["a"] = n.autoJoin;
    o["d"] = n.lastOkDay;
  }
  std::string out;
  ArduinoJson::serializeJson(d, out);
  return out;
}

bool loadNetworks(const std::string& json, std::vector<KnownNet>& out, int maxCount) {
  out.clear();
  if (json.empty()) return true;   // no blob yet = zero networks, not an error
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, json)) return false;
  if (!d.is<ArduinoJson::JsonObject>()) return false;
  // A missing "v" is read as v1: the first shipped schema predates the field being
  // load-bearing, and refusing to parse would silently drop a working device's list.
  ArduinoJson::JsonArrayConst arr = d["n"].as<ArduinoJson::JsonArrayConst>();
  if (arr.isNull()) return true;   // well-formed object with no list = zero networks
  for (ArduinoJson::JsonObjectConst o : arr) {
    if ((int)out.size() >= maxCount) break;   // truncate rather than grow unbounded
    KnownNet n;
    n.ssid      = clampTo(std::string(o["s"] | ""), kSsidMax);
    n.pass      = clampTo(std::string(o["p"] | ""), kPassMax);
    n.autoJoin  = o["a"] | true;
    n.lastOkDay = o["d"] | 0u;
    if (n.ssid.empty()) continue;   // unusable - drop, don't propagate
    out.push_back(n);
  }
  return true;
}

// --- list operations ---------------------------------------------------------

int findNetwork(const std::vector<KnownNet>& nets, const std::string& ssid) {
  for (size_t i = 0; i < nets.size(); i++)
    if (nets[i].ssid == ssid) return (int)i;   // exact: SSIDs are case-sensitive
  return -1;
}

UpsertResult upsertNetwork(std::vector<KnownNet>& nets, const KnownNet& add, int maxCount,
                           const std::string& protectSsid, KnownNet* evicted) {
  if (!ssidUsable(add.ssid)) return UpsertResult::Rejected;

  KnownNet entry = add;
  entry.pass = clampTo(entry.pass, kPassMax);

  const int at = findNetwork(nets, entry.ssid);
  if (at >= 0) {
    // Keep the join history: re-saving a password must not erase when it last worked.
    if (entry.lastOkDay == 0) entry.lastOkDay = nets[(size_t)at].lastOkDay;
    nets.erase(nets.begin() + at);
    nets.insert(nets.begin(), entry);
    return UpsertResult::Updated;
  }

  bool didEvict = false;
  if ((int)nets.size() >= maxCount) {
    // Evict the least-recently-joined, scanning from the back so ties drop the
    // oldest position. The protected SSID (the network in use) is never a victim -
    // dropping it to make room for one that has never worked would be a downgrade.
    int victim = -1;
    uint32_t worst = 0;
    for (int i = (int)nets.size() - 1; i >= 0; i--) {
      if (!protectSsid.empty() && nets[(size_t)i].ssid == protectSsid) continue;
      if (victim < 0 || nets[(size_t)i].lastOkDay < worst) {
        victim = i;
        worst = nets[(size_t)i].lastOkDay;
      }
    }
    if (victim < 0) return UpsertResult::Rejected;   // full and every entry protected
    if (evicted) *evicted = nets[(size_t)victim];
    nets.erase(nets.begin() + victim);
    didEvict = true;
  }
  nets.insert(nets.begin(), entry);
  return didEvict ? UpsertResult::Evicted : UpsertResult::Added;
}

bool forgetNetwork(std::vector<KnownNet>& nets, const std::string& ssid) {
  const int at = findNetwork(nets, ssid);
  if (at < 0) return false;
  nets.erase(nets.begin() + at);
  return true;
}

bool moveNetwork(std::vector<KnownNet>& nets, const std::string& ssid, int toIndex) {
  const int at = findNetwork(nets, ssid);
  if (at < 0) return false;   // absent (this also implies the list is non-empty)
  // Clamp into range: a UI drag or a hand-rolled request can name any index, and a
  // moved-to-out-of-bounds slot must land at an end, never corrupt the vector.
  int dst = toIndex;
  if (dst < 0) dst = 0;
  if (dst >= (int)nets.size()) dst = (int)nets.size() - 1;
  if (dst == at) return false;   // already there - nothing to persist
  KnownNet moved = nets[(size_t)at];
  nets.erase(nets.begin() + at);
  nets.insert(nets.begin() + dst, moved);
  return true;
}

bool touchNetwork(std::vector<KnownNet>& nets, const std::string& ssid, uint32_t day) {
  const int at = findNetwork(nets, ssid);
  if (at < 0) return false;
  // Already the head with today's stamp -> nothing to persist. This is the common
  // case on every reconnect, and returning false is what keeps NVS writes rare.
  if (at == 0 && nets[0].lastOkDay == day) return false;
  KnownNet n = nets[(size_t)at];
  n.lastOkDay = day;
  nets.erase(nets.begin() + at);
  nets.insert(nets.begin(), n);
  return true;
}

// --- candidate selection -----------------------------------------------------

std::vector<Candidate> rankCandidates(const std::vector<KnownNet>& nets,
                                      const std::vector<ScanHit>& scan) {
  std::vector<Candidate> out;
  for (size_t i = 0; i < nets.size(); i++) {
    if (!nets[i].autoJoin) continue;
    // A network can appear more than once in a scan (multiple APs, one SSID) -
    // keep the strongest sighting so a mesh/repeater picks the nearest radio.
    int best = -128;
    bool seen = false;
    for (const ScanHit& h : scan) {
      if (h.ssid != nets[i].ssid) continue;
      if (!seen || h.rssi > best) { best = h.rssi; seen = true; }
    }
    if (!seen) continue;
    Candidate c;
    c.knownIndex = (int)i;
    c.rssi = (int8_t)best;
    out.push_back(c);
  }
  // Strongest first; stable on ties so the more-recently-joined network (nearer the
  // head of the known list) is attempted first.
  std::stable_sort(out.begin(), out.end(),
                   [](const Candidate& a, const Candidate& b) { return a.rssi > b.rssi; });
  return out;
}

int pickBest(const std::vector<KnownNet>& nets, const std::vector<ScanHit>& scan) {
  const std::vector<Candidate> ranked = rankCandidates(nets, scan);
  return ranked.empty() ? -1 : ranked.front().knownIndex;
}

// --- migration ---------------------------------------------------------------

bool migrateLegacySlot(std::vector<KnownNet>& nets, const std::string& legacySsid,
                       const std::string& legacyPass, int maxCount) {
  if (!ssidUsable(legacySsid)) return false;     // nothing provisioned to migrate
  const int at = findNetwork(nets, legacySsid);
  if (at >= 0) {
    // Already migrated - but the legacy slot is still writable by an older image
    // (a rollback, or [env:provision]). If it now holds a DIFFERENT password,
    // the owner corrected it there and the list is the stale copy. Take the
    // newer value rather than silently keeping the one that no longer works;
    // otherwise a later mirror writes the stale password back over the fix and
    // it exists nowhere.
    const std::string fresh = clampTo(legacyPass, kPassMax);
    if (nets[(size_t)at].pass != fresh) {
      nets[(size_t)at].pass = fresh;
      return true;
    }
    return false;                              // genuinely nothing to do
  }
  KnownNet n;
  n.ssid = legacySsid;
  n.pass = clampTo(legacyPass, kPassMax);
  // No lastOkDay: the legacy slot records no join history. It still lands at the head,
  // which is right - it is the network the device was last told to use.
  return upsertNetwork(nets, n, maxCount, /*protectSsid=*/"") != UpsertResult::Rejected;
}

}  // namespace wifi
}  // namespace nimbus
