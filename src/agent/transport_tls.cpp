#include "transport_tls.h"

#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "../sys/agent_log.h"
#include "../sys/net_util.h"
#include "../sys/ps_json.h"
#include "../sys/tls_arbiter.h"
#include "agent_config.h"
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
//   - The request body arrives as ONE contiguous string and goes out in ONE
//     client.write() - the serialize-into-one-buffer-single-write rule (per-
//     chunk TLS records + lwIP pbufs collapsed internal heap 30 KB -> 5 KB,
//     measured live). The string's storage is PSRAM-backed on this firmware:
//     main.cpp calls heap_caps_malloc_extmem_enable(128), so any malloc >=128 B
//     (every real body) spills to PSRAM.
//   - The response body is read whole into out.body (same PSRAM spill) and
//     filter-parsed by the PORTABLE side - the pre-split code stream-parsed off
//     the socket; the trade is one PSRAM-resident copy of the response for a
//     host-testable wire layer. A 256 KB cap bounds a runaway body (the parse
//     then fails clean on the truncated JSON - same failure class as the old
//     read-deadline truncation).
namespace agent {

namespace {

constexpr size_t kMaxResponseBody = 256 * 1024;
// Streaming-retained-size cap: even though only filter-retained fields are stored
// (PSRAM), bound the retained bytes so a hostile/buggy provider can't stream an
// unbounded field into memory (the old whole-body path had kMaxResponseBody; the
// streaming path keeps this guard on the retained document instead).
constexpr size_t kMaxRetainedBytes = 512 * 1024;

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

    // Per-phase INTERNAL-heap trace (owner ask 2026-08-05: measure, don't guess -
    // the deep-turn degradation is unattributed while bodies/TLS live on PSRAM).
    // free/largest sampled at 4 phase boundaries; divergence = fragmentation,
    // a drop at `conn` = handshake/socket internal cost, a drop that survives
    // `done` = leaked/retained internal memory per request. One log line/request.
    struct HeapSample { uint32_t free, largest; };
    auto sampleHeap = [] {
      return HeapSample{
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)};
    };
    const HeapSample hsPre = sampleHeap();
    HeapSample hsConn{0, 0}, hsWrote{0, 0};

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
    auto closeConn = [&]() { if (req.tls) tlsClose(tls); else plain.stop(); };

    // F25: the WHOLE operation (connect retries + I/O) rides one wall clock -
    // 3× connect could otherwise burn ~127 s BEFORE the read deadline even began.
    const uint32_t deadline = millis() + req.timeoutMs;
    auto expired = [&]() { return (int32_t)(millis() - deadline) >= 0; };

    bool connected = false;
    for (int a = 0; a < 3 && !connected && !expired(); a++) {
      if (client->connect(req.host.c_str(), req.port)) { connected = true; break; }
      closeConn();
      if (a < 2) vTaskDelay(pdMS_TO_TICKS(400));
    }
    hsConn = sampleHeap();  // post-connect: handshake + socket internal cost
    if (!connected) {
      arbiter::releaseWork();
      alogf("transport: connect %s://%s:%u failed (3x) heap=%u",
            req.tls ? "https" : "http", req.host.c_str(), (unsigned)req.port,
            ESP.getFreeHeap());
      err = "connect failed";
      return 0;
    }

    // Request head (HTTP/1.0 + close => Content-Length framing, no chunking).
    String head = String(req.method.c_str()) + " " + req.path.c_str() + " HTTP/1.0\r\n" +
                  "Host: " + req.host.c_str() + "\r\n";
    for (const auto& h : req.headers)
      head += String(h.first.c_str()) + ": " + h.second.c_str() + "\r\n";
    head += "Content-Length: " + String((unsigned)req.body.size()) + "\r\nConnection: close\r\n\r\n";
    // ⚠ Both write results are CHECKED. They used to be discarded, which made a
    // short write indistinguishable from a healthy request: the head announces
    // Content-Length: N, fewer than N body bytes go out, the server waits forever
    // for the rest, and the read below times out and reports "no response" - the
    // same string a genuine network failure produces. On a ~50 KB round body under
    // memory pressure that is a real possibility, and it was invisible.
    const size_t headLen = head.length();
    if (client->print(head) != headLen) {
      closeConn();
      arbiter::releaseWork();
      alogf("transport: short head write %s%s heap=%u", req.host.c_str(),
            req.path.c_str(), ESP.getFreeHeap());
      err = "request head write failed";
      return 0;
    }
    // ONE write of the complete (PSRAM-spilled) body - never chunked serialization.
    if (!req.body.empty()) {
      const size_t n = client->write((const uint8_t*)req.body.data(), req.body.size());
      hsWrote = sampleHeap();  // post-write: lwIP TX transit cost for this body size
      if (n != req.body.size()) {
        closeConn();
        arbiter::releaseWork();
        alogf("transport: short body write %u/%u to %s%s heap=%u", (unsigned)n,
              (unsigned)req.body.size(), req.host.c_str(), req.path.c_str(),
              ESP.getFreeHeap());
        // Naming the truncation matters: the caller previously saw "no response"
        // and blamed the network for a request the device never finished sending.
        err = "request body truncated (" + std::to_string(n) + "/" +
              std::to_string(req.body.size()) + " bytes written)";
        return 0;
      }
    }

    // (deadline armed above the connect retry - F25.)
    // Status line: "HTTP/1.0 200 ...".
    int code = 0;
    String status;
    while (!expired()) {
      if (client->available()) { char c = client->read(); if (c == '\n') break; if (c != '\r') status += c; }
      else if (!client->connected() && !client->available()) break;
      else delay(2);
    }
    int sp = status.indexOf(' ');
    if (sp > 0 && (int)status.length() >= sp + 4) code = status.substring(sp + 1, sp + 4).toInt();

    // Skip headers up to the blank line.
    String line;
    bool headersDone = false;
    while (!headersDone && !expired()) {
      if (client->available()) {
        char c = client->read();
        if (c == '\n') { if (line.length() == 0) headersDone = true; line = ""; }
        else if (c != '\r') line += c;
      } else if (!client->connected() && !client->available()) break;
      else delay(2);
    }

    // Body.
    if (headersDone) {
      if (doc) {
        // STREAMING filter-parse off the socket. The parser pulls bytes as it
        // goes (draining lwIP RX pbufs continuously) and stores ONLY the
        // filter-retained fields - the fat response body never fully resides, so
        // any response size is safe on the ~47 K internal-heap budget.
        uint32_t heapBefore = ESP.getFreeHeap();
        BlockingClientReader rdr(client, deadline);
        DeserializationError derr =
            deserializeJson(*doc, rdr, DeserializationOption::Filter(*filter),
                            DeserializationOption::NestingLimit(kResponseNestingLimit));
        // A parse break is NOT cosmetic: ArduinoJson keeps whatever it read
        // before the break, so without this the caller cannot tell a truncated
        // response from a complete one and happily uses the fragment. Report it
        // through `err` (the status still returns, per the seam contract).
        // EmptyInput is excluded so an empty 200/204 behaves like the host path,
        // which skips the parse entirely rather than calling it an error.
        if (derr && derr.code() != DeserializationError::EmptyInput) {
          err = std::string("response parse failed: ") + derr.c_str();
          alogf("transport: stream-parse %s (heap %u->%u)", derr.c_str(),
                (unsigned)heapBefore, (unsigned)ESP.getFreeHeap());
        }
        // Guard against an unbounded retained field (hostile/buggy provider): the
        // filter keeps only small fields, but bound it so PSRAM can't be flooded.
        if (!derr && doc->memoryUsage() > kMaxRetainedBytes) {
          alogf("transport: retained %u B over cap -> drop", (unsigned)doc->memoryUsage());
          doc->clear();
        }
      } else {
        uint8_t buf[512];
        while (!expired() && out->body.size() < kMaxResponseBody) {
          int n = client->read(buf, sizeof(buf));
          if (n > 0) out->body.append((const char*)buf, (size_t)n);
          else if (!client->connected() && !client->available()) break;
          else delay(2);
        }
      }
    }

    closeConn();
    arbiter::releaseWork();

    {
      // done − pre that stays negative across many requests = retained internal
      // memory per request (TIME_WAIT PCBs / leaks); a big pre→conn drop = the
      // handshake's true internal cost; free≫largest anywhere = fragmentation.
      const HeapSample hsDone = sampleHeap();
      alogf("transport: heaptrace %s%s body=%u pre=%u/%u conn=%u/%u wrote=%u/%u done=%u/%u",
            req.host.c_str(), req.path.c_str(), (unsigned)req.body.size(),
            (unsigned)hsPre.free, (unsigned)hsPre.largest,
            (unsigned)hsConn.free, (unsigned)hsConn.largest,
            (unsigned)hsWrote.free, (unsigned)hsWrote.largest,
            (unsigned)hsDone.free, (unsigned)hsDone.largest);
    }

    if (code <= 0) { err = "no response"; return 0; }
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
  pd.freeHeap = [] { return (uint32_t)ESP.getFreeHeap(); };
  pd.largestBlock = [] {
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  };
  pd.alloc = &PsramJsonAllocator::instance();
  return pd;
}

}  // namespace agent
