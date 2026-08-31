#include "nimbus/wifi/copy.h"

namespace nimbus {
namespace wifi {

namespace {

// The panel font is printable ASCII only; anything else renders as garbage. An SSID
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

  // AP-down only matters when there is no working station link to use instead. The
  // setup network self-recovers (the firmware re-asserts it within seconds), so this
  // is a transient to wait out, never a reason to physically restart the device
  // (CUM-190). Say what is happening and the one next step.
  if (!v.apUp)
    return fit("Setup hotspot restarting. Reconnect shortly.", maxChars);

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

// ---- first-run setup instructions (CUM-259 / CUM-260) -----------------------

std::string setupSteps(const std::string& apSsid, const std::string& apUrl) {
  // Lead with the sequence, not the facts. Step 1 is the join (the QR beside this
  // text does it, or the printed password); step 2 is the address to open once
  // joined - the step the owner said was missing. Kept to two short lines plus a
  // one-line safety net; the panel renderer word-wraps each within its column.
  const std::string name = apSsid.empty() ? std::string("this network") : asciiOnly(apSsid);
  const std::string url = apUrl.empty() ? std::string("http://192.168.4.1") : asciiOnly(apUrl);
  return "1. Join " + name + " (scan the code)\n"
         "2. Open " + url + "\n"
         "If nothing opens, go to that address.";
}

std::string setupFallbackLine(const std::string& apUrl, size_t maxChars) {
  const std::string url = apUrl.empty() ? std::string("http://192.168.4.1") : apUrl;
  return fit("If nothing opens, visit " + url, maxChars);
}

std::string setupCtaTitle() { return "Set up Wi-Fi"; }
std::string setupCtaHint() { return "Tap to open setup"; }

// ---- captive-portal probe table (CUM-260 leg 1) -----------------------------

bool isCaptiveProbePath(const std::string& path) {
  // The fixed URLs the major platforms fetch to detect a captive portal. The device
  // must answer each with the captive page so the OS pops it; a path missing here
  // falls through to the generic AP redirect (still pops today), but naming them
  // makes the guarantee explicit and testable. Compared case-insensitively - the OS
  // uses fixed casing, but a lowercase-normalized compare costs nothing and is safer.
  static const char* const kProbes[] = {
      "/hotspot-detect.html",        // iOS / macOS (captive.apple.com)
      "/library/test/success.html",  // iOS / macOS (older path)
      "/generate_204",               // Android / ChromeOS
      "/gen_204",                     // Android (short form)
      "/ncsi.txt",                    // Windows NCSI
      "/connecttest.txt",            // Windows 10+ connectivity test
      "/canonical.html",             // Firefox (detectportal.firefox.com)
      "/success.txt",                // Firefox / NetworkManager
      "/check_network_status.txt",   // NetworkManager
      "/kindle-wifi/wifistub.html",  // Kindle
  };
  std::string p;
  p.reserve(path.size());
  for (char c : path) {
    if (c == '?' || c == '#') break;  // ignore any query / fragment
    p += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
  }
  for (const char* probe : kProbes)
    if (p == probe) return true;
  return false;
}

std::string captiveLandingHtml(const std::string& apSsid, const std::string& openUrl,
                               const std::string& apAddr) {
  const std::string name = apSsid.empty() ? std::string("the setup network") : asciiOnly(apSsid);
  const std::string open = openUrl.empty() ? std::string("/") : openUrl;
  const std::string addr = apAddr.empty() ? std::string("192.168.4.1") : asciiOnly(apAddr);
  // Static, JavaScript-free page: a captive-network mini-browser often cannot run the
  // full setup app, so this always renders and always states the next step. Inline
  // style only. Sentence case, US spelling, no em dash (user-facing copy).
  return
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<title>Nimbus setup</title></head>"
      "<body style=\"font-family:system-ui,sans-serif;max-width:24rem;margin:2rem auto;"
      "padding:0 1rem;line-height:1.5\">"
      "<h1 style=\"font-size:1.3rem\">Nimbus setup</h1>"
      "<p>You are connected to " + name + ".</p>"
      "<p><a href=\"" + open + "\" style=\"display:inline-block;padding:.7rem 1.2rem;"
      "background:#0b7;color:#fff;border-radius:.4rem;text-decoration:none\">Open setup</a></p>"
      "<p style=\"color:#555\">If that button does nothing, open "
      "<b>http://" + addr + "</b> in your browser.</p>"
      "</body></html>";
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

// Collapse duplicate SSIDs to their strongest sighting; return the count of hidden
// (empty-SSID) access points separately. `locked` merges with OR so a secured network
// seen open on another band is never presented as open.
static std::vector<ScanHit> dedupeScan(const std::vector<ScanHit>& scan, int& hidden) {
  std::vector<ScanHit> uniq;
  hidden = 0;
  for (const ScanHit& h : scan) {
    if (h.ssid.empty()) { ++hidden; continue; }
    bool merged = false;
    for (ScanHit& u : uniq) {
      if (u.ssid != h.ssid) continue;
      if (h.rssi > u.rssi) u.rssi = h.rssi;
      u.locked = u.locked || h.locked;
      merged = true;
      break;
    }
    if (!merged) uniq.push_back(h);
  }
  return uniq;
}

std::vector<std::string> buildScanRows(const std::vector<ScanHit>& scan,
                                       const std::vector<KnownNet>& known,
                                       size_t maxChars) {
  int hidden = 0;
  std::vector<ScanHit> uniq = dedupeScan(scan, hidden);
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
