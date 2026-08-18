#pragma once
// http_replay - portable helpers for replaying a tunneled request into the device's
// OWN web server over a loopback socket, and parsing the response back into a `res`
// frame. The device trusts the tunnel (the cloud already authenticated the owner), so
// it stamps its own LAN webAuthToken into the loopback request; that token NEVER
// travels to the cloud. Host-tested (test/test_http_replay). No sockets here: the
// device (src/net/relay_client.cpp) owns the WiFiClient.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The decoded response body can be a full device UI page, larger than the scarce
// internal SRAM. On device (NIMBUS_RELAY_PSRAM_BODY, set as a build flag for every
// firmware env so the type is identical across TUs) the body lives in PSRAM; on host
// it is a plain std::vector. Only the allocator differs; the API is unchanged.
#if defined(NIMBUS_RELAY_PSRAM_BODY)
#include <esp_heap_caps.h>
namespace nimbus {
namespace cloud {
template <class T>
struct PsramAlloc {
  using value_type = T;
  PsramAlloc() = default;
  template <class U>
  PsramAlloc(const PsramAlloc<U>&) {}
  T* allocate(std::size_t n) {
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);  // fall back to internal
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) { heap_caps_free(p); }
};
template <class A, class B>
bool operator==(const PsramAlloc<A>&, const PsramAlloc<B>&) { return true; }
template <class A, class B>
bool operator!=(const PsramAlloc<A>&, const PsramAlloc<B>&) { return false; }
}  // namespace cloud
}  // namespace nimbus
#endif

namespace nimbus {
namespace cloud {
namespace http_replay {

#if defined(NIMBUS_RELAY_PSRAM_BODY)
using BodyBuf = std::vector<uint8_t, nimbus::cloud::PsramAlloc<uint8_t>>;
#else
using BodyBuf = std::vector<uint8_t>;
#endif

using Headers = std::vector<std::pair<std::string, std::string>>;

// Build the HTTP/1.1 request head (through the blank line) for the loopback socket.
// - request line uses `method` + `path` (path carries the query verbatim);
// - Host is forced to 127.0.0.1;
// - X-Nimbus-Token is injected as `token` (any inbound copy is dropped and overridden);
// - hop-by-hop headers (protocol.ts HOP_BY_HOP) and Content-Length are stripped from
//   the passed-through headers; Content-Length is re-set from `bodyLen`;
// - Connection: close is appended.
// The caller writes this head, then the `bodyLen` body bytes.
// ⚠ HTTP/1.1 is mandatory: HTTP/1.0 does NOT de-chunk AsyncWebServer, so a chunked
// route would return chunk-framed bytes with no de-framing header (silent corruption).
std::string buildRequestHead(const std::string& method, const std::string& path,
                             const Headers& headers, size_t bodyLen, const std::string& token);

// Incremental parser for the loopback HTTP response. Feed socket bytes as they arrive;
// call endOfStream() when the socket closes. Handles Content-Length, Transfer-Encoding:
// chunked, and read-until-close bodies. Emits only an allowlisted set of headers for
// the `res` frame (Content-Type, Cache-Control, ETag, Content-Encoding,
// Content-Disposition, Location); Transfer-Encoding/Connection/Content-Length are dropped
// because the cloud re-frames the response.
class ResponseParser {
 public:
  explicit ResponseParser(size_t maxBodyBytes) : maxBodyBytes_(maxBodyBytes) {}

  void feed(const uint8_t* data, size_t len);
  void endOfStream();

  bool complete() const { return state_ == State::Done; }
  bool error() const { return state_ == State::Error; }
  bool overflow() const { return overflow_; }

  int status() const { return status_; }
  const Headers& headers() const { return outHeaders_; }
  const BodyBuf& body() const { return body_; }

 private:
  enum class State { Head, BodyLength, BodyChunked, BodyUntilClose, Done, Error };
  void parseHead_();
  void parseChunked_();

  std::vector<uint8_t> buf_;   // unparsed bytes (bounded by framing; small)
  BodyBuf body_;               // decoded body (PSRAM on device)
  Headers outHeaders_;
  State state_ = State::Head;
  int status_ = 0;
  size_t contentLength_ = 0;
  size_t bodyRead_ = 0;
  size_t maxBodyBytes_;
  bool overflow_ = false;
};

}  // namespace http_replay
}  // namespace cloud
}  // namespace nimbus
