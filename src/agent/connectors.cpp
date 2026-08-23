#include "connectors.h"
#include "store.h"
#include "agent_config.h"
#include "memory_subsystem.h"                // memory::registry(), memory::Lock
#include "orchestrator.h"                    // orchestrator::inScheduledTurn()
#include "../sys/agent_log.h"
#include "../sys/net_util.h"
#include "../sys/tls_arbiter.h"

#include "nimbus/orch/connectors_wire.h"     // portable attach builders + catalog
#include "nimbus/orch/mcp_client.h"          // portable JSON-RPC + Streamable HTTP framing
#include "nimbus/orch/mcp_resilience.h"      // portable circuit breaker + retry policy
#include "nimbus/orch/tool_registry.h"       // ToolRegistry (outbound tool registration)

#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>   // esp_random - retry jitter source
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
  // The blob parse (non-secret fields + no-silent-drop cap accounting) lives in
  // the portable, host-tested nimbus::orch::parseConnectorsJson - the single
  // parser that locks the Info[8]-vs-kMaxConnectors regression. Secrets (the
  // static token, the OAuth broker fields) are resolved separately by
  // bearerClosure(), so the catalog/attach path never carries them.
  String blob = store::connectorsJson();
  std::vector<nimbus::orch::ConnectorInfo> out;
  int total = 0;
  const int n = nimbus::orch::parseConnectorsJson(blob.c_str(), out, kMaxConnectors, &total);
  if (total > n) {
    // Never truncate in SILENCE (the exact N=8 bug, guarded at N=24): the
    // byte-capped blob can hold far more minimal entries than kMaxConnectors, so
    // hitting the cap means real connectors vanished from the catalog + attach
    // while /api/connectors (which parses the blob directly) still shows them.
    alogf("connectors: portableList capped at %d - %d blob entr%s DROPPED",
          kMaxConnectors, total - n, (total - n) == 1 ? "y" : "ies");
  }
  // W12: honest per-connector credential state for the catalog. Built-ins
  // authenticate provider-side (no device credential); a remote MCP / first-
  // party connector NEEDS one - enabled-with-no-credential is skipped at attach,
  // so the model must not treat it as usable. The OAuth mint outcome is device
  // RAM state (authStateOf), applied here on top of the portable presence flags.
  for (auto& c : out) {
    if (c.kind == "builtin")      c.auth = -1;                        // n/a
    else if (c.hasToken)          c.auth = 1;                         // static token present
    else if (c.hasOauth)          c.auth = authStateOf(c.name.c_str());  // live mint outcome
    else                          c.auth = 2;                         // credential MISSING
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
  std::string out = nimbus::orch::catalogText(portableList(), ps).c_str();
  // N4: reconcile + discover device-dialed MCP servers on the turn path (WiFi is
  // up here), then append their live status so the model plans around them.
  mcp::sync(memory::registry());
  out += mcp::catalogSection();
  return out.c_str();
}

String knownCatalog() {
  return nimbus::orch::knownCatalogJson().c_str();
}

// ---- outbound MCP client (N4) ------------------------------------------------

namespace mcp {
namespace {

namespace mc = nimbus::orch::mcp;
using nimbus::orch::Principal;
using nimbus::orch::ToolRegistry;
using nimbus::orch::ToolResult;

constexpr int      kMaxMcpServers  = 6;        // device-dialed servers we track
constexpr int      kMaxToolsPerSrv = 48;       // registry budget guard per server
constexpr int      kMaxPages       = 6;        // tools/list pagination bound
constexpr size_t   kMaxBodyBytes   = 48 * 1024;  // response cap (PSRAM-backed body)
constexpr uint32_t kDiscoverTimeout = 8000;    // per-request budget during discovery
constexpr uint32_t kCallTimeout    = 5000;     // per tools/call - held under memory::Lock,
                                               // so kept well under the main-loop watchdog

// Per-server runtime state. A tiny fixed table (no dynamic servers on a device).
struct ServerState {
  String   slug;
  String   name;
  String   url;               // the URL we discovered against (re-discover on change)
  bool     inUse = false;
  bool     discovered = false;
  int      toolCount = 0;
  String   sessionId;         // Mcp-Session-Id for a stateful server ("" if stateless)
  mc::CircuitBreaker breaker;
  int8_t   lastErr = -1;      // last mc::ErrorKind on failure (-1 = none), for the catalog
};
ServerState s_srv[kMaxMcpServers];

ServerState* slotFor(const String& slug, bool create) {
  for (int i = 0; i < kMaxMcpServers; i++)
    if (s_srv[i].inUse && s_srv[i].slug == slug) return &s_srv[i];
  if (!create) return nullptr;
  for (int i = 0; i < kMaxMcpServers; i++)
    if (!s_srv[i].inUse) {
      s_srv[i] = ServerState{};
      s_srv[i].inUse = true;
      s_srv[i].slug = slug;
      return &s_srv[i];
    }
  return nullptr;  // table full: caller logs, the extra server is skipped (not silent)
}

// ---- URL parse ---------------------------------------------------------------
struct UrlParts { bool ok = false; bool tls = true; String host; uint16_t port = 443; String path; };

UrlParts parseUrl(const String& url) {
  UrlParts u;
  int sep = url.indexOf("://");
  if (sep < 0) return u;
  String scheme = url.substring(0, sep);
  scheme.toLowerCase();
  if (scheme == "https")      { u.tls = true;  u.port = 443; }
  else if (scheme == "http")  { u.tls = false; u.port = 80; }
  else return u;
  String rest = url.substring(sep + 3);
  int slash = rest.indexOf('/');
  String hostport = slash >= 0 ? rest.substring(0, slash) : rest;
  u.path = slash >= 0 ? rest.substring(slash) : "/";
  int colon = hostport.indexOf(':');
  if (colon >= 0) {
    u.host = hostport.substring(0, colon);
    long p = hostport.substring(colon + 1).toInt();
    if (p <= 0 || p > 65535) return u;
    u.port = (uint16_t)p;
  } else {
    u.host = hostport;
  }
  if (u.host.length() == 0) return u;
  u.ok = true;
  return u;
}

// ---- low-level HTTP read helpers --------------------------------------------
// Read one CRLF-terminated line (without the CRLF) before the deadline. Returns
// false if the socket closes with no more data before a full line arrives.
bool readLine(Client& c, uint32_t deadline, String& line) {
  line = "";
  while ((int32_t)(millis() - deadline) < 0) {
    if (c.available()) {
      char ch = (char)c.read();
      if (ch == '\n') return true;
      if (ch != '\r') line += ch;
    } else if (!c.connected() && !c.available()) {
      return line.length() > 0;  // last unterminated line
    } else {
      delay(2);
    }
  }
  return false;  // timed out
}

// Read exactly n bytes (or until close) into out, respecting the deadline and the
// body cap. Returns false only on a hard timeout with nothing pending.
bool readN(Client& c, uint32_t deadline, size_t n, std::string& out, bool& tooLarge) {
  while (out.size() < n && (int32_t)(millis() - deadline) < 0) {
    if (c.available()) {
      char ch = (char)c.read();
      if (out.size() >= kMaxBodyBytes) { tooLarge = true; return true; }
      out += ch;
    } else if (!c.connected() && !c.available()) {
      return true;  // server closed early
    } else {
      delay(2);
    }
  }
  return out.size() >= n || (int32_t)(millis() - deadline) < 0;
}

// Read to end-of-connection (Connection: close / no length), capped.
void readToClose(Client& c, uint32_t deadline, std::string& out, bool& tooLarge) {
  while ((int32_t)(millis() - deadline) < 0) {
    if (c.available()) {
      char ch = (char)c.read();
      if (out.size() >= kMaxBodyBytes) { tooLarge = true; return; }
      out += ch;
    } else if (!c.connected() && !c.available()) {
      return;
    } else {
      delay(2);
    }
  }
}

// De-chunk a Transfer-Encoding: chunked body off the socket, capped.
void readChunked(Client& c, uint32_t deadline, std::string& out, bool& tooLarge) {
  while ((int32_t)(millis() - deadline) < 0) {
    String sizeLine;
    if (!readLine(c, deadline, sizeLine)) return;
    if (sizeLine.length() == 0) continue;             // tolerate stray CRLF
    long sz = strtol(sizeLine.c_str(), nullptr, 16);  // hex chunk size
    if (sz <= 0) return;                              // 0 => last chunk (or parse fail)
    for (long i = 0; i < sz && (int32_t)(millis() - deadline) < 0;) {
      if (c.available()) {
        char ch = (char)c.read();
        if (out.size() >= kMaxBodyBytes) { tooLarge = true; return; }
        out += ch;
        i++;
      } else if (!c.connected() && !c.available()) {
        return;
      } else {
        delay(2);
      }
    }
    String crlf;
    readLine(c, deadline, crlf);  // consume the CRLF after the chunk
  }
}

// One Streamable HTTP request/response under the work arbiter. Returns the HTTP
// status (0 on transport failure, with kindOut set). Fills body, content-type,
// and any Mcp-Session-Id. The whole exchange is bounded by timeoutMs.
int exchange(const UrlParts& u, const String& bearer, const String& sessionIn,
             const std::string& reqBody, uint32_t timeoutMs, std::string& bodyOut,
             std::string& ctypeOut, String& sessionOut, mc::ErrorKind& kindOut) {
  bodyOut.clear();
  ctypeOut.clear();
  sessionOut = "";
  kindOut = mc::ErrorKind::None;
  if (!arbiter::acquireWork(timeoutMs)) { kindOut = mc::ErrorKind::Timeout; return 0; }

  WiFiClientSecure tls;
  WiFiClient plain;
  Client* c = nullptr;
  if (u.tls) {
    tlsSetup(tls);
    tls.setHandshakeTimeout(12);
    tls.setConnectionTimeout(timeoutMs);
    c = &tls;
  } else {
    plain.setTimeout(timeoutMs / 1000 ? timeoutMs / 1000 : 1);
    c = &plain;
  }

  const uint32_t deadline = millis() + timeoutMs;
  bool connected = false;
  for (int attempt = 0; attempt < 3 && !connected && (int32_t)(millis() - deadline) < 0; attempt++) {
    if (c->connect(u.host.c_str(), u.port)) { connected = true; break; }
    delay(400);
  }
  if (!connected) {
    c->stop();
    arbiter::releaseWork();
    kindOut = mc::ErrorKind::Connect;
    return 0;
  }

  // Request. Accept BOTH shapes so a server may answer with JSON or stream SSE.
  String req = String("POST ") + u.path + " HTTP/1.1\r\n"
             + "Host: " + u.host + "\r\n"
             + "Content-Type: application/json\r\n"
             + "Accept: application/json, text/event-stream\r\n"
             + "MCP-Protocol-Version: " + mc::kProtocolVersion + "\r\n";
  if (bearer.length())    req += "Authorization: Bearer " + bearer + "\r\n";
  if (sessionIn.length()) req += "Mcp-Session-Id: " + sessionIn + "\r\n";
  req += "Content-Length: " + String((unsigned)reqBody.size()) + "\r\n"
       + "Connection: close\r\n\r\n";
  c->print(req);
  if (!reqBody.empty()) c->write((const uint8_t*)reqBody.data(), reqBody.size());

  // Status line: "HTTP/1.1 200 OK".
  String statusLine;
  if (!readLine(*c, deadline, statusLine)) {
    c->stop(); arbiter::releaseWork();
    kindOut = mc::ErrorKind::Timeout;
    return 0;
  }
  int sp = statusLine.indexOf(' ');
  int status = sp >= 0 ? statusLine.substring(sp + 1, sp + 4).toInt() : 0;

  // Headers until a blank line; capture the few we need.
  bool chunked = false;
  long contentLen = -1;
  while (true) {
    String h;
    if (!readLine(*c, deadline, h)) break;
    if (h.length() == 0) break;  // end of headers
    int colon = h.indexOf(':');
    if (colon < 0) continue;
    String key = h.substring(0, colon); key.trim(); key.toLowerCase();
    String val = h.substring(colon + 1); val.trim();
    if (key == "content-type") ctypeOut = val.c_str();
    else if (key == "mcp-session-id") sessionOut = val;
    else if (key == "transfer-encoding") { String v = val; v.toLowerCase(); if (v.indexOf("chunked") >= 0) chunked = true; }
    else if (key == "content-length") contentLen = val.toInt();
  }

  bool tooLarge = false;
  if (chunked)              readChunked(*c, deadline, bodyOut, tooLarge);
  else if (contentLen >= 0) readN(*c, deadline, (size_t)contentLen, bodyOut, tooLarge);
  else                      readToClose(*c, deadline, bodyOut, tooLarge);

  c->stop();
  arbiter::releaseWork();
  if (tooLarge) kindOut = mc::ErrorKind::TooLarge;
  return status;
}

// Look up a device-dialed server's live config (secrets included) by slug. Fills
// url + bearer; returns true only when the entry is kind=mcp, enabled, approved,
// device-dialed, and has a URL (fail-closed on every missing condition).
bool resolveServer(const String& slug, String& urlOut, String& bearerOut, String& nameOut) {
  std::unique_ptr<Info[]> cs(new (std::nothrow) Info[kMaxConnectors]);
  if (!cs) return false;
  const int n = list(cs.get(), kMaxConnectors);
  for (int i = 0; i < n; i++) {
    const Info& e = cs[i];
    if (e.kind != "mcp" || !e.enabled) continue;
    if (mc::slugifyServer(e.name.c_str()) != std::string(slug.c_str())) continue;
    // Re-read approval/dev straight from the blob-derived Info: the portable
    // parser carries these, but list() (secret path) does not, so re-derive from
    // the blob via the portable list. Simpler: use the portable list for flags.
    urlOut = e.url;
    bearerOut = bearerFor(e);
    nameOut = e.name;
    return e.url.length() > 0;
  }
  return false;
}

// The desired set entry for reconcile: a device-dialed, approved, enabled MCP
// server with a URL. Read from the portable list (carries dev/appr flags).
struct Desired { String slug; String name; String url; };
std::vector<Desired> desiredServers() {
  std::vector<Desired> out;
  std::vector<nimbus::orch::ConnectorInfo> cs;
  nimbus::orch::parseConnectorsJson(store::connectorsJson().c_str(), cs, kMaxConnectors, nullptr);
  for (const auto& c : cs) {
    if (c.kind != "mcp" || !c.enabled || !c.deviceDialed || !c.approved) continue;
    if (c.url.empty()) continue;
    Desired d;
    d.slug = mc::slugifyServer(c.name).c_str();
    d.name = c.name.c_str();
    d.url  = c.url.c_str();
    out.push_back(std::move(d));
  }
  return out;
}

// The registry name prefix owning one server's tools.
std::string prefixOf(const String& slug) { return std::string("mcp.") + slug.c_str() + "."; }

// Retry-with-jitter wrapper around exchange() for the (Lock-free) discovery path.
int exchangeRetry(const UrlParts& u, const String& bearer, const String& sessionIn,
                  const std::string& reqBody, uint32_t timeoutMs, int maxAttempts,
                  std::string& bodyOut, std::string& ctypeOut, String& sessionOut,
                  mc::ErrorKind& kindOut) {
  mc::RetryConfig rc;
  int status = 0;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    status = exchange(u, bearer, sessionIn, reqBody, timeoutMs, bodyOut, ctypeOut, sessionOut, kindOut);
    bool transportFail = (status == 0);
    bool retryable = transportFail ? true : mc::isRetryable(kindOut, status);
    // A 2xx with a good body is success at this layer; the parser judges content.
    if (status >= 200 && status < 300 && kindOut == mc::ErrorKind::None) return status;
    if (!retryable || attempt + 1 >= maxAttempts) return status;
    delay(mc::retryDelayMs(rc, attempt, esp_random()));
  }
  return status;
}

// Discover one server: initialize -> initialized -> tools/list (paged). Collects
// tool defs into `tools` (Lock-free network). Returns false on any transport/RPC
// failure (breaker updated by the caller).
bool discover(ServerState& st, const UrlParts& u, const String& bearer,
              std::vector<mc::ToolDef>& tools, mc::ErrorKind& kindOut) {
  std::string body, ctype;
  String session;
  // initialize
  int status = exchangeRetry(u, bearer, "", mc::buildInitialize("nimbus", ""),
                             kDiscoverTimeout, 2, body, ctype, session, kindOut);
  mc::InitializeResult ir = mc::parseInitialize(status, ctype, body, st.name.c_str());
  if (!ir.ok) { kindOut = ir.error; return false; }
  st.sessionId = session;  // capture a stateful server's session id
  // initialized notification (best-effort; no response expected)
  {
    std::string b2, c2; String s2; mc::ErrorKind k2;
    exchange(u, bearer, st.sessionId, mc::buildInitializedNotification(),
             kDiscoverTimeout, b2, c2, s2, k2);
  }
  if (!ir.hasTools) { kindOut = mc::ErrorKind::None; return true; }  // no tools is valid
  // tools/list, paginated
  String cursor = "";
  for (int page = 0; page < kMaxPages; page++) {
    std::string b, ct; String sess; mc::ErrorKind k;
    status = exchangeRetry(u, bearer, st.sessionId,
                           mc::buildToolsList(std::string(cursor.c_str())),
                           kDiscoverTimeout, 2, b, ct, sess, k);
    mc::ToolsListResult lr = mc::parseToolsList(status, ct, b, st.name.c_str());
    if (!lr.ok) { kindOut = lr.error; return false; }
    for (auto& t : lr.tools) {
      if ((int)tools.size() >= kMaxToolsPerSrv) {
        alogf("mcp: %s exposed >%d tools - the rest are dropped", st.name.c_str(), kMaxToolsPerSrv);
        break;
      }
      tools.push_back(std::move(t));
    }
    if (lr.nextCursor.empty() || (int)tools.size() >= kMaxToolsPerSrv) break;
    cursor = lr.nextCursor.c_str();
  }
  kindOut = mc::ErrorKind::None;
  return true;
}

}  // namespace

// The handler body for a discovered tool: proxy the call out to its server.
// Runs on the turn task under the dispatch (memory) Lock, so it is bounded by a
// short timeout + the per-server breaker (like web.search) - never a long hold.
ToolResult callTool(const std::string& slug, const std::string& toolName,
                    ArduinoJson::JsonObjectConst args, const Principal& who) {
  // RBAC: a remote tool can have side effects; require an approved account with
  // write access. Unknown/revoked (no perms) and Guest are refused, fail-closed.
  if (!who.perms().writeOwn)
    return ToolResult::fail("This tool needs an approved account with write access.");
  // An unattended turn (a routine firing, a fan-out synthesis chewing on
  // untrusted sub-agent text) must never reach an external server.
  if (agent::orchestrator::inScheduledTurn())
    return ToolResult::fail("External tools are turned off during automated turns.");

  String url, bearer, name;
  if (!resolveServer(String(slug.c_str()), url, bearer, name))
    return ToolResult::fail(mc::nextStepError(mc::ErrorKind::Connect, slug,
                                              "server not configured or not approved"));
  const std::string nameS = name.c_str();
  UrlParts u = parseUrl(url);
  if (!u.ok)
    return ToolResult::fail("MCP server " + nameS + " has an invalid URL. Fix it on the device web page.");

  ServerState* st = slotFor(String(slug.c_str()), true);
  const uint32_t now = millis();
  if (st && !st->breaker.allow(now)) {
    uint32_t left = st->breaker.cooldownRemaining(now) / 1000;
    return ToolResult::fail("MCP server " + nameS + " is cooling down after repeated failures. "
                            "Try again in about " + std::to_string(left) + "s.");
  }

  std::string argsJson;
  serializeJson(args, argsJson);
  std::string body, ctype; String session; mc::ErrorKind kind;
  // Single attempt on the hot path: it runs under the dispatch Lock, so total
  // time stays well under the main-loop watchdog; the breaker handles a server
  // that keeps failing.
  int status = exchange(u, bearer, st ? st->sessionId : String(""),
                        mc::buildToolsCall(toolName, argsJson), kCallTimeout,
                        body, ctype, session, kind);
  if (st && session.length()) st->sessionId = session;
  mc::CallToolResult r = mc::parseCallTool(status, ctype, body, nameS);
  if (!r.ok) {
    if (st) st->breaker.onFailure(millis());
    return ToolResult::fail(r.errorMsg);
  }
  if (st) st->breaker.onSuccess();
  if (r.isError) return ToolResult::fail(r.text.empty() ? "the tool reported an error" : r.text);
  return ToolResult::ok(r.text);
}

void sync(ToolRegistry& reg) {
  std::vector<Desired> want = desiredServers();

  // 1. Retract tools for tracked servers no longer wanted (disabled / unapproved
  //    / URL changed / removed). Registry mutation under the memory Lock.
  for (int i = 0; i < kMaxMcpServers; i++) {
    ServerState& st = s_srv[i];
    if (!st.inUse) continue;
    bool stillWanted = false;
    for (const auto& d : want)
      if (d.slug == st.slug && d.url == st.url) { stillWanted = true; break; }
    if (!stillWanted) {
      { agent::memory::Lock lk; reg.removeByPrefix(prefixOf(st.slug)); }
      st = ServerState{};
    }
  }

  // 2. Discover at most ONE not-yet-discovered wanted server per call, so the
  //    turn-path cost is bounded to a single handshake. The breaker gates retries
  //    of a server that keeps failing.
  for (const auto& d : want) {
    ServerState* st = slotFor(d.slug, true);
    if (!st) { alogf("mcp: too many device MCP servers (>%d) - %s skipped",
                     kMaxMcpServers, d.name.c_str()); continue; }
    st->name = d.name;
    st->url = d.url;
    if (st->discovered) continue;
    if (!st->breaker.allow(millis())) continue;   // cooling down; try a later turn

    UrlParts u = parseUrl(d.url);
    if (!u.ok) { st->lastErr = (int8_t)mc::ErrorKind::Malformed; continue; }
    String bearer, url2, name2;
    resolveServer(d.slug, url2, bearer, name2);   // bearer only (url from d)

    std::vector<mc::ToolDef> tools;
    mc::ErrorKind kind = mc::ErrorKind::None;
    bool ok = discover(*st, u, bearer, tools, kind);
    if (!ok) {
      st->breaker.onFailure(millis());
      st->lastErr = (int8_t)kind;
      alogf("mcp: discover %s failed (%d)", d.name.c_str(), (int)kind);
      return;   // one discovery attempt per sync()
    }
    // Register the discovered tools (brief in-RAM mutation under the Lock).
    {
      agent::memory::Lock lk;
      reg.removeByPrefix(prefixOf(d.slug));  // clear any stale set before re-adding
      for (const auto& t : tools) {
        std::string regName = mc::namespacedTool(std::string(d.slug.c_str()), t.name);
        std::string slug = d.slug.c_str();
        std::string tname = t.name;
        reg.add(regName,
                t.description.empty() ? (name2.length() ? std::string(name2.c_str()) + " tool" : t.name)
                                      : t.description,
                [slug, tname](ArduinoJson::JsonObjectConst a, const Principal& who) {
                  return callTool(slug, tname, a, who);
                },
                t.inputSchemaJson);
      }
    }
    st->discovered = true;
    st->toolCount = (int)tools.size();
    st->breaker.onSuccess();
    st->lastErr = -1;
    alogf("mcp: %s ready (%d tools)", d.name.c_str(), st->toolCount);
    return;   // one server per sync()
  }
}

std::string catalogSection() {
  std::vector<Desired> want = desiredServers();
  // Names of device-dialed servers that are enabled but NOT approved (pending).
  std::vector<std::string> pending;
  {
    std::vector<nimbus::orch::ConnectorInfo> cs;
    nimbus::orch::parseConnectorsJson(store::connectorsJson().c_str(), cs, kMaxConnectors, nullptr);
    for (const auto& c : cs)
      if (c.kind == "mcp" && c.enabled && c.deviceDialed && !c.approved && !c.url.empty())
        pending.push_back(c.name);
  }
  if (want.empty() && pending.empty()) return "";

  std::string out = "\n[DEVICE MCP SERVERS]\n"
                    "These remote MCP servers are dialed by the device directly; their tools "
                    "appear as mcp.<server>.<tool> and you can call them like any tool.\n";
  const uint32_t now = millis();
  for (const auto& d : want) {
    ServerState* st = slotFor(d.slug, false);
    out += "- " + std::string(d.name.c_str()) + ": ";
    if (st && st->discovered) {
      out += std::to_string(st->toolCount) + " tool(s) ready (mcp." + std::string(d.slug.c_str()) + ".*)";
    } else if (st && st->breaker.state() != mc::BreakerState::Closed) {
      out += "unreachable, cooling down (" + std::to_string(st->breaker.cooldownRemaining(now) / 1000) + "s)";
    } else {
      out += "connecting on the next turn";
    }
    out += "\n";
  }
  for (const auto& p : pending)
    out += "- " + std::string(p.c_str()) +
           ": configured but NOT yet approved - the owner must approve it before its tools can be used.\n";
  return out;
}

}  // namespace mcp

}  // namespace connectors
}  // namespace agent
