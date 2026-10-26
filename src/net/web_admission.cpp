#include "web_admission.h"

#include <WiFi.h>
#include <esp_heap_caps.h>

#include "../agent/memory_subsystem.h"   // agent::memory::WebLock (serializes card I/O)
#include "nimbus/sys/web_guard.h"        // the card-busy answer, the filler's try
#include "../agent/orchestrator.h"   // turnInFlight
#include "../sys/heap_watch.h"       // the lan.big activity tag
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

const char kBusyPage[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta http-equiv=\"refresh\" content=\"10\"><title>Nimbus</title></head>"
    "<body style=\"font-family:sans-serif;margin:2em\">"
    "<p>The device is busy right now. This page reloads in a few seconds.</p>"
    "</body></html>";

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
  const uint32_t retryS = agent::orchestrator::turnInFlight() ? nimbus::cloud::kBusyRetryTurnS
                                                              : nimbus::cloud::kBusyRetryLowMemS;
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

AwsResponseFiller cardFileFiller(std::shared_ptr<::File> f) {
  return [f](uint8_t* buf, size_t maxLen, size_t) -> size_t {
    agent::memory::WebLock card(nimbus::sys::kWebFillerWaitMs);
    if (!card) return RESPONSE_TRY_AGAIN;   // a turn's tool holds the card: next poll
    if (!*f) return 0;
    const int n = f->read(buf, maxLen);
    if (n <= 0) { f->close(); return 0; }   // EOF/error ends the stream
    return size_t(n);
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
