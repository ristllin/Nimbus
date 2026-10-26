#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/heap_watch.h"

// exchange_trace - one provider exchange, traced on every exit (the portable half of
// the device transport, src/agent/transport_tls.cpp).
//
// The old transport logged its per-phase heap trace only at the very end, so the
// exchanges that mattered (a failed connect, a failed body write) carried no heap
// numbers at all, and a body that failed after some 4 KB records had gone out read
// "0/71508 bytes written" (the TLS client turns any hard error into 0). Here:
//
//   - runExchange() sequences connect, head, body, status, headers and body read
//     through injected I/O and records the stage it stopped at, so the caller
//     writes exactly one trace line whatever path it took;
//   - the body goes out as 4,096 B slices (the record size the TLS client already
//     uses internally, so no small records), so a failure says how many bytes left;
//   - the trace line keeps today's field order (pre/conn/wrote/done, read by the
//     analysis scripts) and appends fail=, af= (failed allocations during the
//     exchange), min= (the local minimum free, monitored only inside the slot), and
//     at=/ms= (when it held the slot, so overlaps with other work can be read off).
//
// No Arduino.
namespace nimbus::heapwatch {

inline constexpr size_t kBodySliceBytes = 4096;

struct HeapSample {
  uint32_t free = 0;
  uint32_t largest = 0;
};

enum class Stage : uint8_t { None, Connect, Head, Body, Read, Headers, Parse };
const char* stageName(Stage s);

// Write `len` bytes as slices of at most `slice` bytes. `write(off, want)` returns
// the bytes it took; anything short of `want` ends the write. Returns the bytes
// that went out (== len on success).
template <class Write>
size_t writeSlices(size_t len, size_t slice, Write write) {
  if (!slice) slice = len ? len : 1;
  size_t sent = 0;
  while (sent < len) {
    const size_t want = (len - sent) < slice ? (len - sent) : slice;
    const size_t n = write(sent, want);
    sent += n < want ? n : want;
    if (n < want) break;
  }
  return sent;
}

// Forensic floors: a trace whose exchange went below them is evidence (the durable
// log keeps it; a healthy one stays in the RAM ring).
inline constexpr uint32_t kTraceMinFree = 12 * 1024;
inline constexpr uint32_t kTraceMinLargest = 8 * 1024;

struct ExchangeTrace {
  const char* host = "";   // borrowed from the request; never copied (no allocation)
  const char* path = "";
  uint32_t body = 0;       // request body bytes
  HeapSample pre, conn, wrote, done;   // (0,0) = not taken
  Stage fail = Stage::None;
  uint32_t bodySent = 0;   // body bytes out before a body failure
  uint32_t af = 0;         // failed allocations (any task) during the exchange
  uint32_t minFree = UINT32_MAX;   // local minimum free; UINT32_MAX = not monitored
  uint32_t atMs = 0;       // uptime when the exchange took the slot
  uint32_t durMs = 0;      // how long it held the slot

  // "transport: heaptrace api.x.com/v1/messages body=71508 pre=24760/11764 conn=20792/7156
  //  wrote=22128/10228 done=24600/11764 fail=body@8192 af=1 min=6900 at=42651042 ms=1712"
  std::string line() const;
  // Worth the durable log: a failure, a failed allocation, or memory near its floor.
  bool abnormal() const;
};

// Drive one exchange through `io` and record where it stopped. Returns the HTTP
// status (0 when no status line came back or a request phase failed). A response
// whose header block was cut short, or whose body did not parse, keeps its status
// and records Stage::Headers or Stage::Parse.
//
// Io provides:
//   HeapSample sample();
//   bool connect();                        // all retries included
//   bool writeHead();
//   size_t writeBody(size_t off, size_t want);
//   int readStatus();                      // <= 0: no status line
//   bool readHeaders(int code);            // false: the header block was cut short
//   bool readBody();                       // false: the body did not parse
template <class Io>
int runExchange(Io& io, ExchangeTrace& t) {
  const bool connected = io.connect();
  t.conn = io.sample();   // after the handshake, success or not
  if (!connected) { t.fail = Stage::Connect; return 0; }
  if (!io.writeHead()) { t.fail = Stage::Head; return 0; }
  if (t.body) {
    t.bodySent = (uint32_t)writeSlices(t.body, kBodySliceBytes,
                                       [&](size_t off, size_t want) { return io.writeBody(off, want); });
    t.wrote = io.sample();
    if (t.bodySent != t.body) { t.fail = Stage::Body; return 0; }
  }
  const int code = io.readStatus();
  if (code <= 0) { t.fail = Stage::Read; return 0; }
  if (!io.readHeaders(code)) { t.fail = Stage::Headers; return code; }
  if (!io.readBody()) t.fail = Stage::Parse;
  return code;
}

// runExchange plus the one trace line: `finish(t)` runs exactly once, on every path
// (the device closes the socket, releases the slot, takes the done sample and logs),
// an exception included: a turn catches std::bad_alloc (L24), so an allocation that
// fails mid-exchange (a body growing) unwinds through here, and finish still gives the
// slot back. On that path a second failure inside finish is swallowed (it must not
// escape a destructor); the exception that unwound carries on.
template <class Io, class Finish>
int tracedExchange(Io& io, ExchangeTrace& t, Finish finish) {
  struct OnUnwind {
    ExchangeTrace& t;
    Finish& finish;
    bool done;
    ~OnUnwind() {
      if (done) return;
      try {
        finish(t);
      } catch (...) {
      }
    }
  } onUnwind{t, finish, false};
  const int code = runExchange(io, t);
  onUnwind.done = true;
  finish(t);
  return code;
}

// The error text for a stopped exchange (t.fail != None), keeping the markers the
// provider layer reads: "body truncated" / "head write failed" (the request never
// fully left), "no response", "low memory", and "alloc failed" (the allocation
// evidence). With evidence (t.af > 0) the newest failure is named, e.g. "request body
// truncated: write failed after 8192 of 71508 bytes: low memory (alloc failed 1600 B
// DMA|INTERNAL)". Without it today's rule holds (CUM-404 v2): the "low memory" tag
// only when the exchange began below the handshake's block floor (`startedStarved`),
// and never on a missing response. `parseWhy` is the parser's reason; `last` (the
// newest failure record) may be null.
std::string stageError(const ExchangeTrace& t, const char* parseWhy, const FailRecord* last,
                       bool startedStarved);

// The retained-document guard. The streaming parse keeps only the fields the caller's
// filter names, so any response size is safe; this bounds what those fields hold (a
// hostile or buggy provider streaming an unbounded run of kept fields into PSRAM; one
// string holds at most 65,535 B, a longer one already fails the parse as NoMemory).
// `retained` is the serialized size of the kept document (measureJson): ArduinoJson 7
// keeps no memory count (JsonDocument::memoryUsage() always returns 0, which left the
// old guard unable to fire).
inline constexpr size_t kMaxRetainedBytes = 512 * 1024;
// "" within `cap`, else "response too large for the device (N B retained)": the text
// the provider layer types as a body too large to hold (wire::LastExchange::tooLarge).
std::string retainedTooLarge(size_t retained, size_t cap = kMaxRetainedBytes);

}  // namespace nimbus::heapwatch
