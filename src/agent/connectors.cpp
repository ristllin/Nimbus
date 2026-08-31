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

#include <WiFi.h>          // WiFi.status() - the OAuth pump waits for a link
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "nimbus/orch/mcp_oauth.h"           // portable OAuth 2.1 decision layer
#include <esp_system.h>   // esp_random - retry jitter source
#include <cstring>        // strcmp - non-allocating state compare in the OAuth callback
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
  // Assistant > Tools "Code sandbox" (CUM-49): inject the code_interpreter builtin
  // when the toggle is on, so the OpenAI/Mistral sandbox is enabled without an
  // explicit connector card. Skipped if a code_interpreter card is already present.
  if (store::codeSandbox()) {
    bool have = false;
    for (const auto& e : out)
      if ((e.type.empty() ? e.name : e.type) == "code_interpreter" && e.kind == "builtin")
        have = true;
    if (!have) {
      // provMatches() is an EXACT provider match, so inject one entry per provider
      // (openai + mistral, the two with a code_interpreter builtin) rather than a
      // comma list that would match neither.
      for (const char* prov : {"openai", "mistral"}) {
        nimbus::orch::ConnectorInfo sb;
        sb.name = "code_interpreter";
        sb.type = "code_interpreter";
        sb.prov = prov;
        sb.kind = "builtin";
        sb.enabled = true;
        sb.auth = -1;   // builtins authenticate provider-side
        out.push_back(std::move(sb));
      }
    }
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

// Forward declaration (defined after the anonymous namespace) so the discovery
// closures registered below can reference the proxy handler.
nimbus::orch::ToolResult callTool(const std::string& slug, const std::string& toolName,
                                  ArduinoJson::JsonObjectConst args,
                                  const nimbus::orch::Principal& who);

namespace {

namespace mc = nimbus::orch::mcp;
using nimbus::orch::Principal;
using nimbus::orch::ToolRegistry;
using nimbus::orch::ToolResult;

constexpr int      kMaxMcpServers  = 6;        // device-dialed servers we track
constexpr int      kMaxToolsPerSrv = 48;       // registry budget guard per server
constexpr int      kMaxPages       = 6;        // tools/list pagination bound
// Response body cap (PSRAM-backed). Sized from a LIVE capture: the real Linear
// MCP tools/list is ~75 KB (53 tools with rich schemas) and arrives in ONE page,
// so a smaller cap (an earlier 48 KB) would TooLarge a real server on discovery.
// 160 KB clears that with headroom while staying well under the transport's
// 256 KB ceiling; PSRAM holds it, and discovery is Lock-free so the parse time
// is bounded only by the discovery timeout.
constexpr size_t   kMaxBodyBytes   = 160 * 1024;
constexpr uint32_t kDiscoverTimeout = 8000;    // per-request budget during discovery
// A tools/call runs UNDER the dispatch (memory) Lock, and it is reachable on the
// AsyncTCP /mcp task as well as the turn task, so the whole exchange must stay
// well under the main-loop 8 s watchdog: cap the work-slot wait AND the I/O so
// the worst case (busy slot, then a slow server) is ~kCallAcquire + kCallTimeout.
constexpr uint32_t kCallTimeout    = 4000;     // per tools/call I/O deadline
constexpr uint32_t kCallAcquire    = 2000;     // max wait for the TLS work slot on a call

// Per-server runtime state. A tiny fixed table (no dynamic servers on a device).
// ⚠ s_srv is read/written by BOTH the turn task (sync/catalogSection via
// catalog()) and the AsyncTCP /mcp task (callTool via handleRpc). EVERY access
// MUST hold agent::memory::Lock - the one recursive mutex handleMcp already takes
// around dispatch - so the two tasks never race on these String members. The
// network itself always runs OUTSIDE the Lock.
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

// Caller MUST hold agent::memory::Lock (see the s_srv note above).
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

// A request/response for one Streamable HTTP exchange, bundled so the transport
// helpers stay under the argument + complexity budget.
struct McpReq {
  UrlParts    u;
  String      bearer;
  String      session;      // Mcp-Session-Id to echo (empty = none)
  std::string body;
  uint32_t    timeoutMs = 8000;
  uint32_t    acquireMs  = 0;   // max wait for the work slot (0 = use timeoutMs).
                                // The call path sets this small so the memory-Lock
                                // hold stays bounded when the slot is busy.
};
struct McpResp {
  int           status = 0;             // HTTP status (0 = transport failure)
  std::string   body;
  std::string   ctype;                  // Content-Type
  String        session;                // Mcp-Session-Id from the response
  mc::ErrorKind kind = mc::ErrorKind::None;
};

// Open the socket (TLS or plain) with a short connect-retry, bounded by deadline.
bool mcpConnect(Client& c, const UrlParts& u, uint32_t deadline) {
  for (int attempt = 0; attempt < 3 && (int32_t)(millis() - deadline) < 0; attempt++) {
    if (c.connect(u.host.c_str(), u.port)) return true;
    delay(400);
  }
  return false;
}

// Send the POST request line, headers, and body. Accept BOTH response shapes so
// a server may answer with a JSON body or a streamed (SSE) one.
void mcpSend(Client& c, const McpReq& q) {
  String req = String("POST ") + q.u.path + " HTTP/1.1\r\n"
             + "Host: " + q.u.host + "\r\n"
             + "Content-Type: application/json\r\n"
             + "Accept: application/json, text/event-stream\r\n"
             + "MCP-Protocol-Version: " + mc::kProtocolVersion + "\r\n";
  if (q.bearer.length())  req += "Authorization: Bearer " + q.bearer + "\r\n";
  if (q.session.length()) req += "Mcp-Session-Id: " + q.session + "\r\n";
  req += "Content-Length: " + String((unsigned)q.body.size()) + "\r\n"
       + "Connection: close\r\n\r\n";
  c.print(req);
  if (!q.body.empty()) c.write((const uint8_t*)q.body.data(), q.body.size());
}

// Read the status line + headers. Returns the HTTP status (0 on timeout before a
// status line); sets r.ctype/r.session and the body-framing out-params.
int mcpReadHead(Client& c, uint32_t deadline, McpResp& r, bool& chunked, long& contentLen) {
  chunked = false;
  contentLen = -1;
  String statusLine;
  if (!readLine(c, deadline, statusLine)) return 0;
  int sp = statusLine.indexOf(' ');
  int status = sp >= 0 ? statusLine.substring(sp + 1, sp + 4).toInt() : 0;
  String h;
  while (readLine(c, deadline, h) && h.length() > 0) {
    int colon = h.indexOf(':');
    if (colon < 0) continue;
    String key = h.substring(0, colon); key.trim(); key.toLowerCase();
    String val = h.substring(colon + 1); val.trim();
    if (key == "content-type") r.ctype = val.c_str();
    else if (key == "mcp-session-id") r.session = val;
    else if (key == "transfer-encoding") { val.toLowerCase(); if (val.indexOf("chunked") >= 0) chunked = true; }
    else if (key == "content-length") contentLen = val.toInt();
  }
  return status;
}

// One Streamable HTTP request/response under the work arbiter. Fills `r`; on a
// transport failure r.status == 0 and r.kind says why. Bounded by q.timeoutMs.
void exchange(const McpReq& q, McpResp& r) {
  r = McpResp{};
  if (!arbiter::acquireWork(q.acquireMs ? q.acquireMs : q.timeoutMs)) {
    r.kind = mc::ErrorKind::Timeout;
    return;
  }
  WiFiClientSecure tls;
  WiFiClient plain;
  Client* c;
  if (q.u.tls) {
    tlsSetup(tls);
    tls.setHandshakeTimeout(12);
    tls.setConnectionTimeout(q.timeoutMs);
    c = &tls;
  } else {
    plain.setTimeout(q.timeoutMs / 1000 ? q.timeoutMs / 1000 : 1);
    c = &plain;
  }
  const uint32_t deadline = millis() + q.timeoutMs;
  if (!mcpConnect(*c, q.u, deadline)) {
    c->stop(); arbiter::releaseWork();
    r.kind = mc::ErrorKind::Connect;
    return;
  }
  mcpSend(*c, q);
  bool chunked;
  long clen;
  r.status = mcpReadHead(*c, deadline, r, chunked, clen);
  if (r.status == 0) {
    c->stop(); arbiter::releaseWork();
    r.kind = mc::ErrorKind::Timeout;
    return;
  }
  bool tooLarge = false;
  if (chunked)        readChunked(*c, deadline, r.body, tooLarge);
  else if (clen >= 0) readN(*c, deadline, (size_t)clen, r.body, tooLarge);
  else                readToClose(*c, deadline, r.body, tooLarge);
  c->stop();
  arbiter::releaseWork();
  if (tooLarge) r.kind = mc::ErrorKind::TooLarge;
}

// Resolve a device-dialed server's live URL + bearer by slug, FAIL-CLOSED on the
// same predicate as desiredServers (kind=mcp, enabled, device-dialed, approved,
// has url). This is the dial-time gate, so a revoked (appr:0) or non-device
// server whose stale tool is still registered, or a same-slug provider-only MCP,
// is never dialed. The dev/appr flags come from the portable parser (list()/Info
// does not carry them); the secret bearer comes from the secret-carrying list().
bool resolveServer(const String& slug, String& urlOut, String& bearerOut, String& nameOut) {
  std::vector<nimbus::orch::ConnectorInfo> pcs;
  nimbus::orch::parseConnectorsJson(store::connectorsJson().c_str(), pcs, kMaxConnectors, nullptr);
  String name;
  for (const auto& c : pcs) {
    if (c.kind != "mcp" || !c.enabled || !c.deviceDialed || !c.approved || c.url.empty()) continue;
    if (mc::slugifyServer(c.name) != std::string(slug.c_str())) continue;
    name = c.name.c_str();
    urlOut = c.url.c_str();
    break;
  }
  if (name.length() == 0) return false;   // not an approved, device-dialed server
  std::unique_ptr<Info[]> cs(new (std::nothrow) Info[kMaxConnectors]);
  if (!cs) return false;
  const int n = list(cs.get(), kMaxConnectors);
  for (int i = 0; i < n; i++)
    if (cs[i].name == name) { bearerOut = bearerFor(cs[i]); break; }
  nameOut = name;
  return true;
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
void exchangeRetry(const McpReq& q, int maxAttempts, McpResp& r) {
  mc::RetryConfig rc;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    exchange(q, r);
    if (r.status >= 200 && r.status < 300 && r.kind == mc::ErrorKind::None) return;  // parser judges content
    // Retryable: a transport failure (status 0), any 5xx (exchange leaves kind
    // None for HTTP responses, so check the status directly), or a transient kind.
    bool retryable = (r.status == 0) ? true
                   : (r.status >= 500 && r.status < 600) ? true
                   : mc::isRetryable(r.kind, r.status);
    if (!retryable || attempt + 1 >= maxAttempts) return;
    delay(mc::retryDelayMs(rc, attempt, esp_random()));
  }
}

// Collect one tools/list page into `tools`; returns the ToolsListResult so the
// caller can read ok/nextCursor.
mc::ToolsListResult collectToolsPage(const McpReq& base, const String& session,
                                     const String& serverName, const String& cursor,
                                     std::vector<mc::ToolDef>& tools) {
  McpReq lq = base;
  lq.session = session;
  lq.body = mc::buildToolsList(std::string(cursor.c_str()));
  McpResp r;
  exchangeRetry(lq, 2, r);
  mc::ToolsListResult lr = mc::parseToolsList(r.status, r.ctype, r.body, serverName.c_str());
  if (!lr.ok) return lr;
  for (auto& t : lr.tools) {
    if ((int)tools.size() >= kMaxToolsPerSrv) {
      alogf("mcp: %s exposed >%d tools - the rest are dropped", serverName.c_str(), kMaxToolsPerSrv);
      break;
    }
    tools.push_back(std::move(t));
  }
  return lr;
}

// Discover one server: initialize -> initialized -> tools/list (paged). Pure
// NETWORK, touching NO shared s_srv state (so it runs entirely OUTSIDE the memory
// Lock): tool defs land in `tools`, any session id in `sessionOut`. Returns false
// on any transport/RPC failure.
bool discover(const String& serverName, const UrlParts& u, const String& bearer,
              std::vector<mc::ToolDef>& tools, String& sessionOut, mc::ErrorKind& kindOut) {
  McpReq base;
  base.u = u;
  base.bearer = bearer;
  base.timeoutMs = kDiscoverTimeout;
  McpReq init = base;
  init.body = mc::buildInitialize("nimbus", "");
  McpResp r;
  exchangeRetry(init, 2, r);
  mc::InitializeResult ir = mc::parseInitialize(r.status, r.ctype, r.body, serverName.c_str());
  if (!ir.ok) { kindOut = ir.error; return false; }
  sessionOut = r.session;  // capture a stateful server's session id
  {  // initialized notification (best-effort; no response expected)
    McpReq n = base;
    n.session = sessionOut;
    n.body = mc::buildInitializedNotification();
    McpResp nr;
    exchange(n, nr);
  }
  if (!ir.hasTools) { kindOut = mc::ErrorKind::None; return true; }  // no tools is valid
  String cursor = "";
  for (int page = 0; page < kMaxPages; page++) {
    mc::ToolsListResult lr = collectToolsPage(base, sessionOut, serverName, cursor, tools);
    if (!lr.ok) { kindOut = lr.error; return false; }
    if (lr.nextCursor.empty() || (int)tools.size() >= kMaxToolsPerSrv) break;
    cursor = lr.nextCursor.c_str();
  }
  kindOut = mc::ErrorKind::None;
  return true;
}

// Register a discovered server's tools into the registry. CALLER MUST HOLD
// agent::memory::Lock (this is an in-RAM mutation of the shared registry, which
// the AsyncTCP /mcp reader also touches under that Lock).
void registerToolsLocked(ToolRegistry& reg, const String& slug, const std::vector<mc::ToolDef>& tools) {
  reg.removeByPrefix(prefixOf(slug));   // clear any stale set before re-adding
  for (const auto& t : tools) {
    std::string regName = mc::namespacedTool(std::string(slug.c_str()), t.name);
    std::string s = slug.c_str();
    std::string tn = t.name;
    std::string desc = t.description.empty() ? (std::string(slug.c_str()) + " tool") : t.description;
    reg.add(regName, desc,
            [s, tn](ArduinoJson::JsonObjectConst a, const Principal& who) { return callTool(s, tn, a, who); },
            t.inputSchemaJson);
  }
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

  // resolveServer is FAIL-CLOSED: it returns false unless the slug is an enabled,
  // device-dialed, APPROVED mcp entry - so a revoked server whose stale tool is
  // still registered, or a same-slug provider-only MCP, is never dialed.
  String url, bearer, name;
  if (!resolveServer(String(slug.c_str()), url, bearer, name))
    return ToolResult::fail(mc::nextStepError(mc::ErrorKind::Connect, slug,
                                              "server not configured or not approved"));
  const std::string nameS = name.c_str();
  UrlParts u = parseUrl(url);
  if (!u.ok)
    return ToolResult::fail("MCP server " + nameS + " has an invalid URL. Fix it on the device web page.");

  // Guard the shared s_srv slot (see the s_srv note): this handler is reachable on
  // the AsyncTCP /mcp task, so hold the same Lock sync() uses. The Lock is
  // recursive, so nesting under handleMcp's own Lock is a no-op. exchange()'s
  // work-slot wait is capped (kCallAcquire) so the hold stays under the watchdog.
  agent::memory::Lock lk;
  ServerState* st = slotFor(String(slug.c_str()), true);
  const uint32_t now = millis();
  if (st && !st->breaker.allow(now)) {
    uint32_t left = st->breaker.cooldownRemaining(now) / 1000;
    return ToolResult::fail("MCP server " + nameS + " is cooling down after repeated failures. "
                            "Try again in about " + std::to_string(left) + "s.");
  }

  std::string argsJson;
  serializeJson(args, argsJson);
  // Single attempt on the hot path, with a bounded work-slot wait + I/O deadline
  // so the memory-Lock hold stays under the main-loop watchdog; the breaker
  // handles a server that keeps failing.
  McpReq q;
  q.u = u;
  q.bearer = bearer;
  q.session = st ? st->sessionId : String("");
  q.body = mc::buildToolsCall(toolName, argsJson);
  q.timeoutMs = kCallTimeout;
  q.acquireMs = kCallAcquire;
  McpResp resp;
  exchange(q, resp);
  if (st && resp.session.length()) st->sessionId = resp.session;
  mc::CallToolResult r = mc::parseCallTool(resp.status, resp.ctype, resp.body, nameS);
  if (!r.ok) {
    if (st) st->breaker.onFailure(millis());
    return ToolResult::fail(r.errorMsg);
  }
  if (st) st->breaker.onSuccess();
  if (r.isError) return ToolResult::fail(r.text.empty() ? "the tool reported an error" : r.text);
  return ToolResult::ok(r.text);
}

// Phase 1 (LOCK, in-RAM only): retract tools for servers no longer wanted, then
// pick at most ONE not-yet-discovered server to dial. Returns true + fills `pick`.
bool reconcileAndPick(ToolRegistry& reg, const std::vector<Desired>& want, Desired& pick) {
  agent::memory::Lock lk;
  for (int i = 0; i < kMaxMcpServers; i++) {
    ServerState& st = s_srv[i];
    if (!st.inUse) continue;
    bool keep = false;
    for (const auto& d : want)
      if (d.slug == st.slug && d.url == st.url) { keep = true; break; }
    if (!keep) { reg.removeByPrefix(prefixOf(st.slug)); st = ServerState{}; }
  }
  for (const auto& d : want) {
    ServerState* st = slotFor(d.slug, true);
    if (!st) {
      alogf("mcp: too many device MCP servers (>%d) - %s skipped", kMaxMcpServers, d.name.c_str());
      continue;
    }
    st->name = d.name;
    st->url = d.url;
    if (st->discovered) continue;
    if (!st->breaker.allow(millis())) continue;   // cooling down; try a later turn
    pick = d;
    return true;
  }
  return false;
}

// The result of a phase-2 network discovery, handed to phase 3.
struct DiscoveryOutcome {
  bool                     urlOk = false;  // the picked URL parsed
  bool                     ok = false;     // discovery succeeded
  std::vector<mc::ToolDef> tools;
  String                   session;
  mc::ErrorKind            kind = mc::ErrorKind::None;
};

// Phase 3 (LOCK, in-RAM only): commit the discovery outcome to the slot + registry.
void commitDiscovery(ToolRegistry& reg, const Desired& pick, const DiscoveryOutcome& o) {
  agent::memory::Lock lk;
  ServerState* st = slotFor(pick.slug, true);
  if (!st) return;   // table filled up between phases (config churn); next turn retries
  if (!o.ok) {
    st->breaker.onFailure(millis());
    st->lastErr = (int8_t)(o.urlOk ? o.kind : mc::ErrorKind::Malformed);
    alogf("mcp: discover %s failed (%d)", pick.name.c_str(), (int)st->lastErr);
    return;
  }
  registerToolsLocked(reg, pick.slug, o.tools);
  st->discovered = true;
  st->toolCount = (int)o.tools.size();
  st->sessionId = o.session;
  st->breaker.onSuccess();
  st->lastErr = -1;
  alogf("mcp: %s ready (%d tools)", pick.name.c_str(), st->toolCount);
}

void sync(ToolRegistry& reg) {
  std::vector<Desired> want = desiredServers();   // blob parse only, no s_srv - Lock-free
  Desired pick;
  if (!reconcileAndPick(reg, want, pick)) return;  // phase 1 (Lock)
  // phase 2 (LOCK-FREE): the network handshake for the picked server.
  UrlParts u = parseUrl(pick.url);
  String url2, bearer, name2;
  DiscoveryOutcome o;
  o.urlOk = u.ok;
  bool okCfg = u.ok && resolveServer(pick.slug, url2, bearer, name2);
  o.ok = okCfg && discover(pick.name, u, bearer, o.tools, o.session, o.kind);
  commitDiscovery(reg, pick, o);                   // phase 3 (Lock)
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
  {
    agent::memory::Lock lk;   // reads the shared s_srv table (see its note)
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
  }
  for (const auto& p : pending)
    out += "- " + std::string(p.c_str()) +
           ": configured but NOT yet approved - the owner must approve it before its tools can be used.\n";
  return out;
}

// ---- OAuth 2.1 acquisition driver (CUM-256) ----------------------------------
// See connectors.h. All the pure decisions are in nimbus::orch::mcp::oauth (host-
// tested); this driver does the bounded TLS I/O under the shared work arbiter and
// the NVS blob write. It is a single-flight state machine advanced ONE network
// step per pump() on the main loop - never a new task, never a second TLS slot.
namespace oauth {

namespace {
namespace oa = nimbus::orch::mcp::oauth;

// A general (non-MCP) bounded HTTP request over the work arbiter, reusing the
// same socket/read helpers as exchange(). OAuth documents are small; the shared
// body cap (kMaxBodyBytes) is far more than enough.
struct HttpResp {
  int           status = 0;
  std::string   body;
  std::string   ctype;
  mc::ErrorKind kind = mc::ErrorKind::None;
};

// One general HTTP request over the arbiter. Bundled so the call stays under the
// argument-count gate; contentType is only emitted when there is a body.
struct HttpReq {
  const char* method = "GET";
  UrlParts    u;
  const char* contentType = "application/json";
  std::string body;
  uint32_t    timeoutMs = 8000;
};

void httpDo(const HttpReq& q, HttpResp& r) {
  const UrlParts& u = q.u;
  r = HttpResp{};
  if (!u.ok) { r.kind = mc::ErrorKind::Connect; return; }
  if (!arbiter::acquireWork(q.timeoutMs)) { r.kind = mc::ErrorKind::Timeout; return; }
  WiFiClientSecure tls;
  WiFiClient plain;
  Client* c;
  if (u.tls) {
    tlsSetup(tls);
    tls.setHandshakeTimeout(12);
    tls.setConnectionTimeout(q.timeoutMs);
    c = &tls;
  } else {
    plain.setTimeout(q.timeoutMs / 1000 ? q.timeoutMs / 1000 : 1);
    c = &plain;
  }
  const uint32_t deadline = millis() + q.timeoutMs;
  if (!mcpConnect(*c, u, deadline)) {
    c->stop(); arbiter::releaseWork();
    r.kind = mc::ErrorKind::Connect;
    return;
  }
  String req = String(q.method) + " " + u.path + " HTTP/1.1\r\n"
             + "Host: " + u.host + "\r\n"
             + "Accept: application/json\r\n";
  if (!q.body.empty())
    req += String("Content-Type: ") + q.contentType + "\r\n"
         + "Content-Length: " + String((unsigned)q.body.size()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  c->print(req);
  if (!q.body.empty()) c->write((const uint8_t*)q.body.data(), q.body.size());
  McpResp head;  // only for the header reader's out-params
  bool chunked;
  long clen;
  r.status = mcpReadHead(*c, deadline, head, chunked, clen);
  r.ctype = head.ctype;
  if (r.status == 0) {
    c->stop(); arbiter::releaseWork();
    r.kind = mc::ErrorKind::Timeout;
    return;
  }
  bool tooLarge = false;
  if (chunked)        readChunked(*c, deadline, r.body, tooLarge);
  else if (clen >= 0) readN(*c, deadline, (size_t)clen, r.body, tooLarge);
  else                readToClose(*c, deadline, r.body, tooLarge);
  c->stop();
  arbiter::releaseWork();
  if (tooLarge) r.kind = mc::ErrorKind::TooLarge;
}

// An OAuth endpoint must be a PUBLIC https URL. Requiring https protects the
// tokens in flight; requiring a non-private host (reusing the CUM-255 routability
// predicate) stops a malicious/compromised MCP server from steering the device's
// discovery + token requests at an internal address (SSRF). OAuth sign-in is for
// hosted servers; a LAN server should use a static token instead (see docs/mcp.md).
bool publicHttps(const String& url) {
  UrlParts u = parseUrl(url);
  return u.ok && u.tls && nimbus::orch::urlRoutableToProviderHead(std::string(url.c_str()));
}

// scheme://host[:port] origin of a parsed URL (default ports omitted).
String originOf(const UrlParts& u) {
  String o = (u.tls ? "https://" : "http://") + u.host;
  const bool defaultPort = (u.tls && u.port == 443) || (!u.tls && u.port == 80);
  if (!defaultPort) o += ":" + String(u.port);
  return o;
}

// A hex token of `bytes` bytes from the device RNG (for state + the cosmetic
// user code). Never a secret - the PKCE verifier is separate and never displayed.
String randHex(int bytes) {
  static const char* h = "0123456789abcdef";
  String s;
  for (int i = 0; i < bytes; i++) {
    uint8_t b = (uint8_t)(esp_random() & 0xFF);
    s += h[b >> 4];
    s += h[b & 0xf];
  }
  return s;
}

// The single-flight flow, OWNED by the main-loop pump (never touched off it).
enum class Phase : uint8_t { Idle, DiscoverPR, DiscoverAS, Register, ShowConsent, HaveCode, Done, Failed };
struct Flow {
  bool     active = false;
  Phase    phase = Phase::Idle;
  String   connName;
  String   resourceUrl;     // the MCP server URL (RFC 8707 resource)
  String   issuer;
  String   authorizeEndpoint, tokenEndpoint, registrationEndpoint;
  String   scope;
  String   clientId, clientSecret;
  String   verifier, challenge, state, userCode;
  // Per-flow launch key (CUM-274): minted at the authenticated flow start, revealed
  // ONLY to the owner (the auth-gated status verify URL + web-UI QR), and REQUIRED on
  // the otherwise-ungated /oauth/go redirect. Without it a LAN peer cannot obtain the
  // state-bearing authorize URL, so cannot leak `state` and bind their own account.
  String   launchKey;
  String   redirectUri, authorizeUrl, code;
  String   error;
  uint32_t nextStepMs = 0;
};
Flow s_flow;  // main-loop-owned

// Cross-task handoff (AsyncTCP web task <-> main-loop pump), fixed buffers only so
// the critical section never allocates. Strings live only in s_flow / snapshot.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_reqStart = false, s_reqCode = false, s_reqCancel = false;
char s_inName[64] = {0}, s_inRedirect[128] = {0}, s_inCode[1024] = {0}, s_inState[64] = {0};
// snapshot published for the web/screen (read on the web task).
volatile bool s_snapActive = false;
char s_snapPhase[16] = {0}, s_snapConn[64] = {0}, s_snapUserCode[16] = {0},
     s_snapVerify[160] = {0}, s_snapErr[192] = {0}, s_snapAuthUrl[1024] = {0};
// The state expected on the callback while awaiting consent, published so the
// (unauthenticated) /oauth/cb handler can DROP a non-matching callback before it
// touches the single handoff slot - a LAN peer cannot then clobber the real code
// with wrong-state spam. Empty unless a flow is awaiting consent. Not a secret and
// never returned by statusJson.
char s_snapState[64] = {0};
// The per-flow launch key required to read the authorize URL via /oauth/go (CUM-274).
// Published only while awaiting consent (empty otherwise). It reaches the owner only
// through the auth-gated verify URL / web QR, never through an unauthenticated path,
// and is compared by authorizeUrl() before any state-bearing URL is returned.
char s_snapLaunchKey[64] = {0};

void setBuf(char* dst, size_t cap, const char* src) {
  size_t n = 0;
  if (src)
    for (; n + 1 < cap && src[n]; n++) dst[n] = src[n];
  dst[n] = '\0';
}

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Idle:        return "idle";
    case Phase::DiscoverPR:  return "discovering";
    case Phase::DiscoverAS:  return "discovering";
    case Phase::Register:    return "registering";
    case Phase::ShowConsent: return "awaiting-consent";
    case Phase::HaveCode:    return "finishing";
    case Phase::Done:        return "connected";
    case Phase::Failed:      return "failed";
  }
  return "idle";
}

// Publish s_flow -> the snapshot buffers (called from the pump only).
void publish() {
  String verify = s_flow.redirectUri;  // the short device URL the QR/code points at
  if (verify.endsWith("/oauth/cb")) verify = verify.substring(0, verify.length() - 2) + "go";
  // Carry the per-flow launch key on the verify URL (CUM-274): the owner's QR / link
  // authorizes /oauth/go, while a keyless LAN request to it is turned away.
  if (s_flow.launchKey.length()) verify += "?k=" + s_flow.launchKey;
  portENTER_CRITICAL(&s_mux);
  s_snapActive = s_flow.active;
  setBuf(s_snapPhase, sizeof(s_snapPhase), phaseName(s_flow.phase));
  setBuf(s_snapConn, sizeof(s_snapConn), s_flow.connName.c_str());
  setBuf(s_snapUserCode, sizeof(s_snapUserCode), s_flow.userCode.c_str());
  setBuf(s_snapVerify, sizeof(s_snapVerify),
         (s_flow.phase == Phase::ShowConsent) ? verify.c_str() : "");
  setBuf(s_snapErr, sizeof(s_snapErr), s_flow.error.c_str());
  setBuf(s_snapAuthUrl, sizeof(s_snapAuthUrl),
         (s_flow.phase == Phase::ShowConsent) ? s_flow.authorizeUrl.c_str() : "");
  setBuf(s_snapState, sizeof(s_snapState),
         (s_flow.phase == Phase::ShowConsent) ? s_flow.state.c_str() : "");
  setBuf(s_snapLaunchKey, sizeof(s_snapLaunchKey),
         (s_flow.phase == Phase::ShowConsent) ? s_flow.launchKey.c_str() : "");
  portEXIT_CRITICAL(&s_mux);
}

void fail(const String& msg) {
  s_flow.phase = Phase::Failed;
  s_flow.error = msg;
  s_flow.nextStepMs = millis() + 3600000;  // park; a new start/cancel resets it
}

// Find a device-dialed mcp connector by name and set up a fresh flow.
void startFlow(const char* name, const char* redirectBase) {
  s_flow = Flow{};
  s_flow.connName = name;
  std::vector<nimbus::orch::ConnectorInfo> cs;
  nimbus::orch::parseConnectorsJson(store::connectorsJson().c_str(), cs, kMaxConnectors, nullptr);
  for (const auto& c : cs)
    if (c.kind == "mcp" && c.name == std::string(name) && !c.url.empty()) {
      s_flow.resourceUrl = c.url.c_str();
      break;
    }
  if (s_flow.resourceUrl.length() == 0) { s_flow.active = true; fail("This connector has no MCP server URL to connect."); return; }
  if (!publicHttps(s_flow.resourceUrl)) {
    s_flow.active = true;
    fail("Browser sign-in works only with a hosted https server. Use a token for a local server.");
    return;
  }
  String base = redirectBase;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  s_flow.redirectUri = base + "/oauth/cb";
  uint8_t rnd[32];
  for (int i = 0; i < 32; i++) rnd[i] = (uint8_t)(esp_random() & 0xFF);
  oa::Pkce pk = oa::makePkce(rnd);
  s_flow.verifier = pk.verifier.c_str();
  s_flow.challenge = pk.challenge.c_str();
  s_flow.state = randHex(16);
  s_flow.launchKey = randHex(16);   // 128-bit /oauth/go capability (CUM-274)
  s_flow.userCode = randHex(3);  // 6 hex chars, cosmetic
  s_flow.userCode.toUpperCase();
  s_flow.active = true;
  s_flow.phase = Phase::DiscoverPR;
  s_flow.nextStepMs = 0;
}

// --- one network step per phase ----------------------------------------------

// Fetch + parse protected-resource metadata from one candidate well-known URL.
// Fills issuer + scope from it on success; returns whether it yielded an issuer.
bool tryProtectedResource(const String& wk) {
  if (!publicHttps(wk)) return false;
  HttpResp r;
  { HttpReq q; q.method = "GET"; q.u = parseUrl(wk); q.timeoutMs = 8000; httpDo(q, r); }
  if (r.status < 200 || r.status >= 300) return false;
  oa::ProtectedResourceMeta m = oa::parseProtectedResourceMetadata(r.body);
  if (!m.ok) return false;
  String issuer = m.authorizationServers[0].c_str();
  if (!publicHttps(issuer)) return false;  // never follow a private/non-https issuer (SSRF)
  s_flow.issuer = issuer;
  s_flow.scope = oa::scopeStringFor(m.scopesSupported).c_str();
  return true;
}

void stepDiscoverPR() {
  UrlParts ru = parseUrl(s_flow.resourceUrl);
  if (!ru.ok) { fail("The MCP server URL is not a valid https address."); return; }
  const std::string res = std::string(s_flow.resourceUrl.c_str());
  // RFC 9728: try the path-suffixed metadata location first (the canonical spot,
  // where mcp.linear.app advertises it), then the origin-level form. If neither
  // yields an issuer, fall back to the resource's own origin as the issuer (the
  // common same-origin case). All candidates are gated to public https.
  if (!tryProtectedResource(oa::wellKnownProtectedResourcePath(res).c_str()) &&
      !tryProtectedResource(oa::wellKnownProtectedResource(res).c_str())) {
    s_flow.issuer = originOf(ru);  // resource is public https (checked at startFlow)
  }
  s_flow.phase = Phase::DiscoverAS;
  s_flow.nextStepMs = millis() + 300;
}

void stepDiscoverAS() {
  String wk = oa::wellKnownAuthServer(std::string(s_flow.issuer.c_str())).c_str();
  HttpResp r;
  { HttpReq q; q.method = "GET"; q.u = parseUrl(wk); q.timeoutMs = 8000; httpDo(q, r); }
  if (r.status < 200 || r.status >= 300) { fail("Could not read the sign-in service details from the server."); return; }
  oa::AuthServerMeta m = oa::parseAuthServerMetadata(r.body);
  if (!m.ok) { fail("The server did not advertise an OAuth authorization service."); return; }
  if (!oa::supportsS256(m)) { fail("The server's sign-in does not support the secure PKCE method."); return; }
  // The authorize + token endpoints come from server-controlled metadata and the
  // device POSTs to the token one: gate both to public https so a malicious server
  // cannot steer the device at an internal address (SSRF).
  if (!publicHttps(String(m.authorizationEndpoint.c_str())) ||
      !publicHttps(String(m.tokenEndpoint.c_str()))) {
    fail("The server's sign-in endpoints are not hosted at a public https address.");
    return;
  }
  s_flow.authorizeEndpoint = m.authorizationEndpoint.c_str();
  s_flow.tokenEndpoint = m.tokenEndpoint.c_str();
  // Only accept a registration endpoint that is itself public https.
  s_flow.registrationEndpoint =
      publicHttps(String(m.registrationEndpoint.c_str())) ? String(m.registrationEndpoint.c_str()) : String("");
  if (s_flow.scope.length() == 0) s_flow.scope = oa::scopeStringFor(m.scopesSupported).c_str();
  if (s_flow.registrationEndpoint.length() == 0) {
    fail("This server needs a client to be registered by hand; automatic sign-in is not available.");
    return;
  }
  s_flow.phase = Phase::Register;
  s_flow.nextStepMs = millis() + 300;
}

void buildAuthorize() {
  oa::AuthorizeParams ap;
  ap.authorizationEndpoint = s_flow.authorizeEndpoint.c_str();
  ap.clientId = s_flow.clientId.c_str();
  ap.redirectUri = s_flow.redirectUri.c_str();
  ap.codeChallenge = s_flow.challenge.c_str();
  ap.state = s_flow.state.c_str();
  ap.scope = s_flow.scope.c_str();
  ap.resource = s_flow.resourceUrl.c_str();
  s_flow.authorizeUrl = oa::buildAuthorizeUrl(ap).c_str();
}

void stepRegister() {
  oa::RegistrationParams p;
  p.redirectUris = {std::string(s_flow.redirectUri.c_str())};
  if (s_flow.scope.length()) p.scope = s_flow.scope.c_str();
  std::string body = oa::buildRegistrationRequest(p);
  HttpResp r;
  { HttpReq q; q.method = "POST"; q.u = parseUrl(s_flow.registrationEndpoint); q.contentType = "application/json"; q.body = body; q.timeoutMs = 10000; httpDo(q, r); }
  if (r.status < 200 || r.status >= 300) { fail("The server refused to register this device for sign-in."); return; }
  oa::RegistrationResult rr = oa::parseRegistrationResponse(r.body);
  if (!rr.ok) { fail("The server's sign-in registration did not return a client id."); return; }
  s_flow.clientId = rr.clientId.c_str();
  s_flow.clientSecret = rr.clientSecret.c_str();
  buildAuthorize();
  s_flow.phase = Phase::ShowConsent;
  s_flow.nextStepMs = millis() + 3600000;  // wait for the owner's callback
}

// Write the minted refresh token into the connector's T3 oauth blob slot, so
// bearerFor()/refreshAccess() mints access tokens from here on. NEVER logged.
bool writeRefreshToBlob(const oa::TokenResponse& tok) {
  JsonDocument d;
  if (deserializeJson(d, store::connectorsJson()) || !d.is<JsonArray>()) return false;
  for (JsonObject o : d.as<JsonArray>()) {
    if (s_flow.connName != (const char*)(o["name"] | "")) continue;
    JsonObject oauth = o["oauth"].to<JsonObject>();
    oauth["rurl"] = s_flow.tokenEndpoint;
    oauth["cid"] = s_flow.clientId;
    oauth["sec"] = s_flow.clientSecret;   // "" for a public client
    oauth["rtok"] = tok.refreshToken.c_str();
    String out;
    serializeJson(d, out);
    if (out.length() > 3500) return false;   // blob cap (same as the save endpoint)
    store::setConnectorsJson(out);
    store::setOrchConvId("");                // connectors pin at conversation creation
    return true;
  }
  return false;
}

void stepExchange() {
  oa::CodeExchangeParams p;
  p.code = s_flow.code.c_str();
  p.codeVerifier = s_flow.verifier.c_str();
  p.clientId = s_flow.clientId.c_str();
  p.clientSecret = s_flow.clientSecret.c_str();
  p.redirectUri = s_flow.redirectUri.c_str();
  p.resource = s_flow.resourceUrl.c_str();
  std::string body = oa::buildCodeExchangeForm(p);
  HttpResp r;
  { HttpReq q; q.method = "POST"; q.u = parseUrl(s_flow.tokenEndpoint); q.contentType = "application/x-www-form-urlencoded"; q.body = body; q.timeoutMs = 10000; httpDo(q, r); }
  oa::TokenResponse tok = oa::parseTokenResponse(r.body);
  if (!tok.ok) { fail("Sign-in did not complete. Start again from the connector."); return; }
  if (tok.refreshToken.empty()) { fail("The server returned no lasting sign-in; it must allow offline access."); return; }
  if (!writeRefreshToBlob(tok)) { fail("Signed in, but the connector set is full or unreadable. Free a slot and retry."); return; }
  s_flow.phase = Phase::Done;
  s_flow.error = "";
  s_flow.nextStepMs = millis() + 3600000;
  alogf("mcp oauth: %s connected (refresh token stored)", s_flow.connName.c_str());
}

}  // namespace

String begin(const String& name, const String& redirectBase) {
  if (name.length() == 0) return "Pick a connector to connect.";
  if (redirectBase.length() == 0) return "The device address is unknown; reload the page and try again.";
  portENTER_CRITICAL(&s_mux);
  setBuf(s_inName, sizeof(s_inName), name.c_str());
  setBuf(s_inRedirect, sizeof(s_inRedirect), redirectBase.c_str());
  s_reqStart = true;
  portEXIT_CRITICAL(&s_mux);
  return "";
}

bool callback(const String& state, const String& code, String& errOut) {
  if (code.length() == 0) { errOut = "The sign-in did not return an authorization code."; return false; }
  // Drop a callback whose state does not match the flow awaiting consent BEFORE it
  // touches the single handoff slot. Without this, an unauthenticated LAN peer could
  // spam /oauth/cb with a wrong state and overwrite the real code mid-race; the
  // pump's own state check would then discard the (now clobbered) real code. The
  // expected state is published only while awaiting consent (empty otherwise).
  const char* sc = state.c_str();  // no allocation inside the critical section
  const char* cc = code.c_str();
  portENTER_CRITICAL(&s_mux);
  // strcmp/setBuf only - never a String op (heap alloc) inside portENTER_CRITICAL.
  const bool match = s_snapState[0] && sc[0] && strcmp(sc, s_snapState) == 0;
  if (match && !s_reqCode) {
    setBuf(s_inState, sizeof(s_inState), sc);
    setBuf(s_inCode, sizeof(s_inCode), cc);
    s_reqCode = true;
  }
  portEXIT_CRITICAL(&s_mux);
  if (!match) { errOut = "This sign-in response did not match a request in progress."; return false; }
  return true;
}

void cancel() {
  portENTER_CRITICAL(&s_mux);
  s_reqCancel = true;
  portEXIT_CRITICAL(&s_mux);
}

String authorizeUrl(const String& launchKey) {
  char buf[1024], key[64];
  portENTER_CRITICAL(&s_mux);
  setBuf(buf, sizeof(buf), s_snapAuthUrl);
  setBuf(key, sizeof(key), s_snapLaunchKey);   // copy out; compare off the critical section
  portEXIT_CRITICAL(&s_mux);
  // CUM-274: only hand back the state-bearing authorize URL to a caller that presents
  // the owner-only launch key. A keyless / wrong-key request (any unauthenticated LAN
  // peer) gets "" and is redirected to the home page, so `state` never leaks.
  if (!oa::launchAuthorized(std::string(key), std::string(launchKey.c_str()))) return String();
  return String(buf);
}

String statusJson() {
  char phase[16], conn[64], userCode[16], verify[160], err[192];
  bool active;
  portENTER_CRITICAL(&s_mux);
  active = s_snapActive;
  setBuf(phase, sizeof(phase), s_snapPhase);
  setBuf(conn, sizeof(conn), s_snapConn);
  setBuf(userCode, sizeof(userCode), s_snapUserCode);
  setBuf(verify, sizeof(verify), s_snapVerify);
  setBuf(err, sizeof(err), s_snapErr);
  portEXIT_CRITICAL(&s_mux);
  JsonDocument d;
  d["active"] = active;
  d["phase"] = phase;
  d["conn"] = conn;
  d["userCode"] = userCode;
  d["verifyUrl"] = verify;
  d["error"] = err;
  String out;
  serializeJson(d, out);
  return out;
}

void pump() {
  // Drain the cross-task requests first (cheap, always).
  bool doStart, doCode, doCancel;
  char name[64], redir[128], code[600], state[64];
  portENTER_CRITICAL(&s_mux);
  doStart = s_reqStart; doCode = s_reqCode; doCancel = s_reqCancel;
  s_reqStart = s_reqCode = s_reqCancel = false;
  setBuf(name, sizeof(name), s_inName);
  setBuf(redir, sizeof(redir), s_inRedirect);
  setBuf(code, sizeof(code), s_inCode);
  setBuf(state, sizeof(state), s_inState);
  portEXIT_CRITICAL(&s_mux);

  if (doCancel) { s_flow = Flow{}; publish(); return; }
  if (doStart) { startFlow(name, redir); publish(); }
  if (doCode && s_flow.active && s_flow.phase == Phase::ShowConsent &&
      s_flow.state.length() && s_flow.state == String(state)) {
    // Only a callback whose state matches THIS flow advances it. A stray or
    // spoofed callback (wrong/blank state) is ignored, never aborts the flow, so
    // a LAN peer cannot cancel an in-progress consent by hitting /oauth/cb.
    s_flow.code = code;
    s_flow.phase = Phase::HaveCode;
    s_flow.nextStepMs = 0;   // process promptly
  }

  if (!s_flow.active) return;
  if ((int32_t)(millis() - s_flow.nextStepMs) < 0) return;   // throttle
  if (WiFi.status() != WL_CONNECTED) { s_flow.nextStepMs = millis() + 2000; return; }

  switch (s_flow.phase) {
    case Phase::DiscoverPR: stepDiscoverPR(); break;
    case Phase::DiscoverAS: stepDiscoverAS(); break;
    case Phase::Register:   stepRegister();   break;
    case Phase::HaveCode:   stepExchange();   break;
    default: /* ShowConsent / Done / Failed / Idle: nothing to do here */ break;
  }
  publish();
}

}  // namespace oauth

}  // namespace mcp

}  // namespace connectors
}  // namespace agent
