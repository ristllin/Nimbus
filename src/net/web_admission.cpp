#include "web_admission.h"

#include <WiFi.h>
#include <esp_heap_caps.h>

#include <utility>

#include "../agent/memory_subsystem.h"   // agent::memory::WebLock (serializes card I/O)
#include "nimbus/sys/web_guard.h"        // the card-busy answer, the filler's try
#include "../agent/orchestrator.h"   // turnInFlight
#include "../sys/heap_watch.h"       // the lan.big activity tag
#include "../sys/agent_log.h"        // alogf: a card download that waited or gave up
#include "nimbus/cloud/relay_slot.h" // Retry-After values, the busy text
#include "nimbus/fault.h"            // RELAY_NOSLOT: the bench counter-leg (test builds)
#include "sdkconfig.h"               // CONFIG_LWIP_TCP_MSS

namespace nimbus::net {
namespace admission {

namespace {

// What the server offers a chunked filler with nothing in flight: its 2-segment send
// buffer less the 8 bytes of chunk framing (ESPAsyncWebServer WebResponses.cpp).
constexpr size_t kChunkedFullOffer = 2 * CONFIG_LWIP_TCP_MSS - 8;
static_assert(CONFIG_LWIP_TCP_MSS == nimbus::cloud::kTrickleChunk,
              "the trickle chunk is one TCP segment");
static_assert(CONFIG_LWIP_TCP_MSS == nimbus::net::kCardSegment, "a card stream's pace counts TCP segments");
static_assert(nimbus::net::kCardPaceFull.maxInFlight <= CONFIG_LWIP_TCP_SND_BUF_DEFAULT,
              "full speed is at most the socket's send buffer");

const char kBusyPage[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta http-equiv=\"refresh\" content=\"10\"><title>Nimbus</title></head>"
    "<body style=\"font-family:sans-serif;margin:2em\">"
    "<p>The device is busy right now. This page reloads in a few seconds.</p>"
    "</body></html>";

// Retry-After on a busy answer for memory: longer while a turn is in flight.
uint32_t busyRetryS() {
  return agent::orchestrator::turnInFlight() ? nimbus::cloud::kBusyRetryTurnS : nimbus::cloud::kBusyRetryLowMemS;
}

}  // namespace

// Picks the admission policy only. It is not a trust boundary: authentication is
// decided elsewhere, for relay and LAN requests alike.
bool viaRelay(AsyncWebServerRequest* r) {
  if (!r || !r->client()) return false;
  const IPAddress from = r->client()->remoteIP();
  if (from == IPAddress(127, 0, 0, 1)) return true;
  const IPAddress self = WiFi.localIP();
  return (uint32_t)self != 0u && from == self;   // the relay's STA-address fallback
}

nimbus::cloud::ServeDecision decide(AsyncWebServerRequest* r, size_t bodyBytes) {
  using nimbus::cloud::ServeDecision;
  if (bodyBytes > nimbus::cloud::kServeSmallMax) nimbus::heapwatch::touch(nimbus::heapwatch::kLanBig);
  const bool turn = agent::orchestrator::turnInFlight();
  const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (viaRelay(r)) {
    if (nimbus::fault::active(nimbus::fault::RELAY_NOSLOT)) return ServeDecision::Serve;
    return nimbus::cloud::tunnelServeDecision(bodyBytes, turn, intFree);
  }
  return nimbus::cloud::lanServeDecision(bodyBytes, turn, intFree);
}

void sendCardBusy(AsyncWebServerRequest* r) {
  AsyncWebServerResponse* res = r->beginResponse(503, "application/json", nimbus::sys::cardBusyJson());
  res->addHeader("Retry-After", String((unsigned)nimbus::sys::kCardBusyRetryS));
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

void sendBusy(AsyncWebServerRequest* r, bool html) {
  const uint32_t retryS = busyRetryS();
  AsyncWebServerResponse* res = html ? r->beginResponse(503, "text/html", kBusyPage)
                                     : r->beginResponse(503, "text/plain", nimbus::cloud::kBusyBody);
  res->addHeader("Retry-After", String((unsigned)retryS));
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

AwsResponseFiller trickle(AwsResponseFiller inner) {
  return [inner](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
    const size_t grant = nimbus::cloud::trickleGrant(maxLen, kChunkedFullOffer);
    if (!grant) return RESPONSE_TRY_AGAIN;   // two segments still in flight: next ack
    return inner(buf, grant, index);
  };
}

// ---- card streams -----------------------------------------------------------------

namespace {

// The staging buffer of a card stream lives in PSRAM when it can: internal SRAM is the
// scarce pool (panic F2), and lwIP copies the bytes into its own buffers on add().
uint8_t* stagingGet(size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return static_cast<uint8_t*>(p);
}
void stagingPut(uint8_t* buf) { heap_caps_free(buf); }

std::string toStd(const String& s) { return std::string(s.c_str(), s.length()); }

}  // namespace


// The device side of nimbus::net::CardStream: a direct AsyncWebServerResponse (the
// library's AsyncAbstractResponse keeps the credit rule in a final method), its source
// (the route's open and fill, run on the web task), and the connection as its sink.
class CardStreamResponse final : public AsyncWebServerResponse, private nimbus::net::CardSource {
 public:
  CardStreamResponse(AsyncWebServerRequest* r, const char* contentType, CardOpenFn open, CardFillFn fill)
      : open_(std::move(open)),
        fill_(std::move(fill)),
        version_(r->version()),
        stream_(*this, r->version() != 0,
                viaRelay(r) ? nimbus::net::kCardStreamRelayWaitMs : nimbus::net::kCardStreamWaitMs,
                nimbus::net::CardBufferAlloc{&stagingGet, &stagingPut}) {
    _code = 200;
    _contentType = contentType;
    _sendContentLength = false;   // the length is the stream's: chunked, or the close
    _chunked = version_ != 0;
  }
  ~CardStreamResponse() override {
    const uint32_t waited = stream_.waitedMs(millis());
    if (stream_.gaveUp())
      agent::alogf("web: a card download gave up after waiting %u ms for the card (%u holds, %u B sent)",
                   (unsigned)waited, (unsigned)stream_.stalls(), (unsigned)stream_.bodyBytes());
    else if (stream_.stalls())   // routine: the RAM log only (GET /api/log)
      agent::alogif("web: a card download waited %u ms for the card (%u holds, %u B sent)", (unsigned)waited,
                    (unsigned)stream_.stalls(), (unsigned)stream_.bodyBytes());
  }

  bool _sourceValid() const override { return true; }
  void _respond(AsyncWebServerRequest* r) override {
    addHeader("Connection", "close", false);
    run(r, 0, /*first=*/true);
  }
  size_t _ack(AsyncWebServerRequest* r, size_t len, uint32_t) override { return run(r, len, /*first=*/false); }

 private:
  friend class CardOpen;

  // The connection. The stream's abort is carried out by run() after the pass.
  struct Sink : nimbus::net::CardSink {
    explicit Sink(AsyncClient* client) : c(client) {}
    AsyncClient* c;
    bool aborted = false;
    size_t space() override { return c->space(); }
    size_t add(const char* data, size_t len) override { return c->add(data, len); }
    void send() override { c->send(); }
    void abort() override { aborted = true; }
    bool drained() override { return c->space() >= CONFIG_LWIP_TCP_SND_BUF_DEFAULT; }
  };

  // Reset the connection: the download failed (past the wait budget, or a read error).
  size_t cut(AsyncClient* c) {
    owedAbort_ = false;
    _state = RESPONSE_FAILED;
    c->abort();
    return 0;
  }

  size_t run(AsyncWebServerRequest* r, size_t acked, bool first) {
    AsyncClient* c = r->client();
    if (owedAbort_) return cut(c);   // owed by the server's first call (below)
    req_ = r;
    Sink sink(c);
    const size_t n = stream_.pass(sink, acked, millis());
    req_ = nullptr;
    syncState();
    switch (nimbus::net::afterPass(stream_, sink.aborted, first, acked)) {
      case nimbus::net::CardConn::Abort:
        return cut(c);
      case nimbus::net::CardConn::OweAbort:   // carried out at the next pass, not inside _respond
        owedAbort_ = true;
        return 0;
      case nimbus::net::CardConn::Close:
        c->close();
        break;
      case nimbus::net::CardConn::Keep:
        break;
    }
    return n;
  }

  void syncState() {
    using Phase = nimbus::net::CardStream::Phase;
    switch (stream_.phase()) {
      case Phase::Open:
      case Phase::Head:
        _state = RESPONSE_HEADERS;
        break;
      case Phase::Body:
        _state = RESPONSE_CONTENT;
        break;
      case Phase::Done:
        _state = RESPONSE_END;
        break;
      case Phase::Failed:
        break;   // RESPONSE_FAILED once the connection is reset (cut)
    }
  }

  // A whole small answer in the library's own head format.
  std::string whole(int code, const char* contentType, const String& body, uint32_t retryAfterS) {
    AsyncBasicResponse a(code, contentType, body);
    a.addHeader("Cache-Control", "no-store");
    if (retryAfterS) a.addHeader("Retry-After", String((unsigned)retryAfterS));
    String h;
    a._assembleHead(h, version_);
    h += body;
    return toStd(h);
  }

  // nimbus::net::CardSource
  nimbus::net::CardStep open() override {
    if (!open_ || !req_) return nimbus::net::CardStep::Fail;
    CardOpen o(*this, req_);
    return open_(o);
  }
  std::string head() override {
    String h;
    _assembleHead(h, version_);
    return toStd(h);
  }
  std::string answer() override { return std::move(answer_); }
  nimbus::net::CardPace pace() override { return pace_; }
  nimbus::net::CardStep fill(uint8_t* buf, size_t cap, size_t& n) override { return fill_(buf, cap, n); }
  std::string busyAnswer() override {
    return whole(503, "application/json", nimbus::sys::cardBusyJson(), nimbus::sys::kCardBusyRetryS);
  }

  CardOpenFn open_;
  CardFillFn fill_;
  uint8_t version_;
  nimbus::net::CardStream stream_;
  nimbus::net::CardPace pace_ = nimbus::net::kCardPaceFull;
  std::string answer_;
  AsyncWebServerRequest* req_ = nullptr;   // set during a pass only
  bool owedAbort_ = false;
};

nimbus::net::CardStep CardOpen::answer(int code, const char* contentType, const String& body) {
  res_.answer_ = res_.whole(code, contentType, body, 0);
  return nimbus::net::CardStep::Answer;
}

void CardOpen::header(const char* name, const String& value) { res_.addHeader(name, value); }

nimbus::net::CardStep CardOpen::ready(size_t bodyBytes) {
  switch (decide(r_, bodyBytes)) {
    case nimbus::cloud::ServeDecision::Busy:
      res_.answer_ = res_.whole(503, "text/plain", nimbus::cloud::kBusyBody, busyRetryS());
      return nimbus::net::CardStep::Answer;
    case nimbus::cloud::ServeDecision::Trickle:
      res_.pace_ = nimbus::net::kCardPaceTrickle;
      break;
    case nimbus::cloud::ServeDecision::Serve:
      res_.pace_ = nimbus::net::kCardPaceFull;
      break;
  }
  return nimbus::net::CardStep::Ready;
}

AsyncWebServerResponse* cardStream(AsyncWebServerRequest* r, const char* contentType, CardOpenFn open,
                                   CardFillFn fill) {
  return new CardStreamResponse(r, contentType, std::move(open), std::move(fill));
}

CardFillFn cardFileFill(std::shared_ptr<::File> f) {
  // The end is the size the file had when it opened (a read handle's size() is the stat
  // taken at its open), counted in bytes delivered: position() is ftell, which reports
  // -1 after a read error and would pass for the end.
  size_t delivered = 0;
  return [f, delivered](uint8_t* buf, size_t cap, size_t& n) mutable -> nimbus::net::CardStep {
    agent::memory::WebLock card(nimbus::sys::kWebFillerWaitMs);
    if (!card) return nimbus::net::CardStep::Busy;   // a tool holds the card: the next pass
    if (!*f) return nimbus::net::CardStep::Fail;
    const size_t got = f->read(buf, cap);
    const nimbus::net::CardStep st = nimbus::net::fileFillStep(got, cap, delivered, f->size());
    if (st == nimbus::net::CardStep::Data) n = got;
    else f->close();   // the end, or a read error: the file is done either way
    return st;
  };
}

std::shared_ptr<::File> cardFileHandle() {
  return std::shared_ptr<::File>(new ::File(), [](::File* fp) {
    // The response is gone (sent, or the client left mid-stream). A stream that reached
    // its end closed the file under the lock already; a read handle's close writes
    // nothing to the card, so a busy lock is never waited for here.
    agent::memory::WebLock card(nimbus::sys::kWebFillerWaitMs);
    fp->close();
    delete fp;
  });
}

AwsResponseFiller bufferFiller(std::shared_ptr<char> body, size_t len) {
  return [body, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
    if (index >= len) return 0;
    size_t take = len - index;
    if (take > maxLen) take = maxLen;
    memcpy(buf, body.get() + index, take);
    return take;
  };
}

void sendJson(AsyncWebServerRequest* r, String&& body) {
  if (body.isEmpty()) return sendBusy(r, /*html=*/false);
  auto held = std::make_shared<String>(std::move(body));
  const size_t len = held->length();
  AsyncWebServerResponse* res = r->beginResponse(
      "application/json", len, [held, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
        if (index >= len) return 0;
        const size_t take = len - index < maxLen ? len - index : maxLen;
        memcpy(buf, held->c_str() + index, take);
        return take;
      });
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

bool admit(AsyncWebServerRequest* r, size_t bodyBytes, bool html, AwsResponseFiller& filler) {
  switch (decide(r, bodyBytes)) {
    case nimbus::cloud::ServeDecision::Busy:
      sendBusy(r, html);
      return false;
    case nimbus::cloud::ServeDecision::Trickle:
      filler = trickle(std::move(filler));
      return true;
    case nimbus::cloud::ServeDecision::Serve:
      break;
  }
  return true;
}

}  // namespace admission
}  // namespace nimbus::net
