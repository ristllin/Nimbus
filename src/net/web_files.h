#pragma once
#include <ESPAsyncWebServer.h>

// web_files - the E1 artifact-store HTTP surface (docs/ROADMAP.md E1), its own
// translation unit like web_memory. Registered from beginWeb() via
// registerFileRoutes(server).
//
// Routes (ALL token-gated - file contents + names are private). Every route is a
// distinct sibling under /api/files/ - NOT "/api/files" - because ESPAsyncWebServer
// prefix-matches (url.startsWith(uri+"/")): a bare "/api/files" GET handler swallows
// "/api/files/dl" and returns the listing instead of the file.
//   GET  /api/files/list[?project=]         index listing (JSON array)
//   POST /api/files/upload?project=&name=   multipart/raw upload streamed to SD
//   GET  /api/files/dl?project=&name=       download (a card stream: one lock try per read)
//   POST /api/files/rm  project=&name=      delete file + index entry
//
// Concurrency: upload chunk writes take the card lock per chunk and a download's
// reads only try it (SD I/O is serialized by that lock everywhere in this firmware);
// the lock is never held across a network wait. One upload session at a time
// (files_subsystem enforces).
namespace nimbus::net {

void registerFileRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
