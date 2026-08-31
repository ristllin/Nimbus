#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/wifi/known_networks.h"
#include "nimbus/wifi/policy.h"

// Wi-Fi user-facing copy - portable so it can be TESTED. This header exists because
// two real bugs shipped inside `main.cpp`: the Config-QR screen encoded
// "http://0.0.0.0/?t=..." when the access point had failed, and the status line
// hardcoded the compile-time AP name ("Nimbus-setup") instead of the live one, so a
// board named Nimbus-3 printed the wrong network to join on the very screen you
// reach when you are locked out. Neither was catchable: `[env:native]` sets
// `build_src_filter = -<*>`, so `src/` is never compiled host-side, and both
// functions were `static` in main.cpp.
//
// Every string here is printable ASCII and bounded, because the panel renders
// ~48 chars per line and clips silently - truncation is this layer's job, never the
// renderer's. Spelling follows the copy style guide: "Wi-Fi", sentence case.

namespace nimbus {
namespace wifi {

// A flattened snapshot of the link, filled by the device from live radio state.
struct LinkView {
  LinkState   state = LinkState::Idle;
  std::string ssid;        // the network joined / being joined
  std::string staIp;
  std::string apIp;
  std::string apSsid;      // the LIVE setup-AP name, e.g. "Nimbus-3-setup"
  std::string mdnsName;    // e.g. "nimbus-3.local"
  int         rssi = 0;
  bool        apUp = false;
  int         apStations = 0;
  int         knownCount = 0;
  uint32_t    apHoldSecLeft = 0;
  uint32_t    nextRetrySec = 0;
  // Multi-network failover progress (CUM-207). candCount is how many saved networks are
  // in range this cycle; candIndex is the 1-based position of the one being tried now.
  // When candCount > 1 the status reads fail-honest as "trying <ssid> 2/3..." so the
  // owner sees the device working THROUGH its saved networks, not stuck. candCount <= 1
  // (a lone candidate, or none) keeps the plain "Joining <ssid>..." line.
  int         candIndex = 0;
  int         candCount = 0;
};

// The URL encoded into the Sign-in QR / handed to a browser.
// Returns "" when there is no usable address, so the caller draws "no url" instead
// of a QR code that resolves to nothing. An unreachable device used to render a
// confident QR pointing at http://0.0.0.0/ - worse than showing nothing, because it
// looks like it works.
// `code` is the short single-use SIGN-IN CODE (from GET /api/signin/code), carried as
// `?c=`. It replaces the older `?t=<token>`: a bearer token in a URL is logged,
// shoulder-surfed, and shared by accident, so the web side stopped accepting it
// (CUM-45). An empty code yields the bare "http://<ip>/".
std::string deviceUrl(const std::string& ip, const std::string& code);

// One line under the Config QR: what the network is doing, and the next step when
// it is not working.
std::string netStatusLine(const LinkView& v, size_t maxChars = 48);

// ---- first-run setup instructions (CUM-259 / CUM-260) -----------------------
// The SetupInfo screen used to state the network facts but never the NEXT ACTION,
// so an owner who joined the hotspot (via the QR) was left with "not forwarded to
// any website" and nothing telling them what to do. These lead with the SEQUENCE,
// not the facts. `apUrl` is the bare setup address (e.g. "http://192.168.4.1").

// The two-step body for the SetupInfo screen: join this network, then open the
// setup address. Multi-line (the panel renderer word-wraps each line).
std::string setupSteps(const std::string& apSsid, const std::string& apUrl);

// The no-popup safety net, shown on the setup screen, the captive page, and the
// getting-started docs: a captive pop can never be guaranteed (a phone with mobile
// data on often suppresses it), so the manual address is the copy that never fails.
std::string setupFallbackLine(const std::string& apUrl, size_t maxChars = 48);

// StatusIdle first-run call-to-action. A credless, un-onboarded device that lands on
// the idle status screen must NOT look onboarded with an empty session list - it shows
// this CTA (title + hint) and a tap on it returns to Setup (CUM-259).
std::string setupCtaTitle();   // "Set up Wi-Fi"  (a button - title case)
std::string setupCtaHint();    // "Tap to open setup"  (a hint - sentence case)

// ---- captive-portal probe table (CUM-260 leg 1) -----------------------------
// Modern OS captive detection fetches a fixed set of probe URLs and decides
// "there is a portal" from the RESPONSE: Apple expects its "Success" page, Android
// expects a 204, Microsoft expects known text. Answering a probe with a 404 (or the
// success response) tells the OS there is NO portal, so nothing pops. The device
// must instead answer every known probe with the captive page. This is the table of
// those paths; the web seam serves `captiveLandingHtml` for any path it matches.
// A new OS probe path that is not in this table would silently miss (no pop), so the
// host test iterates it as a class rule.
bool isCaptiveProbePath(const std::string& path);

// The captive landing page served to the OS mini-browser. Deliberately tiny and
// static (no SPA JavaScript, which a captive-network mini-browser often cannot run)
// so it ALWAYS renders and always states the next step. `apSsid` is the network the
// phone just joined, `openUrl` is the authenticated link into the full setup page,
// and `apAddr` is the bare address for the manual fallback line. ASCII, US spelling,
// no em dash - it is user-facing copy.
std::string captiveLandingHtml(const std::string& apSsid, const std::string& openUrl,
                               const std::string& apAddr);

// The Connectivity menu's Wi-Fi row (row 0) - a status the owner can act on.
std::string wifiRowLabel(const LinkView& v, size_t maxChars = 44);

// One row of the on-device network picker: name, signal, and whether we have it saved.
std::string scanRowLabel(const ScanHit& h, bool known, size_t maxChars = 42);

// Build the whole on-device Wi-Fi picker list from a raw scan (CUM-48): SAVED networks
// first, then the rest by SIGNAL strength (strongest first). Duplicate SSIDs (one
// network seen on several channels/bands) collapse to their strongest sighting, and
// hidden access points (empty SSID in the scan) collapse into a single trailing
// "hidden network" marker row - they carry no name to pick. Pure + host-tested; the
// device passes the result straight to SettingsMenu::setWifiScan().
std::vector<std::string> buildScanRows(const std::vector<ScanHit>& scan,
                                       const std::vector<KnownNet>& known,
                                       size_t maxChars = 42);

// One row of the forget-network list.
std::string forgetRowLabel(const KnownNet& n, bool current, size_t maxChars = 42);

}  // namespace wifi
}  // namespace nimbus
