#include "provider_file_fetch.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <cctype>
#include <cstdint>

#include "../../sys/agent_log.h"     // alog / alogf
#include "../../sys/net_util.h"      // tlsSetup / tlsClose
#include "../../sys/tls_arbiter.h"   // single-TLS work slot
#include "../store.h"                // store::mistralKey
#include "../files_subsystem.h"      // binaryWriteBegin / writeChunk / finishWrite
#include "nimbus/orch/file_store.h"  // FileStore::validSegment + Limits

namespace agent {
namespace {

constexpr const char* kMistralHost = "api.mistral.ai";
constexpr const char* kOpenAIHost  = "api.openai.com";
constexpr uint16_t    kTlsPort     = 443;
// Bound the fetch: mirror the FileStore per-file cap so a runaway/hostile file
// can't stream forever, and give a slow WiFi link a generous but finite window.
constexpr size_t      kMaxFetchBytes = 8u * 1024 * 1024;   // == FileStore maxFileBytes
constexpr uint32_t    kFetchTimeoutMs = 120000;

static bool before(uint32_t deadline) { return (int32_t)(millis() - deadline) < 0; }

// Lowercased extension of a name ("chart.PDF" -> "pdf"), "" when there is none.
std::string extOf(const std::string& n) {
  const size_t dot = n.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= n.size()) return std::string();
  std::string e = n.substr(dot + 1);
  for (char& c : e) c = (char)::tolower((unsigned char)c);
  // Refuse a "dot" that is really part of a path/space run - keep it alnum only.
  for (char c : e)
    if (!std::isalnum((unsigned char)c)) return std::string();
  return e;
}

// Everything before the last '.' ("Trading Report.pdf" -> "Trading Report").
std::string stemOf(const std::string& n) {
  const size_t dot = n.find_last_of('.');
  return dot == std::string::npos ? n : n.substr(0, dot);
}

// Build a valid FileStore name segment: <stem>.<ext>, replacing any char the
// traversal gate rejects (space, '/', ':', control, ...) with '_'. `fileName`
// (the sandbox name) is authoritative for the extension; `nameHint` (the
// sub-agent's chosen name) leads the stem; `tag` guarantees a non-empty fallback.
std::string safeName(const std::string& nameHint, const std::string& fileName,
                     const std::string& tag) {
  std::string ext = extOf(fileName);
  if (ext.empty()) ext = extOf(nameHint);
  if (ext.empty()) ext = "bin";

  std::string stem = !nameHint.empty() ? stemOf(nameHint)
                                       : (!fileName.empty() ? stemOf(fileName) : std::string());
  std::string clean;
  for (char c : stem) {
    if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') clean += c;
    else clean += '_';
  }
  while (!clean.empty() && clean.front() == '.') clean.erase(clean.begin());  // no leading dot
  if (clean.empty()) clean = std::string("file_") + tag;

  const size_t maxLen = nimbus::orch::FileStore::Limits{}.maxNameLen;   // 48
  // Reserve room for ".<ext>".
  const size_t stemBudget = maxLen > ext.size() + 1 ? maxLen - ext.size() - 1 : 1;
  if (clean.size() > stemBudget) clean.resize(stemBudget);

  std::string out = clean + "." + ext;
  if (!nimbus::orch::FileStore::validSegment(out, maxLen)) {
    out = std::string("file-") + tag + "." + ext;             // last-resort, always valid
    if (out.size() > maxLen) out = std::string("file.") + ext;
  }
  return out;
}

// Streaming byte sink -> the open file session. Bounded; stops on a write error.
struct SinkCtx { size_t bytes = 0; bool writeErr = false; };
bool chunkSink(const uint8_t* d, size_t n, void* ctx) {
  SinkCtx* c = (SinkCtx*)ctx;
  if (c->bytes + n > kMaxFetchBytes) { c->writeErr = true; return false; }
  if (!agent::files::writeChunk(d, n)) { c->writeErr = true; return false; }
  c->bytes += n;
  return true;
}

// Provider-supplied ids ride the request line verbatim, so constrain them to the
// charset Mistral actually uses (UUID-ish) - a CR/LF or space smuggled through a
// parsed response must never rewrite the request that carries the Bearer key.
bool safeFileId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id)
    if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return false;
  return true;
}

// Bearer GET <host><path> streamed to `sink` - the shared download engine for
// provider-produced files (Mistral /v1/files/<id>/content, OpenAI
// /v1/containers/<cid>/files/<fid>/content - BOTH verified live 2026-08-08:
// raw bytes, 200, NO redirect; a 3xx is treated as failure). Mirrors
// src/sys/ota_update.cpp's httpsGetOnce: HTTP/1.0 + Connection: close, a small
// fixed read buffer, one wall-clock deadline. The caller holds the TLS work
// slot. `outExpected` = the Content-Length (SIZE_MAX when the server sent
// none); `outEof` = the body ended in a clean EOF (vs the deadline) - the
// caller uses both to refuse to register a TRUNCATED download as saved.
int httpsGetToSink(const char* host, const std::string& path, const std::string& key,
                   bool (*sink)(const uint8_t*, size_t, void*), void* ctx,
                   size_t* outExpected, bool* outEof) {
  if (outExpected) *outExpected = SIZE_MAX;
  if (outEof) *outEof = false;
  const uint32_t deadline = millis() + kFetchTimeoutMs;
  WiFiClientSecure client;
  tlsSetup(client);
  client.setHandshakeTimeout(12);
  client.setConnectionTimeout(15000);
  bool connected = false;
  for (int a = 0; a < 2 && !connected && before(deadline); a++) {
    if (client.connect(host, kTlsPort)) { connected = true; break; }
    tlsClose(client);
    if (a < 1) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) return 0;

  String req = String("GET ") + path.c_str() + " HTTP/1.0\r\n" +
               "Host: " + host + "\r\n" +
               "Authorization: Bearer " + key.c_str() + "\r\n" +
               "User-Agent: nimbus-fw\r\n" + "Connection: close\r\n\r\n";
  if (client.print(req) != req.length()) { tlsClose(client); return 0; }

  // Status line.
  int code = 0;
  String line;
  while (before(deadline)) {
    if (client.available()) { char c = client.read(); if (c == '\n') break; if (c != '\r') line += c; }
    else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  int sp = line.indexOf(' ');
  if (sp > 0 && (int)line.length() >= sp + 4) code = line.substring(sp + 1, sp + 4).toInt();

  // Read headers up to the blank line, capturing Content-Length - with
  // HTTP/1.0 + close it is the ONLY signal that separates a complete body from
  // a link drop mid-transfer.
  line = "";
  bool headersDone = false;
  while (!headersDone && before(deadline)) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') {
        if (line.length() == 0) { headersDone = true; }
        else {
          String low = line; low.toLowerCase();
          if (low.startsWith("content-length:") && outExpected) {
            long v = low.substring(15).toInt();
            if (v >= 0) *outExpected = (size_t)v;
          }
        }
        line = "";
      }
      else if (c != '\r') line += c;
    } else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  if (!headersDone) { tlsClose(client); return 0; }

  bool sinkOk = true;
  if (code == 200 && sink) {
    uint8_t buf[1024];
    while (before(deadline)) {
      int n = client.read(buf, sizeof(buf));
      if (n > 0) { if (!sink(buf, (size_t)n, ctx)) { sinkOk = false; break; } }
      else if (!client.connected() && !client.available()) {
        if (outEof) *outEof = true;   // server closed - body ended (vs deadline)
        break;
      }
      else delay(2);
    }
  }
  tlsClose(client);
  return sinkOk ? code : 0;
}

}  // namespace

std::string captureProviderFile(const std::string& backend, const std::string& fileId,
                                const std::string& fileName, const std::string& project,
                                const std::string& nameHint, const std::string& tag,
                                const std::string& ownerNs) {
  if (fileId.empty()) return std::string();
  if (!files::available()) return "[file FAILED: no SD card]";

  // Resolve the per-backend download endpoint. Every id segment that rides the
  // request line is charset-validated first (it carries the Bearer key).
  const char* host;
  std::string path, key;
  if (backend == "mistral") {
    if (!safeFileId(fileId)) return "[file FAILED: bad file id from provider]";
    host = kMistralHost;
    path = "/v1/files/" + fileId + "/content";
    key = std::string(store::mistralKey().c_str());
  } else if (backend == "openai") {
    // fileId = "<container_id>/<file_id>" (oaiPoll packs both halves).
    const size_t slash = fileId.find('/');
    if (slash == std::string::npos) return "[file FAILED: bad file ref from provider]";
    const std::string cid = fileId.substr(0, slash);
    const std::string fid = fileId.substr(slash + 1);
    if (!safeFileId(cid) || !safeFileId(fid))
      return "[file FAILED: bad file id from provider]";
    host = kOpenAIHost;
    path = "/v1/containers/" + cid + "/files/" + fid + "/content";
    key = std::string(store::openaiKey().c_str());
  } else {
    return std::string();   // no capture backend for this provider
  }
  if (key.empty()) return "[file FAILED: no provider key]";

  std::string prj = project;
  if (prj.empty() || !nimbus::orch::FileStore::validSegment(
                          prj, nimbus::orch::FileStore::Limits{}.maxProjectLen))
    prj = "files";   // never lose a file to a missing/invalid project tag
  const std::string name = safeName(nameHint, fileName, tag);

  std::string err;
  if (!files::binaryWriteBegin(prj, name, 0, ownerNs, err))
    return "[file FAILED: " + err + "]";

  if (!arbiter::acquireWork(10000)) {
    files::finishWrite(false, err);            // discard the .part
    return "[file FAILED: busy]";
  }
  SinkCtx sc;
  size_t expected = SIZE_MAX;
  bool eof = false;
  const int code = httpsGetToSink(host, path, key, chunkSink, &sc, &expected, &eof);
  arbiter::releaseWork();

  // A partial file registered as saved is a LIE the head then repeats to the
  // owner (prism v4.1 #9): require a clean EOF (not the deadline) AND, when the
  // server declared a Content-Length, an exact byte match.
  const bool complete = eof && (expected == SIZE_MAX || sc.bytes == expected);
  const bool ok = (code == 200 && !sc.writeErr && sc.bytes > 0 && complete);
  if (!files::finishWrite(ok, err)) {
    std::string why = ok ? err
                     : sc.writeErr ? std::string("write error / too large")
                     : (code == 200 && !complete)
                         ? ("download truncated (" + std::to_string(sc.bytes) + " of " +
                            (expected == SIZE_MAX ? std::string("?") : std::to_string(expected)) +
                            " bytes)")
                     : (code ? ("HTTP " + std::to_string(code)) : std::string("no response"));
    return "[file FAILED: " + why + "]";
  }
  alogf("files: captured %s/%s (%u B) from %s file %s", prj.c_str(), name.c_str(),
        (unsigned)sc.bytes, backend.c_str(), fileId.c_str());
  return "[file saved: " + prj + "/" + name + "]";
}

}  // namespace agent
