#include "image_vision.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "../../sys/agent_log.h"
#include "../../sys/net_util.h"      // tlsClose, tlsSetup
#include "../../sys/tls_arbiter.h"   // single-TLS arena
#include "../store.h"

namespace agent {
namespace vision {

namespace {

// Which provider looks at the picture. All three take an image in the same
// shape (a data: URL in a user message), differing only in host, path, and
// where the model name goes - so one request builder serves all of them.
struct Provider {
  const char* host = nullptr;
  const char* path = nullptr;
  const char* model = nullptr;
  String      key;
  bool        anthropic = false;   // different body shape + auth header
};

bool resolve(Provider& p) {
  // Follow the configured priority so the device uses the key the owner
  // actually wants spent, rather than whichever we happened to check first.
  String prio = store::providerPriority();
  if (!prio.length()) prio = "openai,mistral,anthropic";
  int s = 0;
  while (s < (int)prio.length()) {
    int e = prio.indexOf(',', s); if (e < 0) e = prio.length();
    String name = prio.substring(s, e); name.trim();
    s = e + 1;
    if (name == "openai" && store::openaiKey().length()) {
      p = {"api.openai.com", "/v1/chat/completions", "gpt-4o-mini",
           store::openaiKey(), false};
      return true;
    }
    if (name == "mistral" && store::mistralKey().length()) {
      p = {"api.mistral.ai", "/v1/chat/completions", "pixtral-12b-latest",
           store::mistralKey(), false};
      return true;
    }
    if (name == "anthropic" && store::anthropicKey().length()) {
      p = {"api.anthropic.com", "/v1/messages", "claude-3-5-haiku-latest",
           store::anthropicKey(), true};
      return true;
    }
  }
  return false;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode `f` as base64 straight into `out`. Streams in 3-byte groups so the raw
// image is never resident twice.
size_t b64EncodeFile(File& f, char* out, size_t cap) {
  size_t n = 0;
  uint8_t in[3];
  while (true) {
    const int r = f.read(in, 3);
    if (r <= 0) break;
    if (n + 4 > cap) return 0;                 // caller sized it; refuse, never tear
    out[n++] = kB64[in[0] >> 2];
    out[n++] = kB64[((in[0] & 0x03) << 4) | (r > 1 ? (in[1] >> 4) : 0)];
    out[n++] = (r > 1) ? kB64[((in[1] & 0x0F) << 2) | (r > 2 ? (in[2] >> 6) : 0)] : '=';
    out[n++] = (r > 2) ? kB64[in[2] & 0x3F] : '=';
    if (r < 3) break;
  }
  return n;
}

// JSON-escape into a fixed buffer. The caption is the sender's own text and
// rides inside a hand-built body, so it must not be able to close the string.
void appendEscaped(String& dst, const String& src, size_t maxLen) {
  const size_t n = src.length() < maxLen ? src.length() : maxLen;
  for (size_t i = 0; i < n; i++) {
    const char c = src[i];
    switch (c) {
      case '"':  dst += "\\\""; break;
      case '\\': dst += "\\\\"; break;
      case '\n': dst += "\\n";  break;
      case '\r': break;
      case '\t': dst += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) break;          // drop other controls
        dst += c;
    }
  }
}

const char* kPrompt =
    "Describe this image for someone who cannot see it. Be specific and "
    "concrete: what it shows, any text that appears in it, and anything "
    "notable. Two or three sentences. If the sender asked something, answer "
    "that. Do not start with \"This image shows\".";

}  // namespace

bool available() {
  Provider p;
  return resolve(p);
}

String describeImage(const char* path, const char* mime, ::fs::FS* sourceFs,
                     const String& caption) {
  Provider prov;
  if (!path || !*path || !resolve(prov)) return String();

  ::fs::FS& fsRef = sourceFs ? *sourceFs : (::fs::FS&)LittleFS;
  File f = fsRef.open(path, FILE_READ);
  if (!f) { alogf("vision: cannot open %s", path); return String(); }
  const size_t raw = f.size();
  if (raw == 0 || raw > kMaxImageBytes) {
    f.close();
    alogf("vision: %u bytes is outside the budget", (unsigned)raw);
    return String();
  }

  // base64 is 4 bytes per 3, plus the JSON envelope. PSRAM only - this is far
  // past what internal heap can hold, and the whole point of the single write
  // below is that it never becomes per-chunk TLS records.
  const size_t b64cap = ((raw + 2) / 3) * 4 + 4;
  // PSRAM ONLY. The old fallback to the internal heap looked harmless because a
  // big image fails it cleanly - but a MODEST one succeeds, taking ~24 KB of the
  // ~30 KB internal heap moments before c.connect() needs ~50 KB contiguous for
  // the TLS handshake. Failing the description is a far better outcome than
  // OOMing the turn, so there is no fallback: no PSRAM, no vision.
  char* b64 = (char*)heap_caps_malloc(b64cap, MALLOC_CAP_SPIRAM);
  if (!b64) {
    f.close();
    alogf("vision: no PSRAM for a %u-byte image - skipping the description",
          (unsigned)raw);
    return String();
  }
  const size_t b64len = b64EncodeFile(f, b64, b64cap);
  f.close();
  if (!b64len) { free(b64); alog("vision: encode failed"); return String(); }

  // The mime type is SENDER-SUPPLIED (Telegram passes document.mime_type
  // through unvalidated) and it is concatenated into a hand-built JSON string,
  // where one double-quote would close the string and let the rest become body
  // structure. Escaping it would still admit nonsense; an allowlist is the
  // honest answer, because there are only four types worth sending.
  const char* mt = "image/jpeg";
  if (mime && mime[0]) {
    if      (!strcmp(mime, "image/png"))  mt = "image/png";
    else if (!strcmp(mime, "image/gif"))  mt = "image/gif";
    else if (!strcmp(mime, "image/webp")) mt = "image/webp";
    else if (!strcmp(mime, "image/jpeg") || !strcmp(mime, "image/jpg")) mt = "image/jpeg";
  }

  // Build the body by hand. ArduinoJson cannot hold this string at all - its
  // 65535-byte limit (ARDUINOJSON_STRING_LENGTH_SIZE=2) is smaller than even a
  // modest photo's base64 - and assembling it as one buffer is what lets the
  // whole request go out in a single write.
  String head, tail;
  head.reserve(512);
  if (prov.anthropic) {
    head = String("{\"model\":\"") + prov.model +
           "\",\"max_tokens\":512,\"messages\":[{\"role\":\"user\",\"content\":["
           "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"" +
           mt + "\",\"data\":\"";
    tail = "\"}},{\"type\":\"text\",\"text\":\"";
    appendEscaped(tail, String(kPrompt), 512);
    if (caption.length()) {
      tail += "\\n\\nThe sender wrote: ";
      appendEscaped(tail, caption, 400);
    }
    tail += "\"}]}]}";
  } else {
    head = String("{\"model\":\"") + prov.model +
           "\",\"max_tokens\":512,\"messages\":[{\"role\":\"user\",\"content\":["
           "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:" + mt + ";base64,";
    tail = "\"}},{\"type\":\"text\",\"text\":\"";
    appendEscaped(tail, String(kPrompt), 512);
    if (caption.length()) {
      tail += "\\n\\nThe sender wrote: ";
      appendEscaped(tail, caption, 400);
    }
    tail += "\"}]}]}";
  }
  const size_t bodyLen = head.length() + b64len + tail.length();

  if (!arbiter::acquireWork(15000)) { free(b64); alog("vision: arbiter busy"); return String(); }
  WiFiClientSecure c;
  tlsSetup(c);
  c.setHandshakeTimeout(12);
  // One wall clock across connect + I/O: a half-open NAT must not wedge tg_poll
  // (the F25 lesson - setTimeout is inert on this client, setConnectionTimeout
  // is what drives the socket).
  const uint32_t deadline = millis() + 45000;
  c.setConnectionTimeout(45000);
  bool connected = false;
  for (int a = 0; a < 3 && !connected && (int32_t)(millis() - deadline) < 0; a++) {
    if (c.connect(prov.host, 443)) { connected = true; break; }
    tlsClose(c);
    if (a < 2) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) {
    free(b64); arbiter::releaseWork();
    alogf("vision: connect %s failed", prov.host);
    return String();
  }

  c.printf("POST %s HTTP/1.0\r\n", prov.path);
  c.printf("Host: %s\r\n", prov.host);
  if (prov.anthropic) {
    c.printf("x-api-key: %s\r\n", prov.key.c_str());
    c.print("anthropic-version: 2023-06-01\r\n");
  } else {
    c.printf("Authorization: Bearer %s\r\n", prov.key.c_str());
  }
  c.print("Content-Type: application/json\r\n");
  c.printf("Content-Length: %u\r\n", (unsigned)bodyLen);
  c.print("Connection: close\r\n\r\n");
  // Three writes, not one per chunk: header prefix, the base64 blob, suffix.
  c.print(head);
  size_t sent = 0;
  while (sent < b64len && (int32_t)(millis() - deadline) < 0) {
    const size_t chunk = (b64len - sent) > 4096 ? 4096 : (b64len - sent);
    const size_t w = c.write((const uint8_t*)(b64 + sent), chunk);
    if (!w) break;
    sent += w;
  }
  c.print(tail);
  free(b64);
  if (sent != b64len) {
    tlsClose(c); arbiter::releaseWork();
    alog("vision: upload cut short");
    return String();
  }

  char line[512];
  auto readLine = [&](char* b, int cap) -> int {
    int i = 0;
    while ((int32_t)(millis() - deadline) < 0 && i < cap - 1) {
      if (c.available()) { char ch = c.read(); if (ch == '\n') break; if (ch != '\r') b[i++] = ch; }
      else if (!c.connected()) break;
      else vTaskDelay(1);
    }
    b[i] = 0; return i;
  };
  int status = 0;
  if (readLine(line, sizeof(line)) > 0) {
    const char* sp = strchr(line, ' ');
    if (sp) status = atoi(sp + 1);
  }
  while (readLine(line, sizeof(line)) > 0) { /* skip headers */ }

  // The reply is a short description, so a bounded read is enough.
  String body;
  body.reserve(4096);
  while ((int32_t)(millis() - deadline) < 0 && body.length() < 8192) {
    if (c.available()) body += (char)c.read();
    else if (!c.connected()) break;
    else vTaskDelay(1);
  }
  tlsClose(c);
  arbiter::releaseWork();

  if (status < 200 || status >= 300) {
    alogf("vision: HTTP %d: %.120s", status, body.c_str());
    return String();
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) { alog("vision: unparseable reply"); return String(); }
  String out;
  if (prov.anthropic) {
    for (JsonVariantConst blk : doc["content"].as<JsonArrayConst>())
      if (blk["type"] == "text") { out = blk["text"] | ""; break; }
  } else {
    out = doc["choices"][0]["message"]["content"] | "";
  }
  out.trim();
  alogf("vision: described %u bytes -> %u chars", (unsigned)raw, (unsigned)out.length());
  return out;
}

}  // namespace vision
}  // namespace agent
