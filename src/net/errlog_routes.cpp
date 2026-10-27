#include "errlog_routes.h"

#include <FS.h>

#include <memory>
#include <string>

#include "webui.h"                       // nimbus::net::webAuthOk (token gate)
#include "web_admission.h"               // card streams; big log files trickle or answer busy
#include "../sys/errlog.h"               // portable policy: planRetrieval, pathFor, names
#include "../sys/errlog_fs.h"            // activeFs()/readRecent()/listJson()
#include "../agent/memory_subsystem.h"   // agent::memory::WebLock (serializes card I/O)
#include "nimbus/sys/web_guard.h"        // kWebFillerWaitMs: a card stream's try

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

// Stream a log file off the card/flash as a card stream (web_admission.h): never held
// whole in RAM; the open (the tier's file count, the name check, the file) runs inside
// the stream, so a download that starts or meets a tool's hold waits it out.
void streamFile(AsyncWebServerRequest* r, const std::string& fileParam) {
  auto f = net::admission::cardFileHandle();
  auto open = [f, fileParam](net::admission::CardOpen& o) -> nimbus::net::CardStep {
    agent::memory::WebLock card(nimbus::sys::kWebFillerWaitMs);   // tried, never waited for
    if (!card) return o.busy();
    // Validate against the ACTIVE tier's retention count, so a flash-tier device rejects
    // ?file=nimbus.log.2/.3 as a 400 bad name (those indices only exist on the SD tier).
    const size_t maxFiles = nimbus::errlog::capsFor(nimbus::errlog::onSdTier()).maxFiles;
    const nimbus::errlog::RetrievalPlan plan = nimbus::errlog::planRetrieval(false, fileParam, maxFiles);
    if (plan.kind == nimbus::errlog::RetrievalKind::Reject)
      return o.answer(400, "application/json", "{\"error\":\"bad file name\"}");
    const std::string name = plan.kind == nimbus::errlog::RetrievalKind::File ? plan.file : nimbus::errlog::kBaseName;
    *f = nimbus::errlog::activeFs().open(nimbus::errlog::pathFor(name).c_str(), FILE_READ);
    if (!*f) return o.answer(404, "application/json", "{\"error\":\"no such log file\"}");
    const size_t size = f->size();
    o.header("Content-Disposition", String("attachment; filename=\"") + name.c_str() + "\"");
    o.header("X-File-Bytes", String((unsigned long)size));
    // Internal-SRAM admission: a big log file trickles, or answers busy while memory is short.
    return o.ready(size);
  };
  AsyncWebServerResponse* res = net::admission::cardStream(r, "text/plain", open, net::admission::cardFileFill(f));
  res->addHeader("X-Content-Type-Options", "nosniff");
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

void handleErrlog(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  if (!r->hasParam("list")) return streamFile(r, qparam(r, "file"));
  agent::memory::WebLock card;   // the list reads the card: tried (web_guard.h)
  if (!card) return net::admission::sendCardBusy(r);
  r->send(200, "application/json", String(nimbus::errlog::listJson().c_str()));
}

void handleRecent(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  long bytes = 4096;
  if (r->hasParam("bytes")) bytes = r->getParam("bytes")->value().toInt();
  if (bytes <= 0) bytes = 4096;
  agent::memory::WebLock card;   // readRecent reads the card: tried (web_guard.h)
  if (!card) return net::admission::sendCardBusy(r);
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
