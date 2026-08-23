#include "nimbus/wifi/copy.h"

namespace nimbus {
namespace wifi {

namespace {

// The e-ink font is printable ASCII only; anything else renders as garbage. An SSID
// is arbitrary bytes from the air, so sanitise here rather than trusting the panel.
std::string asciiOnly(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) o += (c >= 0x20 && c < 0x7F) ? c : '?';
  return o;
}

std::string clip(const std::string& s, size_t maxChars) {
  if (s.size() <= maxChars) return s;
  if (maxChars <= 3) return s.substr(0, maxChars);
  return s.substr(0, maxChars - 3) + "...";
}

std::string fit(const std::string& s, size_t maxChars) { return clip(asciiOnly(s), maxChars); }

// "14 min" / "45 sec" - a duration a person can act on.
std::string humanSecs(uint32_t sec) {
  if (sec >= 60) return std::to_string((sec + 59) / 60) + " min";
  return std::to_string(sec) + " sec";
}

// The setup network's name, for the "here is how to reach me" half of a message.
// Falls back to a generic phrase rather than printing an empty name.
std::string apName(const LinkView& v) {
  return v.apSsid.empty() ? std::string("the setup network") : asciiOnly(v.apSsid);
}

}  // namespace

std::string deviceUrl(const std::string& ip, const std::string& code) {
  // "0.0.0.0" is what softAPIP()/localIP() report when the interface never came up.
  // It encodes into a perfectly valid QR that goes nowhere, which is exactly how a
  // failed AP still looked healthy on the panel.
  if (ip.empty() || ip == "0.0.0.0") return std::string();
  std::string u = "http://" + ip + "/";
  if (!code.empty()) u += "?c=" + code;   // sign-in CODE, not a bearer token (CUM-45)
  return u;
}

std::string netStatusLine(const LinkView& v, size_t maxChars) {
  // The station link is the useful end state. On a TFT board the temporary setup
  // hotspot is deliberately shut down once the station joins, so AP-down is not an
  // error while the device is reachable on the owner's LAN.
  if (v.state == LinkState::Online) {
    std::string where = !v.staIp.empty() ? asciiOnly(v.staIp) : asciiOnly(v.mdnsName);
    std::string s = "Home Wi-Fi connected";
    if (!where.empty()) s += ": " + where;
    return fit(s, maxChars);
  }

  // AP-down only matters when there is no working station link to use instead.
  if (!v.apUp)
    return fit("Setup hotspot is down. Restart the device.", maxChars);

  if (v.apHoldSecLeft > 0)
    return fit("Setup network on. Wi-Fi paused " + humanSecs(v.apHoldSecLeft) + ".", maxChars);

  switch (v.state) {
    case LinkState::Online:  // handled above
      break;
    case LinkState::Joining:
      return fit("Joining " + asciiOnly(v.ssid) + "...", maxChars);
    case LinkState::Scanning:
      return fit("Looking for a known Wi-Fi network...", maxChars);
    case LinkState::Idle:
    case LinkState::Unreachable:
    default:
      // Both dead ends name the network to join next - this line is on the screen you
      // look at when you cannot reach the device any other way.
      return fit(v.knownCount > 0 ? "No known Wi-Fi found - use " + apName(v)
                                  : "Wi-Fi not set up - use " + apName(v),
                 maxChars);
  }
  return std::string();  // exhaustive switch guard
}

std::string wifiRowLabel(const LinkView& v, size_t maxChars) {
  std::string s;
  if (v.apHoldSecLeft > 0) {
    s = "Wi-Fi: setup network published";
  } else {
    switch (v.state) {
      case LinkState::Online:   s = "Wi-Fi: " + asciiOnly(v.staIp); break;
      case LinkState::Joining:  s = "Wi-Fi: joining " + asciiOnly(v.ssid); break;
      case LinkState::Scanning: s = "Wi-Fi: scanning"; break;
      default:
        s = v.knownCount > 0 ? "Wi-Fi: not joined - setup network on"
                             : "Wi-Fi: not set up";
        break;
    }
  }
  return fit(s + " >", maxChars);
}

std::string scanRowLabel(const ScanHit& h, bool known, size_t maxChars) {
  // Name first (that is what the eye scans for), then signal, then whether we already
  // hold credentials - "saved" is the cue that picking it will just work.
  std::string s = clip(asciiOnly(h.ssid), 24);
  s += "  " + std::to_string((int)h.rssi);
  if (known)          s += "  saved";
  else if (h.locked)  s += "  lock";
  return fit(s, maxChars);
}

std::string forgetRowLabel(const KnownNet& n, bool current, size_t maxChars) {
  std::string s = clip(asciiOnly(n.ssid), 30);
  if (current) s += "  (in use)";
  return fit(s, maxChars);
}

std::vector<std::string> buildScanRows(const std::vector<ScanHit>& scan,
                                       const std::vector<KnownNet>& known,
                                       size_t maxChars) {
  // Collapse duplicate SSIDs to their strongest sighting; count hidden APs separately.
  std::vector<ScanHit> uniq;
  int hidden = 0;
  for (const ScanHit& h : scan) {
    if (h.ssid.empty()) { ++hidden; continue; }   // hidden AP: no name to show/pick
    bool merged = false;
    for (ScanHit& u : uniq) {
      if (u.ssid == h.ssid) { if (h.rssi > u.rssi) u.rssi = h.rssi; u.locked = u.locked && h.locked; merged = true; break; }
    }
    if (!merged) uniq.push_back(h);
  }
  // Saved networks first, then by signal (strongest first). Stable insertion sort keeps
  // equal-key ties in first-seen order, so the output is deterministic for the goldens.
  auto isSaved = [&](const ScanHit& h) { return findNetwork(known, h.ssid) >= 0; };
  for (size_t i = 1; i < uniq.size(); ++i) {
    ScanHit key = uniq[i];
    const bool ks = isSaved(key);
    size_t j = i;
    while (j > 0) {
      const bool js = isSaved(uniq[j - 1]);
      const bool better = (ks && !js) || (ks == js && key.rssi > uniq[j - 1].rssi);
      if (!better) break;
      uniq[j] = uniq[j - 1];
      --j;
    }
    uniq[j] = key;
  }
  std::vector<std::string> rows;
  rows.reserve(uniq.size() + 1);
  for (const ScanHit& h : uniq) rows.push_back(scanRowLabel(h, isSaved(h), maxChars));
  if (hidden > 0) {
    const std::string marker = hidden == 1 ? "(hidden network)"
                                           : "(" + std::to_string(hidden) + " hidden networks)";
    rows.push_back(fit(marker, maxChars));
  }
  return rows;
}

}  // namespace wifi
}  // namespace nimbus
