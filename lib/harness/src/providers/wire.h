#pragma once
#include <ArduinoJson.h>

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"

// wire.h - INTERNAL helpers shared by the four portable provider files. Not part
// of the public harness surface (lives under src/, not include/).
namespace agent {
namespace providers {
namespace wire {

// One provider REST exchange through the transport: complete request in, HTTP
// status out, response body filter-parsed into `doc` (the same bounded-memory
// ArduinoJson filter reads the pre-split adapters did from the socket - now
// from the returned body string). Returns the HTTP status code; 0 on transport
// failure (connect/TLS/timeout/arbiter - the pre-split adapters' 0 and -1
// returns both mapped to "network" at every call site, so they collapse here).
inline int exchange(const ProviderDeps& pd, const char* host, uint16_t port, bool tls,
                    const char* method, const std::string& path,
                    std::vector<std::pair<std::string, std::string>> headers,
                    std::string body, uint32_t timeoutMs,
                    JsonDocument& doc, const JsonDocument& filter) {
  doc.clear();
  if (!pd.http) return 0;
  HttpRequest req;
  req.method = method;
  req.host = host;
  req.port = port;
  req.tls = tls;
  req.path = path;
  req.headers = std::move(headers);
  req.body = std::move(body);
  req.timeoutMs = timeoutMs;
  // execJson parses the response into `doc` through the filter - on the device it
  // STREAMS off the socket (never buffering a fat connector-write body); on the
  // host/fake it filter-parses the scripted body string. Either way only the
  // filter-retained fields land in `doc`. Empty/garbage bodies leave doc cleared.
  std::string terr;
  int status = pd.http->execJson(req, doc, filter, terr);
  if (status == 0 && !terr.empty())
    hlog::logf("provider: transport fail %s%s: %s", host, path.c_str(), terr.c_str());
  // A response that arrived but did not parse leaves `doc` holding whatever was
  // read before the break - a fragment that looks like a thin-but-valid reply.
  // Drop it and log loudly: a partial provider response silently became an empty
  // or truncated answer with nothing anywhere to say why.
  else if (status != 0 && !terr.empty()) {
    hlog::logf("provider: %s%s status %d but %s -- discarding partial response",
               host, path.c_str(), status, terr.c_str());
    doc.clear();
  }
  return status;
}

// Cumulo router HEAD rewrite (CUM-242). When `pd` carries a router override
// (routerBase() non-empty), rewrite an upstream head request IN PLACE to travel
// through the router: host <- bare routerBase() (scheme/port honoured: an
// "http://" base is plain HTTP on port 80, anything else is TLS on 443, an
// explicit ":port" wins), path <- "/router/<upstream>" + path, and the client's
// own auth (Authorization / x-api-key) is dropped and replaced by a single
// "Authorization: Bearer routerKey()" - the router validates that cumulo key and
// injects the upstream's real provider key server-side. `upstream` is the wire's
// own provider ("anthropic" | "openai" | "mistral"). Returns true when it
// rewrote; a no-op returning false leaves everything byte-identical to the direct
// wire (so a device with no Cumulo head, and every existing wire test, is
// untouched). Kept here, next to exchange(), so all three provider files share
// ONE rewrite and it cannot drift between them.
// The mutable request coordinates applyRouter rewrites in place. Bundled into one
// struct so callers build it once and hand it straight to exchange() (and so the
// helper stays under the args<=6 complexity gate).
struct UpstreamReq {
  std::string host;
  uint16_t port = 443;
  bool tls = true;
  std::string path;
  std::vector<std::pair<std::string, std::string>> headers;
};
// Drop the client's own upstream auth headers in place (case-insensitive). The
// router sets the real provider auth itself, so Authorization / x-api-key from the
// device must not ride the request. Split out to keep applyRouter under the gate.
inline void dropClientAuth(std::vector<std::pair<std::string, std::string>>& headers) {
  std::vector<std::pair<std::string, std::string>> kept;
  kept.reserve(headers.size() + 1);
  for (auto& h : headers) {
    std::string k = h.first;
    for (char& c : k) c = (char)tolower((unsigned char)c);
    if (k == "authorization" || k == "x-api-key") continue;
    kept.push_back(std::move(h));
  }
  headers.swap(kept);
}
inline bool applyRouter(const ProviderDeps& pd, const char* upstream, UpstreamReq& r) {
  std::string base = pd.routerBase ? pd.routerBase() : std::string();
  if (base.empty()) return false;
  const bool http = base.rfind("http://", 0) == 0;   // plain HTTP only when explicit
  r.tls  = !http;
  r.port = http ? 80 : 443;
  size_t sch = base.find("://"); if (sch != std::string::npos) base = base.substr(sch + 3);
  size_t sl  = base.find('/');   if (sl  != std::string::npos) base = base.substr(0, sl);
  size_t colon = base.find(':');
  if (colon != std::string::npos) {
    r.port = (uint16_t)atoi(base.c_str() + colon + 1);
    base = base.substr(0, colon);
  }
  r.host = base;
  r.path = std::string("/router/") + upstream + r.path;
  dropClientAuth(r.headers);
  std::string rk = pd.routerKey ? pd.routerKey() : std::string();
  // SECURITY (mirror custom.cpp's keyless-http contract): the Cumulo key is the
  // master "one key, all upstreams" credential - NEVER put it on a cleartext
  // socket. A plain-http base (a LAN rig) gets NO Authorization header rather than
  // leaking cumulo_sk_ to a passive sniffer; the router requires auth, so an http
  // base then fails honestly (use https). A missing routerKey also adds nothing.
  if (rk.empty()) return true;
  if (http)
    hlog::logf("router: base is http:// - NOT sending the Cumulo key in cleartext (use https)");
  else
    r.headers.push_back({"Authorization", "Bearer " + rk});
  return true;
}

// Serialize a request document into ONE contiguous string (the transport then
// writes it in a single client.write - the serialize-into-one-buffer rule; on
// the device the string's storage lands in PSRAM via the >=128 B malloc spill).
inline std::string serializeBody(const JsonDocument& d) {
  std::string out;
  out.reserve(measureJson(d));
  serializeJson(d, out);
  return out;
}

// JsonDocument bound to the deps allocator (device: PSRAM) or the default.
inline JsonDocument makeDoc(const ProviderDeps& pd) {
  return pd.alloc ? JsonDocument(pd.alloc) : JsonDocument();
}

inline std::string s(const std::function<std::string()>& f) { return f ? f() : std::string(); }

// The system prompt for ONE round. On the forced tool-less round (capReason set)
// it carries kFinalRoundNotice, which tells the model its tools are gone, why,
// and that there is no later turn.
//
// ⚠ Without this every provider confabulated: the loop removed the tools
// silently, and rather than admit it could not finish, each model promised work
// it would never do ("I'll report back when the scan finishes"). The device then
// went quiet. Reproduced on the host against all three providers - see
// kFinalRoundNotice in nimbus/orch/head_loop.h.
inline std::string roundInstructions(const std::string& instructions,
                                     const std::string& capReason) {
  if (capReason.empty()) return instructions;
  const char* why = nimbus::orch::capReasonText(capReason);
  std::string notice = nimbus::orch::kFinalRoundNotice;
  const size_t at = notice.find("%s");
  if (at != std::string::npos) notice.replace(at, 2, why);
  return instructions + notice;
}

}  // namespace wire
}  // namespace providers
}  // namespace agent
