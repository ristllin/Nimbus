#include "safety_routes.h"

#include <string>

#include "webui.h"                          // nimbus::net::webAuthOk (token gate)
#include "errlog_routes.h"                  // registerDeferredWebRoute (self-registration)
#include "../agent/safety_activity_store.h" // agent::safety:: surface
#include "../agent/store.h"                 // agent::store::hasCumuloKey

namespace nimbus::net {
namespace {

bool authBlocked(AsyncWebServerRequest* r) {
  if (webAuthOk(r)) return false;
  r->send(401, "application/json",
          "{\"error\":\"Access token required. Scan the Sign-in QR on the device.\"}");
  return true;
}

std::string postParam(AsyncWebServerRequest* r, const char* name) {
  if (r->hasParam(name, true)) return std::string(r->getParam(name, true)->value().c_str());
  return std::string();
}

void handleList(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  r->send(200, "application/json",
          String(agent::safety::listJson(agent::store::hasCumuloKey()).c_str()));
}

void handleDismiss(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  const std::string id = postParam(r, "id");
  if (id.empty()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
  if (!agent::safety::dismiss(id)) { r->send(404, "application/json", "{\"error\":\"unknown entry\"}"); return; }
  r->send(200, "application/json", "{\"ok\":true}");
}

void handleApprove(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  const std::string id = postParam(r, "id");
  if (id.empty()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
  nimbus::orch::AllowScope scope;
  if (!nimbus::orch::allowScopeFromName(postParam(r, "scope"), scope)) {
    r->send(400, "application/json",
            "{\"error\":\"scope must be sender, content-class, or pattern\"}");
    return;
  }
  std::string msg;
  if (!agent::safety::approve(id, scope, msg)) {
    // Unknown id or an empty derived value (which would be a catch-all): 400/404.
    const int code = (msg == "Entry not found.") ? 404 : 400;
    JsonDocument d; d["error"] = msg;
    String body; serializeJson(d, body);
    r->send(code, "application/json", body);
    return;
  }
  JsonDocument d; d["ok"] = true; d["message"] = msg;
  String body; serializeJson(d, body);
  r->send(200, "application/json", body);
}

void handleRevoke(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  const std::string id = postParam(r, "id");
  if (id.empty()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
  if (!agent::safety::revokeAllow(id)) { r->send(404, "application/json", "{\"error\":\"unknown rule\"}"); return; }
  r->send(200, "application/json", "{\"ok\":true}");
}

void handleReport(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  const std::string id = postParam(r, "id");
  if (id.empty()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
  // The actual Cumulo POST runs on a worker task (never inline on the AsyncTCP task):
  // this only queues it. The tab polls GET /api/safety report.last for the outcome.
  std::string msg;
  const agent::safety::ReportRequest rq = agent::safety::requestReport(id, msg);
  int code;
  const char* key = "error";
  switch (rq) {
    case agent::safety::ReportRequest::Started:       code = 202; key = "message"; break;
    case agent::safety::ReportRequest::NoEntitlement: code = 403; break;   // subscription gate
    case agent::safety::ReportRequest::NotFound:      code = 404; break;
    case agent::safety::ReportRequest::Busy:
    default:                                          code = 409; break;
  }
  JsonDocument d;
  d[key] = msg;
  String body; serializeJson(d, body);
  r->send(code, "application/json", body);
}

}  // namespace

void registerSafetyRoutes(AsyncWebServer& server) {
  // More specific paths first (defensive; keeps intent explicit).
  server.on("/api/safety/allow/revoke", HTTP_POST, handleRevoke);
  server.on("/api/safety/dismiss",      HTTP_POST, handleDismiss);
  server.on("/api/safety/approve",      HTTP_POST, handleApprove);
  server.on("/api/safety/report",       HTTP_POST, handleReport);
  server.on("/api/safety",              HTTP_GET,  handleList);
}

// Self-register into the deferred-route registry at static-init time.
namespace {
struct SafetyRouteReg {
  SafetyRouteReg() { registerDeferredWebRoute(&registerSafetyRoutes); }
};
SafetyRouteReg g_safetyRouteReg;
}  // namespace

}  // namespace nimbus::net
