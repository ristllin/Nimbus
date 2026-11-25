// tools_plain_skills.cpp - skills, /skill, /prefs and the held notes on engine 2 (lane E2-P-SKILL;
// tools_plain_skills.h). Binds engine 1's code: the shared skill store (skills.h), its owner
// commands and words, engine 1's /prefs, and the notes library's pending shelf.
#include "tools_plain_skills.h"

#include <ArduinoJson.h>

#include <vector>

#include "../net/web_prefs.h"                     // setPrefsPageRows - the held notes on the Rules page
#include "../sys/agent_log.h"
#include "memory_subsystem.h"                     // memory::Lock - the store's lock
#include "migrate_device.h"                       // importRulesOnce - the rules into the notes now
#include "nimbus/harness/plain/chat_commands.h"      // skillCommand, prefsCommandPlain, the held notes
#include "nimbus/harness/plain/chat_skill_tools.h"
#include "nimbus/harness/turn_memory.h"           // PsramFirstScope
#include "nimbus/orch/rbac.h"
#include "notes_device.h"                         // notesdev::notebook
#include "orchestrator.h"                         // firstAllowedChat
#include "prefs_subsystem.h"                      // prefs::context - engine 1's /prefs
#include "skills.h"                               // the shared skill store
#include "telegram.h"                             // the owner's notice, as engine 1 sends it

namespace agent::tools::skillsplain {

namespace {

namespace notes = nimbus::orch::notes;

nimbus::orch::Principal owner() { return nimbus::orch::principalForRole("owner", nimbus::orch::Role::Admin); }

plain::SkillToolDeps s_deps;

plain::HeldNoteWeb s_web;   // the Rules page's rows and answers, run on tg_poll (the notes' only writer)

void tellOwner(const std::string& text) {
  const String chat = orchestrator::firstAllowedChat();
  if (chat.length()) agent::telegram::send(chat, String(text.c_str()), /*block=*/false);
}

// GET /api/prefs: each held note, the owner's and the members', as a pending row the page approves or removes.
void addRows(ArduinoJson::JsonArray items) {
  std::vector<plain::HeldMember> members;
  for (const nimbus::orch::Tenant& t : orchestrator::tenantSnapshot())
    if (t.role != nimbus::orch::Role::Admin) members.push_back({t.chatId, t.label});
  for (const plain::HeldRow& r : s_web.rows(notesdev::notebook(), owner(), members)) {
    ArduinoJson::JsonObject o = items.add<ArduinoJson::JsonObject>();
    o["id"] = r.id;
    o["text"] = r.text;
    o["state"] = "pending";
    o["origin"] = "note";
  }
}

bool actOnRow(const std::string& action, const std::string& id, std::string& message) {
  if (!plain::HeldNoteWeb::isRow(id)) return false;
  if (action != "approve" && action != "remove") {
    message = "A held note can be approved or removed.";
    return true;
  }
  message = s_web.queue(action == "approve", id)
                ? "Sent. The device applies it on its next turn; the row shows its answer."
                : "Too many answers are waiting. Try again in a moment.";
  return true;
}

}  // namespace

void registerSkills(nimbus::orch::ToolRegistry& reg, notes::OriginFn origin) {
  s_deps.save = [](const std::string& id, const std::string& md, const SkillCaller& who) {
    const PsramFirstScope psram;
    return skills::save(id, md, who);
  };
  s_deps.remove = [](const std::string& id, const SkillCaller& who) { return skills::remove(id, who); };
  s_deps.list = [] { return skills::list(); };
  s_deps.unattended = [origin] { return origin && origin() == notes::Origin::Unattended; };
  s_deps.tainted = [origin] { return origin && origin() == notes::Origin::Tainted; };
  s_deps.tellOwner = tellOwner;
  plain::registerSkillTools(reg, s_deps);
  nimbus::net::PrefsPageRows rows;
  rows.add = addRows;
  rows.act = actOnRow;
  nimbus::net::setPrefsPageRows(std::move(rows));
}

bool railHeld(const plain::TurnCtx& t, const plain::ToolCall& c, plain::Status& out) {
  if (c.name != "skill_save" || !t.who) return false;
  nimbus::orch::ToolResult r;
  (void)plain::skillSaveHeld(s_deps, c.name, c.argsJson, *t.who, r);
  out = r.success ? plain::Status::success()
                  : plain::Status::fail(nimbus::orch::tx::Err::InvalidArgument, "e2.rails.skill");
  if (!r.success) alogf("e2: a held skill_save was not kept: %s", r.error.c_str());
  return true;
}

String command(const std::string& verb, const std::string& args) {
  const PsramFirstScope psram;
  if (verb == "skill") {
    memory::Lock g;   // the store's read-modify-write, as skills.cpp holds it
    return String(plain::skillCommand(skills::store(), args, skills::selfApproveOn(), skills::setSelfApprove).c_str());
  }
  const std::string reply = plain::prefsCommandPlain(prefs::context(), notesdev::notebook(), owner(), args);
  migratedev::importRulesOnce();   // a changed rule is in the notes before the next turn
  return String(reply.c_str());
}

void pump() {
  for (const std::string& line : s_web.pump(notesdev::notebook(), owner())) {
    alogf("e2: a held note answer from the Rules page was refused: %s", line.c_str());
    tellOwner(line);   // the page showed it sent: a refusal is told too
  }
}

}  // namespace agent::tools::skillsplain
