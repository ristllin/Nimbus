#pragma once
#include <Arduino.h>

#include <string>

class AsyncWebServerRequest;

// web_tg_archive - the Telegram archive over HTTP (owner token on every route).
//   GET  /api/tg/archive                       stats + chats
//   GET  /api/tg/archive?chat=<id>             that chat's days, newest first (paged)
//   GET  /api/tg/archive?chat=<id>&day=<d>     that day's index lines (paged)
//   GET  /api/tg/archive/item?chat=&day=&name= one record or media file (streamed)
//   POST /api/tg/archive/import?chat=&day=&name=r<hex16>.<ext>&kind=&role=&date=&note=
//                                              the restore tool's upload (raw body)
// The routes self-register; the three functions below let Memory & Files list and
// serve archived media as read-only folders named "telegram-<chat>".
namespace nimbus::net {

// Append newest archived media to a /api/files/list "files" array (a JSON array
// string, "[...]"), filtered by `project` ("" = across chats, "telegram-<chat>" =
// one chat). Nothing is added when the filter names another folder.
void tgArchiveAppendFiles(String& filesJson, const String& project);
// True for an archive entry: project "telegram-<chat>" and a name "<day>/<file>"
// (file-store names never hold '/', so this never matches a member's own file).
bool tgArchiveIsItem(const std::string& project, const std::string& name);
// Serve /api/files/dl for an archive entry. Returns false when it is not one, so the
// caller serves the file store as before.
bool tgArchiveServeFile(AsyncWebServerRequest* r, const std::string& project,
                        const std::string& name);

}  // namespace nimbus::net
