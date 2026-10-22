// The Telegram archive over HTTP: list by chat and day (paged), one record or media
// file, the restore tool's upload, and the read-only "telegram-<chat>" folders in
// Memory & Files. Every route needs the owner token. Reads go through the archive's
// card seam (memory::Lock per operation); a file streams in chunks, each read under
// the lock, never held whole in RAM.
#include "web_tg_archive.h"

#include <ESPAsyncWebServer.h>
#include <FS.h>

#include <memory>
#include <string>

#include "../agent/memory_subsystem.h"   // Lock, dataFs, haveSd
#include "../agent/tg_archive.h"         // available, statsJson, cardFs, onRestored
#include "errlog_routes.h"               // registerDeferredWebRoute
#include "nimbus/net/archive_page.h"     // the day page byte budget (prism row 7)
#include "nimbus/tg_archive_zip.h"       // Export day: one day as a streamed ZIP
#include "web_admission.h"               // the internal-SRAM gate for big responses
#include "nimbus/orch/file_store.h"      // fileKindName / fileKindForName (Memory & Files kinds)
#include "nimbus/tg_archive.h"
#include "webui.h"                       // webAuthOk

namespace nimbus::net {

namespace {

namespace arc = nimbus::tgarc;

constexpr const char* kFolderPrefix = "telegram-";
constexpr size_t kRowsDefault = 50, kRowsMax = 200;
constexpr size_t kFilesAll = 50, kFilesChat = 200;

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

std::string qp(AsyncWebServerRequest* r, const char* name) {
  if (r->hasParam(name)) return std::string(r->getParam(name)->value().c_str());
  if (r->hasParam(name, true)) return std::string(r->getParam(name, true)->value().c_str());
  return std::string();
}

size_t qnum(AsyncWebServerRequest* r, const char* name, size_t def, size_t max) {
  const std::string v = qp(r, name);
  if (v.empty()) return def;
  const long n = atol(v.c_str());
  if (n <= 0) return v == "0" ? 0 : def;
  return (size_t)n > max ? max : (size_t)n;
}

void jsonStr(String& out, const std::string& s) {
  out += '"';
  for (char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((unsigned char)c >= 0x20) out += c;
  }
  out += '"';
}

const char* contentTypeFor(const std::string& name) {
  static const char* const kMap[][2] = {
      {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".png", "image/png"},
      {".gif", "image/gif"},  {".webp", "image/webp"}, {".ogg", "audio/ogg"},
      {".oga", "audio/ogg"},  {".mp3", "audio/mpeg"},  {".m4a", "audio/mp4"},
      {".mp4", "video/mp4"},  {".webm", "video/webm"}, {".mov", "video/quicktime"},
      {".pdf", "application/pdf"}, {".txt", "text/plain"}, {".json", "application/json"},
      {".jsonl", "application/x-ndjson"},
  };
  for (const auto& kv : kMap) {
    const size_t n = strlen(kv[0]);
    if (name.size() >= n && name.compare(name.size() - n, n, kv[0]) == 0) return kv[1];
  }
  return "application/octet-stream";
}

// Shown inside the page only when it cannot run script on this origin: bitmaps and
// plain text (the records render as text). Everything else downloads.
const char* inlineType(const char* ctype) {
  static const char* const kImages[] = {"image/jpeg", "image/png", "image/gif", "image/webp"};
  for (const char* t : kImages)
    if (!strcmp(ctype, t)) return t;
  if (!strcmp(ctype, "text/plain") || !strcmp(ctype, "application/json") ||
      !strcmp(ctype, "application/x-ndjson"))
    return "text/plain";
  return nullptr;
}

// Stream one archive file. chat, day and name are validated by the caller.
void streamItem(AsyncWebServerRequest* r, const std::string& chat, const std::string& day,
                const std::string& name, bool wantInline) {
  const std::string path = arc::dayDirPath(chat, day) + "/" + name;
  auto f = std::shared_ptr<File>(new File(), [](File* fp) {
    agent::memory::Lock g;
    fp->close();
    delete fp;
  });
  size_t size = 0;
  {
    agent::memory::Lock g;
    *f = agent::memory::dataFs().open(path.c_str(), FILE_READ);
    if (!*f || f->isDirectory()) { sendJson(r, 404, "{\"error\":\"not found\"}"); return; }
    size = f->size();
  }
  const char* ctype = contentTypeFor(name);
  const char* shown = wantInline ? inlineType(ctype) : nullptr;
  // Up to 20 MB: through the internal-SRAM admission like every big response (prism,
  // wave 1 row 6): it trickles, or answers busy when memory is short.
  AwsResponseFiller fill = admission::cardFileFiller(f);
  if (!admission::admit(r, size, /*html=*/false, fill)) return;
  AsyncWebServerResponse* res = r->beginChunkedResponse(shown ? shown : ctype, fill);
  res->addHeader("Content-Disposition",
                 String(shown ? "inline" : "attachment") + "; filename=\"" + name.c_str() + "\"");
  res->addHeader("X-Content-Type-Options", "nosniff");
  res->addHeader("Content-Security-Policy",
                 "default-src 'none'; img-src 'self' data:; style-src 'unsafe-inline'; sandbox");
  res->addHeader("Cache-Control", "no-store");
  res->addHeader("X-File-Bytes", String((unsigned long)size));
  r->send(res);
}

// ---- GET /api/tg/archive -------------------------------------------------------

String chatsJson(arc::Fs& fs) {
  String out = "[";
  bool first = true;
  for (const std::string& c : arc::listChats(fs)) {
    const std::vector<std::string> days = arc::listDays(fs, c);
    if (!first) out += ',';
    first = false;
    out += "{\"chat\":";
    jsonStr(out, c);
    out += ",\"days\":" + String((unsigned)days.size()) + ",\"newest\":";
    jsonStr(out, days.empty() ? std::string() : days[0]);
    out += '}';
  }
  return out + "]";
}

String daysJson(arc::Fs& fs, const std::string& chat, size_t from, size_t limit) {
  const std::vector<std::string> days = arc::listDays(fs, chat);
  String out = "{\"chat\":";
  jsonStr(out, chat);
  out += ",\"days\":[";
  size_t i = from;
  for (; i < days.size() && i < from + limit; ++i) {
    if (i > from) out += ',';
    jsonStr(out, days[i]);
  }
  out += "],\"from\":" + String((unsigned)from) + ",\"next\":" + String((unsigned)i) +
         ",\"more\":" + (i < days.size() ? "true" : "false") + "}";
  return out;
}

// A day page carries only the rows that fit the small-reply budget (prism row 7);
// `next` then points at the first row left out, so paging continues from there.
String rowsJson(arc::Fs& fs, const std::string& chat, const std::string& day, size_t from,
                size_t limit) {
  const arc::RowPage pg = arc::readRows(fs, chat, day, from, limit);
  std::vector<std::string> rows;
  std::vector<size_t> sizes;
  rows.reserve(pg.rows.size());
  for (const std::string& raw : pg.rows) {
    rows.push_back(arc::escapeControls(raw));   // one JSON object per line
    sizes.push_back(rows.back().size());
  }
  const size_t fit = archiveRowsThatFit(sizes, 128 + chat.size() + day.size());
  const bool cut = fit < rows.size();
  String out = "{\"chat\":";
  jsonStr(out, chat);
  out += ",\"day\":";
  jsonStr(out, day);
  out += ",\"rows\":[";
  for (size_t i = 0; i < fit; ++i) {
    if (i) out += ',';
    out += rows[i].c_str();
  }
  const size_t next = cut ? from + fit : pg.next;
  out += "],\"from\":" + String((unsigned)from) + ",\"next\":" + String((unsigned)next) +
         ",\"more\":" + ((cut || pg.more) ? "true" : "false") + "}";
  return out;
}

void handleList(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  const String stats = agent::tgarchive::statsJson();
  if (!agent::tgarchive::available()) {
    sendJson(r, 200, String("{\"stats\":") + stats + ",\"chats\":[]}");
    return;
  }
  arc::Fs& fs = agent::tgarchive::cardFs();
  const std::string chat = qp(r, "chat"), day = qp(r, "day");
  const size_t from = qnum(r, "from", 0, 1u << 20);
  const size_t limit = qnum(r, "limit", kRowsDefault, kRowsMax);
  if (chat.empty()) {
    sendJson(r, 200, String("{\"stats\":") + stats + ",\"chats\":" + chatsJson(fs) + "}");
  } else if (!arc::validChatDir(chat) || (!day.empty() && !arc::validDay(day))) {
    sendJson(r, 400, "{\"error\":\"bad chat or day\"}");
  } else if (day.empty()) {
    sendJson(r, 200, daysJson(fs, chat, from, limit));
  } else {
    sendJson(r, 200, rowsJson(fs, chat, day, from, limit));
  }
}

// ---- GET /api/tg/archive/item --------------------------------------------------

void handleItem(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  if (!agent::tgarchive::available()) {
    sendJson(r, 503, "{\"error\":\"The archive needs the SD card.\"}");
    return;
  }
  const std::string chat = qp(r, "chat"), day = qp(r, "day"), name = qp(r, "name");
  if (!arc::validChatDir(chat) || !arc::validDay(day) || !arc::validItemName(name)) {
    sendJson(r, 400, "{\"error\":\"bad chat, day or name\"}");
    return;
  }
  streamItem(r, chat, day, name, qp(r, "inline") == "1");
}

// ---- GET /api/tg/archive/zip (Export day) ----------------------------------------

// One archived day as a ZIP, streamed (tg_archive_zip.h): no file is held in RAM,
// each read takes the card lock, and the response goes through the admission gate.
void handleZip(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  if (!agent::tgarchive::available()) {
    sendJson(r, 503, "{\"error\":\"The archive needs the SD card.\"}");
    return;
  }
  const std::string chat = qp(r, "chat"), day = qp(r, "day");
  if (!arc::validChatDir(chat) || !arc::validDay(day)) {
    sendJson(r, 400, "{\"error\":\"bad chat or day\"}");
    return;
  }
  auto zip = std::make_shared<arc::DayZip>(agent::tgarchive::cardFs(), chat, day);
  if (zip->fileCount() == 0) {
    sendJson(r, 404, "{\"error\":\"nothing archived that day\"}");
    return;
  }
  if (zip->tooLarge()) {
    sendJson(r, 413, "{\"error\":\"That day is too large for one ZIP. Save its files one by one.\"}");
    return;
  }
  AwsResponseFiller fill = [zip](uint8_t* buf, size_t maxLen, size_t) -> size_t {
    return zip->fill(buf, maxLen);
  };
  if (!admission::admit(r, (size_t)zip->estimatedBytes(), /*html=*/false, fill)) return;
  AsyncWebServerResponse* res = r->beginChunkedResponse("application/zip", fill);
  res->addHeader("Content-Disposition",
                 String("attachment; filename=\"") + zip->downloadName().c_str() + "\"");
  res->addHeader("X-Content-Type-Options", "nosniff");
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// ---- POST /api/tg/archive/import (the restore tool) ----------------------------

// One upload at a time; the request carries only a generation number (POD, freed
// by the framework), the open file lives here, all under memory::Lock.
File        g_impFile;
std::string g_impPart;   // the open "<file>.part", removed if the upload is cut
uint32_t    g_impGen = 0;
bool        g_impBusy = false;

struct ImportState {
  uint32_t gen;
  uint16_t code;      // the refusal's HTTP status
  bool     refused;
  bool     skipped;
  bool     done;
  uint64_t bytes;
  char     reason[72];
};

void impRefuse(ImportState* st, uint16_t code, const char* why) {
  st->refused = true;
  st->code = code;
  strncpy(st->reason, why, sizeof(st->reason) - 1);
}

// The upload's identity, read once from the query.
arc::Restored importParams(AsyncWebServerRequest* r) {
  arc::Restored item;
  item.chat = qp(r, "chat");
  item.day = qp(r, "day");
  item.file = qp(r, "name");
  item.kind = qp(r, "kind");
  item.role = qp(r, "role");
  item.note = qp(r, "note");
  item.date = atoll(qp(r, "date").c_str());
  return item;
}

bool importParamsOk(const arc::Restored& it) {
  return arc::validChatDir(it.chat) && arc::validDay(it.day) && arc::validRestoredName(it.file) &&
         it.kind.size() <= 24 && it.role.size() <= 12 && it.note.size() <= 40;
}

void impAbort(uint32_t gen) {
  agent::memory::Lock g;
  if (!g_impBusy || gen != g_impGen) return;
  g_impFile.close();
  agent::memory::dataFs().remove(g_impPart.c_str());
  g_impBusy = false;
  agent::tgarchive::noteImport(false);
}

// First chunk: authorize, validate, and open "<file>.part". A file already on the
// card is a rerun of the tool: nothing is overwritten, but its index line is handed
// over again (recorded once), so a rerun repairs a line an earlier run lost.
void impOpen(AsyncWebServerRequest* r, ImportState* st, size_t total) {
  if (!webAuthOk(r)) return impRefuse(st, 401, "auth required");
  if (!agent::tgarchive::available()) return impRefuse(st, 503, "no SD card");
  const arc::Restored it = importParams(r);
  if (!importParamsOk(it)) return impRefuse(st, 400, "bad chat, day, name, kind, role or note");
  if (total == 0 || total > arc::kMaxFileBytes) return impRefuse(st, 400, "size out of range");
  if (!agent::tgarchive::queueHasRoom()) return impRefuse(st, 429, "the device is busy, try again");
  const std::string path = arc::itemPath(it.chat, it.day, it.file);
  arc::Fs& fs = agent::tgarchive::cardFs();
  if (fs.exists(path)) {
    arc::Restored again = it;
    again.bytes = (uint64_t)(fs.size(path) > 0 ? fs.size(path) : 0);
    if (!agent::tgarchive::onRestored(again)) return impRefuse(st, 429, "the device is busy, try again");
    st->skipped = true;
    return;
  }
  agent::memory::Lock g;
  if (g_impBusy) return impRefuse(st, 429, "another import is in progress");
  if (!fs.mkdirs(arc::dayDirPath(it.chat, it.day))) return impRefuse(st, 500, "SD write failed");
  g_impPart = path + ".part";
  g_impFile = agent::memory::dataFs().open(g_impPart.c_str(), FILE_WRITE);
  if (!g_impFile) return impRefuse(st, 500, "SD write failed");
  g_impBusy = true;
  agent::tgarchive::noteImport(true);
  st->gen = ++g_impGen;
  const uint32_t gen = st->gen;
  r->onDisconnect([gen]() { impAbort(gen); });
}

void impFinish(AsyncWebServerRequest* r, ImportState* st) {
  arc::Restored it = importParams(r);
  const std::string path = arc::itemPath(it.chat, it.day, it.file);
  {
    agent::memory::Lock g;
    g_impFile.close();
    g_impBusy = false;
  }
  const bool renamed = agent::tgarchive::cardFs().rename(path + ".part", path);
  agent::tgarchive::noteImport(false);
  if (!renamed) return impRefuse(st, 500, "SD rename failed");
  it.bytes = st->bytes;
  // The file is on the card; if its index line could not be handed over, the retry
  // takes the rerun path above, which records it.
  if (!agent::tgarchive::onRestored(it)) return impRefuse(st, 429, "the device is busy, try again");
  st->done = true;
}

// One body chunk, under memory::Lock and behind the factory-erase barrier (the
// erase takes the same lock before it sets that barrier).
void impBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  ImportState* st = static_cast<ImportState*>(r->_tempObject);
  if (!st) {
    st = static_cast<ImportState*>(calloc(1, sizeof(ImportState)));
    r->_tempObject = st;
    if (!st) return;
    impOpen(r, st, total);
  }
  if (st->refused || st->skipped || index != st->bytes) return;
  bool ok;
  {
    agent::memory::Lock g;
    ok = agent::tgarchive::available() && g_impBusy && st->gen == g_impGen &&
         g_impFile.write(data, len) == len;
  }
  if (!ok) {
    impAbort(st->gen);
    return impRefuse(st, 500, "SD write failed");
  }
  st->bytes += len;
  if (st->bytes == total) impFinish(r, st);
}

void impReply(AsyncWebServerRequest* r) {
  ImportState* st = static_cast<ImportState*>(r->_tempObject);
  if (!st) {
    sendJson(r, 400, "{\"error\":\"empty body\"}");
    return;
  }
  if (st->refused) {
    sendJson(r, st->code, String("{\"error\":\"") + st->reason + "\"}");
    return;
  }
  if (st->skipped) {
    sendJson(r, 200, "{\"ok\":true,\"skipped\":true}");
    return;
  }
  if (!st->done) {
    impAbort(st->gen);   // a short body: free the slot and drop the partial file
    sendJson(r, 500, "{\"error\":\"upload incomplete\"}");
    return;
  }
  sendJson(r, 200, String("{\"ok\":true,\"bytes\":") + String((unsigned long)st->bytes) + "}");
}

void registerTgArchiveRoutes(AsyncWebServer& server) {
  // The longer paths first (ESPAsyncWebServer prefix-matches "<uri>/").
  server.on("/api/tg/archive/item", HTTP_GET, handleItem);
  server.on("/api/tg/archive/zip", HTTP_GET, handleZip);
  server.on("/api/tg/archive/import", HTTP_POST, impReply, nullptr, impBody);
  server.on("/api/tg/archive", HTTP_GET, handleList);
}

struct TgArchiveRouteReg {
  TgArchiveRouteReg() { registerDeferredWebRoute(&registerTgArchiveRoutes); }
};
TgArchiveRouteReg g_tgArchiveRouteReg;

}  // namespace

// ---- Memory & Files: archived media as read-only folders ------------------------
// An archive entry is "telegram-<chat>" plus a name "<day>/<file>". A file-store
// name can never hold a '/', so the name alone tells an archive item from a file a
// member saved in an ordinary project that happens to be called "telegram-<id>".

namespace {

std::string folderChat(const std::string& project) {
  const size_t n = strlen(kFolderPrefix);
  if (project.compare(0, n, kFolderPrefix) != 0) return std::string();
  const std::string chat = project.substr(n);
  return arc::isTelegramChat(chat) ? chat : std::string();
}

}  // namespace

bool tgArchiveIsItem(const std::string& project, const std::string& name) {
  return !folderChat(project).empty() && name.find('/') != std::string::npos;
}

void tgArchiveAppendFiles(String& filesJson, const String& project) {
  const std::string proj(project.c_str());
  if (!agent::tgarchive::available() || filesJson.length() < 2) return;
  const std::string chat = folderChat(proj);
  if (!proj.empty() && chat.empty()) return;
  const std::vector<arc::MediaRef> media =
      arc::newestMedia(agent::tgarchive::cardFs(), chat, proj.empty() ? kFilesAll : kFilesChat);
  String add;
  for (const arc::MediaRef& m : media) {
    if (!arc::isTelegramChat(m.chat)) continue;
    if (add.length()) add += ',';
    add += "{\"project\":";
    jsonStr(add, kFolderPrefix + m.chat);
    add += ",\"name\":";
    jsonStr(add, m.day + "/" + m.name);
    add += ",\"bytes\":" + String((unsigned long)m.bytes) + ",\"kind\":\"" +
           nimbus::orch::fileKindName(nimbus::orch::fileKindForName(m.name)) +
           "\",\"createdAt\":0,\"readOnly\":true}";
  }
  if (!add.length()) return;
  filesJson.remove(filesJson.length() - 1);   // the closing ']'
  if (filesJson.length() > 1) filesJson += ',';
  filesJson += add + "]";
}

bool tgArchiveServeFile(AsyncWebServerRequest* r, const std::string& project,
                        const std::string& name) {
  if (!tgArchiveIsItem(project, name)) return false;
  const size_t slash = name.find('/');
  const std::string day = name.substr(0, slash), file = name.substr(slash + 1);
  if (!agent::tgarchive::available() || !arc::validDay(day) || !arc::isMediaName(file)) {
    sendJson(r, 404, "{\"error\":\"not found\"}");
    return true;
  }
  streamItem(r, folderChat(project), day, file, r->hasParam("inline") && r->getParam("inline")->value() == "1");
  return true;
}

}  // namespace nimbus::net
