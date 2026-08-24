#pragma once
// tunnel_guard - portable secret-containment policy for the cloud relay tunnel.
//
// The relay replays a tunneled browser request into the device's OWN web server over a
// loopback socket, stamping a valid LAN webAuthToken onto it (http_replay). The invariant
// is "the durable LAN token (and the setup-AP password) never leave the device". Two
// independent bypasses of the old two-entry path denylist motivated this module:
//
//   * The denylist omitted the sign-in endpoints, so a hostile relay could tunnel
//     GET /api/signin/code then POST /api/signin/exchange and read the durable token.
//   * The denylist string-compared the RAW tunneled path, but ESPAsyncWebServer
//     percent-DECODES the path before routing, so /api/%63onnect slipped past the
//     compare yet dispatched to /api/connect (leaking token + AP password).
//
// The containment here is defense in depth and endpoint-agnostic where it can be:
//   1. canonicalizePath() reduces a raw tunneled path to the exact form the local router
//      dispatches on (strip query, percent-decode, strip a trailing slash), so the
//      denylist sees what actually runs;
//   2. isDeniedPath() hard-refuses the known secret-bearing endpoints on the canonical
//      path (a 403, so the handler never even executes);
//   3. scrubJsonSecrets() redacts the durable-secret field values (webAuthToken, AP
//      password) from any tunneled JSON response body as a final backstop, so a future
//      un-denied endpoint can still never carry those secrets off the device.
//
// Host-tested (test/test_tunnel_guard). No sockets, no Arduino: the device
// (src/net/relay_client.cpp) wires this to the loopback replay.
#include <string>

namespace nimbus {
namespace cloud {
namespace tunnel {

// Canonicalize a raw tunneled request path to the form the local ESPAsyncWebServer router
// matches on: drop the query (everything from the first '?' or '#'), percent-decode %XX
// escapes (malformed escapes are left literal), and strip a single trailing '/' (except
// root). No case folding and no '..'/'//' rewriting: the router matches the decoded path
// exactly, so those never reach a handler anyway and folding them would only over-deny.
std::string canonicalizePath(const std::string& rawPath);

// True if the canonical path targets an endpoint whose RESPONSE reflects a durable LAN
// secret (the webAuthToken, the setup-AP password, or a single-use sign-in code that
// exchanges for the token) and therefore must NEVER be served over the tunnel.
bool isDeniedPath(const std::string& canonicalPath);

// Convenience: canonicalize `rawPath` then deny-check it. This is what the relay calls on
// the raw tunneled path.
bool isTunnelDenied(const std::string& rawPath);

// Backstop scrubber. If `contentType` is application/json, redact the string values of the
// durable-secret keys ("token", "apPass") in-place, leaving the JSON shape intact
// (e.g. {"token":"abcd..."} -> {"token":""}). Returns true if any bytes changed. The
// sign-in "code" field is deliberately NOT blanket-scrubbed: its name collides with the
// legitimate cloud-pairing `code` in the relay-status response, and every code-bearing
// endpoint is already denied outright.
bool scrubJsonSecrets(const std::string& contentType, std::string& body);

}  // namespace tunnel
}  // namespace cloud
}  // namespace nimbus
