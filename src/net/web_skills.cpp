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

// The JSON summary of one store op: ok, the outcome slug, and the owner's
// plain words (skillResultText). Warnings ride owner saves.
String resultJson(const std::string& id, const agent::SkillResult& res) {
  JsonDocument d;
  if (!res.ok) {
    d["error"] = res.err;
  } else {
    d["ok"] = true;
    d["outcome"] = agent::skillOutcomeSlug(res.outcome);
    d["rev"] = res.rev;
    d["message"] = agent::skillResultText(id, res);
    if (!res.warnings.empty()) {
      JsonArray w = d["warnings"].to<JsonArray>();
      for (const auto& s : res.warnings) w.add(s);
    }
  }
  String out;
  serializeJson(d, out);
  return out;
}

// One owner store op, shared by the approve/deny/discard/delete routes. A
// plain function (no capture): the handler lambdas outlive this call, so a
// captured stack local would dangle by the time a request arrives.
void ownerOp(AsyncWebServerRequest* r,
             agent::SkillResult (*op)(const std::string&, const agent::SkillCaller&)) {
  if (authBlocked(r)) return;
  const std::string id(qparam(r, "id").c_str());
  const agent::SkillResult res = op(id, agent::SkillCaller::ownerCall());
  sendJson(r, res.ok ? 200 : 400, resultJson(id, res));
}

}  // namespace

void registerSkillRoutes(AsyncWebServer& server) {
  // ---- GET /api/skills/list - built-ins + stored skills ------------------------
  server.on("/api/skills/list", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    JsonDocument d;
    d["sd"] = skills::sdAvailable();
    d["auto"] = skills::selfApproveOn();   // the D-4 switch, shown in the panel
    JsonArray arr = d["skills"].to<JsonArray>();
    for (const auto& m : skills::list()) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = m.id;
      o["title"] = m.title;
      if (!m.desc.empty()) o["desc"] = m.desc;
      if (!m.version.empty()) o["version"] = m.version;
      o["source"] = m.source;
      if (!m.builtin()) {
        o["origin"] = m.origin;
        o["inject"] = m.inject;
        o["rev"] = m.rev;
        if (!m.approved) o["pending"] = true;
        if (m.pendingEdit) {
          o["pendingEdit"] = true;
          o["nextRev"] = m.nextRev;
          if (m.nextStale) o["nextStale"] = true;
        }
      }
    }
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });

  // ---- GET /api/skills/get?id= - one skill for the editor ----------------------
  // Stored skills return the RAW SKILL.md (front matter included - that is
  // what the owner edits) plus the proposed SKILL.next.md when one waits;
  // built-ins return their body read-only.
  server.on("/api/skills/get", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string id(qparam(r, "id").c_str());
    std::string md = skills::raw(id);
    const bool stored = !md.empty();
    if (!stored) md = skills::get(id);   // builtin body (read-only in the editor)
    if (md.empty()) { sendJson(r, 404, "{\"error\":\"unknown skill id\"}"); return; }
    JsonDocument d;
    d["id"] = id;
    d["source"] = stored ? "sd" : "builtin";
    d["md"] = md;
    const std::string next = skills::rawNext(id);
    if (!next.empty()) d["next_md"] = next;
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });

  // ---- POST /api/skills/save id=&md= - the owner's save ------------------------
  // The owner's save replaces the live file (approved, the owner's, rev+1);
  // lint findings ride as warnings, never a refusal.
  server.on("/api/skills/save", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const std::string id(qparam(r, "id").c_str());
    const std::string md(qparam(r, "md").c_str());
    const agent::SkillResult res = skills::save(id, md, agent::SkillCaller::ownerCall());
    if (!res.ok) {
      const bool noSd = res.err.find("no SD") != std::string::npos;
      sendJson(r, noSd ? 507 : 400, resultJson(id, res));
      return;
    }
    sendJson(r, 200, resultJson(id, res));
  });

  // ---- POST /api/skills/approve|deny|discard|delete id= ------------------------
  server.on("/api/skills/approve", HTTP_POST, [](AsyncWebServerRequest* r) {
    ownerOp(r, skills::approve);
  });
  server.on("/api/skills/deny", HTTP_POST, [](AsyncWebServerRequest* r) {
    ownerOp(r, skills::deny);
  });
  server.on("/api/skills/discard", HTTP_POST, [](AsyncWebServerRequest* r) {
    ownerOp(r, skills::discard);
  });
  server.on("/api/skills/delete", HTTP_POST, [](AsyncWebServerRequest* r) {
    ownerOp(r, skills::remove);
  });

  // ---- POST /api/skills/auto on|off - the D-4 self-approve switch --------------
  server.on("/api/skills/auto", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const String arg = qparam(r, "on", "off");
    if (arg != "on" && arg != "off") {
      sendJson(r, 400, "{\"error\":\"send on or off\"}");
      return;
    }
    if (!skills::setSelfApprove(arg == "on")) {
      sendJson(r, 507, "{\"error\":\"the switch could not be written\"}");
      return;
    }
    sendJson(r, 200, String("{\"ok\":true,\"auto\":") + (arg == "on" ? "true" : "false") + "}");
  });
}

}  // namespace nimbus::net
