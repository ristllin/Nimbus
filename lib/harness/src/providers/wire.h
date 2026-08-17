#pragma once
#include <ArduinoJson.h>

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
