#pragma once
#include <cstddef>
#include <string>

// Provider-file capture policy - the PORTABLE decisions behind the code-sandbox SD
// round-trip (CUM-49). When the assistant's sandbox produces a file, the device
// streams it from the provider's Files API onto the SD card (src/agent/adapters/
// provider_file_fetch.cpp). The streaming, TLS and SD I/O are device glue, but the
// two safety decisions are plain data and live here so they are host-tested:
//
//   1. captureFitsCap - the streaming byte LIMIT ("sandbox exec limit"): a runaway
//      or hostile sandbox file must not stream forever and fill the card.
//   2. captureVerdict - the post-download INTEGRITY gate: a truncated body must
//      never be registered as saved (a partial file recorded as complete is a lie
//      the assistant then repeats to the owner).

namespace nimbus {
namespace orch {

// The per-file capture cap. Mirrors FileStore::Limits::maxFileBytes so a captured
// sandbox file obeys the same ceiling as any other artifact.
constexpr size_t kProviderFileCapBytes = 8u * 1024 * 1024;

// May `add` more bytes be written when `have` are already written, under `cap`?
// Overflow-safe (never wraps for a hostile huge `add`). false == stop the stream.
bool captureFitsCap(size_t have, size_t add, size_t cap);

struct CaptureVerdict {
  bool        ok = false;
  std::string reason;   // empty when ok; user/log-facing cause otherwise
};

// Decide whether a streamed download may be committed as saved.
//   httpCode  - the response status (0 == no response / connection failure)
//   bytes     - bytes actually written to the card
//   expected  - the server's Content-Length, or SIZE_MAX when it sent none
//   cleanEof  - the body ended on a clean server EOF (not the wall-clock deadline)
//   writeErr  - the sink hit an SD write error OR the byte cap (either is fatal)
// ok requires: HTTP 200, no write error, at least one byte, a clean EOF, and - when
// the server declared a length - an exact byte match. Anything else is refused with
// a specific reason.
CaptureVerdict captureVerdict(int httpCode, size_t bytes, size_t expected,
                              bool cleanEof, bool writeErr);

}  // namespace orch
}  // namespace nimbus
