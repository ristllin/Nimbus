#include "nimbus/orch/mcp_oauth.h"

#include <ArduinoJson.h>

#include "nimbus/util/b64url.h"
#include "nimbus/util/sha256.h"

namespace nimbus {
namespace orch {
namespace mcp {
namespace oauth {

using ArduinoJson::DeserializationError;
using ArduinoJson::JsonArray;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

// ---- URL / form encoding -----------------------------------------------------

std::string pctEncode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                            c == '_' || c == '~';
    if (unreserved) {
      out += (char)c;
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xf];
    }
  }
  return out;
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& kv) {
  std::string out;
  for (const auto& p : kv) {
    if (p.second.empty()) continue;  // omit empty values (see header)
    if (!out.empty()) out += '&';
    out += pctEncode(p.first);
    out += '=';
    out += pctEncode(p.second);
  }
  return out;
}

// ---- PKCE --------------------------------------------------------------------

Pkce makePkce(const uint8_t rand[32]) {
  Pkce p;
  p.verifier = b64::urlEncode(rand, 32);
  p.challenge = b64::urlEncode(crypto::Sha256::digest(p.verifier));
  p.method = "S256";
  return p;
}

// ---- small URL helper --------------------------------------------------------

namespace {
// Extract the scheme://host[:port] origin of an absolute http(s) URL. "" if the
// URL is not http/https or has no host.
std::string originOf(const std::string& url) {
  size_t sep = url.find("://");
  if (sep == std::string::npos) return "";
  std::string scheme = url.substr(0, sep);
  for (char& c : scheme) c = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  if (scheme != "http" && scheme != "https") return "";
  size_t hostStart = sep + 3;
  size_t pathStart = url.find('/', hostStart);
  std::string authority =
      (pathStart == std::string::npos) ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
  if (authority.empty()) return "";
  return scheme + "://" + authority;
}

// Collect a JSON string array field into a vector (skips non-strings).
void collectStrings(JsonArrayConst arr, std::vector<std::string>& out) {
  for (JsonArrayConst::iterator it = arr.begin(); it != arr.end(); ++it) {
    const char* s = (*it).as<const char*>();
    if (s && s[0]) out.push_back(s);
  }
}
}  // namespace

// ---- metadata discovery ------------------------------------------------------

std::string wellKnownProtectedResource(const std::string& resourceUrl) {
  std::string o = originOf(resourceUrl);
  if (o.empty()) return "";
  return o + "/.well-known/oauth-protected-resource";
}

std::string wellKnownAuthServer(const std::string& issuer) {
  std::string o = originOf(issuer);
  if (o.empty()) return "";
  return o + "/.well-known/oauth-authorization-server";
}

std::string resourceMetadataFromWwwAuth(const std::string& wwwAuthenticate) {
  // Look for resource_metadata="<url>" (quoted) or resource_metadata=<url>.
  const std::string key = "resource_metadata";
  size_t k = wwwAuthenticate.find(key);
  if (k == std::string::npos) return "";
  size_t eq = wwwAuthenticate.find('=', k + key.size());
  if (eq == std::string::npos) return "";
  size_t i = eq + 1;
  while (i < wwwAuthenticate.size() && (wwwAuthenticate[i] == ' ' || wwwAuthenticate[i] == '\t')) i++;
  std::string val;
  if (i < wwwAuthenticate.size() && wwwAuthenticate[i] == '"') {
    size_t end = wwwAuthenticate.find('"', i + 1);
    if (end == std::string::npos) return "";
    val = wwwAuthenticate.substr(i + 1, end - (i + 1));
  } else {
    size_t end = i;
    while (end < wwwAuthenticate.size() && wwwAuthenticate[end] != ',' &&
           wwwAuthenticate[end] != ' ' && wwwAuthenticate[end] != ';')
      end++;
    val = wwwAuthenticate.substr(i, end - i);
  }
  return originOf(val).empty() ? "" : val;  // only accept an absolute http(s) URL
}

ProtectedResourceMeta parseProtectedResourceMetadata(const std::string& json) {
  ProtectedResourceMeta r;
  JsonDocument d;
  if (deserializeJson(d, json) != DeserializationError::Ok || !d.is<JsonObjectConst>()) return r;
  r.resource = (const char*)(d["resource"] | "");
  collectStrings(d["authorization_servers"].as<JsonArrayConst>(), r.authorizationServers);
  collectStrings(d["scopes_supported"].as<JsonArrayConst>(), r.scopesSupported);
  // A doc with no authorization_servers is not useful for the flow.
  r.ok = !r.authorizationServers.empty();
  return r;
}

std::string scopeStringFor(const std::vector<std::string>& scopesSupported) {
  std::string out;
  for (const auto& s : scopesSupported) {
    if (s == "openid" || s == "profile" || s == "email") continue;  // OIDC, not needed
    if (!out.empty()) out += ' ';
    out += s;
  }
  return out;
}

AuthServerMeta parseAuthServerMetadata(const std::string& json) {
  AuthServerMeta m;
  JsonDocument d;
  if (deserializeJson(d, json) != DeserializationError::Ok || !d.is<JsonObjectConst>()) return m;
  m.issuer = (const char*)(d["issuer"] | "");
  m.authorizationEndpoint = (const char*)(d["authorization_endpoint"] | "");
  m.tokenEndpoint = (const char*)(d["token_endpoint"] | "");
  m.registrationEndpoint = (const char*)(d["registration_endpoint"] | "");
  collectStrings(d["code_challenge_methods_supported"].as<JsonArrayConst>(), m.codeChallengeMethodsSupported);
  collectStrings(d["grant_types_supported"].as<JsonArrayConst>(), m.grantTypesSupported);
  collectStrings(d["scopes_supported"].as<JsonArrayConst>(), m.scopesSupported);
  collectStrings(d["token_endpoint_auth_methods_supported"].as<JsonArrayConst>(),
                 m.tokenEndpointAuthMethodsSupported);
  m.ok = !m.authorizationEndpoint.empty() && !m.tokenEndpoint.empty();
  return m;
}

bool supportsS256(const AuthServerMeta& m) {
  if (m.codeChallengeMethodsSupported.empty()) return false;
  for (const auto& s : m.codeChallengeMethodsSupported)
    if (s == "S256") return true;
  return false;
}

// ---- dynamic client registration ---------------------------------------------

std::string buildRegistrationRequest(const RegistrationParams& p) {
  JsonDocument d;
  d["client_name"] = p.clientName;
  JsonArray ru = d["redirect_uris"].to<JsonArray>();
  for (const auto& u : p.redirectUris) ru.add(u);
  d["token_endpoint_auth_method"] = p.tokenEndpointAuthMethod;
  JsonArray gt = d["grant_types"].to<JsonArray>();
  for (const auto& g : p.grantTypes) gt.add(g);
  JsonArray rt = d["response_types"].to<JsonArray>();
  for (const auto& t : p.responseTypes) rt.add(t);
  if (!p.scope.empty()) d["scope"] = p.scope;
  std::string out;
  serializeJson(d, out);
  return out;
}

RegistrationResult parseRegistrationResponse(const std::string& json) {
  RegistrationResult r;
  JsonDocument d;
  if (deserializeJson(d, json) != DeserializationError::Ok || !d.is<JsonObjectConst>()) {
    r.error = "unreadable registration response";
    return r;
  }
  const char* err = d["error"] | "";
  if (err[0]) {
    r.error = err;
    return r;
  }
  r.clientId = (const char*)(d["client_id"] | "");
  r.clientSecret = (const char*)(d["client_secret"] | "");
  r.ok = !r.clientId.empty();
  if (!r.ok) r.error = "registration response carried no client_id";
  return r;
}

// ---- authorization request ---------------------------------------------------

std::string buildAuthorizeUrl(const AuthorizeParams& p) {
  std::string url = p.authorizationEndpoint;
  const bool hasQuery = url.find('?') != std::string::npos;
  char sep = hasQuery ? '&' : '?';
  auto add = [&](const char* k, const std::string& v) {
    if (v.empty()) return;
    url += sep;
    sep = '&';
    url += k;
    url += '=';
    url += pctEncode(v);
  };
  add("response_type", "code");
  add("client_id", p.clientId);
  add("redirect_uri", p.redirectUri);
  add("code_challenge", p.codeChallenge);
  add("code_challenge_method", p.codeChallenge.empty() ? "" : std::string("S256"));
  add("state", p.state);
  add("scope", p.scope);
  add("resource", p.resource);
  return url;
}

// ---- token endpoint ----------------------------------------------------------

std::string buildCodeExchangeForm(const CodeExchangeParams& p) {
  return formEncode({
      {"grant_type", "authorization_code"},
      {"code", p.code},
      {"redirect_uri", p.redirectUri},
      {"client_id", p.clientId},
      {"client_secret", p.clientSecret},
      {"code_verifier", p.codeVerifier},
      {"resource", p.resource},
  });
}

std::string buildRefreshForm(const RefreshParams& p) {
  return formEncode({
      {"grant_type", "refresh_token"},
      {"refresh_token", p.refreshToken},
      {"client_id", p.clientId},
      {"client_secret", p.clientSecret},
      {"scope", p.scope},
  });
}

TokenResponse parseTokenResponse(const std::string& json) {
  TokenResponse r;
  JsonDocument d;
  if (deserializeJson(d, json) != DeserializationError::Ok || !d.is<JsonObjectConst>()) {
    r.error = "invalid_response";
    r.errorDescription = "token endpoint response could not be read";
    return r;
  }
  const char* err = d["error"] | "";
  if (err[0]) {
    r.error = err;
    r.errorDescription = (const char*)(d["error_description"] | "");
    return r;
  }
  r.accessToken = (const char*)(d["access_token"] | "");
  r.refreshToken = (const char*)(d["refresh_token"] | "");
  r.expiresIn = d["expires_in"] | 0L;
  r.tokenType = (const char*)(d["token_type"] | "");
  r.scope = (const char*)(d["scope"] | "");
  r.ok = !r.accessToken.empty();
  if (!r.ok) {
    r.error = "invalid_response";
    r.errorDescription = "token endpoint returned no access_token";
  }
  return r;
}

// ---- flow planning -----------------------------------------------------------

Step planNextStep(const FlowState& s) {
  if (s.failed) return Step::Failed;
  if (s.haveRefreshToken) return Step::Done;
  if (!s.haveIssuer) return Step::DiscoverProtectedResource;
  if (!s.haveEndpoints) return Step::DiscoverAuthServer;
  if (!s.haveClientId) return Step::Register;
  if (!s.authorizeShown) return Step::Authorize;
  if (!s.haveCode) return Step::AwaitConsent;
  return Step::Exchange;
}

}  // namespace oauth
}  // namespace mcp
}  // namespace orch
}  // namespace nimbus
