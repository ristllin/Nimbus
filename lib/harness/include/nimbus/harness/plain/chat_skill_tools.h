#pragma once
#include <ArduinoJson.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/plain/chat_skills.h"   // skillPath
#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/harness/skill_md.h"        // kSkillInjectCap
#include "nimbus/harness/skill_store.h"     // SkillStore, SkillCaller, the owner's words
#include "nimbus/orch/tool_registry.h"

// plain/chat_skill_tools.h - the skill tools on engine 2 (lane E2-P-SKILL; the owner's /skill and /prefs
// are in chat_commands.h). Engine 1's code, bound, not copied:
//   - skill_list, skill_save, skill_delete (behind more, group skills) call the shared SkillStore
//     with engine 1's approval rule (memory_subsystem.cpp skill.save / skill.delete): an admin's
//     conversation only; a new skill or an edit of an approved one waits for the owner unless his
//     "may approve its own" switch is on and the turn is attended and read no outside content; the
//     owner is told each proposal. A turn that read outside content reaches skill_save through the
//     rails' pending verdict (skillSaveHeld): the store itself is the approval shelf, so the save is
//     kept waiting, never live.
//   - The store's two lines that name engine 1's tools (skill.save, skill.get) are said in engine 2's
//     words here (plainWords), so the store's own text, engine 1's catalog, is unchanged (PC-0249).
// Every literal here is model text (gen_model_text_catalog.py E2_EXTRA_SOURCES, PC-0247 to PC-0249).
// Header only, host-tested in test_plain_chat_binding with the scripted provider.
namespace agent {
namespace plain {

struct SkillToolDeps {
  // The store's ops as the device runs them (under its storage lock).
  std::function<SkillResult(const std::string& id, const std::string& md, const SkillCaller& who)> save;
  std::function<SkillResult(const std::string& id, const SkillCaller& who)> remove;
  std::function<std::vector<SkillMeta>()> list;
  std::function<bool()> unattended;                       // no person's message started the turn
  std::function<bool()> tainted;                          // the turn read outside content
  std::function<void(const std::string& text)> tellOwner;  // null: nobody is told
};

inline std::string skillArg(ArduinoJson::JsonObjectConst a, const char* key) {
  return a[key].is<const char*>() ? std::string(a[key].as<const char*>()) : std::string();
}

// The store's lines in engine 2's words: a refusal past the size cap names skill_save, and the note
// on a long skill says how engine 2 carries it (no attached copy: whole in the pinned block, or listed).
inline std::string plainWords(const std::string& id, const std::string& line) {
  const std::string e1Save = "(skill.save ";
  const size_t at = line.find(e1Save);
  if (at != std::string::npos) return line.substr(0, at) + "(skill_save with id " + line.substr(at + e1Save.size());
  if (line.rfind("the text is ", 0) == 0 && line.find(" skill.get ") != std::string::npos)
    return "the text is longer than the " + std::to_string(kSkillInjectCap) +
           " bytes notes and skills share in every message, so it is listed and read when needed (read_file skills/" +
           id + ".md reads it all)";
  return line;
}

// skill_save as engine 1 runs skill.save; `tainted` is the turn's mark (the rails pass true).
inline nimbus::orch::ToolResult skillSaveRun(const SkillToolDeps& d, ArduinoJson::JsonObjectConst a,
                                             const nimbus::orch::Principal& who, bool tainted) {
  if (!who.perms().manageTenants) return nimbus::orch::ToolResult::fail("only an admin's conversation can author skills");
  const std::string id = skillArg(a, "id");
  const SkillCaller caller = SkillCaller::agentCall(true, d.unattended && d.unattended(), tainted);
  SkillResult r = d.save(id, skillArg(a, "md"), caller);
  if (!r.ok) return nimbus::orch::ToolResult::fail(plainWords(id, r.err));
  for (std::string& w : r.warnings) w = plainWords(id, w);
  const bool proposed = r.outcome == SkillOutcome::Created || r.outcome == SkillOutcome::Replaced ||
                        r.outcome == SkillOutcome::EditProposed;
  std::string out = proposed ? skillProposalNotice(id, r) : skillResultText(id, r);
  out += skillSaveNotes(r);
  if (!r.similar.empty())
    out += " A skill named '" + r.similar + "' already exists - if you meant to edit it, save there instead.";
  if (d.tellOwner)
    d.tellOwner(r.outcome == SkillOutcome::WentLive ? skillSelfApproveNotice(id, r.rev, r.outcome == SkillOutcome::Created)
                                                    : skillProposalNotice(id, r));
  return nimbus::orch::ToolResult::ok(out);
}

inline nimbus::orch::ToolResult skillDeleteRun(const SkillToolDeps& d, ArduinoJson::JsonObjectConst a,
                                               const nimbus::orch::Principal& who) {
  if (!who.perms().manageTenants) return nimbus::orch::ToolResult::fail("only an admin's conversation can manage skills");
  const std::string id = skillArg(a, "id");
  const SkillResult r = d.remove(id, SkillCaller::agentCall(true, d.unattended && d.unattended(), false));
  if (!r.ok) return nimbus::orch::ToolResult::fail(r.err);
  return nimbus::orch::ToolResult::ok("skill '" + id + "' deleted");
}

// PC-0247: one line per stored skill, "skills/<id>.md: <state> - <when to read it>".
inline nimbus::orch::ToolResult skillListRun(const SkillToolDeps& d) {
  std::string out;
  for (const SkillMeta& m : d.list()) {
    if (m.builtin()) continue;
    out += (out.empty() ? "" : "\n") + skillPath(m.id) + ": " +
           (!m.approved ? "waiting for the owner" : m.pendingEdit ? "approved, an edit waiting for the owner" : "approved") +
           " - " + (m.desc.empty() ? m.title : m.desc);
  }
  return nimbus::orch::ToolResult::ok(out.empty() ? std::string("no saved skills") : out);
}

inline void registerSkillTools(nimbus::orch::ToolRegistry& reg, SkillToolDeps deps) {
  const auto shared = std::make_shared<SkillToolDeps>(std::move(deps));
  auto add = [&reg](const char* name, nimbus::orch::ToolHandler h) {
    if (const auto* decl = toolDecl(name)) reg.add(name, decl->description, std::move(h), declSchema(*decl, false));
  };
  add("skill_list", [shared](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) { return skillListRun(*shared); });
  add("skill_save", [shared](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
    return skillSaveRun(*shared, a, who, shared->tainted && shared->tainted());
  });
  add("skill_delete", [shared](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
    return skillDeleteRun(*shared, a, who);
  });
}

// The rails' pending verdict for skill_save (a turn that read outside content): the save runs as a
// tainted one, so the store keeps it waiting for the owner. False: the call is not skill_save.
inline bool skillSaveHeld(const SkillToolDeps& d, const std::string& name, const std::string& argsJson,
                          const nimbus::orch::Principal& who, nimbus::orch::ToolResult& out) {
  if (name != "skill_save") return false;
  ArduinoJson::JsonDocument doc;
  if (deserializeJson(doc, argsJson) || !doc.is<ArduinoJson::JsonObject>()) {
    out = nimbus::orch::ToolResult::fail("skill_save needs id and md");
    return true;
  }
  out = skillSaveRun(d, doc.as<ArduinoJson::JsonObjectConst>(), who, true);
  return true;
}

}  // namespace plain
}  // namespace agent
