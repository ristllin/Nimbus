#pragma once
#include <cstdint>
#include <string>
#include <vector>

// mcp_oauth - the PORTABLE decision layer for the outbound MCP client's OAuth 2.1
// acquisition flow (host-tested via pio test -e native). This owns the pure
// pieces of getting a REFRESH TOKEN for a hosted, OAuth-only MCP server (the
// named target: https://mcp.linear.app/mcp). It builds request bodies/URLs and
// parses responses; it has NO Arduino / TLS / store / RNG. The device seam
// (src/agent/connectors.cpp + the web callback in src/net/webui.cpp) supplies the
// TLS POSTs under the work arbiter, the device-RNG bytes, the LAN redirect URI,
// and writes the resulting refresh token into the connector's T3 oauth blob slot;
// from there the existing refreshAccess() broker mints access tokens as before.
//
// Flow (MCP 2025-06-18 authorization, Authorization Code + PKCE):
//   1. Discover the protected-resource metadata (RFC 9728) - from the server's
//      .well-known or the 401 WWW-Authenticate `resource_metadata` - to learn its
//      authorization server(s).
//   2. Discover the authorization-server metadata (RFC 8414) - endpoints + caps.
//   3. Dynamically register a client (RFC 7591) whose redirect_uri is the device's
//      own LAN web callback; token_endpoint_auth_method "none" (public client).
//   4. Build the authorization URL (PKCE S256, state, resource). The owner opens
//      it on a phone/laptop and consents; the AS redirects back to the device.
//   5. Exchange the returned code + PKCE verifier for {access, refresh} tokens.
//
// Every function here is a pure transform locked by a host test; the class rule
// applies (the token/metadata/registration parsers are exercised over a table of
// real and adversarial inputs, not one happy path).

namespace nimbus {
namespace orch {
namespace mcp {
namespace oauth {

// ---- URL / form encoding -----------------------------------------------------

// Percent-encode `s` per RFC 3986 (unreserved ALPHA/DIGIT/-._~ pass through, all
// else -> %XX uppercase). Safe for a query-parameter value or a form field.
std::string pctEncode(const std::string& s);

// application/x-www-form-urlencoded body from ordered {key,value} pairs. Empty
// values are omitted (an OAuth endpoint treats an empty client_secret as a
// public client, and a stray `client_secret=` trips some servers).
std::string formEncode(const std::vector<std::pair<std::string, std::string>>& kv);

// ---- PKCE (RFC 7636) ---------------------------------------------------------

struct Pkce {
  std::string verifier;   // base64url(rand[32]) - 43 chars, in the 43..128 range
  std::string challenge;  // base64url(sha256(verifier))
  std::string method;     // "S256"
};

// Derive a PKCE pair from 32 caller-supplied random bytes (device: esp_random()).
// The verifier is the base64url of those bytes; the challenge is the S256 of the
// verifier. Deterministic in its input, so a test pins it against RFC 7636.
Pkce makePkce(const uint8_t rand[32]);

// ---- metadata discovery ------------------------------------------------------

// The .well-known/oauth-protected-resource URL for a resource server URL
// (RFC 9728), ORIGIN-level form: scheme+host(+port) origin, then the well-known
// path. "" if `url` is not a valid absolute http(s) URL.
std::string wellKnownProtectedResource(const std::string& resourceUrl);

// The RFC 9728 PATH-SUFFIXED metadata URL for a path-scoped resource: the
// resource's own path is appended after the well-known segment (resource
// `https://h/mcp` -> `https://h/.well-known/oauth-protected-resource/mcp`). This
// is the canonical location; a root resource yields the same as the origin form.
// The device tries this first, then the origin form, so both server styles work.
std::string wellKnownProtectedResourcePath(const std::string& resourceUrl);

// The .well-known/oauth-authorization-server URL for an issuer URL (RFC 8414),
// derived from the issuer's origin. "" if `issuer` is not a valid http(s) URL.
std::string wellKnownAuthServer(const std::string& issuer);

// Pull a `resource_metadata="<url>"` hint out of a 401 `WWW-Authenticate` header
// value (RFC 9728 §5.1). "" if absent. Lets discovery skip straight to the doc
// the server itself points at.
std::string resourceMetadataFromWwwAuth(const std::string& wwwAuthenticate);

struct ProtectedResourceMeta {
  bool                     ok = false;
  std::string              resource;            // the canonical resource id
  std::vector<std::string> authorizationServers;  // issuer URLs, in server order
  std::vector<std::string> scopesSupported;     // the scopes to request at authorize
};

// Join scopes into a single space-delimited `scope` string, dropping OpenID
// Connect scopes (openid/profile/email) the device does not use - it wants API
// access, not an id token. "" when there is nothing to request.
std::string scopeStringFor(const std::vector<std::string>& scopesSupported);
// Parse an RFC 9728 protected-resource metadata document.
ProtectedResourceMeta parseProtectedResourceMetadata(const std::string& json);

struct AuthServerMeta {
  bool                     ok = false;
  std::string              issuer;
  std::string              authorizationEndpoint;
  std::string              tokenEndpoint;
  std::string              registrationEndpoint;         // "" -> no RFC 7591 support
  std::vector<std::string> codeChallengeMethodsSupported;  // e.g. ["S256"]
  std::vector<std::string> grantTypesSupported;
  std::vector<std::string> scopesSupported;
  std::vector<std::string> tokenEndpointAuthMethodsSupported;
};
// Parse an RFC 8414 authorization-server metadata document. ok is true only when
// the two endpoints a code+PKCE flow cannot proceed without - authorization and
// token - are both present.
AuthServerMeta parseAuthServerMetadata(const std::string& json);

// True if the server advertises S256 (or advertises nothing, which RFC 8414 says
// defaults to "plain" but real MCP servers all do S256 - we require S256 and let
// a genuinely plain-only server fail loudly rather than downgrade silently).
bool supportsS256(const AuthServerMeta& m);

// ---- dynamic client registration (RFC 7591) ----------------------------------

struct RegistrationParams {
  std::string              clientName = "Nimbus";
  std::vector<std::string> redirectUris;    // the device LAN callback(s)
  std::string              scope;           // "" -> omit
  // Public client, no secret to store: PKCE is the proof. A server that insists
  // on a secret returns one and the flow stores it in the oauth blob.
  std::string              tokenEndpointAuthMethod = "none";
  std::vector<std::string> grantTypes = {"authorization_code", "refresh_token"};
  std::vector<std::string> responseTypes = {"code"};
};
// Build the RFC 7591 registration request body (JSON).
std::string buildRegistrationRequest(const RegistrationParams& p);

struct RegistrationResult {
  bool        ok = false;
  std::string clientId;
  std::string clientSecret;  // "" for a public client
  std::string error;         // RFC 7591 error / a parse failure note when !ok
};
// Parse an RFC 7591 registration response.
RegistrationResult parseRegistrationResponse(const std::string& json);

// ---- authorization request ---------------------------------------------------

struct AuthorizeParams {
  std::string authorizationEndpoint;
  std::string clientId;
  std::string redirectUri;
  std::string codeChallenge;   // PKCE S256 challenge
  std::string state;           // CSRF/binding value the callback must echo
  std::string scope;           // "" -> omit
  std::string resource;        // RFC 8707 resource indicator (the MCP server URL)
};
// Build the full authorization URL the owner opens to consent. Preserves any
// existing query string on the endpoint and appends the OAuth params.
std::string buildAuthorizeUrl(const AuthorizeParams& p);

// ---- token endpoint ----------------------------------------------------------

struct CodeExchangeParams {
  std::string code;
  std::string codeVerifier;
  std::string clientId;
  std::string clientSecret;  // "" for a public client
  std::string redirectUri;
  std::string resource;      // "" -> omit
};
// Build the authorization_code grant form body (application/x-www-form-urlencoded).
std::string buildCodeExchangeForm(const CodeExchangeParams& p);

struct RefreshParams {
  std::string refreshToken;
  std::string clientId;
  std::string clientSecret;  // "" for a public client
  std::string scope;         // "" -> omit
};
// Build the refresh_token grant form body. Mirrors the device T3 broker's own
// form so the acquisition and the renewal speak the same wire.
std::string buildRefreshForm(const RefreshParams& p);

struct TokenResponse {
  bool        ok = false;
  std::string accessToken;
  std::string refreshToken;   // may be empty (server rotates or reuses)
  long        expiresIn = 0;  // seconds; 0 -> caller uses a default TTL
  std::string tokenType;      // typically "Bearer"
  std::string scope;
  std::string error;          // OAuth error code when !ok (e.g. "invalid_grant")
  std::string errorDescription;
};
// Parse a token endpoint response (success or RFC 6749 §5.2 error object).
TokenResponse parseTokenResponse(const std::string& json);

// ---- flow planning -----------------------------------------------------------

// What the device still needs to do, computed from what it already holds. Lets
// the device seam be a thin driver and the decision be host-tested as a table.
enum class Step : uint8_t {
  DiscoverProtectedResource = 0,  // no auth-server issuer known yet
  DiscoverAuthServer,             // issuer known, endpoints not
  Register,                       // endpoints known, no client_id yet
  Authorize,                      // client_id known, waiting to show the URL
  AwaitConsent,                   // URL shown, no callback code yet
  Exchange,                       // code in hand, mint the tokens
  Done,                           // refresh token stored
  Failed,                         // a terminal error was recorded
};

// The minimal state the planner reasons over (no secrets beyond what the device
// already holds transiently during the flow).
struct FlowState {
  bool        haveIssuer = false;        // an authorization-server issuer is known
  bool        haveEndpoints = false;     // authorization + token endpoints known
  bool        haveClientId = false;      // dynamic registration done (or preset)
  bool        authorizeShown = false;    // the consent URL/code is displayed
  bool        haveCode = false;          // the callback delivered an auth code
  bool        haveRefreshToken = false;  // the exchange stored a refresh token
  bool        failed = false;            // a terminal error occurred
};
// The next step for a given state. Pure and total.
Step planNextStep(const FlowState& s);

// ---- launch-key gate (CUM-274) -----------------------------------------------

// May a caller of the device's `/oauth/go` redirect be handed the state-bearing
// authorization URL? Only when it presents the exact per-flow launch key the device
// minted at the (authenticated) flow start and revealed ONLY to the owner (the
// auth-gated status verify URL + the web-UI QR). `/oauth/go` itself is not token-
// gated - the owner opens it fresh on a phone - so this per-flow secret is what
// stops an unauthenticated LAN peer from reading `state` out of the redirect and
// binding their own account (account fixation). Fail-closed: an empty expected key
// (no flow awaiting consent) or an empty/mismatched presented key is never
// authorized. Constant-length compare is not required - the key is a 128-bit,
// per-flow value (valid only for the one consent window) with no oracle to iterate
// against.
bool launchAuthorized(const std::string& expectedKey, const std::string& providedKey);

}  // namespace oauth
}  // namespace mcp
}  // namespace orch
}  // namespace nimbus
