#include "http_multipart.h"

#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include <cstdlib>    // atol
#include <string>
#include <strings.h>  // strncasecmp

#include "nimbus/audio_req.h"        // core::ByteReader + core::readHttpBody (shared, host-tested)
#include "../../sys/net_util.h"      // tlsClose
#include "../../sys/tls_arbiter.h"   // single-TLS arena
#include "../../sys/agent_log.h"
#include "../memory_subsystem.h"   // memory::lock/unlock - serialize SD reads (lockSrc)

namespace agent {
namespace httpmp {

namespace {
const char* kBoundary = "----NimbusFormBoundary8xJ3kLpQ";

// Conditional RAII over the memory:: SD mutex. When the file part streams off the
// SD data store (dataFs()), every SD touch here MUST be serialized by memory::Lock
// like every other SD accessor in the firmware - but the lock can NEVER be held
// across the blocking TLS write, so we take it per-open / per-read / per-close only.
struct SdGuard {
  bool on;
  explicit SdGuard(bool o) : on(o) { if (on) memory::lock(); }
  ~SdGuard() { if (on) memory::unlock(); }
  SdGuard(const SdGuard&) = delete;
  SdGuard& operator=(const SdGuard&) = delete;
};

// One field part: --boundary\r\n Content-Disposition...\r\n\r\n value\r\n
// The disposition line is: 38 bytes up to+including the opening quote
//   (`Content-Disposition: form-data; name="`) + the name + 5 bytes (`"` `\r` `\n`
//   `\r` `\n`). The final count was 4, making Content-Length one byte short - a
//   strict multipart parser (OpenAI) then rejected the whole body with
//   "Could not parse multipart form" (Voxtral read to EOF and tolerated it).
//   Reproduced against the live API: CL-1 -> HTTP 400, exact CL -> HTTP 200.
size_t fieldLen(const Field& f) {
  return 2 + strlen(kBoundary) + 2                       // --boundary\r\n
       + 38 + f.name.length() + 5                        // Content-Disposition: form-data; name="N"\r\n\r\n
       + f.value.length() + 2;                           // value\r\n
}
void writeField(WiFiClientSecure& c, const Field& f) {
  c.printf("--%s\r\n", kBoundary);
  c.printf("Content-Disposition: form-data; name=\"%s\"\r\n\r\n", f.name.c_str());
  c.print(f.value);
  c.print("\r\n");
}
}  // namespace

bool post(const char* host, int port, const char* path, const String& bearer,
          const std::vector<Field>& fields, const char* fileField,
          const char* fileName, const char* fileMime, const char* filePath,
          String& respBody, String& err, fs::FS* srcFs, bool lockSrc,
          const uint8_t* filePrefix, size_t filePrefixLen) {
  fs::FS& src = srcFs ? *srcFs : LittleFS;   // nullptr = LittleFS (historical default)
  const bool haveFile = filePath && filePath[0];
  size_t fileSize = 0;
  if (haveFile) {
    SdGuard g(lockSrc);
    File f = src.open(filePath, FILE_READ);
    if (!f) { err = "file open failed"; return false; }
    fileSize = f.size();
    f.close();
  }

  // The file part header string, built once so its exact length feeds Content-Length.
  String fileHdr;
  if (haveFile) {
    fileHdr = String("--") + kBoundary + "\r\n" +
              "Content-Disposition: form-data; name=\"" + fileField +
              "\"; filename=\"" + fileName + "\"\r\n" +
              "Content-Type: " + fileMime + "\r\n\r\n";
  }

  size_t contentLen = 0;
  for (const auto& f : fields) contentLen += fieldLen(f);
  if (haveFile)   // part header + inline prefix (e.g. a WAV header) + file bytes + \r\n
    contentLen += fileHdr.length() + filePrefixLen + fileSize + 2;
  contentLen += 2 + strlen(kBoundary) + 4;                       // --boundary--\r\n

  if (!arbiter::acquireWork(10000)) { err = "tls arbiter busy"; return false; }
  WiFiClientSecure c;
  tlsSetup(c);
  c.setHandshakeTimeout(12);
  // F25: setTimeout is inert on this client - setConnectionTimeout bounds the
  // socket, and ONE wall clock covers connect + upload + response (the STT/voice
  // upload is the speak-path sibling that can wedge tg_poll on a half-open NAT).
  const uint32_t opDeadline = millis() + 60000;   // upload + server transcription + read
  c.setConnectionTimeout(45000);
  bool connected = false;
  for (int a = 0; a < 3 && !connected && (int32_t)(millis() - opDeadline) < 0; a++) {
    if (c.connect(host, port)) { connected = true; break; }
    tlsClose(c);
    if (a < 2) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) {
    arbiter::releaseWork();
    err = "connect failed";
    alogf("multipart: connect %s failed heap=%u", host, ESP.getFreeHeap());
    return false;
  }

  // Request headers.
  c.printf("POST %s HTTP/1.0\r\n", path);
  c.printf("Host: %s\r\n", host);
  if (bearer.length()) c.printf("Authorization: Bearer %s\r\n", bearer.c_str());
  c.printf("Content-Type: multipart/form-data; boundary=%s\r\n", kBoundary);
  c.printf("Content-Length: %u\r\n", (unsigned)contentLen);
  c.print("Connection: close\r\n\r\n");

  // Body: fields, then the streamed file, then the closing boundary.
  for (const auto& f : fields) writeField(c, f);
  if (haveFile) {
    c.print(fileHdr);
    if (filePrefix && filePrefixLen) c.write(filePrefix, filePrefixLen);
    // Stream the file: open/read/close each hold the SD mutex (lockSrc); the TLS
    // write happens OUTSIDE the lock so it's never held across the network.
    File f;
    { SdGuard g(lockSrc); f = src.open(filePath, FILE_READ); }
    if (f) {
      uint8_t buf[512];
      for (;;) {
        int n;
        { SdGuard g(lockSrc); if (!f.available()) break; n = f.read(buf, sizeof(buf)); }
        if (n <= 0) break;
        // F25: bound the upload - a dead socket's send would otherwise grind the
        // WHOLE file through at ~30 s/chunk. write() returns short on a stalled
        // socket; stop on a short write or once the operation deadline passes.
        if ((int)c.write(buf, n) != n || (int32_t)(millis() - opDeadline) >= 0) break;
      }
      SdGuard g(lockSrc);
      f.close();
    }
    c.print("\r\n");
  }
  c.printf("--%s--\r\n", kBoundary);

  // Response: skip status + headers, then read the body.
  // ⚠ The read window must cover the SERVER's processing time, not just transit: a
  // transcription of a long (up to 60 s) hold-to-talk clip can take many seconds
  // before the status line even arrives. 20 s was too tight for a full-length
  // capture - the status line never came, status resolved to 0 -> "HTTP 0" -> a
  // silently empty transcript. captureVoiceTurn() runs with the task WDT deleted and
  // tg_poll isn't under the 8 s WDT, so 45 s is safe; it's still bounded so a
  // host-less device can't hang the writer.
  const uint32_t deadline = millis() + 45000;   // response window (server transcription)
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
  // Skip the header block, but capture the response Content-Length so the body read
  // stops exactly when the server declares a length (else it reads until close).
  // F26: also detect Transfer-Encoding: chunked - we speak HTTP/1.0 so a compliant
  // server never chunks, but a non-compliant proxy would otherwise hand raw chunk
  // framing to the JSON parser (silently empty transcript).
  long respLen = -1;
  bool chunked = false;
  while (readLine(line, sizeof(line)) > 0) {
    if (!strncasecmp(line, "Content-Length:", 15)) respLen = atol(line + 15);
    else if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
      // Transfer-coding tokens are case-insensitive (RFC 9112) - and the whole
      // scenario is already a non-compliant proxy, so scan case-insensitively.
      for (const char* q = line + 18; *q; ++q)
        if (!strncasecmp(q, "chunked", 7)) { chunked = true; break; }
    }
  }
  // Read the body through the portable, host-tested reader - the SAME code path the
  // regression test exercises. The old 2048-byte cap truncated long transcripts
  // mid-JSON, so deserializeJson failed and the transcript came back empty - the
  // audio-input regression this fixes. The 8 KB ceiling only guards a runaway/error
  // page: a 60 s transcript is a sentence or two (≤ ~5.4 KB even if EVERY char is a
  // 6-byte \uXXXX escape), and the body is briefly double-buffered here (std::string
  // + the String copy below) against this board's tight ~25 KB resting internal heap,
  // so a bigger ceiling buys nothing but OOM risk.
  struct ClientReader : core::ByteReader {
    WiFiClientSecure& cl;
    uint32_t dl;
    ClientReader(WiFiClientSecure& c_, uint32_t d) : cl(c_), dl(d) {}
    int available() override { return cl.available(); }
    int read(uint8_t* b, int n) override { return cl.read(b, n); }
    bool connected() override { return cl.connected(); }
    void idle() override { vTaskDelay(1); }
    bool timedOut() override { return static_cast<int32_t>(millis() - dl) >= 0; }
  } reader(c, deadline);
  std::string body;
  // Chunked framing has no usable Content-Length - read to close, then decode.
  core::readHttpBody(reader, chunked ? -1 : respLen, 8192, body);
  if (chunked) core::dechunkHttpBody(body);   // false = not actually chunked; body kept as-is
  respBody = body.c_str();  // JSON body, no embedded NULs
  tlsClose(c);
  arbiter::releaseWork();

  if (status < 200 || status >= 300) {
    err = String("HTTP ") + status;
    // SECURITY: the Telegram send path is "/bot<token>/sendXxx" - never let the token
    // reach the log ring, which is served (token-gated) at GET /api/log. Redact the
    // /bot<token>/ segment before logging.
    String safe(path);
    int b = safe.indexOf("/bot");
    if (b >= 0) {
      int e = safe.indexOf('/', b + 4);
      if (e > b) safe = safe.substring(0, b + 4) + "***" + safe.substring(e);
    }
    alogf("multipart: %s -> HTTP %d: %.80s", safe.c_str(), status, respBody.c_str());
    return false;
  }
  return true;
}

}  // namespace httpmp
}  // namespace agent
