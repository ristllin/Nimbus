#pragma once
#include <ArduinoJson.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/skill_attach.h"
#include "nimbus/harness/skill_md.h"
#include "nimbus/harness/skill_store.h"
#include "nimbus/orch/tool_registry.h"
#include "hosted_settings.h"             // localTime: the zone lock
#include "posix_fs.h"

// hosted_skills - skills on a hosted instance: the portable SkillStore and AttachTable
// (every rule of who may write and approve what) over the instance disk, with the
// device's four skill.* tools. The role src/agent/skills.cpp plays on the device.
// Single-context: engine thread only.
namespace nimbusd {

class PosixSkillFs : public agent::SkillFs {
 public:
  bool read(const std::string& path, size_t cap, std::string& out) override {
    std::string all;
    if (!fsutil::readFile(path, all)) return false;
    out = all.substr(0, cap);
    return true;
  }
  bool writeAtomic(const std::string& path, const std::string& text) override {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) fsutil::mkdirs(path.substr(0, slash));
    return fsutil::writeFileAtomic(path, text);
  }
  std::vector<std::string> listDirs(const std::string& dir) override {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
      const std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      struct stat st{};
      if (::stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(name);
    }
    closedir(d);
    return out;
  }
  bool exists(const std::string& path) override {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
  }
  bool remove(const std::string& path) override { return ::unlink(path.c_str()) == 0; }
  bool rename(const std::string& from, const std::string& to) override { return ::rename(from.c_str(), to.c_str()) == 0; }
  void removeDir(const std::string& dir) override { ::rmdir(dir.c_str()); }
};

struct HostedSkillHooks {
  std::function<bool()> inScheduledTurn;
  std::function<void(const std::string& text)> notifyOwner;   // a skill waits for approval
};

class HostedSkills {
 public:
  explicit HostedSkills(const std::string& root) : store_(fs_, config(root), [] { return stamp(); }) {}
  void setHooks(HostedSkillHooks h) { h_ = std::move(h); }

  agent::SkillStore& store() { return store_; }
  std::string indexText() { return store_.indexText(); }
  std::vector<agent::AttachPick> picksFor(const std::string& chat) {
    attach_.noteTurn(chat, nowMs());
    return agent::selectAttach(attach_.stickyFor(chat), store_.list());
  }
  std::string attachBody(const std::string& id) { return store_.liveBody(id, agent::SkillSide{}); }
  void noteUsed(const std::string& chat, const std::string& id) { attach_.noteUsed(chat, id, nowMs()); }
  void dropChat(const std::string& chat) { attach_.dropChat(chat); }

  void registerTools(nimbus::orch::ToolRegistry& reg);

 private:
  static agent::SkillStoreConfig config(const std::string& root) {
    agent::SkillStoreConfig cfg;
    cfg.root = root;
    cfg.sourceSlug = "sd";   // the page's word for "a stored skill, not a built-in"
    cfg.fileCap = 8192;
    cfg.pendingCap = 4;
    cfg.maxStored = 0;
    return cfg;
  }
  static uint32_t nowMs() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  static std::string stamp() { return hosted::localTime(time(nullptr), "%Y-%m-%d %H:%M"); }
  bool scheduled() const { return h_.inScheduledTurn && h_.inScheduledTurn(); }

  PosixSkillFs fs_;
  agent::SkillStore store_;
  agent::AttachTable attach_;
  HostedSkillHooks h_;
};

inline void HostedSkills::registerTools(nimbus::orch::ToolRegistry& reg) {
  using nimbus::orch::Principal;
  using nimbus::orch::ToolResult;
  reg.add("skill.list",
          "List the skills (task playbooks and knowledge capsules) with their status: approved, waiting for "
          "the owner's approval, or an edit waiting on an approved one.",
          [this](ArduinoJson::JsonObjectConst, const Principal&) -> ToolResult {
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
          "Read a skill - a task playbook or knowledge capsule - by id from the [SKILLS] index. Read the matching "
          "one BEFORE starting (or refusing) a task it covers. Once read it stays attached to this chat's next "
          "turns ([ACTIVE SKILLS]), so do not read it again.",
          [this](ArduinoJson::JsonObjectConst a, const Principal&) -> ToolResult {
            const std::string id = a["id"] | "";
            bool trusted = false;
            const std::string body = store_.get(id, agent::SkillSide{}, &trusted);
            if (body.empty()) return ToolResult::fail("unknown skill id (call skill_list)");
            return ToolResult::ok(body);
          },
          R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
  reg.add("skill.save",
          std::string("Save or update a skill - a task playbook stored on this instance. id: a-z0-9-_ max 23 chars. md: "
                      "the SKILL.md text. Front matter keys you may write: ") +
              agent::skillAuthorKeysDoc() +
              ". Saving a NEW skill waits for the owner's approval (it is not usable in the current task); saving an "
              "existing APPROVED skill proposes an EDIT - the approved version keeps running until the owner approves "
              "it, so an edit is never a step of the current task either.",
          [this](ArduinoJson::JsonObjectConst a, const Principal& who) -> ToolResult {
            if (!who.perms().manageTenants) return ToolResult::fail("only an admin's conversation can author skills");
            const std::string id = a["id"] | "", md = a["md"] | "";
            const agent::SkillResult r =
                store_.save(id, md, agent::SkillCaller::agentCall(true, scheduled(), agent::skillTurnTainted()), false);
            if (!r.ok) return ToolResult::fail(r.err);
            const bool proposal = r.outcome == agent::SkillOutcome::Created || r.outcome == agent::SkillOutcome::Replaced ||
                                  r.outcome == agent::SkillOutcome::EditProposed;
            std::string out = proposal ? agent::skillProposalNotice(id, r) : agent::skillResultText(id, r);
            if (!r.similar.empty())
              out += " A skill named '" + r.similar + "' already exists - if you meant to edit it, save there instead.";
            if (h_.notifyOwner) h_.notifyOwner(agent::skillProposalNotice(id, r));
            return ToolResult::ok(out);
          },
          R"({"type":"object","properties":{"id":{"type":"string"},"md":{"type":"string"}},"required":["id","md"]})");
  reg.add("skill.delete",
          "Delete a skill capsule YOU authored (created_by: agent). Owner-made skills can only be deleted by the owner "
          "(web UI or /skill delete).",
          [this](ArduinoJson::JsonObjectConst a, const Principal& who) -> ToolResult {
            if (!who.perms().manageTenants) return ToolResult::fail("only an admin's conversation can delete skills");
            const std::string id = a["id"] | "";
            const agent::SkillResult r =
                store_.remove(id, agent::SkillCaller::agentCall(true, scheduled(), agent::skillTurnTainted()));
            if (!r.ok) return ToolResult::fail(r.err);
            return ToolResult::ok("skill '" + id + "' deleted");
          },
          R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
}

}  // namespace nimbusd
