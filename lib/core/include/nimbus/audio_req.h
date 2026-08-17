#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Portable, host-testable builders for the Voxtral STT upload. No Arduino deps.
// The hardware glue (src/hw/audio.*) records PCM, prepends the WAV header, and
// streams [head][wav+pcm][tail] over WiFiClientSecure with the right
// Content-Type / Content-Length.
namespace core {

// 44-byte canonical PCM WAV header for the given format + payload size.
std::vector<uint8_t> wavHeader(uint32_t sampleRate, uint16_t channels,
                               uint16_t bitsPerSample, uint32_t dataBytes);

// multipart/form-data pieces for POST /v1/audio/transcriptions.
// The file body (WAV header + PCM) is streamed between `head` and `tail`.
struct Multipart {
  std::string contentType;  // value for the Content-Type header
  std::string head;         // bytes before the file content
  std::string tail;         // bytes after the file content
  size_t contentLength(size_t fileLen) const {
    return head.size() + fileLen + tail.size();
  }
};

Multipart buildSttUpload(const std::string& boundary, const std::string& model,
                         const std::string& filename, const std::string& fileMime,
                         const std::string& language = "");

// ---- HTTP response side (host-testable) -----------------------------------
// These back the STT/multipart RESPONSE read so the device and the regression
// test share ONE code path. The read used to live inline in http_multipart.cpp
// with a fixed 2048-byte cap that truncated long (up to 60 s) transcripts mid-JSON
// -> deserializeJson failed -> a silently empty transcript. Extracting it here lets
// a host test assert a >2048-byte body is read in full.

// Abstract byte source over an HTTP response body. The device adapts
// WiFiClientSecure; the host feeds a canned buffer. `read` returns the count
// (0 = nothing ready right now, <0 = error). `idle` yields when nothing is ready
// (device: vTaskDelay); `timedOut` breaks the read when the device deadline passes.
struct ByteReader {
  virtual ~ByteReader() = default;
  virtual int available() = 0;
  virtual int read(uint8_t* buf, int len) = 0;
  virtual bool connected() = 0;
  virtual void idle() {}
  virtual bool timedOut() { return false; }
};

// Read an HTTP body into `out`. If contentLen >= 0, stops at exactly that many
// bytes; otherwise reads until the source disconnects. Always bounded by `cap`
// (guards a runaway/error page). Returns the number of bytes read.
size_t readHttpBody(ByteReader& src, long contentLen, size_t cap, std::string& out);

// F26: decode a Transfer-Encoding: chunked body IN PLACE (raw chunk framing ->
// payload bytes). Our requests are HTTP/1.0 (a compliant server never chunks),
// but a non-compliant proxy/provider is one config change away - raw framing fed
// to the JSON parser yields a silently empty transcript. Tolerant of a missing
// terminal 0-chunk (connection-close truncation): everything decoded so far is
// kept. Returns false (body left UNCHANGED) if the data doesn't parse as chunked
// framing at all - the caller then treats it as a plain body, same as today.
bool dechunkHttpBody(std::string& body);

// Parse a transcription response body ({"text":"..."}) into the trimmed text.
// Returns "" on any failure. When `ok` is non-null it is set false ONLY if the JSON
// failed to parse (truncated/garbage) - distinct from valid JSON with an empty or
// absent "text" (ok=true, "" returned). Uses a real JSON decoder so \uXXXX escapes
// (accents/emoji) decode correctly.
std::string parseTranscription(const char* json, bool* ok = nullptr);

}  // namespace core
