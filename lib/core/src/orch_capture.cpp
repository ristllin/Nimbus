#include "nimbus/orch/capture.h"

#include <limits>

namespace nimbus {
namespace orch {

bool captureFitsCap(size_t have, size_t add, size_t cap) {
  if (have > cap) return false;                 // already over (defensive)
  return add <= cap - have;                     // no addition, no overflow
}

CaptureVerdict captureVerdict(int httpCode, size_t bytes, size_t expected,
                              bool cleanEof, bool writeErr) {
  CaptureVerdict v;
  if (writeErr) { v.reason = "write error / too large"; return v; }
  if (httpCode != 200) {
    v.reason = httpCode ? ("HTTP " + std::to_string(httpCode)) : std::string("no response");
    return v;
  }
  if (bytes == 0) { v.reason = "empty file"; return v; }
  // A truncated body (deadline instead of EOF, or short of the declared length) is
  // NOT a saved file - the head must not report a partial download as complete.
  const bool complete = cleanEof &&
      (expected == std::numeric_limits<size_t>::max() || bytes == expected);
  if (!complete) {
    v.reason = "download truncated (" + std::to_string(bytes) + " of " +
               (expected == std::numeric_limits<size_t>::max() ? std::string("?")
                                                               : std::to_string(expected)) +
               " bytes)";
    return v;
  }
  v.ok = true;
  return v;
}

}  // namespace orch
}  // namespace nimbus
