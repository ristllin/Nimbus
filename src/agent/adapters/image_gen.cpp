#include "image_gen.h"

#include <WiFiClientSecure.h>

#include "nimbus/orch/image_gen.h"   // portable wire builder (single source of truth)
#include "../../sys/net_util.h"      // tlsSetup / tlsClose
#include "../../sys/tls_arbiter.h"   // single-TLS arena
#include "../store.h"
#include "../../sys/agent_log.h"
#include "b64_stream.h"              // shared base64 socket->file stream decoder

namespace agent {
namespace imagegen {

bool available() { return store::hasOpenaiKey(); }

uint8_t* generateToBuffer(const String& prompt, const char* model, const char* size,
                          const char* quality, size_t& outLen, String& errOut) {
  outLen = 0;
  if (prompt.length() == 0) { errOut = "empty prompt"; return nullptr; }
  const String key = store::openaiKey();
  if (key.length() == 0) { errOut = "no OpenAI key"; return nullptr; }

  const std::string body = nimbus::orch::imageGenRequestBody(
      std::string(prompt.c_str()), model ? std::string(model) : std::string(),
      size ? std::string(size) : std::string(),
      quality ? std::string(quality) : std::string());

  // The whole image decodes into PSRAM - never the ~266 KB internal heap, and never
  // the SD during the read (see the header). Allocate before the TLS so a failure
  // here costs no provider call.
  uint8_t* buf = (uint8_t*)ps_malloc(kGenMaxBytes);
  if (!buf) { errOut = "out of memory"; alog("imagegen: ps_malloc failed"); return nullptr; }

  if (!arbiter::acquireWork(10000)) { free(buf); errOut = "device busy"; alog("imagegen: arbiter busy"); return nullptr; }
  WiFiClientSecure c;
  tlsSetup(c);
  c.setHandshakeTimeout(12);
  // Generation is slow server-side (gpt-image-1 can take 30-90 s) THEN a multi-MB
  // base64 download - ride ONE generous wall clock over connect + I/O so a half-open
  // NAT can't wedge the turn. MUST run on the turn task (tg_poll), never AsyncTCP.
  const uint32_t deadline = millis() + 120000;
  c.setConnectionTimeout(120000);
  const char* host = "api.openai.com";
  bool connected = false;
  for (int a = 0; a < 3 && !connected && (int32_t)(millis() - deadline) < 0; a++) {
    if (c.connect(host, 443)) { connected = true; break; }
    tlsClose(c);
    if (a < 2) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) { free(buf); arbiter::releaseWork(); errOut = "connect failed"; alogf("imagegen: connect failed heap=%u", ESP.getFreeHeap()); return nullptr; }

  c.printf("POST /v1/images/generations HTTP/1.0\r\n");
  c.printf("Host: %s\r\n", host);
  c.printf("Authorization: Bearer %s\r\n", key.c_str());
  c.print("Content-Type: application/json\r\n");
  c.printf("Content-Length: %u\r\n", (unsigned)body.length());
  c.print("Connection: close\r\n\r\n");
  c.print(body.c_str());

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
  if (readLine(line, sizeof(line)) > 0) { const char* sp = strchr(line, ' '); if (sp) status = atoi(sp + 1); }
  while (readLine(line, sizeof(line)) > 0) { /* skip headers */ }
  if (status < 200 || status >= 300) {
    char errb[200] = {}; int n = 0;
    while ((int32_t)(millis() - deadline) < 0 && n < (int)sizeof(errb) - 1 && (c.available() || c.connected())) {
      if (c.available()) errb[n++] = c.read(); else vTaskDelay(1);
    }
    free(buf); tlsClose(c); arbiter::releaseWork();
    errOut = "HTTP " + String(status);
    alogf("imagegen: HTTP %d: %.140s", status, errb);
    return nullptr;
  }

  // Body is {"created":..,"data":[{"b64_json":"<base64 png>"}], ...} - find the key
  // TOLERANT of whitespace after the colon (compact and pretty JSON differ), then
  // decode the value into PSRAM.
  size_t total = 0;
  bool clean = false;
  String head;
  while ((int32_t)(millis() - deadline) < 0 && head.length() < 2048) {
    if (c.available()) head += (char)c.read();
    else if (!c.connected()) break;
    else { vTaskDelay(1); continue; }
    int k = head.indexOf("\"b64_json\"");
    if (k < 0) continue;
    int colon = head.indexOf(':', k + 10);
    if (colon < 0) continue;                    // need more bytes: colon not read yet
    int q = head.indexOf('"', colon + 1);       // opening quote of the value
    if (q < 0) continue;                         // need more bytes: value quote not read yet
    total = b64decodeToBuffer(c, buf, kGenMaxBytes, head.substring(q + 1), deadline, clean);
    break;
  }
  tlsClose(c);
  arbiter::releaseWork();
  if (total == 0) {
    free(buf);
    errOut = "no image data in response";
    // Log the response head so an unexpected shape is diagnosable without a reflash.
    alogf("imagegen: no b64_json; head=%.180s", head.c_str());
    return nullptr;
  }
  if (!clean) {
    free(buf);
    errOut = (total >= kGenMaxBytes) ? "image too large" : "image download truncated";
    alogf("imagegen: incomplete decoded=%u clean=0 (cap=%u)", (unsigned)total, (unsigned)kGenMaxBytes);
    return nullptr;
  }
  outLen = total;
  alogf("imagegen: decoded %u bytes (PSRAM)", (unsigned)total);
  return buf;   // caller writes it to SD under a brief lock, then free()s it
}

}  // namespace imagegen
}  // namespace agent
