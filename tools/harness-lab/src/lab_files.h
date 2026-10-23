#pragma once
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/file_append.h"  // planAppend + the tail view (files.append)
#include "nimbus/orch/file_store.h"
#include "nimbus/orch/store_full.h"   // a full store is said once (storm leg g)
#include "nimbus/orch/file_append.h"  // planAppend/tailRows (CUM-644)
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
    registerListAndSave(reg);
    registerRead(reg);
    registerAppend(reg);
  }

  void registerListAndSave(nimbus::orch::ToolRegistry& reg) {
    reg.add("files.list",
            "List stored files. Optional 'project' narrows to one project. "
            "Returns one 'project/name (N bytes)' per line.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
              return listTool(a, who);
            },
            R"({"type":"object","properties":{"project":{"type":"string"}}})");

    reg.add("artifact.save",
            "Save a text file under a project. Overwrites an existing file of the same name.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
              return saveTool(a, who);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"}},)"
            R"("required":["project","name","text"]})");
  }

  void registerRead(nimbus::orch::ToolRegistry& reg) {
    reg.add("files.read",
            "Read a stored file's contents. Pass 'tail' (a row count) to read only the "
            "last rows of a log or CSV; with 'sum' (a numeric column) the tail also "
            "reports that column's total for today.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
              return readTool(a, who);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"tail":{"type":"number"},)"
            R"("sum":{"type":"string"}},"required":["project","name"]})");
  }

  void registerAppend(nimbus::orch::ToolRegistry& reg) {
    // CUM-644: the lab's files.append and tail view match the device's, so a
    // lab run exercises the same calls the firmware will see.
    reg.add("files.append",
            "Add rows to the END of a TEXT document (a log, a CSV): only the new rows are "
            "written. For a keyed CSV pass 'key' (the column whose value identifies a row) "
            "to UPDATE that row instead of adding a duplicate, and 'sum' (a numeric column) "
            "to get its total for today in the reply.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
              return appendTool(a, who);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"},)"
            R"("key":{"type":"string"},"sum":{"type":"string"}},)"
            R"("required":["project","name","text"]})");
  }

  // One tool handler each - the registrations above stay one-liners, and each
  // handler's logic stays under the complexity ceiling on its own.
  nimbus::orch::ToolResult listTool(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
    const std::string proj = str(a, "project");
    std::string out;
    for (const auto& kv : files_) {
      if (!proj.empty() && kv.first.rfind(proj + "/", 0) != 0) continue;
      out += kv.first + " (" + std::to_string(kv.second.size()) + " bytes)\n";
    }
    return out.empty() ? nimbus::orch::ToolResult::ok("(no files)")
                       : nimbus::orch::ToolResult::ok(out);
  }

  nimbus::orch::ToolResult saveTool(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
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
  }

  nimbus::orch::ToolResult readTool(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
    const std::string p = str(a, "project"), n = str(a, "name");
    auto it = files_.find(p + "/" + n);
    if (it == files_.end())
      return nimbus::orch::ToolResult::fail("no such file: " + p + "/" + n);
    if (a["tail"].is<float>() && a["tail"].as<double>() >= 1) {
      const size_t rows = (size_t)a["tail"].as<double>();
      // The device's tail view (nimbus/orch/file_append.h), so the lab teaches
      // the same read-the-end habit the firmware serves: the window over the
      // end, and the CSV's header row riding along when it is outside it.
      const std::string& file = it->second;
      const size_t win = std::min(file.size(), nimbus::orch::kTailWindowBytes);
      const nimbus::orch::TailView v =
          nimbus::orch::tailRows(file.substr(file.size() - win), file.size() - win, rows);
      std::string header;
      const size_t eol = file.find('\n');
      if (eol != std::string::npos) header = file.substr(0, eol);
      std::string text = nimbus::orch::renderTail(v, n, file.size(), header);
      const std::string sum = str(a, "sum");
      if (!sum.empty()) {
        double total = 0;
        if (nimbus::orch::csvDayTotal(it->second, sum, todayStr(), total)) {
          char buf[48];
          std::snprintf(buf, sizeof buf, "%g", total);
          text += "today's " + sum + ": " + buf + "\n";
        } else {
          text += "[no day total: the file has no date column or no column '" + sum + "']\n";
        }
      }
      return nimbus::orch::ToolResult::ok(text);
    }
    return nimbus::orch::ToolResult::ok(it->second);
  }

  nimbus::orch::ToolResult appendTool(ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
    const std::string p = str(a, "project"), n = str(a, "name"),
                      t = str(a, "text"), key = str(a, "key"), sum = str(a, "sum");
    if (p.empty() || n.empty() || t.empty())
      return nimbus::orch::ToolResult::fail("need 'project', 'name' and 'text'");
    if (!key.empty()) {
      const nimbus::orch::CsvUpsert up =
          nimbus::orch::planCsvUpsert(files_[p + "/" + n], t, key, sum, todayStr());
      if (!up.ok) return nimbus::orch::ToolResult::fail(up.err);
      if (up.changed) files_[p + "/" + n] = up.out;
      std::string note = "updated " + std::to_string(up.replaced) +
                         (up.replaced == 1 ? " row" : " rows") + ", appended " +
                         std::to_string(up.appended) +
                         (up.appended == 1 ? " row" : " rows") + " in " + p + "/" + n;
      if (up.hasTotal) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%g", up.todayTotal);
        note += "; today's " + sum + ": " + buf;
      }
      return nimbus::orch::ToolResult::ok(note);
    }
    return appendPlain(p, n, t);
  }

  nimbus::orch::ToolResult appendPlain(const std::string& p, const std::string& n,
                                       const std::string& t) {
    const std::string path = p + "/" + n;
    const nimbus::orch::AppendPlan plan = nimbus::orch::planAppend(
        t, files_[path].size(), files_[path].empty() ? '\0' : files_[path].back());
    if (!plan.ok) return nimbus::orch::ToolResult::fail(plan.err);
    files_[path] += plan.bytes;
    return nimbus::orch::ToolResult::ok(
        "appended " + std::to_string(plan.rows) +
        (plan.rows == 1 ? " row" : " rows") + " to " + p + "/" + n + " (now " +
        std::to_string(files_[path].size()) + " bytes)");
  }


  // The lab's "today" (the same device rule the upsert and the tail view use).
  static std::string todayStr() {
    const time_t now = std::time(nullptr);
    std::tm loc{};
    if (!localtime_r(&now, &loc)) return std::string();
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", loc.tm_year + 1900, loc.tm_mon + 1,
                  loc.tm_mday);
    return buf;
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
