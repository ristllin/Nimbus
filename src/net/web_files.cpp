#include "web_files.h"

#include <memory>
#include <string>

#include "../agent/files_subsystem.h"
#include "../agent/memory_subsystem.h"   // memory::Lock, dataFs()
#include "webui.h"                        // webAuthOk() - token gate

namespace nimbus::net {

namespace {

namespace files = agent::files;

void sendJson(AsyncWebServerRequest* r, int code, const String& body) {
  AsyncWebServerResponse* res = r->beginResponse(code, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

bool authBlocked(AsyncWebServerRequest* r) {
  if (webAuthOk(r)) return false;
  sendJson(r, 401, "{\"error\":\"Access token required.\"}");
  return true;
}

String qparam(AsyncWebServerRequest* r, const char* name, const char* def = "") {
  if (r->hasParam(name)) return r->getParam(name)->value();
  if (r->hasParam(name, true)) return r->getParam(name, true)->value();
  return def;
}

// Upload session state, stashed in request->_tempObject. MUST be malloc'd POD:
// the framework free()s a non-null _tempObject when the request dies, which also
// covers the abort-mid-upload path (onDisconnect cleans the SD side).
struct UpState {
  bool refused;
  bool finished;
  bool ok;
  char reason[96];
};

void refuse(UpState* st, const std::string& why) {
  if (st->refused) return;
  st->refused = true;
  strncpy(st->reason, why.c_str(), sizeof(st->reason) - 1);
  st->reason[sizeof(st->reason) - 1] = 0;
}

const char* contentTypeFor(const String& name) {
  String n = name; n.toLowerCase();
  if (n.endsWith(".md") || n.endsWith(".txt") || n.endsWith(".log")) return "text/plain";
  if (n.endsWith(".html")) return "text/html";
  if (n.endsWith(".csv"))  return "text/csv";
  if (n.endsWith(".json")) return "application/json";
  if (n.endsWith(".pdf"))  return "application/pdf";
  if (n.endsWith(".png"))  return "image/png";
  if (n.endsWith(".jpg") || n.endsWith(".jpeg")) return "image/jpeg";
  if (n.endsWith(".gif"))  return "image/gif";
  if (n.endsWith(".webp")) return "image/webp";
  if (n.endsWith(".ogg") || n.endsWith(".opus")) return "audio/ogg";
  if (n.endsWith(".mp3"))  return "audio/mpeg";
  if (n.endsWith(".wav"))  return "audio/wav";
  return "application/octet-stream";
}

// May this file be shown INSIDE the web UI's own origin?
//
// This is a security question, not a convenience one. Files arrive from anyone
// the device talks to - a Telegram member, an upload, the assistant itself - and
// the web UI keeps the device access token in localStorage. Serving an uploaded
// .html or .svg inline would run its script on this origin, with the token in
// reach: stored XSS straight to full device control. So the viewer renders only
// what cannot execute - bitmap images and plain text - and everything else, SVG
// and HTML explicitly included, keeps the download disposition it has today.
//
// PDFs are excluded too: the browser's viewer is a scriptable surface, and there
// is no reason to take that risk for a preview.
bool inlineViewable(const char* mime) {
  if (!mime) return false;
  return !strcmp(mime, "image/png")  || !strcmp(mime, "image/jpeg") ||
         !strcmp(mime, "image/gif")  || !strcmp(mime, "image/webp") ||
         !strcmp(mime, "text/plain") || !strcmp(mime, "text/csv");
}

}  // namespace

void registerFileRoutes(AsyncWebServer& server) {
  // ---- GET /api/files/list[?project=] - index listing --------------------------
  // NOT "/api/files": ESPAsyncWebServer prefix-matches (url.startsWith(uri+"/")), so
  // a "/api/files" GET handler would SWALLOW "/api/files/dl" (also GET) and return the
  // listing JSON instead of the file (caught in device smoke). All four routes are
  // now distinct siblings under /api/files/ - none is a prefix of another.
  server.on("/api/files/list", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    files::StorageTruth t = files::storageTruth();
    if (!t.present) {
      // Absent card, or a mounted card too small to support the store.
      sendJson(r, 200, t.unsupported
                           ? "{\"present\":false,\"unsupported\":true,\"files\":[]}"
                           : "{\"present\":false,\"files\":[]}");
      return;
    }
    // CUM-7: the four distinct truths so a client never reconciles two payloads:
    // count (files), bytes (used), quota (card - 512 MB reserve), cardFree
    // (free-on-card). freeBytes stays = quota headroom for back-compat.
    const uint64_t headroom = t.quota > t.used ? t.quota - t.used : 0;
    String body = "{\"present\":true,\"unsupported\":" + String(t.unsupported ? "true" : "false") +
                  ",\"count\":" + String(t.files) +
                  ",\"bytes\":" + String((unsigned long long)t.used) +
                  ",\"quota\":" + String((unsigned long long)t.quota) +
                  ",\"cardTotal\":" + String((unsigned long long)t.cardTotal) +
                  ",\"cardFree\":" + String((unsigned long long)t.cardFree) +
                  ",\"freeBytes\":" + String((unsigned long long)headroom) +
                  ",\"files\":" + files::listJson(qparam(r, "project")) + "}";
    sendJson(r, 200, body);
  });

  // ---- POST /api/files/upload?project=&name= - multipart streamed to SD ----------
  server.on(
      "/api/files/upload", HTTP_POST,
      // onRequest runs AFTER the upload completes (or failed): report the verdict.
      [](AsyncWebServerRequest* r) {
        UpState* st = static_cast<UpState*>(r->_tempObject);
        if (!st) { sendJson(r, 400, "{\"error\":\"no file part (multipart form-data required)\"}"); return; }
        if (st->refused) {
          const bool auth = strncmp(st->reason, "auth", 4) == 0;
          const bool busy = strstr(st->reason, "in progress") != nullptr;
          String body = String("{\"error\":\"") + st->reason + "\"}";
          sendJson(r, auth ? 401 : (busy ? 429 : 400), body);
          return;
        }
        if (!st->finished || !st->ok) { sendJson(r, 500, "{\"error\":\"upload incomplete\"}"); return; }
        sendJson(r, 200, "{\"ok\":true}");
        // st is free()d by the framework with the request.
      },
      // onUpload: per-chunk streaming. First chunk authorizes + opens the session;
      // every chunk writes under memory::Lock (short hold, no network inside).
      [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data,
         size_t len, bool final) {
        UpState* st = static_cast<UpState*>(r->_tempObject);
        if (!st) {
          st = static_cast<UpState*>(calloc(1, sizeof(UpState)));
          r->_tempObject = st;
          if (!st) return;
          // Gate BEFORE the first byte hits the SD (the framework has already
          // buffered this chunk; nothing is written unless authorized).
          if (!webAuthOk(r)) { refuse(st, "auth required"); }
          else {
            const std::string project(qparam(r, "project", "uploads").c_str());
            String nm = qparam(r, "name");
            if (nm.length() == 0) nm = filename;          // multipart filename fallback
            const std::string name(nm.c_str());
            // contentLength includes multipart overhead - an UPPER bound, which is
            // exactly what the quota pre-check wants; the exact size re-checks at
            // finish. (A lying client is stopped by the per-chunk hard cap too.)
            std::string err;
            if (!files::beginWrite(project, name, r->contentLength(), err)) {
              refuse(st, err);
            } else {
              // Scope the disconnect-abort to THIS session's generation: a stale
              // onDisconnect from a finished upload must not abort a later one that
              // took the slot (review finding).
              const uint32_t gen = files::writeGen();
              r->onDisconnect([gen]() { files::abortWriteGen(gen); });
            }
          }
        }
        if (st->refused) return;                           // swallow remaining chunks
        if (len && !files::writeChunk(data, len)) {
          refuse(st, "SD write failed (or size cap exceeded)");
          files::abortWrite();
          return;
        }
        if (final) {
          std::string err;
          st->ok = files::finishWrite(true, err);
          st->finished = true;
          if (!st->ok) refuse(st, err);
        }
      });

  // ---- GET /api/files/dl?project=&name= - chunked download ----------------------
  server.on("/api/files/dl", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string project(qparam(r, "project").c_str());
    const String nameQ = qparam(r, "name");
    const std::string name(nameQ.c_str());
    const std::string path = files::absPath(project, name);
    if (path.empty()) { sendJson(r, 404, "{\"error\":\"not found\"}"); return; }

    // Open under the lock; stream chunk reads under the SAME lock (per chunk - SD
    // I/O is serialized by memory::Lock across every task in this firmware, and
    // the lock is never held across the network send of the chunk). The custom
    // deleter closes the File under the lock too, so the close is serialized
    // whether we reach EOF or the client disconnects mid-download (the framework
    // then destroys the response, dropping this shared_ptr's last ref).
    auto f = std::shared_ptr<File>(new File(), [](File* fp) {
      agent::memory::Lock g;
      fp->close();
      delete fp;
    });
    size_t size = 0;
    {
      agent::memory::Lock g;
      *f = agent::memory::dataFs().open(path.c_str(), FILE_READ);
      if (!*f) { sendJson(r, 500, "{\"error\":\"SD open failed\"}"); return; }
      size = f->size();
    }
    const char* ctype = contentTypeFor(nameQ);
    AsyncWebServerResponse* res = r->beginChunkedResponse(
        ctype,
        [f](uint8_t* buf, size_t maxLen, size_t) -> size_t {
          agent::memory::Lock g;
          if (!*f) return 0;
          const int n = f->read(buf, maxLen);
          if (n <= 0) { f->close(); return 0; }   // EOF/error ends the stream
          return size_t(n);
        });
    // ?inline=1 asks the browser to SHOW it rather than save it - the file
    // explorer's preview. Honored only for types that cannot execute (see
    // inlineViewable); anything else keeps the attachment disposition however
    // the request was framed, because the caller does not get to decide whether
    // a file is safe to run on this origin.
    const bool wantInline = qparam(r, "inline") == "1" && inlineViewable(ctype);
    res->addHeader("Content-Disposition",
                   (wantInline ? String("inline; filename=\"")
                               : String("attachment; filename=\"")) + nameQ + "\"");
    // Defence in depth for the preview: never let the browser second-guess the
    // type we declared, and strip the page's own privileges from whatever renders.
    res->addHeader("X-Content-Type-Options", "nosniff");
    res->addHeader("Content-Security-Policy",
                   "default-src 'none'; img-src 'self' data:; style-src 'unsafe-inline'; sandbox");
    res->addHeader("Cache-Control", "no-store");
    res->addHeader("X-File-Bytes", String((unsigned long)size));
    r->send(res);
  });

  // ---- POST /api/files/rm  project=&name= - delete -------------------------------
  server.on("/api/files/rm", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string project(qparam(r, "project").c_str());
    const std::string name(qparam(r, "name").c_str());
    if (!files::removeFile(project, name)) {
      sendJson(r, 404, "{\"error\":\"not found\"}");
      return;
    }
    sendJson(r, 200, "{\"ok\":true}");
  });

  // ---- POST /api/files/rmproject?project= - delete a whole project/folder ------
  server.on("/api/files/rmproject", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    // Server-side confirm (mirrors /api/sdreset + the memory flush): a folder-wide
    // delete shouldn't ride on the token alone from a stale/scripted caller.
    if (qparam(r, "confirm") != "DELETE") { sendJson(r, 400, "{\"error\":\"confirm phrase required\"}"); return; }
    const std::string project(qparam(r, "project").c_str());
    const int n = files::removeProject(project);
    if (n < 0) { sendJson(r, 404, "{\"error\":\"not found\"}"); return; }
    char body[48];
    snprintf(body, sizeof body, "{\"ok\":true,\"removed\":%d}", n);
    sendJson(r, 200, body);
  });
}

}  // namespace nimbus::net
