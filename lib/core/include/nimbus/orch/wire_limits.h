#pragma once
#include <cstddef>

// wire_limits - the byte ceilings every provider-bound string lives under, in one
// place so the caps that feed a request field can never drift above the ceiling
// that field is serialized under.
//
// ArduinoJson (both the ESP32 and the 64-bit host build) stores a string of
// 65,536 B or more as null and marks the document overflowed: the provider then
// sees a missing system prompt, tool result or fold input and answers 400, or,
// worse, accepts the hole. So every model-bound string stays at or under
// kJsonStrGuard, a margin below that ceiling:
//   - tool results, call arguments and replayed prose are clipped to it, UTF-8-safe,
//     with a visible note (wire::putModelStr);
//   - the system prompt and the user seed are never clipped silently: a request
//     that carries a longer one is refused before it is sent
//     (wire::serializeChecked), and the caps that build them sit below it.
namespace nimbus {
namespace orch {

inline constexpr size_t kJsonStringCeiling = 65535;   // ArduinoJson's per-string limit
inline constexpr size_t kJsonStrGuard = 60000;        // every model-bound string <= this
static_assert(kJsonStrGuard < kJsonStringCeiling, "the guard must sit below the ceiling");

// A registry tool's text, after the UTF-8 repair, is clipped to this with its note.
inline constexpr size_t kToolTextMax = kJsonStrGuard;

// The fold sends ONE user string: "[PREVIOUS SUMMARY]\n" + summary + "\n\n" +
// "[CONVERSATION SINCE]\n" + digest (orch_compact.cpp buildCompactInputs). The
// framing is 42 bytes; 64 leaves room for a wording change.
inline constexpr size_t kFoldFramingBytes = 64;

// The largest digest a fold may carry next to a summary of `summaryBytes`, so the
// whole fold input stays under kJsonStrGuard. 0 when the summary alone fills it.
inline constexpr size_t foldDigestCap(size_t summaryBytes) {
  return summaryBytes + kFoldFramingBytes >= kJsonStrGuard
             ? 0
             : kJsonStrGuard - summaryBytes - kFoldFramingBytes;
}

// The per-result clamp the head loop applies (budget toolResultBytes) leaves room
// for its own "...[truncated N of M B - fetch the rest with results.get(..)]" note
// and the "ERROR: " prefix two renderers add, so a clamped result still fits.
inline constexpr size_t kToolResultCapMax = kJsonStrGuard - 1000;

}  // namespace orch
}  // namespace nimbus
