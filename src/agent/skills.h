#pragma once
#include <string>
#include <vector>

// skills - on-device knowledge/instruction capsules (P7 static + v4.0.0
// dynamic, both origins).
//
// Two sources, merged (SD wins on an id collision):
//   1. PROGMEM built-ins (architecture / hardware / tools-guide /
//      deep-research) - work with no SD and no WiFi; static playbooks the model
//      pulls via skill.get.
//   2. SD capsules at /mem/skills/<id>/SKILL.md - a front-matter header
//      (title / version / inject / created_by / created_at / approved; parser
//      is the PORTABLE agent::parseSkillMd in lib/harness) over a markdown
//      body. TWO writers since v4.0.0:
//        - the OWNER via the token-gated web CRUD (src/net/web_skills.cpp);
//        - the MODEL via the skill.save/skill.delete tools (memory_subsystem)
//          - server-stamped created_by: agent, approved: false (INERT for
//          spawn injection until the owner approves via web or Telegram
//          `/skill approve <id>`), reserved built-in ids refused, capped
//          pending queue. The model may delete ONLY agent-origin capsules;
//          the owner may delete anything.
//
// Consumption: skill.list / skill.get registry tools, the web Tools tab, and
// PER-SPAWN INJECTION: spawnCapsule(id) is wired as the JobEngine's
// resolveSkill, so a sub-agent spawned with a skill id gets that capsule
// PREPENDED to its instruction (inject: spawn|both, APPROVED capsules only).
//
// Memory discipline: SD file reads go through a PSRAM staging buffer (8 KB file
// cap; the injected capsule is further capped at 4 KB by the portable
// composer); nothing here holds steady-state internal-SRAM.
namespace agent::skills {

struct Capsule {
  std::string id;
  std::string title;
  std::string desc;      // one line for the ambient [SKILLS] index (W15)
  std::string version;   // "" for built-ins / headerless files
  std::string source;    // "builtin" | "sd"
  std::string origin;    // "builtin" | "user" | "agent"
  bool        approved = true;   // false only for pending agent capsules
};

// All capsules (metadata only - no bodies), built-ins + SD merged, SD-wins.
std::vector<Capsule> list();

// The ambient [SKILLS] index block for the per-turn dynamic context (W15):
// one "- id: desc" line per non-pending capsule, rendered by the portable
// agent::skillsIndexText (host-tested bounding). "" when no capsules exist.
std::string indexText();

// A capsule's full body by id ("" if unknown). SD front matter is stripped.
std::string get(const std::string& id);

// The body to inject at sub-agent spawn: APPROVED SD capsules whose front
// matter says inject: spawn|both. Built-ins, context-only capsules and
// PENDING agent capsules return "" (inert until the owner approves).
std::string spawnCapsule(const std::string& id);

// A built-in id (un-shadowable by AGENT saves - a shadowed tools-guide would be
// a prompt-substitution attack; the OWNER may still override via the web).
bool reservedId(const std::string& id);

// Count of agent-created capsules still awaiting approval (the pending cap).
int pendingAgentCount();

// ---- CRUD (SD-gated) --------------------------------------------------------
bool sdAvailable();                       // the SD tier is up (memory::haveSd)
// Raw SKILL.md text for the editor ("" if no SD capsule; built-ins are not
// editable and return "" here - the editor shows their body via get()).
std::string raw(const std::string& id);
// Validate id + parse + SERVER-STAMP origin (byAgent => created_by: agent,
// approved: false - ALWAYS, including re-saves, so approve-then-mutate can't
// bypass review; user saves => created_by: user, approved: true) + re-emit the
// CANONICAL front matter (composeSkillMd - forged fields never reach disk) +
// atomic write. False with err set on bad id / no SD / oversize / IO failure.
bool save(const std::string& id, const std::string& md, std::string& err,
          bool byAgent = false);
// Flip an SD capsule to approved (owner action: web or /skill approve).
bool approve(const std::string& id, std::string& err);
// byAgent deletes refuse non-agent-origin capsules; owner deletes anything.
bool remove(const std::string& id, std::string& err, bool byAgent = false);

}  // namespace agent::skills
