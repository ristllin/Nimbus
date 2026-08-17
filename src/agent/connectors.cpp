#include "connectors.h"
#include "store.h"
#include "agent_config.h"
#include "../sys/agent_log.h"
#include "../sys/net_util.h"
#include "../sys/tls_arbiter.h"

#include "nimbus/orch/connectors_wire.h"   // portable attach builders + catalog

#include <WiFiClientSecure.h>
#include <memory>
#include <new>       // std::nothrow - alloc failure degrades, never panics
#include <vector>

namespace agent {
namespace connectors {

int list(Info out[], int maxN) {
  String blob = store::connectorsJson();
  if (blob.length() == 0) return 0;
  JsonDocument d;
  if (deserializeJson(d, blob)) return 0;   // malformed blob: no connectors
  int n = 0;
  for (JsonObjectConst c : d.as<JsonArrayConst>()) {
    if (n >= maxN) {
      // Never truncate in SILENCE (prism 2026-08-09): the byte-capped blob can
      // hold ~60 minimal entries, so hitting maxN means real connectors just
      // vanished from the caller's view - the exact N=8 bug, recreated at N=24
      // - while /api/connectors (which parses the blob directly) still shows
      // them. Loud log so the divergence is observable.
      alogf("connectors: list() capped at %d - %d blob entr%s DROPPED",
            maxN, (int)d.as<JsonArrayConst>().size() - n,
            (int)d.as<JsonArrayConst>().size() - n == 1 ? "y" : "ies");
      break;
    }
    Info& i = out[n];
    i = Info{};
    i.name        = (const char*)(c["name"] | "");
    i.prov        = (const char*)(c["prov"] | "any");
    i.kind        = (const char*)(c["kind"] | "mcp");
    i.url         = (const char*)(c["url"]  | "");
    i.connectorId = (const char*)(c["cid"]  | "");
    i.type        = (const char*)(c["type"] | "");
    i.tok         = (const char*)(c["tok"]  | "");
    i.enabled     = (c["en"] | 0) != 0;
    if (!c["oauth"].isNull()) {
      i.hasOauth      = true;
      i.oauthUrl      = (const char*)(c["oauth"]["rurl"] | "");
      i.oauthClientId = (const char*)(c["oauth"]["cid"]  | "");
      i.oauthSecret   = (const char*)(c["oauth"]["sec"]  | "");
      i.oauthRefresh  = (const char*)(c["oauth"]["rtok"] | "");
    }
    if (i.name.length()) n++;               // nameless entries are skipped
  }
  return n;
}

// ---- T3: OAuth refresh broker ------------------------------------------------
// RAM cache of minted access tokens, keyed by connector name. Tokens are NEVER
// persisted (NVS holds only the refresh credentials); a reboot just re-mints.
namespace {
struct TokCache { String name; String access; uint32_t expiresAt; };
static TokCache s_tok[4];

// W12: per-connector auth OUTCOME (RAM, since boot). The connectors blob holds
// only owner-declared config - enabled was rendered as if it meant working.
// -1 = no live signal yet (static token present, or never attached);
//  1 = an OAuth mint SUCCEEDED this boot (the credential provably works);
//  0 = the last OAuth mint FAILED (bad/expired refresh credential - needs the
//      owner). Keyed like s_tok; small fixed table.
struct AuthState { String name; int8_t st; };
static AuthState s_auth[8];
void noteAuth(const String& name, int8_t st) {
  int slot = 0;
  for (int i = 0; i < 8; i++) {
    if (s_auth[i].name == name) { slot = i; break; }
    if (s_auth[i].name.length() == 0) slot = i;
  }
  s_auth[slot] = {name, st};
}

// POST a form-encoded refresh grant to the token URL, parse {access_token,
// expires_in}. One bounded TLS exchange under the work arbiter (same discipline
// as the provider adapters). Returns "" on any failure (attach proceeds unauth'd
// and the provider's 401 shows up in /api/log - never a crash path).
String refreshAccess(const Info& c) {
  int hs = c.oauthUrl.indexOf("://");
  if (hs < 0) return "";
  String rest = c.oauthUrl.substring(hs + 3);
  int slash = rest.indexOf('/');
  String host = slash > 0 ? rest.substring(0, slash) : rest;
  String path = slash > 0 ? rest.substring(slash) : "/";

  String form = String("grant_type=refresh_token")
              + "&refresh_token=" + c.oauthRefresh
              + "&client_id="     + c.oauthClientId;
  if (c.oauthSecret.length()) form += "&client_secret=" + c.oauthSecret;

  if (!arbiter::acquireWork(10000)) return "";
  WiFiClientSecure client;
  tlsSetup(client);
  client.setHandshakeTimeout(12);
  client.setConnectionTimeout(15000);  // F25: real socket bound (setTimeout is inert here)
  if (!client.connect(host.c_str(), 443)) {
    arbiter::releaseWork();
    alogf("connectors: oauth connect %s failed", host.c_str());
    return "";
  }
  String req = String("POST ") + path + " HTTP/1.0\r\n"
             + "Host: " + host + "\r\n"
             + "Content-Type: application/x-www-form-urlencoded\r\n"
             + "Content-Length: " + form.length() + "\r\n"
             + "Connection: close\r\n\r\n";
  client.print(req); client.print(form);

  const uint32_t deadline = millis() + 15000;
  String status;
  while ((int32_t)(millis() - deadline) < 0) {
    if (client.available()) { char ch = client.read(); if (ch == '\n') break; if (ch != '\r') status += ch; }
    else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  String line; bool headersDone = false;
  while (!headersDone && (int32_t)(millis() - deadline) < 0) {
    if (client.available()) {
      char ch = client.read();
      if (ch == '\n') { if (line.length() == 0) headersDone = true; line = ""; }
      else if (ch != '\r') line += ch;
    } else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  JsonDocument doc;
  if (headersDone) deserializeJson(doc, client);
  tlsClose(client);
  arbiter::releaseWork();

  const char* at = doc["access_token"] | "";
  if (!at[0]) {
    alogf("connectors: oauth refresh %s failed (%s)", c.name.c_str(), status.c_str());
    noteAuth(c.name, 0);   // provable sign-in failure - surfaced in the catalog
    return "";
  }
  const long ttl = doc["expires_in"] | 3600;
  // Cache (evict the stalest slot), expire 60 s early so an attach never rides
  // a token that dies mid-request.
  int slot = 0;
  for (int i = 0; i < 4; i++) {
    if (s_tok[i].name == c.name) { slot = i; break; }
    if (s_tok[i].expiresAt < s_tok[slot].expiresAt) slot = i;
  }
  s_tok[slot] = {c.name, String(at), millis() + (uint32_t)(ttl > 120 ? (ttl - 60) * 1000UL : 60000UL)};
  alogf("connectors: oauth minted for %s (ttl %lds)", c.name.c_str(), ttl);
  noteAuth(c.name, 1);   // the credential provably works (mint succeeded)
  return s_tok[slot].access;
}
}  // namespace

int8_t authStateOf(const String& name) {
  for (int i = 0; i < 8; i++)
    if (s_auth[i].name == name) return s_auth[i].st;
  return -1;   // no live signal since boot
}

String bearerFor(const Info& c) {
  if (c.tok.length()) return c.tok;          // T2 static token
  if (!c.hasOauth || !c.oauthUrl.length() || !c.oauthRefresh.length()) return "";
  for (int i = 0; i < 4; i++)                // T3: cached mint still fresh?
    if (s_tok[i].name == c.name && (int32_t)(s_tok[i].expiresAt - millis()) > 0)
      return s_tok[i].access;
  return refreshAccess(c);
}

// ---- portable-layer glue ------------------------------------------------------
// Parse the NVS blob into the portable ConnectorInfo vector (no secrets - the
// bearer is resolved separately via the closure below).
namespace {
std::vector<nimbus::orch::ConnectorInfo> portableList() {
  // Heap, not stack: kMaxConnectors Infos are ~5 KB of Strings - see the
  // kMaxConnectors note in connectors.h (the old Info[8] here silently hid
  // every connector past the eighth from BOTH the catalog and the wire attach).
  std::unique_ptr<Info[]> cs(new (std::nothrow) Info[kMaxConnectors]);
  if (!cs) return {};   // exhausted heap: an empty catalog beats a panic-reboot
  const int n = list(cs.get(), kMaxConnectors);
  std::vector<nimbus::orch::ConnectorInfo> out;
  out.reserve(n);
  for (int i = 0; i < n; i++) {
    nimbus::orch::ConnectorInfo c;
    c.name        = cs[i].name.c_str();
    c.prov        = cs[i].prov.c_str();
    c.kind        = cs[i].kind.c_str();
    c.url         = cs[i].url.c_str();
    c.connectorId = cs[i].connectorId.c_str();
    c.type        = cs[i].type.length() ? cs[i].type.c_str() : cs[i].name.c_str();
    c.enabled     = cs[i].enabled;
    // W12: honest per-connector credential state for the catalog. Built-ins
    // authenticate provider-side (no device credential); a remote MCP/first-
    // party connector NEEDS one - enabled-with-no-credential is skipped at
    // attach, so the model must not treat it as usable.
    if (cs[i].kind == "builtin")            c.auth = -1;              // n/a
    else if (cs[i].tok.length())            c.auth = 1;               // static token present
    else if (cs[i].hasOauth)                c.auth = authStateOf(cs[i].name);  // live mint outcome
    else                                    c.auth = 2;               // credential MISSING
    out.push_back(std::move(c));
  }
  return out;
}

// Bearer closure: re-parse the matching blob entry (for the tok/OAuth secrets the
// portable ConnectorInfo deliberately omits) and mint/return its bearer.
//
// ⚠ prism (2026-08-09): this was the MISSED fourth Info[8] site - and the
// kMaxConnectors fix elsewhere made it a live bug rather than dead code:
// portableList() now surfaces connector #9+, the wire attaches it, this closure
// re-listed only the first 8, found no name match, returned "" - and the
// connector went out UNAUTHENTICATED (provider 401) on exactly the entries the
// fix un-hid. Heap-allocated like the other three sites (tg_poll stack), and
// nothrow so an exhausted heap degrades to "no bearer" instead of a panic.
nimbus::orch::BearerFn bearerClosure() {
  return [](const nimbus::orch::ConnectorInfo& pc) -> std::string {
    std::unique_ptr<Info[]> cs(new (std::nothrow) Info[kMaxConnectors]);
    if (!cs) return std::string();
    const int n = list(cs.get(), kMaxConnectors);
    for (int i = 0; i < n; i++) {
      if (cs[i].name == pc.name.c_str()) return bearerFor(cs[i]).c_str();
    }
    return std::string();
  };
}

// The resolved current head: explicit orchHost(), else the first token of
// providerPriority() - the same rule compose.cpp/engine.cpp use.
std::string currentHost() {
  String h = store::orchHost();
  if (h.length()) return h.c_str();
  String pri = store::providerPriority();
  int e = 0;
  while (e < (int)pri.length()) {
    char ch = pri[e];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) break;
    e++;
  }
  return std::string(pri.c_str(), e);
}
}  // namespace

// ---- attach -------------------------------------------------------------------

void attachOpenAI(JsonDocument& d) {
  nimbus::orch::attachOpenAIWire(d, portableList(), bearerClosure());
}

void attachMistral(JsonDocument& d) {
  nimbus::orch::attachMistralWire(d, portableList());
}

void attachAnthropic(JsonDocument& agentBody) {
  nimbus::orch::attachAnthropicWire(agentBody, portableList(), bearerClosure());
}

// ---- catalog ------------------------------------------------------------------

String catalog() {
  nimbus::orch::ProviderState ps;
  ps.openaiKeyed    = store::hasOpenaiKey();
  ps.anthropicKeyed = store::hasAnthropicKey();
  ps.mistralKeyed   = store::hasMistralKey();
  ps.openaiVerified    = store::verifyResult("openai");
  ps.anthropicVerified = store::verifyResult("anthropic");
  ps.mistralVerified   = store::verifyResult("mistral");
  ps.capProbe       = (int8_t)store::capProbe();   // W3b: off => no verified claims
  ps.currentHost    = currentHost();
  return nimbus::orch::catalogText(portableList(), ps).c_str();
}

String knownCatalog() {
  return nimbus::orch::knownCatalogJson().c_str();
}

}  // namespace connectors
}  // namespace agent
