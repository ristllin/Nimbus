#pragma once
#include <ESPAsyncWebServer.h>
#include <FS.h>

#include <cstddef>
#include <memory>

#include "nimbus/cloud/relay_heap.h"

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

// The plain fillers the big routes stream with: an open card file (read under the
// card lock; closed at EOF or error), or a buffer of `len` bytes. The card filler never
// waits for the lock: busy, it asks the server to call again (web_guard.h).
AwsResponseFiller cardFileFiller(std::shared_ptr<::File> f);
// The shared handle those card files live in: its deleter closes the file under the
// card lock when it is free (a read handle's close writes nothing to the card).
std::shared_ptr<::File> cardFileHandle();
AwsResponseFiller bufferFiller(std::shared_ptr<char> body, size_t len);

// The whole gate for a chunked big response: decide; on Busy send the busy answer and
// return false (the caller stops); on Trickle wrap `filler`; on Serve leave it.
bool admit(AsyncWebServerRequest* r, size_t bodyBytes, bool html, AwsResponseFiller& filler);

}  // namespace admission
}  // namespace nimbus::net
