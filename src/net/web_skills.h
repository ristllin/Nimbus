#pragma once
#include <ESPAsyncWebServer.h>

// web_skills - the owner's skills HTTP surface (lane I: skills the assistant
// uses and can edit), its own translation unit like web_files. Registered from
// beginWeb() via registerSkillRoutes(server).
//
// Routes (ALL token-gated; the owner is the only writer here - the assistant
// writes through the skill.save tool, which never lands an approved skill's
// live file). Every route is a distinct sibling under /api/skills/ - never a
// bare "/api/skills" GET, which would prefix-swallow "/api/skills/get"
// (ESPAsyncWebServer url.startsWith(uri+"/") - the lesson web_files learned):
//   GET  /api/skills/list            built-ins + stored: id/title/desc, source,
//                                   origin, inject, rev, approved, pendingEdit
//   GET  /api/skills/get?id=         one skill: raw SKILL.md (stored) or body
//                                   (builtin), next_md + warnings when present
//   POST /api/skills/save id=&md=    the owner's save: the live file, approved
//                                   (507 no storage; 400 with the reason)
//   POST /api/skills/approve id=     approve a waiting skill or promote an edit
//   POST /api/skills/deny id=        drop a waiting skill or a proposed edit
//   POST /api/skills/discard id=     drop only the proposed edit
//   POST /api/skills/delete id=      remove the skill (built-ins refuse)
//   POST /api/skills/auto on|off     "the assistant may approve its own skill
//                                   revisions" (default off)
//
// Concurrency: skills.cpp takes memory::Lock around every storage op; no lock
// is held here across the response.
namespace nimbus::net {

void registerSkillRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
