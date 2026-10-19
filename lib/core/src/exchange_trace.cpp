#include "nimbus/exchange_trace.h"

#include <cstdio>

namespace nimbus::heapwatch {

const char* stageName(Stage s) {
  switch (s) {
    case Stage::None: return "none";
    case Stage::Connect: return "connect";
    case Stage::Head: return "head";
    case Stage::Body: return "body";
    case Stage::Read: return "read";
    case Stage::Headers: return "headers";
    case Stage::Parse: return "parse";
  }
  return "?";
}

std::string ExchangeTrace::line() const {
  char fail[32];
  if (this->fail == Stage::Body) snprintf(fail, sizeof(fail), "body@%u", (unsigned)bodySent);
  else snprintf(fail, sizeof(fail), "%s", stageName(this->fail));
  char minBuf[16];
  if (minFree == UINT32_MAX) snprintf(minBuf, sizeof(minBuf), "-");
  else snprintf(minBuf, sizeof(minBuf), "%u", (unsigned)minFree);
  // Today's fields first, in today's order (the analysis scripts read them), then
  // the new ones.
  char b[320];
  snprintf(b, sizeof(b),
           "transport: heaptrace %s%s body=%u pre=%u/%u conn=%u/%u wrote=%u/%u done=%u/%u "
           "fail=%s af=%u min=%s at=%u ms=%u",
           host ? host : "", path ? path : "", (unsigned)body, (unsigned)pre.free,
           (unsigned)pre.largest, (unsigned)conn.free, (unsigned)conn.largest, (unsigned)wrote.free,
           (unsigned)wrote.largest, (unsigned)done.free, (unsigned)done.largest, fail,
           (unsigned)af, minBuf, (unsigned)atMs, (unsigned)durMs);
  return b;
}

bool ExchangeTrace::abnormal() const {
  if (fail != Stage::None || af > 0) return true;
  if (minFree != UINT32_MAX && minFree < kTraceMinFree) return true;
  // A (0,0) sample is "not taken" (no body, so no post-write sample), never a reading.
  for (const HeapSample* h : {&pre, &conn, &wrote, &done}) {
    if (!h->free && !h->largest) continue;
    if (h->free < kTraceMinFree || h->largest < kTraceMinLargest) return true;
  }
  return false;
}

namespace {

// "low memory (alloc failed 1600 B DMA|INTERNAL)" when there is evidence, else "".
std::string evidenceText(uint32_t af, const FailRecord* last) {
  if (!af) return std::string();
  char b[96];
  if (last)
    snprintf(b, sizeof(b), "low memory (alloc failed %u B %s)", (unsigned)last->size,
             capsString(last->caps).c_str());
  else
    snprintf(b, sizeof(b), "low memory (alloc failed)");
  return b;
}

// The text of a request phase that stopped (connect, head, body).
std::string requestPhaseText(const ExchangeTrace& t) {
  if (t.fail == Stage::Connect) return "connect failed";
  if (t.fail == Stage::Head) return "request head write failed";
  char b[128];
  snprintf(b, sizeof(b), "request body truncated: write failed after %u of %u bytes",
           (unsigned)t.bodySent, (unsigned)t.body);
  return b;
}

// A response-side stop: `base` plus the evidence, joined by `sep`.
std::string withEvidence(const char* base, const std::string& evidence, const char* sep) {
  return evidence.empty() ? std::string(base) : std::string(base) + sep + evidence;
}

}  // namespace

std::string stageError(const ExchangeTrace& t, const char* parseWhy, const FailRecord* last,
                       bool startedStarved) {
  const std::string evidence = evidenceText(t.af, last);
  switch (t.fail) {
    case Stage::None:
      return std::string();
    case Stage::Read:   // a missing response was never tagged "low memory" without evidence
      return withEvidence("no response", evidence, ": ");
    case Stage::Headers:   // the status line arrived: a cut response, not a missing one
      return withEvidence("response headers cut short", evidence, ": ");
    case Stage::Parse: {
      std::string s = std::string("response parse failed: ") + (parseWhy ? parseWhy : "");
      if (!evidence.empty()) return s + " (" + evidence + ")";
      return startedStarved ? s + " (low memory)" : s;
    }
    default:
      break;
  }
  const std::string s = requestPhaseText(t);
  if (!evidence.empty()) return s + ": " + evidence;
  return startedStarved ? s + ": low memory" : s;
}

std::string retainedTooLarge(size_t retained, size_t cap) {
  if (retained <= cap) return std::string();
  char b[80];
  snprintf(b, sizeof(b), "response too large for the device (%u B retained)", (unsigned)retained);
  return b;
}

}  // namespace nimbus::heapwatch
