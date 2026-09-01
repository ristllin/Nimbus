#pragma once
#include <string>

#include "webui_page.h"  // generated: kWebUiParts[] / kWebUiPartCount

// web_ui - serve the REAL Nimbus web app from the hosted daemon (CUM-265).
//
// nimbusd is the device's engine compiled for POSIX, so a Virtual Nimbus can
// serve the SAME single-page app the desk device serves (Home / Chat / Memory /
// Assistant / Device) instead of the CUM-263 bare chat page. The page bytes are
// the device's blessed fragment assembly (nimbusd/tools/gen_webui.py extracts
// them from include/web/ui_*.h; the suite asserts they equal
// tools/webui_page.snapshot), streamed here as one document.
//
// Tunnel sign-in (auth seam): the device app gates every view on a per-device
// web token stored in the browser (localStorage 'nimbusTok'); with no token it
// shows a full-screen "Sign in / scan the QR" gate. Inside the cloud tunnel that
// gate would be a SECOND sign-in the owner cannot satisfy (there is no device
// screen to scan). The relay session has already authenticated the owner, so the
// served page seeds this instance's own web token the way the device's first-run
// setup signs a browser in automatically - one injected <script>, run before the
// app script. The API stays token-gated (the seeded token rides the
// X-Nimbus-Token header on every request); an empty gate (dev) seeds a sentinel
// so the app renders and the ungated API accepts it.
namespace nimbusd {

// Build the app the browser stores its token under. `webToken` is this
// instance's web token (NIMBUSD_WEB_TOKEN); empty means the API is ungated (dev),
// so a sentinel is seeded purely to skip the client gate.
inline std::string buildWebUiPage(const std::string& webToken) {
  // The token becomes a JS string literal - escape the two characters that could
  // break out of it. Web tokens are alphanumeric in practice; this is belt-and-
  // suspenders so a hand-set token can never inject script.
  std::string tok;
  const std::string src = webToken.empty() ? std::string("tunnel") : webToken;
  for (char c : src) {
    if (c == '\\' || c == '\'') tok += '\\';
    if (c == '<') { tok += "\\x3c"; continue; }  // never emit a literal </script>
    tok += c;
  }
  // Runs before the app script (injected right after the page <title>): seed the
  // token so the client-side gate is already satisfied inside the tunnel, and set
  // the honest-UI flag (CUM-279) so the shared page hides device-only chrome (ring,
  // screen, mic, AP, ESP OTA) that is faked or dead on a hosted instance. The flag
  // is set synchronously before the app script, so no dead control ever flashes.
  const std::string bootstrap =
      "<script>/* Virtual Nimbus tunnel sign-in (CUM-265) + honest-UI flag "
      "(CUM-279): the relay session has already authenticated the owner, so sign "
      "this browser in as the device's first-run setup does, and mark this a hosted "
      "instance. */\n"
      "window.NIMBUS_HOSTED=true;\n"
      "try{localStorage.setItem('nimbusTok','" + tok + "');}catch(e){}</script>";

  std::string page;
  size_t total = bootstrap.size();
  for (size_t i = 0; i < kWebUiPartCount; i++) total += std::char_traits<char>::length(kWebUiParts[i]);
  page.reserve(total);
  for (size_t i = 0; i < kWebUiPartCount; i++) page += kWebUiParts[i];

  // Inject the bootstrap directly after the page <title>, which sits at the very
  // top of the first fragment (ui_shell) - long before the single app <script> -
  // so localStorage is seeded before any app code reads nimbusTok().
  const std::string anchor = "<title>Nimbus</title>";
  const size_t at = page.find(anchor);
  if (at != std::string::npos)
    page.insert(at + anchor.size(), bootstrap);
  else
    page.insert(0, bootstrap);  // anchor moved: still seed (degrades, never fakes)
  return page;
}

}  // namespace nimbusd
