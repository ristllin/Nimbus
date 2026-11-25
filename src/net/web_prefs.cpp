#include "web_prefs.h"

#include <ArduinoJson.h>

#include <string>

#include "../agent/memory_subsystem.h"   // nowHours
#include "../agent/prefs_subsystem.h"
#include "nimbus/orch/owner_prefs.h"
#include "nimbus/orch/prefs_tools.h"
#include "webui.h"   // webAuthOk() - token gate
#include "web_admission.h"   // the card-busy answer (web_guard.h)

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

PrefsPageRows s_rows;   // set once at boot, before the server starts

String qparam(AsyncWebServerRequest* r, const char* name, const char* def = "") {
  if (r->hasParam(name)) return r->getParam(name)->value();
  if (r->hasParam(name, true)) return r->getParam(name, true)->value();
  return def;
}

// GET's body with the engine's rows added to its items.
std::string withRows(const std::string& body) {
  if (!s_rows.add) return body;
  JsonDocument d;
  if (deserializeJson(d, body)) return body;
  JsonArray items = d["items"].as<JsonArray>();
  if (items.isNull()) items = d["items"].to<JsonArray>();
  s_rows.add(items);
  std::string out;
  serializeJson(d, out);
  return out;
}

// A refused action: the store's words and the rule's id.
void sendPrefErr(AsyncWebServerRequest* r, orch::PrefErr e, const orch::WebActionOut& res) {
  JsonDocument d;
  d["error"] = orch::prefErrWords(e);
  if (!res.id.empty()) d["id"] = res.id;
  String out;
  serializeJson(d, out);
  sendJson(r, orch::prefErrHttpStatus(e), out);
}

// A POST on one of the engine's rows: answered here, true.
bool answeredByRows(AsyncWebServerRequest* r, const std::string& action, const std::string& id) {
  std::string message;
  if (!s_rows.act || !s_rows.act(action, id, message)) return false;
  JsonDocument d;
  d["ok"] = true;
  d["message"] = message;
  String out;
  serializeJson(d, out);
  sendJson(r, 200, out);
  return true;
}

}  // namespace

void setPrefsPageRows(PrefsPageRows rows) { s_rows = std::move(rows); }

void registerPrefsRoutes(AsyncWebServer& server) {
  server.on("/api/prefs", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    agent::memory::WebLock card;   // the card: tried, never waited for (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
    std::string body;
    const uint32_t nowH = agent::memory::nowHours();   // each rule's age (lane CTX)
    const bool ran = prefs::withStore([&](orch::OwnerPrefs& p) { body = orch::prefsJson(p, nowH); });
    if (!ran) { sendJson(r, 507, "{\"error\":\"The rules file area is not available.\"}"); return; }
    sendJson(r, 200, String(withRows(body).c_str()));
  });

  server.on("/api/prefs", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    agent::memory::WebLock card;   // the card: tried, never waited for (web_guard.h)
    if (!card) return net::admission::sendCardBusy(r);
    const std::string action(qparam(r, "action").c_str());
    const std::string id(qparam(r, "id").c_str());
    const std::string text(qparam(r, "text").c_str());
    if (answeredByRows(r, action, id)) return;
    orch::PrefErr e = orch::PrefErr::PersistFailed;
    orch::WebActionOut res;
    bool ran = false;
    if (prefs::withStore([&](orch::OwnerPrefs& p) {
          ran = true;
          e = orch::prefsWebAction(p, action, id, text, agent::memory::nowHours(), &res);
        }))
      ;
    if (!ran) { sendJson(r, 507, "{\"error\":\"The rules file area is not available.\"}"); return; }
    if (e != orch::PrefErr::Ok) return sendPrefErr(r, e, res);
    JsonDocument d;
    d["ok"] = true;
    if (!res.id.empty()) d["id"] = res.id;
    if (action == "add" || action == "import") d["message"] = "Saved.";
    // Lane POL2: a full list made room; the page says which rule retired.
    if (!res.evicted.empty()) {
      d["message"] = "Saved. The list was full, so an older rule retired to make room:";
      JsonArray arr = d["retired"].to<JsonArray>();
      for (const orch::PrefEntry& ev : res.evicted) arr.add(ev.text);
    }
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });
}

}  // namespace nimbus::net
