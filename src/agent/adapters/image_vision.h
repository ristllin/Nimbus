#pragma once
#include <Arduino.h>

namespace fs { class FS; }

// image_vision - turn a picture into a sentence.
//
// The device never puts image BYTES in the conversation. A 200 KB photo is
// ~270 KB of base64, which alone exceeds ArduinoJson's 65535-byte string limit
// (ARDUINOJSON_STRING_LENGTH_SIZE=2) and would dominate every subsequent turn's
// context for as long as it stayed in the window. So a photo is analyzed ONCE on
// arrival and what enters the conversation is the resulting description - a few
// hundred bytes that summarize, compact, and re-read like any other text.
//
// The original file is kept on the SD card, so "what was in that photo I sent
// you on Tuesday" is answerable by looking again rather than by hoarding pixels
// in context.
namespace agent {
namespace vision {

// Is a provider configured that can look at an image? (OpenAI, Anthropic and
// Mistral all can; the first with a key wins, following the usual priority.)
bool available();

// Describe the image at `path`, reading through `sourceFs` (the SD filesystem;
// nullptr = LittleFS). Returns a plain-text description, or "" on any failure -
// callers fall back to acknowledging the photo without describing it, never to
// an error in the middle of a conversation.
//
// `caption` is the sender's own words, passed to the model so the description
// answers what they actually asked about ("is this rash spreading?" gets a
// different description than "what wine is this?").
//
// Cost note: the whole base64 body is assembled in PSRAM and written in ONE
// socket write. Serializing directly into the TLS client produces a TLS record
// per chunk and collapses internal heap - the lesson from the tool-loop work.
String describeImage(const char* path, const char* mime, ::fs::FS* sourceFs,
                     const String& caption);

// Bytes of image the adapter will encode. Larger images are refused rather than
// truncated (a torn JPEG describes as garbage). Matches tg::kPhotoBudget.
constexpr size_t kMaxImageBytes = 512u * 1024;

}  // namespace vision
}  // namespace agent
