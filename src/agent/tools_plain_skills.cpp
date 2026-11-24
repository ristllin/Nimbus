// tools_plain_skills.cpp - skills, /skill, /prefs and the held notes on engine 2 (lane E2-P-SKILL;
// tools_plain_skills.h). Binds engine 1's code: the shared skill store (skills.h), its owner
// commands and words, engine 1's /prefs, and the notes library's pending shelf.
#include "tools_plain_skills.h"

#include <ArduinoJson.h>

#include <mutex>
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

constexpr char kNotePrefix[] = "note-";   // a held note's id on the Rules page ("note-p1")

nimbus::orch::Principal owner() { return nimbus::orch::principalForRole("owner", nimbus::orch::Role::Admin); }

plain::SkillToolDeps s_deps;

// The Rules page's answers, run on tg_poll (the notes' only writer).
struct Answer {
  bool approve;
  std::string id;
};
std::mutex s_mu;
std::vector<Answer> s_answers;

void tellOwner(const std::string& text) {
  const String chat = orchestrator::firstAllowedChat();
  if (chat.length()) agent::telegram::send(chat, String(text.c_str()), /*block=*/false);
}

// GET /api/prefs: each held note as a pending row the page approves or removes.
void addRows(ArduinoJson::JsonArray items) {
  std::vector<notes::PendingNote> held;
  if (!notesdev::notebook().pending(owner(), held)) return;
  for (const notes::PendingNote& p : held) {
    ArduinoJson::JsonObject o = items.add<ArduinoJson::JsonObject>();
    o["id"] = std::string(kNotePrefix) + p.id;
    o["text"] = "Note " + plain::heldNoteLine(p);
    o["state"] = "pending";
    o["origin"] = "note";
  }
}

bool actOnRow(const std::string& action, const std::string& id, std::string& message) {
  if (id.rfind(kNotePrefix, 0) != 0) return false;
  if (action != "approve" && action != "remove") {
    message = "A held note can be approved or removed.";
    return true;
  }
  {
    std::lock_guard<std::mutex> g(s_mu);
    if (s_answers.size() >= 16) {
      message = "Too many answers are waiting. Try again in a moment.";
      return true;
    }
    s_answers.push_back(Answer{action == "approve", id.substr(sizeof(kNotePrefix) - 1)});
  }
  message = action == "approve" ? "Approved. It applies from the next reply." : "Removed.";
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
  std::vector<Answer> todo;
  {
    std::lock_guard<std::mutex> g(s_mu);
    todo.swap(s_answers);
  }
  for (const Answer& a : todo) {
    std::string reply;
    const PsramFirstScope psram;
    (void)plain::heldNoteAnswer(notesdev::notebook(), owner(), (a.approve ? "approve note " : "deny note ") + a.id, reply);
    alogf("e2: held note %s from the Rules page: %s", a.id.c_str(), reply.c_str());
    if (reply.rfind("Not changed", 0) == 0) tellOwner(reply);   // the page said done: a refusal is told
  }
}

}  // namespace agent::tools::skillsplain
