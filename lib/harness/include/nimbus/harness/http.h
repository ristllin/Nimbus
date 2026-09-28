#pragma once
#include <ArduinoJson.h>
#include <strings.h>   // strncasecmp

#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// HttpTransport - the seam that makes the provider adapters portable. The
// harness builds a complete request (headers + one contiguous body - the
// serialize-into-one-buffer-then-single-write rule that keeps TLS records off
// the internal heap lives on the CALLER side of this seam) and receives a
// complete response. TLS, the work-slot arbiter, cert policy, and chunked
// decoding all live in the device implementation (src/agent/transport_tls.cpp);
// host tests script a FakeHttpTransport with canned exchanges.
namespace agent {

// Response parser recursion bound, shared by the host (default execJson) and the
// device (streaming execJson) so both paths parse identically. ArduinoJson counts
// nesting for FILTERED-OUT subtrees too (it must descend a skipped value to find
// its end), so this must clear the depth of the WHOLE raw response - e.g. a
// connector tool-execution payload (a Notion block tree) that the filter discards.
// 64 covers real connector responses; capped well below a level that could stress
// the 16 KB tg_poll stack (recursion is bounded AT this limit).
static constexpr uint8_t kResponseNestingLimit = 64;

using HeaderList = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
  std::string method;   // "GET" | "POST"
  std::string host;     // e.g. "api.anthropic.com" (443 implied unless set)
  uint16_t    port = 443;
  bool        tls = true;
  std::string path;     // "/v1/messages"
  HeaderList  headers;
  std::string body;
  uint32_t    timeoutMs = 30000;
  // OPTIONAL error-header capture (off by default). When both are set, a transport
  // that supports it copies each header of an ERROR response (status >= 400) whose
  // name starts with errHeaderPrefix (lowercase, matched case-insensitively) into
  // *errHeaders, name lowercased, bounded (captureErrHeader). Nothing is kept for a
  // healthy response or a header that does not match. The Mistral adapter uses it
  // for the x-ratelimit-* quota headers on a
  // 429: the body reads the same for a per-minute limit, a spent daily quota and a
  // plan that allows 0 requests, and only the headers say which (CUM-460). A
  // transport without support leaves *errHeaders empty; callers fall back to the
  // response body.
  const char* errHeaderPrefix = nullptr;
  HeaderList* errHeaders = nullptr;
};

struct HttpResponse {
  int         status = 0;   // 0 => transport failure (see err)
  std::string body;
  HeaderList  headers;      // response headers, for a transport that fills them (the test
                            // fake); the device transport captures while it reads instead
};

// The error-header capture rule, shared by every transport: an error status, a
// requested capture, and a case-insensitive name-prefix match. `name` may be the
// whole "Name: value" line (the prefix never contains a colon).
inline bool wantsErrHeader(const HttpRequest& req, int status, const char* name) {
  if (status < 400 || !req.errHeaders || !req.errHeaderPrefix) return false;
  return strncasecmp(name, req.errHeaderPrefix, std::strlen(req.errHeaderPrefix)) == 0;
}
// Bounded: a response is untrusted input, and on the device every kept byte is
// scarce heap, so at most kMaxErrHeaders headers of at most kMaxErrHeaderLen
// bytes (name and value each) are kept. Mistral's full quota set is ~15 pairs.
static constexpr size_t kMaxErrHeaders = 32;
static constexpr size_t kMaxErrHeaderLen = 64;
inline void captureErrHeader(const HttpRequest& req, int status, const std::string& name,
                             const std::string& value) {
  if (!wantsErrHeader(req, status, name.c_str())) return;
  if (req.errHeaders->size() >= kMaxErrHeaders) return;
  std::string n = name.substr(0, kMaxErrHeaderLen);
  for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  req.errHeaders->push_back({std::move(n), value.substr(0, kMaxErrHeaderLen)});
}

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  // Returns false on transport failure (connect/TLS/timeout); err describes it.
  // A false return MUST leave out.status == 0.
  virtual bool exec(const HttpRequest& req, HttpResponse& out, std::string& err) = 0;

  // JSON exec - parse the response into `doc` using `filter`, return the HTTP
  // status (0 on transport failure). The DEFAULT reads the whole body via exec()
  // then filter-parses the string (host/fake path - unchanged). The DEVICE
  // transport OVERRIDES this to stream-parse the response directly off the
  // socket, so a large (connector-write) body never fully resides in RAM. This
  // is the size-transparent path: only the filter-retained fields are stored.
  //
  // `err` reports BOTH failure classes, distinguished by the return value:
  //   status == 0  => transport failure (connect/TLS/timeout); `doc` is empty.
  //   status != 0 && !err.empty() => the response arrived but did NOT parse.
  // The second case used to be silent, which made a truncated or corrupt body
  // indistinguishable from a complete one: ArduinoJson populates whatever it
  // read before the break, so the caller saw a plausible-looking partial
  // document and reported success. Callers that only branch on `status` are
  // unaffected; callers that care about integrity must check `err` too.
  virtual int execJson(const HttpRequest& req, JsonDocument& doc,
                       const JsonDocument& filter, std::string& err) {
    HttpResponse out;
    if (!exec(req, out, err)) return 0;
    for (const auto& h : out.headers) captureErrHeader(req, out.status, h.first, h.second);
    doc.clear();
    err.clear();
    if (!out.body.empty()) {
      DeserializationError derr =
          deserializeJson(doc, out.body, DeserializationOption::Filter(filter),
                          DeserializationOption::NestingLimit(kResponseNestingLimit));
      if (derr) err = std::string("response parse failed: ") + derr.c_str();
    }
    return out.status;
  }
};

}  // namespace agent
