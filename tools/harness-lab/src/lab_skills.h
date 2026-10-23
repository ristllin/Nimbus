#pragma once
#include <map>
#include <string>
#include <vector>

#include "nimbus/harness/skill_attach.h"
#include "nimbus/harness/skill_md.h"
#include "nimbus/harness/skill_store.h"
#include "nimbus/orch/tool_registry.h"

// lab_skills - the lab's stand-in for the device's skills adapter
// (src/agent/skills.cpp): the portable SkillStore and AttachTable over an
// in-memory SkillFs, plus the four skill.* tools shaped EXACTLY as the device
// registers them (a lab that taught a different shape would teach the model a
// habit the firmware doesn't serve). Scenarios seed skills through the same
// store rules (owner saves are live; assistant saves wait).
namespace lab {

class LabSkills {
 public:
  LabSkills() : store_(fs_, config(), [] { return std::string("2026-10-23 09:00"); }) {}

  // The four tools, worded as the device registers them (memory_subsystem.cpp).
  void registerTools(nimbus::orch::ToolRegistry& reg);

  // The engine deps (the same wiring src/agent/orchestrator.cpp does).
  std::string indexText() { return store_.indexText(); }
  std::vector<agent::AttachPick> picksFor(const std::string& chatId);
  std::string attachBody(const std::string& id) { return store_.liveBody(id, agent::SkillSide{}); }
  void noteUsed(const std::string& chat, const std::string& id);

  agent::SkillStore& store() { return store_; }
  // An owner save (approved, the owner's) - how scenarios seed a live skill.
  agent::SkillResult ownerSave(const std::string& id, const std::string& md) {
    return store_.save(id, md, agent::SkillCaller::ownerCall(), false);
  }
  // The last (chat, id) pair the engine reported for a skill.get, if any.
  std::vector<std::pair<std::string, std::string>> noted;

 private:
  class MemFs : public agent::SkillFs {
   public:
    std::map<std::string, std::string> files;
    bool failWrites = false;
    bool read(const std::string& path, size_t cap, std::string& out) override {
      auto it = files.find(path);
      if (it == files.end()) return false;
      out = it->second.substr(0, cap);
      return true;
    }
    bool writeAtomic(const std::string& path, const std::string& text) override {
      if (failWrites) return false;
      files[path] = text;
      return true;
    }
    std::vector<std::string> listDirs(const std::string& dir) override {
      std::vector<std::string> out;
      const std::string pre = dir + "/";
      for (const auto& kv : files)
        if (kv.first.rfind(pre, 0) == 0) {
          const size_t slash = kv.first.find('/', pre.size());
          if (slash != std::string::npos) out.push_back(kv.first.substr(pre.size(), slash - pre.size()));
        }
      return out;
    }
    bool exists(const std::string& path) override { return files.count(path) > 0; }
    bool remove(const std::string& path) override { return files.erase(path) > 0; }
    bool rename(const std::string& from, const std::string& to) override {
      auto it = files.find(from);
      if (it == files.end()) return false;
      files[to] = it->second;
      files.erase(it);
      return true;
    }
    void removeDir(const std::string&) override {}
  };

  static agent::SkillStoreConfig config() {
    agent::SkillStoreConfig cfg;
    cfg.root = "/mem/skills";
    cfg.sourceSlug = "sd";
    cfg.fileCap = 8192;
    cfg.pendingCap = 4;
    return cfg;
  }

  MemFs fs_;
  agent::SkillStore store_;
  agent::AttachTable attach_;
};

}  // namespace lab
