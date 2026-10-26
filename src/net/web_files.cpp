#include "web_files.h"
#include "web_admission.h"   // big downloads trickle or answer busy (internal SRAM)

#include <memory>
#include <string>

#include "../agent/files_subsystem.h"
#include "../agent/memory_subsystem.h"   // memory::Lock, dataFs()
#include "../sfx/music.h"                 // CUM-40: /music player control + listing
#include "../sfx/music_store.h"           // CUM-40: streaming write into SD /music
#include "webui.h"                        // webAuthOk() - token gate
#include "web_tg_archive.h"               // the Telegram archive's read-only folders

namespace nimbus::net {

namespace {

namespace files = agent::files;

void sendJson(AsyncWebServerRequest* r, int code, const String& body) {
  AsyncWebServerResponse* res = r->beginResponse(code, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// The Telegram archive keeps everything; only the storage-pressure policy and the
// factory reset remove from it.
const char* const kArchiveReadOnly =
    "{\"error\":\"Saved Telegram messages and files can't be deleted here.\"}";

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
  uint32_t gen;      // the write session this upload opened (0: none)
  char reason[96];
};

// An upload refused because a turn's tool held the card (web_guard.h): 503 + Retry-After.
const char* const kCardBusyReason = "card busy";

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

// The first authorized chunk of a file upload opens its write session. Caller holds a
// WebLock (this one re-enters).
bool openUpload(AsyncWebServerRequest* r, const String& filename, UpState* st) {
  agent::memory::WebLock card;
  if (!card) { refuse(st, kCardBusyReason); return false; }
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
    return false;
  }
  // Scope the disconnect-abort to THIS session's generation: a stale
  // onDisconnect from a finished upload must not abort a later one that
  // took the slot (review finding).
  const uint32_t gen = files::writeGen();
  st->gen = gen;
  r->onDisconnect([gen]() { files::abortWriteGen(gen); });
  return true;
}

}  // namespace

void registerMusicRoutes(AsyncWebServer& server);   // CUM-40: SD /music routes (below)

void registerFileRoutes(AsyncWebServer& server) {
  // ---- GET /api/files/list[?project=] - index listing --------------------------
  // NOT "/api/files": ESPAsyncWebServer prefix-matches (url.startsWith(uri+"/")), so
  // a "/api/files" GET handler would SWALLOW "/api/files/dl" (also GET) and return the
  // listing JSON instead of the file (caught in device smoke). All four routes are
  // now distinct siblings under /api/files/ - none is a prefix of another.
  server.on("/api/files/list", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    agent::memory::WebLock card;   // never wait for a turn's tool (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
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
                  ",\"files\":";
    String list = files::listJson(qparam(r, "project"));
    tgArchiveAppendFiles(list, qparam(r, "project"));   // saved Telegram media, read-only
    sendJson(r, 200, body + list + "}");
  });

  // ---- POST /api/files/upload?project=&name= - multipart streamed to SD ----------
  server.on(
      "/api/files/upload", HTTP_POST,
      // onRequest runs AFTER the upload completes (or failed): report the verdict.
      [](AsyncWebServerRequest* r) {
        UpState* st = static_cast<UpState*>(r->_tempObject);
        if (!st) { sendJson(r, 400, "{\"error\":\"no file part (multipart form-data required)\"}"); return; }
        if (st->refused) {
          if (!strcmp(st->reason, kCardBusyReason)) return net::admission::sendCardBusy(r);
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
      // every chunk tries the card lock (a WebLock) for its one short write.
      [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data,
         size_t len, bool final) {
        UpState* st = static_cast<UpState*>(r->_tempObject);
        if (!st) {
          st = static_cast<UpState*>(calloc(1, sizeof(UpState)));
          r->_tempObject = st;
          if (!st) return;
          // Gate BEFORE the first byte hits the SD (the framework has already
          // buffered this chunk; nothing is written unless authorized).
          if (!webAuthOk(r)) refuse(st, "auth required");
        }
        if (st->refused) return;                           // swallow remaining chunks
        // Every chunk is card work: tried, never waited for (web_guard.h). A busy card
        // refuses the upload (503, the client retries) and its write session is dropped
        // as soon as the card is free (abortWriteGen).
        agent::memory::WebLock card;
        if (!card) {
          refuse(st, kCardBusyReason);
          if (st->gen) files::abortWriteGen(st->gen);
          return;
        }
        if (!st->gen && !openUpload(r, filename, st)) return;
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
    agent::memory::WebLock card;   // the open and its size: tried, never waited for (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
    if (tgArchiveServeFile(r, project, name)) return;   // a "telegram-<chat>" folder
    const std::string path = files::absPath(project, name);
    if (path.empty()) { sendJson(r, 404, "{\"error\":\"not found\"}"); return; }

    // Open under the lock; stream chunk reads under the SAME lock (per chunk - SD
    // I/O is serialized by memory::Lock across every task in this firmware, and
    // the lock is never held across the network send of the chunk). The filler and
    // the handle's deleter only try the lock (web_guard.h): a busy chunk is sent on
    // the server's next poll, and the handle closes whether we reach EOF or the
    // client disconnects mid-download.
    auto f = net::admission::cardFileHandle();
    *f = agent::memory::dataFs().open(path.c_str(), FILE_READ);
    if (!*f) { sendJson(r, 500, "{\"error\":\"SD open failed\"}"); return; }
    const size_t size = f->size();
    const char* ctype = contentTypeFor(nameQ);
    AwsResponseFiller fill = net::admission::cardFileFiller(f);
    // Internal-SRAM admission: a big file trickles, or waits (busy) while memory is short.
    if (!net::admission::admit(r, size, /*html=*/false, fill)) return;
    AsyncWebServerResponse* res = r->beginChunkedResponse(ctype, fill);
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
    agent::memory::WebLock card;   // never wait for a turn's tool (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
    const std::string project(qparam(r, "project").c_str());
    const std::string name(qparam(r, "name").c_str());
    if (tgArchiveIsItem(project, name)) { sendJson(r, 403, kArchiveReadOnly); return; }
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
    agent::memory::WebLock card;   // never wait for a turn's tool (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
    const std::string project(qparam(r, "project").c_str());
    const int n = files::removeProject(project);
    if (n < 0) { sendJson(r, 404, "{\"error\":\"not found\"}"); return; }
    char body[48];
    snprintf(body, sizeof body, "{\"ok\":true,\"removed\":%d}", n);
    sendJson(r, 200, body);
  });

  registerMusicRoutes(server);
}

// ---- CUM-40: the SD /music surface -------------------------------------------
// The artifact store (/api/files/*) writes /mem/files; the music player reads
// /music. These are separate trees on purpose (the player scans a raw folder, not
// the indexed store), so music gets its own small route set: upload a validated
// track INTO /music, list what is there, play/stop it, and remove one. This is the
// user path that was missing - before it, /music could only be populated over the
// serial console, so /play always reported an empty folder. Handlers are split into
// file-scope helpers so each stays under the complexity gate.
namespace {

// The final verdict once the upload finished (or refused). Mirrors the files route.
void musicUploadReply(AsyncWebServerRequest* r) {
  UpState* st = static_cast<UpState*>(r->_tempObject);
  if (!st) { sendJson(r, 400, "{\"error\":\"no file part (multipart form-data required)\"}"); return; }
  if (st->refused) {
    if (!strcmp(st->reason, kCardBusyReason)) return net::admission::sendCardBusy(r);
    const bool auth = strncmp(st->reason, "auth", 4) == 0;
    const bool busy = strstr(st->reason, "in progress") != nullptr;
    sendJson(r, auth ? 401 : (busy ? 429 : 400), String("{\"error\":\"") + st->reason + "\"}");
    return;
  }
  if (!st->finished || !st->ok) { sendJson(r, 500, "{\"error\":\"upload incomplete\"}"); return; }
  sendJson(r, 200, "{\"ok\":true}");
}

// First authorized chunk: open the streaming /music write session.
void musicUploadOpen(AsyncWebServerRequest* r, const String& filename, UpState* st) {
  agent::memory::WebLock card;   // re-enters the chunk's lock (web_guard.h)
  if (!card) { refuse(st, kCardBusyReason); return; }
  String nm = qparam(r, "name");
  if (nm.length() == 0) nm = filename;              // multipart filename fallback
  std::string err;
  if (!music::store::uploadBegin(std::string(nm.c_str()), err)) { refuse(st, err); return; }
  const uint32_t gen = music::store::uploadGen();
  st->gen = gen;
  r->onDisconnect([gen]() { music::store::uploadAbortGen(gen); });
}

// Per-chunk streaming body: open on the first chunk, stream, commit on the last.
void musicUploadChunk(AsyncWebServerRequest* r, const String& filename, size_t,
                      uint8_t* data, size_t len, bool final) {
  UpState* st = static_cast<UpState*>(r->_tempObject);
  if (!st) {
    st = static_cast<UpState*>(calloc(1, sizeof(UpState)));
    r->_tempObject = st;
    if (!st) return;
    if (!webAuthOk(r)) refuse(st, "auth required");   // before any card work
  }
  if (st->refused) return;
  agent::memory::WebLock card;   // every chunk is card work: tried (web_guard.h)
  if (!card) {
    refuse(st, kCardBusyReason);
    if (st->gen) music::store::uploadAbortGen(st->gen);
    return;
  }
  if (!st->gen) {
    musicUploadOpen(r, filename, st);
    if (st->refused) return;
  }
  if (len && !music::store::uploadChunk(data, len)) {
    refuse(st, "SD write failed (or file too large)");
    music::store::uploadAbort();
    return;
  }
  if (final) {
    std::string err;
    st->ok = music::store::uploadFinish(true, err);
    st->finished = true;
    if (!st->ok) refuse(st, err);
  }
}

// Apply a playback control verb (or a bare/`all` play-everything).
void musicApplyPlay(const String& action, const String& name) {
  agent::memory::WebLock card;   // re-enters the route's lock: play-all scans the card
  if (!card) return;
  if (action == "stop")        music::stop();
  else if (action == "pause")  music::pause();
  else if (action == "resume") music::resume();
  else if (action == "next")   music::next();
  else if (name.length())      music::playNow({std::string(name.c_str())});
  else                         music::playAll();     // bare / action=all
}

}  // namespace

void registerMusicRoutes(AsyncWebServer& server) {
  // POST /api/music/upload?name= - multipart streamed straight to SD /music.
  server.on("/api/music/upload", HTTP_POST, musicUploadReply, musicUploadChunk);

  // GET /api/music/list - tracks in /music + the current player state.
  server.on("/api/music/list", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    agent::memory::WebLock card;   // the folder scan: tried, never waited for (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
    if (!agent::memory::haveSd()) { sendJson(r, 200, "{\"present\":false,\"tracks\":[]}"); return; }
    String body = "{\"present\":true,\"tracks\":[";
    auto tracks = music::listMusicDir();
    for (size_t i = 0; i < tracks.size(); i++) { if (i) body += ","; body += "\""; body += tracks[i].c_str(); body += "\""; }
    body += "],\"player\":"; body += music::statusJson(); body += "}";
    sendJson(r, 200, body);
  });

  // POST /api/music/play  action=all|stop|pause|resume|next  OR  name=.
  server.on("/api/music/play", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    agent::memory::WebLock card;   // play-all scans the folder: tried (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
    musicApplyPlay(qparam(r, "action"), qparam(r, "name"));
    sendJson(r, 200, String("{\"ok\":true,\"player\":") + music::statusJson() + "}");
  });

  // POST /api/music/rm  name= - delete one track from /music.
  server.on("/api/music/rm", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    agent::memory::WebLock card;   // never wait for a turn's tool (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
    std::string err;
    if (!music::store::removeTrack(std::string(qparam(r, "name").c_str()), err)) {
      sendJson(r, 404, String("{\"error\":\"") + err.c_str() + "\"}");
      return;
    }
    sendJson(r, 200, "{\"ok\":true}");
  });
}

}  // namespace nimbus::net
