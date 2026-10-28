#pragma once
#include <ArduinoJson.h>
#include <strings.h>   // strncasecmp

#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/exchange_trace.h"   // heapwatch::kBodySliceBytes: the one slice constant (4,096 B)

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

// A binary payload written into the request body at send time (R1: photos to the
// head as images). `len` bytes at `bytes` go out at byte offset `at` of `body`:
// base64-encoded when `base64` (an image inside a JSON string), raw otherwise (a
// multipart file part). So image bytes never become a JSON string (ArduinoJson
// stores at most 65,535 B in one) and the whole body is never assembled: the
// transport encodes it slice by slice from the one copy of the bytes (PSRAM on the
// device). The bytes must outlive the exchange.
struct BodySplice {
  size_t at = 0;                  // offset into HttpRequest::body (ascending, <= body.size())
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  bool base64 = true;
};
using BodySplices = std::vector<BodySplice>;

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
  // Payloads spliced into `body` at send time (see BodySplice); empty = `body` is
  // the whole request body. Content-Length is bodyLength(), never body.size().
  BodySplices splices;
};

// ---- the spliced body (portable; the device transport and the host fakes) ------
inline size_t base64Len(size_t raw) { return ((raw + 2) / 3) * 4; }
inline size_t spliceLen(const BodySplice& s) { return s.base64 ? base64Len(s.len) : s.len; }

// The bytes the request body puts on the wire: `body` plus every splice.
inline size_t bodyLength(const HttpRequest& r) {
  size_t n = r.body.size();
  for (const auto& s : r.splices) n += spliceLen(s);
  return n;
}

namespace detail {
inline char b64Char(uint8_t v) {
  return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[v & 0x3F];
}
// Base64 character `e` of the encoding of bytes[0..len).
inline char b64At(const uint8_t* bytes, size_t len, size_t e) {
  const size_t g = (e / 4) * 3;
  const size_t have = len - g < 3 ? len - g : 3;
  const uint8_t a = bytes[g], b = have > 1 ? bytes[g + 1] : 0, c = have > 2 ? bytes[g + 2] : 0;
  switch (e % 4) {
    case 0: return b64Char(a >> 2);
    case 1: return b64Char(((a & 0x03) << 4) | (b >> 4));
    case 2: return have > 1 ? b64Char(((b & 0x0F) << 2) | (c >> 6)) : '=';
    default: return have > 2 ? b64Char(c) : '=';
  }
}
}  // namespace detail

// Copy up to `want` bytes of the logical body, starting at `off`, into `out`;
// returns how many were copied (0 past the end). Any offset works, so the device
// transport can fill one 4,096 B slice at a time.
inline size_t bodyRead(const HttpRequest& r, size_t off, uint8_t* out, size_t want) {
  size_t got = 0, pos = 0, bodyPos = 0;   // pos: logical offset; bodyPos: offset into body
  auto copyBody = [&](size_t upto) {       // body[bodyPos..upto)
    const size_t segLen = upto - bodyPos;
    if (got < want && off < pos + segLen) {
      const size_t from = off > pos ? off - pos : 0;
      size_t n = segLen - from;
      if (n > want - got) n = want - got;
      std::memcpy(out + got, r.body.data() + bodyPos + from, n);
      got += n;
      off += n;
    }
    pos += segLen;
    bodyPos = upto;
  };
  for (const auto& s : r.splices) {
    copyBody(s.at < r.body.size() ? s.at : r.body.size());
    const size_t segLen = spliceLen(s);
    for (size_t e = off > pos ? off - pos : 0; got < want && off >= pos && e < segLen; ++e, ++off)
      out[got++] = s.base64 ? (uint8_t)detail::b64At(s.bytes, s.len, e) : s.bytes[e];
    pos += segLen;
  }
  copyBody(r.body.size());
  return got;
}

// The send side's headroom rule (R1: photos make large bodies). Before each body slice
// after the first, the device transport waits in short steps while the largest free
// internal block is under kSliceHeadroom, so the TCP window drains (acknowledged
// segments free their buffers) before the next slice needs its own; bounded per slice,
// after which the slice is written anyway. Measured on bench unit L (23 Oct): 130 KB
// photo uploads and 0.6 MB inline bodies took the largest block to 7 KB and failed an
// allocation now and then (af=1..4; one body cut at 36,864 B and resent).
constexpr uint32_t kSliceHeadroom = 12288;
// The DMA-capable pool's floor (the photo-DMA regression, bench unit L): the send's
// own per-record AES chunks (2 x 1,600 B DMA) and pbufs allocate from the
// MALLOC_CAP_DMA pool, while a fragmented heap's INTERNAL reading is carried by
// the ~7.6 KB of free internal memory ABOVE SOC_DMA_HIGH that DMA cannot reach
// (measured on unit L: internal free 19,072 B of which DMA free 11,408 B). A
// floor the resting pool clears (6 KB < the 7,668 B the 21.5 KB leg rests at)
// waits only while a slice's predecessors still hold their pbufs, and keeps
// room for one record's chunks.
constexpr uint32_t kDmaChunkFloor = 6144;
constexpr uint32_t kSliceWaitStepMs = 10;
constexpr uint32_t kSliceWaitMaxMs = 1500;
inline bool sliceShouldWait(uint32_t largestFree, uint32_t waitedMs, uint32_t floor = kSliceHeadroom) {
  return largestFree < floor && waitedMs < kSliceWaitMaxMs;
}

// Waiting only helps while it is working (v4.6.1). A wait helps only while the
// previous slice's acknowledged segments are releasing their buffers, and that is over
// one round trip after the slice. On a heap that is fragmented but stable the largest
// block never grows at all (Lumi on v4.6.0 rested at 7,156 B all through its first
// hour), so every slice waited the full 1,500 ms and every 70 to 85 KB model request
// paid 24 to 30 s. So a slice stops waiting once the largest block has set no new high
// for kSliceStallMs (its first sample is the baseline; the window covers the round
// trip), and one exchange (one request on the wire; a resend is a new one) never waits
// more than kRequestPaceMaxMs in all, nor more than a tenth of its own deadline
// (paceCapMs): after that every slice is written at once, and the transport logs
// "paced 2000 ms for internal headroom".
//
// Measured on bench unit L (26 Oct, the v4.6.0 rule run to the end with every new high
// of the largest block and of free memory logged).
//  - Draining (225 short slices over 130 KB photo uploads and photo requests): the
//    block comes back in one step (219 of 225; two steps in the rest) when the slice's
//    segments are acknowledged (8,692-11,252 B -> 12,788-14,324 B), one round trip
//    later: 10 to 110 ms, median 40, p90 61, p99 90, with no growth before it. So the
//    window must cover the round trip: replayed, a 60 ms window would have written 31 of
//    the 225 slices before their buffers came back, 80 ms 5, 100 ms 1.
//  - Fragmented (13 waits at 7,156 B, Lumi's figure): the block never set a new high in
//    1.5 s. Free memory rose 1,820-2,060 B per wait as the buffers came back, 90% of it
//    within 10-90 ms; a 100 ms window catches 91-97% of that rise, the rest of the
//    1.5 s nothing measurable.
// A slice that misses its step goes out at 8 to 11 KB, as every slice did before
// v4.6.0 (allocation failures began near 7 KB). The request cap covers the measured
// drains (1,080 to 1,340 ms per 130 KB upload, 400 to 790 ms per photo request) and
// bounds a stable heap's cost: the 100 ms window per slice, at most 2 s per request
// (less when a tenth of the request's deadline is less).
// The cap counts every wait, so a body that drains on every slice is covered for about
// 50 slices (200 KB at 40 ms each); the rest of a larger inline body (a 0.6 MB album
// re-sent inline) goes out at once, as before v4.6.0.
constexpr uint32_t kSliceStallMs = 100;
constexpr uint32_t kRequestPaceMaxMs = 2000;
// Pacing never takes more than a tenth of the exchange's own deadline either, so it can
// never eat the time the request needs to be answered (Lumi on v4.6.0: "paced 58280 ms"
// inside a 60,000 ms photo request, then no answer).
constexpr uint32_t kPaceDeadlineShare = 10;
inline uint32_t paceCapMs(uint32_t timeoutMs) {
  uint32_t share = timeoutMs / kPaceDeadlineShare;
  share -= share % kSliceWaitStepMs;   // whole steps: the last step never crosses the cap
  return share < kRequestPaceMaxMs ? share : kRequestPaceMaxMs;
}

// A body-sized drain budget on top of the flat cap. A send's per-slice AES
// chunks and pbufs land in the DMA-capable pool; each slice's wait is what lets
// the previous slice's acknowledged segments go back (one stall window per
// slice, the measured round trip). The flat 2 s cap covers ~20 slices: a
// photo body (70+) at low memory - the Lumi-like shapes the photo legs
// reproduce - would exhaust it halfway and stream the rest into allocation
// failures (the v4.6.1 photo-DMA regression: esp-aes 1,600 B chunk allocs
// failed mid-body). The budget scales the cap with the body's own slice
// count; the deadline's tenth stays the ceiling (a small deadline keeps the
// original min(share, 2 s) cap).
inline uint32_t paceCapForBody(uint32_t timeoutMs, size_t bodyBytes) {
  uint32_t share = timeoutMs / kPaceDeadlineShare;
  share -= share % kSliceWaitStepMs;   // whole steps: the last step never crosses the cap
  const size_t slices = bodyBytes ? (bodyBytes + nimbus::heapwatch::kBodySliceBytes - 1) / nimbus::heapwatch::kBodySliceBytes : 0;
  const uint64_t need = (uint64_t)kRequestPaceMaxMs + (uint64_t)slices * kSliceStallMs;
  const uint32_t budget = need > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)need;
  return budget < share ? budget : share;
}

// One slice's wait. Before each step the caller passes next() a fresh sample of the
// largest free block and of FREE internal memory, the exchange's wait before this
// slice and its cap (paceCapForBody); true means wait one more kSliceWaitStepMs
// (already counted in waitedMs). Any new high counts as growth (the smallest
// measured step of a draining block was 512 B) - of the LARGEST, or of FREE: on a
// heap that is fragmented but draining (the Lumi-like photo shapes), the largest
// block never moves while the acknowledged segments return their pbufs and FREE
// climbs; a stall window with neither is a heap where waiting helps nothing, and
// the slice goes out (the stall rule's latency case, kept).
struct SliceWait {
  uint32_t waitedMs = 0;   // this slice's wait so far
  uint32_t best = 0;       // the largest block seen during it
  uint32_t bestFree = 0;   // the most free seen during it (the drain signal)
  uint32_t grewAtMs = 0;   // waitedMs when either last grew
  uint32_t floor = kSliceHeadroom;   // the pool's protective floor (kSliceHeadroom, or kDmaChunkFloor)

  bool next(uint32_t largestFree, uint32_t freeNow, uint32_t requestPacedMs, uint32_t capMs) {
    if (!sliceShouldWait(largestFree, waitedMs, floor)) return false;   // room, or the slice bound
    if (requestPacedMs + waitedMs >= capMs) return false;               // the exchange's cap
    if (largestFree > best || freeNow > bestFree) {
      if (largestFree > best) best = largestFree;
      if (freeNow > bestFree) bestFree = freeNow;
      grewAtMs = waitedMs;
    }
    if (waitedMs - grewAtMs >= kSliceStallMs) return false;             // neither grew for a window
    waitedMs += kSliceWaitStepMs;
    return true;
  }
};

// One slice's whole wait, as the device transport runs it before every slice after the
// first: `capMs` is the exchange's cap (paceCapMs of its timeout), `io.largest()` samples
// the largest free block of the pool, `io.free8()` its free bytes, `io.sleep(ms)` waits,
// `io.expired()` is the exchange's deadline (asked first: next() counts the step it
// grants). Adds the wait to `pacedMs`, the exchange's total, and returns it. `floorMs` is
// the pool the caller samples (kSliceHeadroom for the internal heap, kDmaChunkFloor when
// the samples come from the DMA-capable pool). The four seams ride one small bundle so
// the helper stays inside the repo's argument gate.
template <class Largest, class Free, class Sleep, class Expired>
struct SliceIo {
  Largest largest;
  Free free8;
  Sleep sleep;
  Expired expired;
};
template <class Largest, class Free, class Sleep, class Expired>
SliceIo(Largest, Free, Sleep, Expired)->SliceIo<Largest, Free, Sleep, Expired>;

template <class Io>
uint32_t paceSlice(uint32_t& pacedMs, uint32_t capMs, uint32_t floorMs, Io io) {
  SliceWait w;
  w.floor = floorMs;
  while (!io.expired() && w.next(io.largest(), io.free8(), pacedMs, capMs)) io.sleep(kSliceWaitStepMs);
  pacedMs += w.waitedMs;
  return w.waitedMs;
}

// The whole logical body as one string (host fakes and tests; never on the device).
inline std::string materializeBody(const HttpRequest& r) {
  if (r.splices.empty()) return r.body;
  std::string out(bodyLength(r), '\0');
  bodyRead(r, 0, (uint8_t*)&out[0], out.size());
  return out;
}

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
