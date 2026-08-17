#pragma once
#include <string>

#include "http.h"

// websearch - the portable half of the device's "web.search" capability (Tavily
// POST /search). Everything except the key lookup lives here, behind the
// HttpTransport seam, so the whole path is host-testable and runs unchanged in
// tools/harness-lab.
//
// ⚠ Why this exists as a portable unit at all: the device-only predecessor read
// the response into an Arduino String capped at 6000 bytes. Real Tavily replies
// for the default call shape (max_results=5, include_answer) measure 7-8 KB, so
// the JSON was truncated mid-string on EVERY search, failed to parse, and came
// back as an empty string that the caller reported as "network / no results".
// It had never once succeeded in the field. Going through HttpTransport::execJson
// stream-parses off the socket through a filter - only the handful of retained
// fields are stored - so response size stops being a failure mode.
namespace agent {
namespace websearch {

struct Result {
  bool        ok = false;
  std::string digest;  // model-facing text: answer + top hits
  std::string err;     // NAMES the cause when !ok - never a catch-all
};

// One search round-trip. `maxResults` is clamped to [1,10].
//
// On failure `err` distinguishes the cases the caller has to tell apart: an
// empty key, a transport failure (with the transport's own reason), an HTTP
// status (with the provider's message when it sent one), and a response that
// arrived but would not parse. A search that legitimately matched nothing is a
// SUCCESS whose digest says so - callers must not infer "no results" from a
// failure, which is exactly the conflation the old adapter shipped.
// `alloc` binds the response document to the caller's allocator - on the device
// that is the PSRAM one, so the retained fields never sit in the ~46 KB internal
// heap (the same rule the provider adapters follow). Null uses the default.
Result search(HttpTransport& http, const std::string& apiKey,
              const std::string& query, int maxResults = 5,
              uint32_t timeoutMs = 20000,
              ArduinoJson::Allocator* alloc = nullptr);

// Response filter (exposed for tests): keeps answer/results[title,url,content]
// plus the provider's error message, and discards the bulky rest (images,
// follow-up questions, raw content, request ids).
void buildFilter(JsonDocument& filter);

// Render a parsed (filtered) response into the model-facing digest. Split out so
// the rendering is testable without a transport.
std::string renderDigest(const JsonDocument& doc, int maxResults);

}  // namespace websearch
}  // namespace agent
