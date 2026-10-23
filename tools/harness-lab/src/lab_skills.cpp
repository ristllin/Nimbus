#include "lab_skills.h"

#include <cstdio>
#include <ctime>

#include "nimbus/harness/skill_store.h"

namespace lab {

// The device's clock stamps (memory_subsystem.cpp writes real times); a fixed
// stamp keeps every run byte-comparable.
std::vector<agent::AttachPick> LabSkills::picksFor(const std::string& chatId) {
  attach_.noteTurn(chatId, (uint32_t)(::time(nullptr) * 1000u));
  return agent::selectAttach(attach_.stickyFor(chatId), store_.list());
}

void LabSkills::noteUsed(const std::string& chat, const std::string& id) {
  noted.push_back({chat, id});
  attach_.noteUsed(chat, id, (uint32_t)(::time(nullptr) * 1000u));
}

void LabSkills::registerTools(nimbus::orch::ToolRegistry& reg) {
  using nimbus::orch::ToolResult;
  reg.add("skill.list",
          "List the skills (task playbooks and knowledge capsules) with their status: "
          "approved, waiting for the owner's approval, or an edit waiting on an approved one.",
          [this](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) -> ToolResult {
            JsonDocument d;
            JsonArray arr = d.to<JsonArray>();
            for (const auto& m : store_.list()) {
              JsonObject o = arr.add<JsonObject>();
              o["id"] = m.id;
              o["title"] = m.title;
              if (!m.desc.empty()) o["desc"] = m.desc;
              o["status"] = agent::skillStatusSlug(m.status());
              if (!m.builtin()) {
                o["origin"] = m.origin;
                o["inject"] = m.inject;
                o["rev"] = m.rev;
              }
            }
            std::string s;
            serializeJson(d, s);
            return ToolResult::ok(s);
          },
          R"({"type":"object","properties":{}})");
  reg.add("skill.get",
          "Read a skill - a task playbook or knowledge capsule - by id from the [SKILLS] "
          "index. Read the matching one BEFORE starting (or refusing) a task it covers. "
          "Once read it stays attached to this chat's next turns ([ACTIVE SKILLS]), so do "
          "not read it again.",
          [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> ToolResult {
            std::string id = a["id"].is<const char*>() ? std::string(a["id"].as<const char*>()) : std::string();
            bool trusted = false;
            std::string body = store_.get(id, agent::SkillSide{}, &trusted);
            if (body.empty()) return ToolResult::fail("unknown skill id (call skill_list)");
            return ToolResult::ok(body);
          },
          R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
  reg.add("skill.save",
          std::string("Save or update a skill - a task playbook stored on the device. ") +
              "id: a-z0-9-_ max 23 chars. md: the SKILL.md text. Front matter keys you may " +
              "write: " + agent::skillAuthorKeysDoc() + ". Saving a NEW skill waits for the " +
              "owner's approval (it is not usable in the current task); saving an existing " +
              "APPROVED skill proposes an EDIT - the approved version keeps running until the " +
              "owner approves it, so an edit is never a step of the current task either.",
          [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> ToolResult {
            std::string id = a["id"].is<const char*>() ? a["id"].as<const char*>() : "";
            std::string md = a["md"].is<const char*>() ? a["md"].as<const char*>() : "";
            const agent::SkillCaller who = agent::SkillCaller::agentCall(true, false, false);
            const agent::SkillResult r = store_.save(id, md, who, false);
            if (!r.ok) return ToolResult::fail(r.err);
            std::string out = (r.outcome == agent::SkillOutcome::Created ||
                               r.outcome == agent::SkillOutcome::Replaced ||
                               r.outcome == agent::SkillOutcome::EditProposed)
                                  ? agent::skillProposalNotice(id, r)
                                  : agent::skillResultText(id, r);
            if (!r.similar.empty())
              out += " A skill named '" + r.similar + "' already exists - if you meant to " +
                     "edit it, save there instead.";
            return ToolResult::ok(out);
          },
          R"({"type":"object","properties":{"id":{"type":"string"},"md":{"type":"string"}},"required":["id","md"]})");
  reg.add("skill.delete",
          "Delete a skill capsule YOU authored (created_by: agent). Owner-made "
          "skills can only be deleted by the owner (web UI or /skill delete).",
          [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> ToolResult {
            std::string id = a["id"].is<const char*>() ? a["id"].as<const char*>() : "";
            const agent::SkillCaller who = agent::SkillCaller::agentCall(true, false, false);
            const agent::SkillResult r = store_.remove(id, who);
            if (!r.ok) return ToolResult::fail(r.err);
            return ToolResult::ok("skill '" + id + "' deleted");
          },
          R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
}

}  // namespace lab
