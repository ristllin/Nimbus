#include "web_skills.h"

#include <ArduinoJson.h>

#include <string>

#include "../agent/skills.h"
#include "webui.h"   // webAuthOk() - token gate

namespace nimbus::net {

namespace {

namespace skills = agent::skills;

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

void registerSkillRoutes(AsyncWebServer& server) {
  // ---- GET /api/skills/list - capsule metadata (builtin + SD merged) ---------
  server.on("/api/skills/list", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    JsonDocument d;
    d["sd"] = skills::sdAvailable();
    JsonArray arr = d["skills"].to<JsonArray>();
    for (const auto& c : skills::list()) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = c.id;
      o["title"] = c.title;
      if (!c.version.empty()) o["version"] = c.version;
      o["source"] = c.source;
      o["origin"] = c.origin;
      if (!c.approved) o["pending"] = true;
    }
    String out; serializeJson(d, out);
    sendJson(r, 200, out);
  });

  // ---- GET /api/skills/get?id= - one capsule for the editor ------------------
  // SD capsules return the RAW SKILL.md (front matter included - that's what the
  // owner edits); built-ins return their body read-only.
  server.on("/api/skills/get", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string id(qparam(r, "id").c_str());
    std::string md = skills::raw(id);
    const bool sd = !md.empty();
    if (!sd) md = skills::get(id);   // builtin body (read-only in the editor)
    if (md.empty()) { sendJson(r, 404, "{\"error\":\"unknown skill id\"}"); return; }
    JsonDocument d;
    d["id"] = id;
    d["source"] = sd ? "sd" : "builtin";
    d["md"] = md;
    String out; serializeJson(d, out);
    sendJson(r, 200, out);
  });

  // ---- POST /api/skills/save id=&md= - owner-only write (SD-gated) -----------
  server.on("/api/skills/save", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string id(qparam(r, "id").c_str());
    const std::string md(qparam(r, "md").c_str());
    std::string err;
    if (!skills::save(id, md, err)) {
      const bool noSd = err.find("no SD") != std::string::npos;
      sendJson(r, noSd ? 507 : 400, String("{\"error\":\"") + err.c_str() + "\"}");
      return;
    }
    sendJson(r, 200, "{\"ok\":true}");
  });

  // ---- POST /api/skills/approve id= - activate a pending agent capsule -------
  // The owner's one-click approval (web counterpart of `/skill approve <id>`).
  // Until approved, an agent-authored capsule is INERT for spawn injection.
  server.on("/api/skills/approve", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string id(qparam(r, "id").c_str());
    std::string err;
    if (!skills::approve(id, err)) {
      sendJson(r, 400, String("{\"error\":\"") + err.c_str() + "\"}");
      return;
    }
    sendJson(r, 200, "{\"ok\":true}");
  });

  // ---- POST /api/skills/delete id= - remove an SD capsule --------------------
  server.on("/api/skills/delete", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string id(qparam(r, "id").c_str());
    std::string err;
    if (!skills::remove(id, err)) {
      sendJson(r, 400, String("{\"error\":\"") + err.c_str() + "\"}");
      return;
    }
    sendJson(r, 200, "{\"ok\":true}");
  });
}

}  // namespace nimbus::net
