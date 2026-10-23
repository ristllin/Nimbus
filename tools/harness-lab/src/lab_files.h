#pragma once
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/file_append.h"  // planAppend + the tail view (files.append)
#include "nimbus/orch/file_store.h"
#include "nimbus/orch/store_full.h"
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
              // A full store refuses as the device does (files_subsystem refusalForHead).
              if (full_ != Full::No)
                return nimbus::orch::ToolResult::fail(
                    std::string(nimbus::orch::FileStore::kErrTooManyFiles) + nimbus::orch::kStoreFullToolNote);
              files_[p + "/" + n] = t;
              return nimbus::orch::ToolResult::ok("saved " + p + "/" + n + " (" +
                                                  std::to_string(t.size()) + " bytes)");
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"}},)"
            R"("required":["project","name","text"]})");

    reg.add("files.read", "Read a stored file's contents. Optional 'tail': read only the "
                         "last N rows (a CSV's header row comes with it).",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name");
              auto it = files_.find(p + "/" + n);
              if (it == files_.end())
                return nimbus::orch::ToolResult::fail("no such file: " + p + "/" + n);
              int tail = a["tail"].is<int>() ? a["tail"].as<int>() : 0;
              if (tail <= 0) return nimbus::orch::ToolResult::ok(it->second);
              // The device's tail view (nimbus/orch/file_append.h), so the lab
              // teaches the same read-the-end habit the firmware serves.
              const std::string& file = it->second;
              const size_t win = std::min(file.size(), nimbus::orch::kTailWindowBytes);
              const nimbus::orch::TailView v =
                  nimbus::orch::tailRows(file.substr(file.size() - win),
                                         file.size() - win, (size_t)tail);
              std::string header;
              const size_t eol = file.find('\n');
              if (eol != std::string::npos) header = file.substr(0, eol);
              return nimbus::orch::ToolResult::ok(
                  nimbus::orch::renderTail(v, n, file.size(), header));
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"tail":{"type":"integer"}},"required":["project","name"]})");

    reg.add("files.append",
            "Append rows to the end of a text document (a CSV, .log, .md, .txt, .json). "
            "Only the new rows are written - never read a whole log and save it back to "
            "add a row.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name"), t = str(a, "text");
              if (p.empty() || n.empty())
                return nimbus::orch::ToolResult::fail("need 'project' and 'name'");
              if (n.rfind(".csv") == std::string::npos && n.rfind(".log") == std::string::npos &&
                  n.rfind(".md") == std::string::npos && n.rfind(".txt") == std::string::npos &&
                  n.rfind(".json") == std::string::npos)
                return nimbus::orch::ToolResult::fail(
                    "files.append adds rows to a text document (.csv, .log, .md, .txt, .json)");
              if (!nimbus::orch::FileStore::validSegment(p, 24) ||
                  !nimbus::orch::FileStore::validSegment(n, 48))
                return nimbus::orch::ToolResult::fail("invalid project or name");
              // The device's own append rules (normalize rows, cap bytes, repair a
              // cut last row visibly) - a lab that taught a different shape would
              // teach the model a habit the firmware doesn't serve.
              std::string& file = files_[p + "/" + n];
              const nimbus::orch::AppendPlan plan =
                  nimbus::orch::planAppend(t, file.size(), file.empty() ? '\n' : file.back());
              if (!plan.ok) return nimbus::orch::ToolResult::fail(plan.err);
              file += plan.bytes;
              return nimbus::orch::ToolResult::ok(
                  "appended " + std::to_string(plan.rows) + " row(s) to " + p + "/" + n +
                  (plan.repairedCut
                       ? " (the file's last row was incomplete; the new rows start on a fresh line)"
                       : ""));
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"}},)"
            R"("required":["project","name","text"]})");
  }

  void seed(const std::string& path, const std::string& text) { files_[path] = text; }
  // The store is full: every save is refused with the device's words (the store's
  // error and the tool note: the owner is told once).
  enum class Full { No, Told };
  void setFull(Full f) { full_ = f; }
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
  Full full_ = Full::No;
};

}  // namespace lab
