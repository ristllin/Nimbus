#include "fold_routes.h"

#include <ArduinoJson.h>

#include <string>

#include "errlog_routes.h"            // registerDeferredWebRoute (self-registration)
#include "webui.h"                    // nimbus::net::webAuthOk (token gate)
#include "../agent/orchestrator.h"    // stageManualFold, foldStatusJsonFor

namespace nimbus::net {
namespace {

bool authBlocked(AsyncWebServerRequest* r) {
  if (webAuthOk(r)) return false;
  r->send(401, "application/json",
          "{\"error\":\"Access token required. Scan the Sign-in QR on the device.\"}");
  return true;
}

// The chat a request names: a form field on POST, a query parameter on GET.
std::string chatParam(AsyncWebServerRequest* r, bool post) {
  if (r->hasParam("chat", post)) return std::string(r->getParam("chat", post)->value().c_str());
  return std::string();
}

void sendNoChat(AsyncWebServerRequest* r) {
  r->send(404, "application/json", "{\"error\":\"No conversation by that name on this device.\"}");
}

void handleStatus(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  std::string body;
  if (!agent::orchestrator::foldStatusJsonFor(chatParam(r, false), body)) return sendNoChat(r);
  AsyncWebServerResponse* res = r->beginResponse(200, "application/json", String(body.c_str()));
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

void handleRun(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  std::string status;
  // Only a conversation the device knows (it has a fold state, or it is the owner's or
  // the web chat): a summary's notices go to that chat, never to a stranger's id.
  if (!agent::orchestrator::foldStatusJsonFor(chatParam(r, true), status)) return sendNoChat(r);
  JsonDocument st;
  const std::string chat = deserializeJson(st, status) ? std::string() : std::string(st["chat"] | "");
  if (chat.empty()) {
    r->send(500, "application/json", "{\"error\":\"Couldn't read the summary state. Try again.\"}");
    return;
  }
  if (!agent::orchestrator::stageManualFold(chat.c_str())) {
    r->send(429, "application/json", "{\"error\":\"Other summaries are waiting. Try again in a few minutes.\"}");
    return;
  }
  JsonDocument d;
  d["ok"] = true;
  d["queued"] = true;
  d["chat"] = chat;
  String body;
  serializeJson(d, body);
  r->send(202, "application/json", body);
}

}  // namespace

void registerFoldRoutes(AsyncWebServer& server) {
  server.on("/api/fold/status", HTTP_GET, handleStatus);
  server.on("/api/fold/run", HTTP_POST, handleRun);
}

// Self-register into the deferred-route registry at static-init time.
namespace {
struct FoldRouteReg {
  FoldRouteReg() { registerDeferredWebRoute(&registerFoldRoutes); }
};
FoldRouteReg g_foldRouteReg;
}  // namespace

}  // namespace nimbus::net
