#pragma once
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/file_store.h"
#include "nimbus/orch/tool_registry.h"

// Host stand-in for the device's SD-backed artifact tools
// (src/agent/files_subsystem.cpp - which needs a real card, so it cannot be
// lifted as-is). The TOOL SHAPE is the same - files.list / artifact.save /
// files.read over project + name - so a scenario can exercise the behaviour we
// actually care about: can the agent find its way around its own storage, and
// chain several calls to do it? The bytes live in a std::map, not on flash.
namespace lab {

class LabFiles {
 public:
  void registerTools(nimbus::orch::ToolRegistry& reg) {
    reg.add("files.list",
            "List stored files. Optional 'project' narrows to one project. "
            "Returns one 'project/name (N bytes)' per line.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string proj = str(a, "project");
              std::string out;
              for (const auto& kv : files_) {
                if (!proj.empty() && kv.first.rfind(proj + "/", 0) != 0) continue;
                out += kv.first + " (" + std::to_string(kv.second.size()) + " bytes)\n";
              }
              return out.empty() ? nimbus::orch::ToolResult::ok("(no files)")
                                 : nimbus::orch::ToolResult::ok(out);
            },
            R"({"type":"object","properties":{"project":{"type":"string"}}})");

    reg.add("artifact.save",
            "Save a text file under a project. Overwrites an existing file of the same name.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name"), t = str(a, "text");
              if (p.empty() || n.empty())
                return nimbus::orch::ToolResult::fail("need 'project' and 'name'");
              // The device's real traversal gate, reused verbatim - a lab that
              // accepted "../../etc/passwd" would teach the model a habit the
              // firmware rejects.
              if (!nimbus::orch::FileStore::validSegment(p, 24) ||
                  !nimbus::orch::FileStore::validSegment(n, 48))
                return nimbus::orch::ToolResult::fail("invalid project or name");
              files_[p + "/" + n] = t;
              return nimbus::orch::ToolResult::ok("saved " + p + "/" + n + " (" +
                                                  std::to_string(t.size()) + " bytes)");
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"}},)"
            R"("required":["project","name","text"]})");

    reg.add("files.read", "Read a stored file's contents.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name");
              auto it = files_.find(p + "/" + n);
              if (it == files_.end())
                return nimbus::orch::ToolResult::fail("no such file: " + p + "/" + n);
              return nimbus::orch::ToolResult::ok(it->second);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"}},"required":["project","name"]})");
  }

  void seed(const std::string& path, const std::string& text) { files_[path] = text; }
  size_t count() const { return files_.size(); }
  bool has(const std::string& path) const { return files_.count(path) > 0; }
  std::string get(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? std::string() : it->second;
  }

 private:
  static std::string str(ArduinoJson::JsonObjectConst a, const char* k) {
    return a[k].is<const char*>() ? std::string(a[k].as<const char*>()) : std::string();
  }
  std::map<std::string, std::string> files_;
};

}  // namespace lab
