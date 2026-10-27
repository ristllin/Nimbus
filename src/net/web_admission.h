#pragma once
#include <ESPAsyncWebServer.h>
#include <FS.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "nimbus/cloud/relay_heap.h"
#include "nimbus/net/card_stream.h"

// web_admission - internal-SRAM admission for the web server's big responses (panic
// F2). The rule is portable and host-tested (nimbus/cloud/relay_heap.h
// tunnelServeDecision / lanServeDecision); this is the device side for the routes that
// can send more than 16 KB: the config page, file downloads, the durable log files,
// stored blobs, and the PSRAM-streamed JSON pages (episodic memory, vector browse).
//
//   - up to 16 KB: served as today;
//   - larger: trickled (about two TCP segments in flight per connection instead of a
//     full send buffer, 5,744 B of internal packet buffers each), or answered 503 with
//     Retry-After when internal free is under the floor. The cloud relay's own
//     loopback replay (127.0.0.1, or the device's own address) also gets the 503 while
//     a turn is in flight: the cloud retries it after the turn; a LAN browser trickles.
namespace nimbus::net {
namespace admission {

// Is this request the cloud relay's loopback replay of a tunneled request?
bool viaRelay(AsyncWebServerRequest* r);

// The decision for a response of `bodyBytes` to this request.
nimbus::cloud::ServeDecision decide(AsyncWebServerRequest* r, size_t bodyBytes);

// The busy answer: 503 + Retry-After. `html`: a small page that reloads itself.
void sendBusy(AsyncWebServerRequest* r, bool html);

// The card-busy answer (nimbus/sys/web_guard.h): 503 + Retry-After kCardBusyRetryS and
// a JSON body, for a route that cannot answer while its WebLock found the card lock busy.
void sendCardBusy(AsyncWebServerRequest* r);

// A chunked filler that trickles.
AwsResponseFiller trickle(AwsResponseFiller inner);

// The plain filler the big in-memory routes stream with: a buffer of `len` bytes.
AwsResponseFiller bufferFiller(std::shared_ptr<char> body, size_t len);

// ---- card downloads (nimbus/net/card_stream.h) -------------------------------------
//
// A download read from the SD card (the durable log, a stored file, an archive item or
// day) is a card stream, never a library filler: the library's in-flight credits run
// out when a card read answers "try again" with nothing in flight, and the download
// ends early (AsyncTCP's ack timeout). A card stream opens the file inside the stream
// and reads it pass by pass; every step only tries the card lock (a WebLock with
// kWebFillerWaitMs, web_guard.h) and answers Busy when a tool holds it, so a download
// waits out a hold of any length up to kCardStreamWaitMs without the web task ever
// waiting, then answers 503 (nothing sent yet) or is cut (reset).

class CardStreamResponse;

// What a card stream's open step answers. It runs on the web task, on the server's
// first call and again on each poll while the card is held; it tries the card lock
// itself and returns busy(), answer(...) or ready(...).
class CardOpen {
 public:
  nimbus::net::CardStep busy() const { return nimbus::net::CardStep::Busy; }
  // No stream: a whole small answer (an error, a list).
  nimbus::net::CardStep answer(int code, const char* contentType, const String& body);
  // A header for the stream's head; call before ready().
  void header(const char* name, const String& value);
  // The stream starts; `bodyBytes` (its size, or an estimate) goes through the memory
  // admission: a big body trickles, or the answer is busy (503) when memory is short.
  nimbus::net::CardStep ready(size_t bodyBytes);

 private:
  friend class CardStreamResponse;
  CardOpen(CardStreamResponse& res, AsyncWebServerRequest* r) : res_(res), r_(r) {}
  CardStreamResponse& res_;
  AsyncWebServerRequest* r_;
};
using CardOpenFn = std::function<nimbus::net::CardStep(CardOpen&)>;
// One read: up to `cap` bytes into `buf` (Data sets `n`), Busy, End or Fail.
using CardFillFn = std::function<nimbus::net::CardStep(uint8_t* buf, size_t cap, size_t& n)>;

// The response for a card download (send it with r->send()).
AsyncWebServerResponse* cardStream(AsyncWebServerRequest* r, const char* contentType, CardOpenFn open,
                                   CardFillFn fill);
// The shared handle a card file lives in: its deleter closes the file under the card
// lock when it is free (a read handle's close writes nothing to the card).
std::shared_ptr<::File> cardFileHandle();
// The read of an open card file: Busy while the card is held, End at the end of the
// file, Fail on a read error before it (never a short download that looks whole).
CardFillFn cardFileFill(std::shared_ptr<::File> f);
// A built JSON body sent through the filler path, never the library's basic response:
// that one's write() reports 0 when lwIP queued the bytes but could not output them yet
// (tcp_output ERR_MEM while the packet buffers run short, as in a photo turn) and then
// queues the same bytes again, so a body over one send buffer (5,744 B less the
// headers) arrives with its start repeated past that point (bench, 26 Oct: /api/state).
// The filler path counts what was queued. An empty body (not built) answers busy.
void sendJson(AsyncWebServerRequest* r, String&& body);

// The whole gate for a chunked big response: decide; on Busy send the busy answer and
// return false (the caller stops); on Trickle wrap `filler`; on Serve leave it.
bool admit(AsyncWebServerRequest* r, size_t bodyBytes, bool html, AwsResponseFiller& filler);

}  // namespace admission
}  // namespace nimbus::net
