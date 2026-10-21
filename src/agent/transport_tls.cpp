#include "transport_tls.h"

#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "../sys/agent_log.h"
#include "../sys/heap_watch.h"   // failed-allocation count, local-minimum monitor
#ifdef NIMBUS_TEST
#include "../sys/crash_report.h"   // testPanicPoint (L39 crash-forensics legs)
#endif
#include "../sys/net_util.h"
#include "../sys/ps_json.h"
#include "../sys/tls_arbiter.h"
#include "agent_config.h"
#include "nimbus/exchange_trace.h"   // the traced exchange: one line on every path
#include "nimbus/fault.h"
#include "connectors.h"
#include "store.h"

// The device HttpTransport - the socket mechanics that used to live inside each
// provider adapter's antRequestCore/requestCore/mistralRequestCore/custRequest,
// now in ONE place behind the portable seam. Behavior preserved verbatim:
//
//   - tls_arbiter work-slot around the whole exchange (10 s acquire; busy =>
//     transport failure - the pre-split adapters' -1 and 0 returns were both
//     "network" at every call site, so they collapse into status 0 here).
//   - tlsSetup (CA bundle vs setInsecure) + 12 s handshake bound + 3x connect
//     retry with a 400 ms settle (rapid consecutive handshakes to the same host
//     intermittently fail; a fresh socket clears it). Plain-HTTP requests (the
//     custom http:// endpoint) use WiFiClient and a normal stop(); TLS gets the
//     RST-close (no lingering TIME_WAIT PCBs - see net_util.h).
//   - HTTP/1.0 + Connection: close => Content-Length framing, NO chunked
//     encoding to decode (none of the pre-split adapters ever handled chunks).
//   - The request body arrives as ONE contiguous string (never chunked
//     serialization: per-chunk TLS records + lwIP pbufs collapsed internal heap
//     30 KB -> 5 KB, measured live) and goes out as 4,096 B slices of it - the
//     record size the TLS client cuts one big write into anyway, so the wire is
//     unchanged, but a failure now says how many bytes left ("after N of M"),
//     where one write reported "0/M" after an unknown number of records. The
//     string's storage is PSRAM-backed on this firmware: main.cpp calls
//     heap_caps_malloc_extmem_enable(128), so any malloc >=128 B spills to PSRAM.
//   - Every exchange that took the slot writes exactly one trace line on every
//     path out (nimbus/exchange_trace.h): heap at four phases, fail=, af=, min=.
//   - The response body is read whole into out.body (same PSRAM spill) and
//     filter-parsed by the PORTABLE side - the pre-split code stream-parsed off
//     the socket; the trade is one PSRAM-resident copy of the response for a
//     host-testable wire layer. A 256 KB cap bounds a runaway body (the parse
//     then fails clean on the truncated JSON - same failure class as the old
//     read-deadline truncation).
namespace agent {

namespace {

constexpr size_t kMaxResponseBody = 256 * 1024;
// The streaming path bounds the RETAINED document instead (the old whole-body path
// had kMaxResponseBody): nimbus::heapwatch::kMaxRetainedBytes and retainedTooLarge()
// in nimbus/exchange_trace.h, host-tested there.

// A read-only Arduino Stream over a Client whose read() BLOCKS until a byte is
// available or the connection truly closes / the deadline passes - so ArduinoJson
// can stream-parse a TLS response without mistaking a momentary socket stall for
// end-of-input (a bare WiFiClient::read() returns -1 immediately when idle).
class BlockingClientReader : public Stream {
 public:
  BlockingClientReader(Client* c, uint32_t deadline) : c_(c), deadline_(deadline) {}
  int available() override { return c_->available(); }
  int peek() override { return c_->peek(); }
  int read() override {
    for (;;) {
      int v = c_->read();
      if (v >= 0) return v;
      if (!c_->connected() && c_->available() == 0) return -1;  // real EOF
      if ((int32_t)(millis() - deadline_) >= 0) return -1;      // timed out (wrap-safe)
      delay(1);
    }
  }
  size_t write(uint8_t) override { return 0; }  // read-only sink
 private:
  Client* c_;
  uint32_t deadline_;
};

// One "Name: value" header line -> the caller's error-header capture, when it
// asked for this one (the shared rule in http.h: error status + name prefix). The
// rule is checked on the raw line first, so a healthy response allocates nothing.
void keepErrHeader(const HttpRequest& req, int code, const String& line) {
  if (!wantsErrHeader(req, code, line.c_str())) return;
  const int colon = line.indexOf(':');
  if (colon <= 0) return;
  String value = line.substring(colon + 1);
  value.trim();
  captureErrHeader(req, code, std::string(line.c_str(), size_t(colon)),
                   std::string(value.c_str()));
}

using nimbus::heapwatch::ExchangeTrace;
using nimbus::heapwatch::HeapSample;
using nimbus::heapwatch::Stage;

HeapSample sampleHeap() {
  return HeapSample{
      (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)};
}

// The socket phases of one exchange, driven by nimbus::heapwatch::runExchange (the
// portable sequencer that records where an exchange stopped). Behavior per phase is
// the pre-trace transport's, verbatim, except the body write: 4,096 B slices.
struct TlsIo {
  const HttpRequest& req;
  Client* client;
  WiFiClientSecure& tls;
  WiFiClient& plain;
  HttpResponse* out;
  JsonDocument* doc;
  const JsonDocument* filter;
  uint32_t deadline;   // F25: the WHOLE operation (connect retries + I/O) rides one clock
  std::string parseWhy;   // the parser's reason when readBody() fails
  std::string tooLarge;   // set when the body parsed whole but was over the retained cap

  bool expired() const { return (int32_t)(millis() - deadline) >= 0; }
  void close() { if (req.tls) tlsClose(tls); else plain.stop(); }
  HeapSample sample() { return sampleHeap(); }

  // 3x connect with a 400 ms settle (rapid consecutive handshakes to the same host
  // intermittently fail; a fresh socket clears it).
  bool connect() {
    for (int a = 0; a < 3 && !expired(); a++) {
      if (client->connect(req.host.c_str(), req.port)) return true;
      close();
      if (a < 2) vTaskDelay(pdMS_TO_TICKS(400));
    }
    return false;
  }

  // Request head (HTTP/1.0 + close => Content-Length framing, no chunking). Both
  // write results are CHECKED: a short write used to read as "no response" after the
  // server waited forever for the announced Content-Length.
  bool writeHead() {
    String head = String(req.method.c_str()) + " " + req.path.c_str() + " HTTP/1.0\r\n" +
                  "Host: " + req.host.c_str() + "\r\n";
    for (const auto& h : req.headers)
      head += String(h.first.c_str()) + ": " + h.second.c_str() + "\r\n";
    head += "Content-Length: " + String((unsigned)req.body.size()) + "\r\nConnection: close\r\n\r\n";
    return client->print(head) == head.length();
  }

  // One body slice (the sequencer passes 4,096 B: the record size the TLS client
  // already cuts a big write into, so no small records and no chunked serialization).
  // The TLS client returns 0 on any hard error after an unknown number of records,
  // so slicing is what lets the failure say how many bytes went out.
  size_t writeBody(size_t off, size_t want) {
#ifdef NIMBUS_TEST
    if (off == 0) nimbus::crash::testPanicPoint();   // L39: an armed panic lands here, before any body byte
#endif
    return client->write((const uint8_t*)req.body.data() + off, want);
  }

  // Status line: "HTTP/1.0 200 ...".
  int readStatus() {
    String status;
    while (!expired()) {
      if (client->available()) { char c = client->read(); if (c == '\n') break; if (c != '\r') status += c; }
      else if (!client->connected() && !client->available()) break;
      else delay(2);
    }
    int code = 0;
    int sp = status.indexOf(' ');
    if (sp > 0 && (int)status.length() >= sp + 4) code = status.substring(sp + 1, sp + 4).toInt();
    return code;
  }

  // Skip headers up to the blank line. An error response keeps the headers the
  // caller asked for (HttpRequest::errHeaders, e.g. a 429's x-ratelimit-* quota
  // window); a healthy response keeps nothing.
  bool readHeaders(int code) {
    String line;
    while (!expired()) {
      if (client->available()) {
        char c = client->read();
        if (c == '\n') {
          if (line.length() == 0) return true;
          keepErrHeader(req, code, line);
          line = "";
        }
        else if (c != '\r') line += c;
      } else if (!client->connected() && !client->available()) return false;
      else delay(2);
    }
    return false;
  }

  bool readBody() {
    if (!doc) {
      uint8_t buf[512];
      while (!expired() && out->body.size() < kMaxResponseBody) {
        int n = client->read(buf, sizeof(buf));
        if (n > 0) out->body.append((const char*)buf, (size_t)n);
        else if (!client->connected() && !client->available()) break;
        else delay(2);
      }
      return true;
    }
    // STREAMING filter-parse off the socket. The parser pulls bytes as it goes
    // (draining lwIP RX pbufs continuously) and stores ONLY the filter-retained
    // fields - the fat response body never fully resides, so any response size is
    // safe on the ~47 K internal-heap budget.
    uint32_t heapBefore = ESP.getFreeHeap();
    BlockingClientReader rdr(client, deadline);
    DeserializationError derr =
        deserializeJson(*doc, rdr, DeserializationOption::Filter(*filter),
                        DeserializationOption::NestingLimit(kResponseNestingLimit));
    // A parse break is NOT cosmetic: ArduinoJson keeps whatever it read before the
    // break, so without this the caller cannot tell a truncated response from a
    // complete one and happily uses the fragment. EmptyInput is excluded so an empty
    // 200/204 behaves like the host path, which skips the parse entirely.
    if (derr && derr.code() != DeserializationError::EmptyInput) {
      parseWhy = derr.c_str();
      alogf("transport: stream-parse %s (heap %u->%u)", derr.c_str(),
            (unsigned)heapBefore, (unsigned)ESP.getFreeHeap());
      return false;
    }
    // Guard against unbounded retained fields (hostile/buggy provider): the
    // filter keeps only small fields, but bound what it kept. Measured as the kept
    // document's serialized size (ArduinoJson 7's memoryUsage() always returns 0).
    // Named, never silent: a 2xx the device could not hold used to read as an
    // empty answer (a "stalled" round, a job polled forever); wire::exchange
    // turns the error text into a transport failure the step can classify. The
    // exchange itself completed, so the trace keeps fail=none.
    if (!derr) {
      const size_t kept = measureJson(*doc);
      tooLarge = nimbus::heapwatch::retainedTooLarge(kept);
      if (!tooLarge.empty()) {
        alogf("transport: retained %u B over cap -> drop", (unsigned)kept);
        doc->clear();
      }
    }
    return true;
  }
};

class TlsTransport : public HttpTransport {
 public:
  bool exec(const HttpRequest& req, HttpResponse& out, std::string& err) override {
    out.status = execCore(req, &out, nullptr, nullptr, err);
    return out.status != 0;
  }

  // Size-transparent JSON exec - STREAM-parse the response off the socket into
  // `doc` through `filter`. Only the filter-retained fields are stored; the fat
  // body (a big connector-write response) never fully resides in RAM. This is the
  // core crash fix for heavy connector writes.
  int execJson(const HttpRequest& req, JsonDocument& doc, const JsonDocument& filter,
               std::string& err) override {
    return execCore(req, nullptr, &doc, &filter, err);
  }

 private:
  // ONE core exchange. When `doc` is set the body is stream-parsed off the socket
  // into it through `filter` (size-transparent); otherwise the whole body is read
  // into out->body (legacy path, kept for any non-JSON caller). Returns the HTTP
  // status (0 on transport failure, with `err` set).
  int execCore(const HttpRequest& req, HttpResponse* out, JsonDocument* doc,
               const JsonDocument* filter, std::string& err) {
    err.clear();
    if (out) { out->status = 0; out->body.clear(); }
    if (doc) doc->clear();

    // FAULT provider (TEST-only; active() is constant-false in production):
    // simulate a total LLM-provider outage - the exact "no response" a dead
    // network produces - for the LLM API hosts ONLY, so Telegram keeps working
    // and the failure path's owner-visible messages remain observable by HIL.
    if (nimbus::fault::active(nimbus::fault::PROVIDER) &&
        (req.host == "api.openai.com" || req.host == "api.anthropic.com" ||
         req.host == "api.mistral.ai")) {
      err = "no response";
      return 0;
    }

    if (!arbiter::acquireWork(10000)) { err = "tls work-slot busy"; return 0; }

    // The exchange trace (heap F1): ONE line on every path out of here, with the
    // internal heap at four phase boundaries (free/largest; divergence =
    // fragmentation, a drop at `conn` = the handshake's cost, a drop that survives
    // `done` = memory kept per request), where it stopped (fail=), the failed
    // allocations of any task during it (af=), and its local minimum free (min=,
    // monitored only inside the slot: one global monitor, serialized by it).
    ExchangeTrace trace;
    trace.host = req.host.c_str();
    trace.path = req.path.c_str();
    trace.body = (uint32_t)req.body.size();
    trace.pre = sampleHeap();
    trace.atMs = millis();
    const uint32_t afStart = nimbus::heapwatch::allocFails();
    const bool monitored = nimbus::heapwatch::monitorStart();
    nimbus::heapwatch::tlsIoBegin("transport");

    // CUM-404 v2: without allocation evidence, a failure reads as MEMORY pressure only
    // when this exchange BEGAN below the handshake's largest-block need (the same
    // floor the chat gate uses). Enough contiguous memory to start means a connect /
    // write / parse failure is a genuine network or provider error that MUST still
    // fail over (CUM-41 ladder) and be reported honestly - the CYD/freenove keeps its
    // largest internal block near the floor (a persistent ~5 KB DMA bounce buffer),
    // and sampling at the failure turned every provider outage into a fake "low
    // memory". With evidence (a failed allocation during the exchange) the failure is
    // memory-caused whatever the block was at the start (heap F4).
    const bool startedStarved = trace.pre.largest < (uint32_t)ORCH_TURN_MIN_LARGEST_BLOCK;

    WiFiClient plain;
    WiFiClientSecure tls;
    Client* client;
    if (req.tls) {
      tlsSetup(tls);
      tls.setHandshakeTimeout(12);              // bound a failed handshake; retry below
      // F25: setTimeout() is a NO-OP on this client (it writes Stream::_timeout,
      // which NetworkClientSecure never reads - the socket ran at the hardcoded
      // 30 s default). setConnectionTimeout() is the real API: it drives
      // SO_RCVTIMEO/SO_SNDTIMEO so a half-open NAT read can't hang past this.
      tls.setConnectionTimeout(req.timeoutMs);
      client = &tls;
    } else {
      plain.setConnectionTimeout(req.timeoutMs);
      client = &plain;
    }
    TlsIo io{req, client, tls, plain, out, doc, filter, millis() + req.timeoutMs, {}, {}};

    const int code = nimbus::heapwatch::tracedExchange(io, trace, [&](ExchangeTrace& t) {
      io.close();
      nimbus::heapwatch::tlsIoEnd();
      t.minFree = nimbus::heapwatch::monitorStop(monitored);
      t.durMs = millis() - t.atMs;
      arbiter::releaseWork();
      t.af = nimbus::heapwatch::allocFails() - afStart;
      t.done = sampleHeap();
      // Forensic window (heap F1.5): a healthy exchange's trace stays in the RAM ring and
      // serial; a failed one, one with a failed allocation, or one that took internal
      // memory near its floor is durable evidence.
      alogLevelf(t.abnormal() ? nimbus::errlog::Level::Warn : nimbus::errlog::Level::Info, "%s",
                 t.line().c_str());
    });

    if (trace.fail != Stage::None) {
      nimbus::heapwatch::FailRecord last;
      const bool haveLast = trace.af && nimbus::heapwatch::lastFail(last);
      err = nimbus::heapwatch::stageError(trace, io.parseWhy.c_str(), haveLast ? &last : nullptr,
                                          startedStarved);
    } else if (!io.tooLarge.empty()) {
      err = io.tooLarge;   // a whole 2xx the device could not hold: wire::exchange types it
    }
    if (code <= 0) return 0;
    if (out) out->status = code;
    return code;
  }
};

}  // namespace

HttpTransport& deviceTransport() {
  static TlsTransport t;
  return t;
}

providers::ProviderDeps deviceProviderDeps() {
  providers::ProviderDeps pd;
  pd.http = &deviceTransport();
  pd.key = [](const char* host) -> std::string {
    if (!strcmp(host, "openai"))    return std::string(store::openaiKey().c_str());
    if (!strcmp(host, "anthropic")) return std::string(store::anthropicKey().c_str());
    if (!strcmp(host, "mistral"))   return std::string(store::mistralKey().c_str());
    if (!strcmp(host, "custom"))    return std::string(store::customKey().c_str());
    // The router heads: the adapters never read these (each builds its own deps),
    // but the provider park fingerprints every head's key, so a new key un-parks it.
    if (!strcmp(host, "zai"))       return std::string(store::zaiKey().c_str());
    if (!strcmp(host, "cumulo"))    return std::string(store::cumuloKey().c_str());
    return std::string();
  };
  pd.orchModel = [](const char* host) { return std::string(store::orchModel(host).c_str()); };
  pd.toolLoopOn = [] { return store::orchToolLoop(); };
  pd.antEnvId = [] { return std::string(store::antEnvId().c_str()); };
  pd.setAntEnvId = [](const std::string& v) { store::setAntEnvId(v.c_str()); };
  pd.antAgentMap = [] { return std::string(store::antAgentMap().c_str()); };
  pd.setAntAgentMap = [](const std::string& v) { store::setAntAgentMap(v.c_str()); };
  pd.customBase = [] { return std::string(store::customBase().c_str()); };
  pd.customKey = [] { return std::string(store::customKey().c_str()); };
  pd.customConv = [] { return std::string(store::customConv().c_str()); };
  pd.customModel = [] { return std::string(store::customModel().c_str()); };
  pd.attachOpenAI = [](JsonDocument& d) { connectors::attachOpenAI(d); };
  pd.attachMistral = [](JsonDocument& d) { connectors::attachMistral(d); };
  pd.attachAnthropic = [](JsonDocument& d) { connectors::attachAnthropic(d); };
  pd.nowMs = [] { return (uint32_t)millis(); };
  // CUM-477: every head loop's one resend of an unsent request may wait (bounded)
  // for the internal heap to recover - on the calling task, no new task.
  pd.delayMs = [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); };
  pd.freeHeap = [] { return (uint32_t)ESP.getFreeHeap(); };
  pd.largestBlock = [] {
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  };
  pd.alloc = &PsramJsonAllocator::instance();
  return pd;
}

}  // namespace agent
