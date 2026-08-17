#pragma once
#include <ESPAsyncWebServer.h>

// web_skills - the dynamic-skills HTTP surface (roadmap P2: owner-authored SD
// capsules), its own translation unit like web_files. Registered from
// beginWeb() via registerSkillRoutes(server).
//
// Routes (ALL token-gated; the OWNER is the only writer - the model can read
// capsules via skill.get but has NO write tool, by design until P7). Every
// route is a distinct sibling under /api/skills/ - never a bare "/api/skills"
// GET, which would prefix-swallow "/api/skills/get" (ESPAsyncWebServer
// url.startsWith(uri+"/") - the same lesson web_files learned on-device):
//   GET  /api/skills/list          list (id/title/version/source) + sd flag
//   GET  /api/skills/get?id=       one capsule: raw SKILL.md (sd) or body (builtin)
//   POST /api/skills/save id=&md=  write /mem/skills/<id>/SKILL.md (507 no SD)
//   POST /api/skills/delete id=    remove an SD capsule (built-ins refuse)
//
// Concurrency: skills.cpp takes memory::Lock around every SD op; no lock is
// held here across the response.
namespace nimbus::net {

void registerSkillRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
