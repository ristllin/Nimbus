#pragma once
// ws_write - a pure, host-tested driver for writing a whole buffer over a stream
// that may accept only part of a write (a short/partial write) or none of it (a
// would-block). Header-only so the firmware WS path and the host test share the
// EXACT same loop.
//
// CUM-182: the tunnel's large res frame (the ~370 KB config/login page) does not
// always leave in one WiFiClientSecure::write. The old code treated "did not
// write it all in one call" as failure, which truncated the frame on the wire
// and dropped the whole session mid-page (the field 502 + reconnect). This
// driver instead resumes at the sent offset until the buffer is fully out, the
// connection dies, or a deadline passes.
#include <cstddef>
#include <cstdint>

namespace nimbus {
namespace cloud {

// Drive a reliable, chunked, whole-buffer write.
//   sink(off, want) -> bytes accepted this call (0 = would-block, retry later)
//   alive()         -> false when the connection has died (hard failure)
//   expired()       -> true once the write deadline has passed (bounds a stuck write)
//   yield()         -> cooperative delay between retries
// Returns true iff all `len` bytes were accepted before expired() trips. `chunk`
// caps the size handed to the sink per call (0 is treated as "no cap").
template <class Sink, class Alive, class Expired, class Yield>
bool drainAll(size_t len, size_t chunk, Sink sink, Alive alive, Expired expired, Yield yield) {
  size_t sent = 0;
  while (sent < len) {
    if (!alive()) return false;
    size_t want = len - sent;
    if (chunk && want > chunk) want = chunk;
    size_t n = sink(sent, want);
    if (n > 0) {
      sent += n;
    } else {
      if (expired()) return false;
      yield();
    }
  }
  return true;
}

}  // namespace cloud
}  // namespace nimbus
