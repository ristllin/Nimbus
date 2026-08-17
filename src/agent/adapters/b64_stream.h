#pragma once
#include <Arduino.h>
#include <FS.h>
#include <WiFiClientSecure.h>

// b64_stream - decode a base64 JSON string value straight from a TLS socket to a
// file, never buffering the whole payload. Both the TTS reply (Mistral's
// {"audio_data":"<base64 mp3>"}) and image generation (OpenAI's
// {"data":[{"b64_json":"<base64 png>"}]}) return a big base64 field inside a
// JSON body; a full clip/image held in RAM would blow the ~266 KB internal heap.
// This is the shared streaming decoder for both.

namespace agent {

inline int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;  // '=' padding, whitespace, or the closing quote -> skip/stop
}

// Stream-decode base64 from `pre` (chars already read past the field's opening
// quote), then from the socket, writing raw bytes to `f` until the closing '"'.
// Bit-accumulator with a small output buffer so a multi-megabyte image is not
// 2M single-byte SD writes. Returns bytes written. If `clean` is non-null it is
// set true ONLY when the closing quote was reached - false means the deadline or
// a dropped socket cut the value short, so the file on disk is TRUNCATED and the
// caller must not treat it as a complete payload.
inline size_t b64decodeToFile(WiFiClientSecure& c, File& f, const String& pre,
                              uint32_t deadline, bool* clean = nullptr) {
  uint32_t acc = 0; int bits = 0; size_t out = 0; bool stop = false; bool wok = true;
  uint8_t obuf[512]; size_t on = 0;
  // Flush the output buffer and REPORT whether the filesystem accepted every byte.
  // A partial f.write (SD full, flaky card, bus contention) was silently dropped
  // before, so a truncated image landed on disk yet was reported as a complete
  // download - a corrupt file saved as success.
  auto flush = [&]() { if (on) { if ((size_t)f.write(obuf, on) != on) wok = false; on = 0; } };
  auto put = [&](char ch) -> bool {
    if (ch == '"') return true;
    int v = b64val(ch);
    if (v < 0) return false;
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if (bits >= 8) {
      bits -= 8; obuf[on++] = (acc >> bits) & 0xFF; out++;
      if (on == sizeof(obuf)) flush();
    }
    return false;
  };
  for (size_t i = 0; i < pre.length() && !stop && wok; i++) stop = put(pre[i]);
  while (!stop && wok && (int32_t)(millis() - deadline) < 0) {
    if (c.available()) stop = put((char)c.read());
    else if (!c.connected()) break;
    else vTaskDelay(1);
  }
  flush();
  // "clean" now means: reached the closing quote AND every decoded byte was
  // written. Either miss => the file on disk is incomplete; the caller must fail.
  if (clean) *clean = stop && wok;
  return out;
}

// Stream-decode base64 from `pre` then the socket into `buf` (capacity `cap`),
// until the closing '"'. Unlike b64decodeToFile this touches NO filesystem - image
// generation decodes into a PSRAM buffer during the long TLS read, because the SD
// bus cannot be held for a whole ~1 MB download (it would either stall the main
// loop's watchdog under the memory Lock, or race other SD users without it). The
// caller writes the finished buffer to SD afterward, under a brief lock. Sets
// `clean` true only when the closing quote was reached AND the data fit in `cap`.
// Returns bytes decoded (may exceed `cap` - check `clean`).
inline size_t b64decodeToBuffer(WiFiClientSecure& c, uint8_t* buf, size_t cap,
                                const String& pre, uint32_t deadline, bool& clean) {
  uint32_t acc = 0; int bits = 0; size_t out = 0; bool stop = false; bool over = false;
  auto put = [&](char ch) -> bool {
    if (ch == '"') return true;
    int v = b64val(ch);
    if (v < 0) return false;
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (out < cap) buf[out] = (acc >> bits) & 0xFF; else over = true;
      out++;
    }
    return false;
  };
  for (size_t i = 0; i < pre.length() && !stop && !over; i++) stop = put(pre[i]);
  while (!stop && !over && (int32_t)(millis() - deadline) < 0) {
    if (c.available()) stop = put((char)c.read());
    else if (!c.connected()) break;
    else vTaskDelay(1);
  }
  clean = stop && !over;   // reached the closing quote AND fit within cap
  return out;
}

}  // namespace agent
