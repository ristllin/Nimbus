#include "web_prefs.h"

#include <ArduinoJson.h>

#include <string>

#include "../agent/memory_subsystem.h"   // nowHours
#include "../agent/prefs_subsystem.h"
#include "nimbus/orch/owner_prefs.h"
#include "nimbus/orch/prefs_tools.h"
#include "webui.h"   // webAuthOk() - token gate

namespace nimbus::net {

namespace {

namespace prefs = agent::prefs;

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

}  // namespace

void registerPrefsRoutes(AsyncWebServer& server) {
  server.on("/api/prefs", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    std::string body;
    const bool ran = prefs::withStore([&](orch::OwnerPrefs& p) { body = orch::prefsJson(p); });
    if (!ran) { sendJson(r, 507, "{\"error\":\"The rules file area is not available.\"}"); return; }
    sendJson(r, 200, String(body.c_str()));
  });

  server.on("/api/prefs", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string action(qparam(r, "action").c_str());
    const std::string id(qparam(r, "id").c_str());
    const std::string text(qparam(r, "text").c_str());
    orch::PrefErr e = orch::PrefErr::PersistFailed;
    std::string idOut;
    bool ran = false;
    if (prefs::withStore([&](orch::OwnerPrefs& p) {
          ran = true;
          e = orch::prefsWebAction(p, action, id, text, agent::memory::nowHours(), idOut);
        }))
      ;
    if (!ran) { sendJson(r, 507, "{\"error\":\"The rules file area is not available.\"}"); return; }
    if (e != orch::PrefErr::Ok) {
      JsonDocument d;
      d["error"] = orch::prefErrWords(e);
      if (!idOut.empty()) d["id"] = idOut;
      String out;
      serializeJson(d, out);
      sendJson(r, orch::prefErrHttpStatus(e), out);
      return;
    }
    JsonDocument d;
    d["ok"] = true;
    if (!idOut.empty()) d["id"] = idOut;
    if (action == "add" || action == "import") d["message"] = "Saved.";
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });
}

}  // namespace nimbus::net
