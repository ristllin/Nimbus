#pragma once
#include <Arduino.h>
#include <FS.h>

// image_gen - turn a text prompt into a picture (OpenAI /v1/images/generations).
//
// The PNG is base64 in the response; gpt-image-1 returns it natively (dall-e needs
// a response_format param the API now rejects, or hands back a URL a one-TLS-slot
// device can't re-fetch). The whole image is decoded into a PSRAM buffer during the
// TLS read - never the ~266 KB internal heap, and never the SD DURING the read (the
// bus can't be held for a whole ~1 MB download without either tripping the main
// loop's watchdog under the memory Lock or racing other SD users). The caller
// writes the finished buffer to SD afterward, under a brief lock.

namespace agent {
namespace imagegen {

// Is a provider configured that can generate an image? (OpenAI only today.)
bool available();

// Cap on a generated image: gpt-image-1 low ~1 MB, higher tiers more. A response
// past this is refused (clean=false) rather than overrunning the buffer.
constexpr size_t kGenMaxBytes = 3u * 1024 * 1024;

// Generate an image from `prompt` into a freshly ps_malloc'd buffer. `model` blank
// => gpt-image-1; `size` blank => 1024x1024 (normalized per model); `quality` blank
// => low. Returns the buffer (CALLER MUST free() it) and sets `outLen`, or nullptr
// with a reason in `errOut`. Touches no filesystem. Blocks up to ~120 s (generation
// is slow) - MUST run on the turn task, never AsyncTCP.
uint8_t* generateToBuffer(const String& prompt, const char* model, const char* size,
                          const char* quality, size_t& outLen, String& errOut);

}  // namespace imagegen
}  // namespace agent
