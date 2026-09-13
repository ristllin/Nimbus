#include "errlog_routes.h"

#include <FS.h>

#include <memory>
#include <string>

#include "webui.h"                       // nimbus::net::webAuthOk (token gate)
#include "../sys/errlog.h"               // portable policy: planRetrieval, pathFor, names
#include "../sys/errlog_fs.h"            // activeFs()/readRecent()/listJson()
#include "../agent/memory_subsystem.h"   // agent::memory::Lock (serializes card I/O)

// Retrieval surface for the durable error log. Three routes, all token-gated:
//   GET /api/errlog                 -> stream the active log file (attachment)
//   GET /api/errlog?file=nimbus.log.1 -> stream a specific rotated file (validated)
//   GET /api/errlog?list=1          -> JSON {tier, files:[{name,bytes}]}
//   GET /api/errlog/recent?bytes=N  -> the last N bytes, line-aligned (agent-read seam
//                                      over HTTP; same data Nimbus reads in-process)

namespace nimbus::net {
namespace {

bool authBlocked(AsyncWebServerRequest* r) {
  if (webAuthOk(r)) return false;
  r->send(401, "application/json",
          "{\"error\":\"Access token required. Scan the Sign-in QR on the device.\"}");
  return true;
}

std::string qparam(AsyncWebServerRequest* r, const char* name, const char* def = "") {
  if (r->hasParam(name)) return std::string(r->getParam(name)->value().c_str());
  return def;
}

// Chunk-stream a known log file straight off the card/flash - never buffers the whole
// file in RAM. Mirrors the /api/files/dl pattern: the File is opened and every chunk
// read under agent::memory::Lock, and the custom deleter closes it under the lock too,
// so a client disconnect mid-download still closes cleanly.
void streamFile(AsyncWebServerRequest* r, const std::string& name) {
  auto f = std::shared_ptr<::File>(new ::File(), [](::File* fp) {
    agent::memory::Lock g;
    fp->close();
    delete fp;
  });
  size_t size = 0;
  {
    agent::memory::Lock g;
    *f = nimbus::errlog::activeFs().open(nimbus::errlog::pathFor(name).c_str(), FILE_READ);
    if (!*f) { r->send(404, "application/json", "{\"error\":\"no such log file\"}"); return; }
    size = f->size();
  }
  AsyncWebServerResponse* res = r->beginChunkedResponse(
      "text/plain", [f](uint8_t* buf, size_t maxLen, size_t) -> size_t {
        agent::memory::Lock g;
        if (!*f) return 0;
        const int n = f->read(buf, maxLen);
        if (n <= 0) { f->close(); return 0; }
        return size_t(n);
      });
  res->addHeader("Content-Disposition",
                 String("attachment; filename=\"") + name.c_str() + "\"");
  res->addHeader("X-Content-Type-Options", "nosniff");
  res->addHeader("Cache-Control", "no-store");
  res->addHeader("X-File-Bytes", String((unsigned long)size));
  r->send(res);
}

void handleErrlog(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  const bool wantList = r->hasParam("list");
  const std::string fileParam = qparam(r, "file");
  // Validate against the ACTIVE tier's retention count, so a flash-tier device rejects
  // ?file=nimbus.log.2/.3 as a 400 bad name (those indices only exist on the SD tier).
  const size_t maxFiles = nimbus::errlog::capsFor(nimbus::errlog::onSdTier()).maxFiles;
  const nimbus::errlog::RetrievalPlan plan =
      nimbus::errlog::planRetrieval(wantList, fileParam, maxFiles);
  switch (plan.kind) {
    case nimbus::errlog::RetrievalKind::List:
      r->send(200, "application/json", String(nimbus::errlog::listJson().c_str()));
      return;
    case nimbus::errlog::RetrievalKind::Current:
      streamFile(r, nimbus::errlog::kBaseName);
      return;
    case nimbus::errlog::RetrievalKind::File:
      streamFile(r, plan.file);
      return;
    case nimbus::errlog::RetrievalKind::Reject:
    default:
      r->send(400, "application/json", "{\"error\":\"bad file name\"}");
      return;
  }
}

void handleRecent(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  long bytes = 4096;
  if (r->hasParam("bytes")) bytes = r->getParam("bytes")->value().toInt();
  if (bytes <= 0) bytes = 4096;
  r->send(200, "text/plain", String(nimbus::errlog::readRecent((size_t)bytes).c_str()));
}

}  // namespace

void registerErrlogRoutes(AsyncWebServer& server) {
  // Register the more specific path first (defensive; ESPAsyncWebServer matches exact
  // paths, but this keeps the intent explicit alongside the /api/files precedent).
  server.on("/api/errlog/recent", HTTP_GET, handleRecent);
  server.on("/api/errlog", HTTP_GET, handleErrlog);
}

// Self-register into the deferred-route registry at static-init time.
namespace {
struct ErrlogRouteReg {
  ErrlogRouteReg() { registerDeferredWebRoute(&registerErrlogRoutes); }
};
ErrlogRouteReg g_errlogRouteReg;
}  // namespace

}  // namespace nimbus::net
