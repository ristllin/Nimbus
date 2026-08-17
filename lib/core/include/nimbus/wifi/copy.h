#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

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
// Every string here is printable ASCII and bounded, because the e-ink panel renders
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
};

// The URL encoded into the Config QR / handed to a browser.
// Returns "" when there is no usable address, so the caller draws "no url" instead
// of a QR code that resolves to nothing. An unreachable device used to render a
// confident QR pointing at http://0.0.0.0/ - worse than showing nothing, because it
// looks like it works.
std::string deviceUrl(const std::string& ip, const std::string& token);

// One line under the Config QR: what the network is doing, and the next step when
// it is not working.
std::string netStatusLine(const LinkView& v, size_t maxChars = 48);

// The Connectivity menu's Wi-Fi row (row 0) - a status the owner can act on.
std::string wifiRowLabel(const LinkView& v, size_t maxChars = 44);

// One row of the on-device network picker: name, signal, and whether we have it saved.
std::string scanRowLabel(const ScanHit& h, bool known, size_t maxChars = 42);

// One row of the forget-network list.
std::string forgetRowLabel(const KnownNet& n, bool current, size_t maxChars = 42);

}  // namespace wifi
}  // namespace nimbus
