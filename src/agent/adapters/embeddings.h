#pragma once
#include <Arduino.h>

#include <cstdint>
#include <vector>

// embeddings - the device seam that turns text into a stored vector. The JSON
// build + parse is portable + host-tested (lib/core nimbus/orch/embedding.h);
// this is the thin TLS glue that POSTs to the provider /v1/embeddings endpoint
// over the same arbiter-gated WiFiClientSecure path the chat adapters use, then
// quantizes the float embedding to int8 (VectorMemory::quantize).
//
// Config comes from agent::store (embedProvider/embedModel/embedDims, set-once).
// LIVE-GATED: needs the matching provider key + STA WiFi; returns an empty
// vector (with `err` set to the real cause: no key, key rejected, tls busy,
// connect failed, timeout, HTTP <code> [+ router refusal code], or parse) rather
// than storing a garbage vector. The memory tools map `err` to honest words and,
// for search, fall back to a keyword scan (CUM-435).
namespace agent {
namespace embeddings {

// Embed `text` using the configured provider/model/dims. On success returns the
// int8-quantized vector (length == embedDims(), or the provider width when dims
// is 0) and leaves `err` empty. On failure returns an empty vector and sets
// `err`. Blocking (one bounded TLS round-trip); call off the watchdog'd loop.
std::vector<int8_t> embed(const String& text, String& err);

// Embed with an EXPLICIT provider/model/dims instead of the stored config. Used to
// (a) validate a candidate embedding config with a real API call BEFORE saving it
// (owner: "once replaced, a quick real api call to validate"), and (b) recompute the
// whole VDB during a provider/model migration. Same blocking one-TLS-call contract.
std::vector<int8_t> embedWith(const String& text, String& err, const String& provider,
                              const String& model, int dims);

// Convenience: does the configured provider have a usable key right now?
bool available();

}  // namespace embeddings
}  // namespace agent
