#pragma once
#include <ArduinoJson.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <string>

#include "nimbus/orch/file_append.h"
#include "nimbus/orch/file_store.h"
#include "nimbus/orch/provider_caps.h"  // LocalDate - the upsert's today
#include "nimbus/orch/tool_registry.h"
#include "posix_fs.h"

// posix_files - the daemon's files.* / artifact.save tool surface, disk-backed.
//
// The device's real files.* are SD-backed (files_subsystem.cpp needs a card);
// harness-lab stands them in with an in-RAM std::map. nimbusd does the real
// thing over a POSIX tree: bytes live at <root>/<project>/<name>, the index is
// the portable FileStore (its .index persisted with the same atomic writer as
// the memory blobs), and the ONE traversal gate - FileStore::validSegment - is
// reused verbatim so a hostile "../../etc/passwd" name is rejected exactly as
// the firmware rejects it. This makes the multi-tool / tool-loop scenarios
// exercise genuine persistence, and the artifacts survive a restart.
namespace nimbusd {

class PosixFiles {
 public:
  // `root` is the files directory (e.g. /data/mem/files). Loads the index if it
  // exists; construction is cheap and does not scan the tree.
  explicit PosixFiles(std::string root) : root_(std::move(root)) {
    std::string idx;
    if (fsutil::readFile(indexPath(), idx)) store_.load(idx);
  }

  // The hosted store has no per-person namespaces (the device keeps each person's
  // files apart), so it belongs to the owner alone: an approved member or guest
  // (CUM-459 RBAC) neither sees these tools advertised nor can run them.
  static constexpr const char* kOwnerOnly =
      "Files on this instance belong to its owner; ask the owner to share what you need.";

  // Register one owner-only tool: the handler refuses a non-owner (enforcement) and
  // the registry leaves it out of a non-owner's list (advertisement), in one place.
  static void addOwnerOnly(nimbus::orch::ToolRegistry& reg, const char* name, const char* desc,
                           nimbus::orch::ToolHandler fn, const char* schema) {
    reg.add(name, desc,
            [fn](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
              return who.owner ? fn(a, who) : nimbus::orch::ToolResult::fail(kOwnerOnly);
            },
            schema);
    reg.setAdminOnly(name);
  }

  void registerTools(nimbus::orch::ToolRegistry& reg) {
    addOwnerOnly(reg, "files.list",
            "List stored files. Optional 'project' narrows to one project. "
            "Returns one 'project/name (N bytes)' per line.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string proj = str(a, "project");
              std::string out;
              for (const auto* e : store_.list(proj))
                out += e->project + "/" + e->name + " (" +
                       std::to_string(e->bytes) + " bytes)\n";
              return out.empty() ? nimbus::orch::ToolResult::ok("(no files)")
                                 : nimbus::orch::ToolResult::ok(out);
            },
            R"({"type":"object","properties":{"project":{"type":"string"}}})");

    addOwnerOnly(reg, "artifact.save",
            "Save a text file under a project. Overwrites an existing file of the same name.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name"), t = str(a, "text");
              if (p.empty() || n.empty())
                return nimbus::orch::ToolResult::fail("need 'project' and 'name'");
              std::string err;
              if (!save(p, n, t, err)) return nimbus::orch::ToolResult::fail(err);
              return nimbus::orch::ToolResult::ok("saved " + p + "/" + n + " (" +
                                                  std::to_string(t.size()) + " bytes)");
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"}},)"
            R"("required":["project","name"]})");

    addOwnerOnly(reg, "files.read",
            "Read a stored file's contents. Pass 'tail' (a row count) to read only the "
            "last rows of a log or CSV.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name");
              std::string body;
              if (!read(p, n, body))
                return nimbus::orch::ToolResult::fail("no such file: " + p + "/" + n);
              if (a["tail"].is<float>() && a["tail"].as<double>() >= 1)
                return nimbus::orch::ToolResult::ok(tailOf(n, body, (size_t)a["tail"].as<double>()));
              return nimbus::orch::ToolResult::ok(body);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"tail":{"type":"number"}},"required":["project","name"]})");

    // The device's files.append (file_append.h): the new rows go on the end of the
    // file; nothing stored is rewritten. With 'key' it is the keyed CSV upsert
    // (G1, rows 32/33) and the reply carries the day total.
    addOwnerOnly(reg, "files.append",
            "Add rows to the END of a text file (a log, a CSV): only the new rows are "
            "written. Creates the file when missing (put a CSV's header row first). Up to "
            "4 KB per call. Use it to log a row instead of re-saving the whole file. For "
            "a keyed CSV pass 'key' (the column whose value identifies a row) to UPDATE "
            "that row instead of adding a duplicate, and 'sum' (a numeric column) to get "
            "its total for today in the reply.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name"), t = str(a, "text"),
                                key = str(a, "key"), sum = str(a, "sum");
              if (p.empty() || n.empty())
                return nimbus::orch::ToolResult::fail("need 'project', 'name' and 'text'");
              std::string err, note;
              if (!key.empty()) {
                if (!upsert(p, n, t, key, sum, err, note))
                  return nimbus::orch::ToolResult::fail(err);
                return nimbus::orch::ToolResult::ok(note);
              }
              if (!append(p, n, t, err, note)) return nimbus::orch::ToolResult::fail(err);
              return nimbus::orch::ToolResult::ok(note);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"},)"
            R"("key":{"type":"string"},"sum":{"type":"string"}},)"
            R"("required":["project","name","text"]})");
  }

  // ---- direct access (scenario seeding + assertions) ----
  void seed(const std::string& path, const std::string& text) {
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return;
    std::string err;
    save(path.substr(0, slash), path.substr(slash + 1), text, err);
  }
  bool has(const std::string& path) const {
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return false;
    return store_.find(path.substr(0, slash), path.substr(slash + 1)) != nullptr;
  }
  std::string get(const std::string& path) const {
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return std::string();
    std::string body;
    read(path.substr(0, slash), path.substr(slash + 1), body);
    return body;
  }
  size_t count() const { return store_.count(); }
  uint64_t totalBytes() const { return store_.totalBytes(); }

  // ---- the web app's Files panel (device /api/files/*, CUM-543) ----
  // The listing in the device's shape (web_files.cpp + files_subsystem listJson).
  void listInto(JsonArray arr, const std::string& project) const {
    for (const auto* e : store_.list(project)) {
      JsonObject o = arr.add<JsonObject>();
      o["project"] = e->project;
      o["name"] = e->name;
      o["bytes"] = e->bytes;
      o["kind"] = nimbus::orch::fileKindName(e->kind);
      o["createdAt"] = e->createdAt;
    }
  }
  // Store an uploaded file (any bytes) under the same traversal gate as the tools.
  bool put(const std::string& p, const std::string& n, const std::string& bytes, std::string& err) {
    return save(p, n, bytes, err);
  }
  bool fetch(const std::string& p, const std::string& n, std::string& out) const { return read(p, n, out); }
  bool remove(const std::string& p, const std::string& n) {
    if (!store_.find(p, n)) return false;
    ::unlink(bytesPath(p, n).c_str());
    store_.remove(p, n);
    fsutil::writeFileAtomic(indexPath(), store_.dump());
    return true;
  }
  // Delete every file in a project; -1 when the project holds none.
  int removeProject(const std::string& p) {
    std::vector<std::string> names;
    for (const auto* e : store_.list(p)) names.push_back(e->name);
    if (p.empty() || names.empty()) return -1;
    for (const auto& n : names) remove(p, n);
    ::rmdir((root_ + "/" + p).c_str());
    return (int)names.size();
  }

 private:
  std::string indexPath() const { return root_ + "/.index"; }
  std::string bytesPath(const std::string& p, const std::string& n) const {
    return root_ + "/" + p + "/" + n;
  }

  // The store's limits are checked BEFORE the first byte lands: a refused file
  // (past the entry or byte cap) must leave nothing on the volume, since the index
  // is what the Files panel lists and deletes by, and an unindexed file could never
  // be seen or reclaimed from the app.
  bool save(const std::string& p, const std::string& n, const std::string& text,
            std::string& err) {
    if (!nimbus::orch::FileStore::validSegment(p, 24) ||
        !nimbus::orch::FileStore::validSegment(n, 48)) {
      err = "invalid project or name";
      return false;
    }
    if (store_.wouldExceed(p, n, text.size(), err)) return false;
    const bool replacing = store_.find(p, n) != nullptr;
    if (!fsutil::writeFileAtomic(bytesPath(p, n), text)) {
      err = "write failed";
      return false;
    }
    nimbus::orch::FileEntry e;
    e.project = p;
    e.name = n;
    e.bytes = (uint32_t)text.size();
    e.createdAt = (uint32_t)time(nullptr);
    e.kind = nimbus::orch::fileKindForName(n);
    if (!store_.add(e, err)) {
      if (!replacing) ::unlink(bytesPath(p, n).c_str());   // a late refusal leaves no new file behind
      return false;
    }
    fsutil::writeFileAtomic(indexPath(), store_.dump());
    return true;
  }

  bool append(const std::string& p, const std::string& n, const std::string& text,
              std::string& err, std::string& note) {
    if (!nimbus::orch::FileStore::validSegment(p, 24) ||
        !nimbus::orch::FileStore::validSegment(n, 48)) {
      err = "invalid project or name";
      return false;
    }
    std::string cur;
    const nimbus::orch::FileEntry* old = store_.find(p, n);
    if (old) read(p, n, cur);
    const nimbus::orch::AppendPlan plan =
        nimbus::orch::planAppend(text, cur.size(), cur.empty() ? '\0' : cur.back());
    if (!plan.ok) {
      err = plan.err;
      return false;
    }
    if (store_.wouldExceed(p, n, cur.size() + plan.bytes.size(), err)) return false;
    ::mkdir((root_ + "/" + p).c_str(), 0755);
    FILE* f = std::fopen(bytesPath(p, n).c_str(), "ab");
    if (!f) {
      err = "write failed";
      return false;
    }
    const size_t wrote = std::fwrite(plan.bytes.data(), 1, plan.bytes.size(), f);
    std::fclose(f);
    nimbus::orch::FileEntry e = old ? *old : nimbus::orch::FileEntry{};
    if (!old) {
      e.project = p;
      e.name = n;
      e.createdAt = (uint32_t)time(nullptr);
      e.kind = nimbus::orch::fileKindForName(n);
    }
    e.bytes = (uint32_t)(cur.size() + wrote);
    e.hash = 0;
    if (!store_.add(e, err)) return false;
    fsutil::writeFileAtomic(indexPath(), store_.dump());
    if (wrote != plan.bytes.size()) {
      err = "short write - read the tail before adding the rows again";
      return false;
    }
    note = "appended " + std::to_string(plan.rows) + (plan.rows == 1 ? " row" : " rows") + " to " +
           p + "/" + n + " (now " + std::to_string(e.bytes) + " bytes)" +
           (plan.repairedCut ? "; its last row was incomplete and is left as it was" : "");
    return true;
  }

  // The keyed CSV upsert (file_append.h): the daemon merges by key and reports
  // the day total, the same reply the device gives.
  static std::string todayStr() {
    const nimbus::orch::LocalDate d = nimbus::orch::localDateOf(time(nullptr));
    if (!d.valid()) return std::string();
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", d.year, d.month, d.day);
    return buf;
  }

  bool upsert(const std::string& p, const std::string& n, const std::string& text,
              const std::string& key, const std::string& sum, std::string& err,
              std::string& note) {
    std::string cur;
    const nimbus::orch::FileEntry* old = store_.find(p, n);
    if (old) read(p, n, cur);
    const nimbus::orch::CsvUpsert up =
        nimbus::orch::planCsvUpsert(cur, text, key, sum, todayStr());
    if (!up.ok) {
      err = up.err;
      return false;
    }
    if (up.changed) {
      if (store_.wouldExceed(p, n, up.out.size(), err)) return false;
      if (!fsutil::writeFileAtomic(bytesPath(p, n), up.out)) {
        err = "write failed";
        return false;
      }
      nimbus::orch::FileEntry e = old ? *old : nimbus::orch::FileEntry{};
      if (!old) {
        e.project = p;
        e.name = n;
        e.createdAt = (uint32_t)time(nullptr);
        e.kind = nimbus::orch::fileKindForName(n);
      }
      e.bytes = (uint32_t)up.out.size();
      e.hash = 0;
      if (!store_.add(e, err)) return false;
      fsutil::writeFileAtomic(indexPath(), store_.dump());
    }
    note = "updated " + std::to_string(up.replaced) +
           (up.replaced == 1 ? " row" : " rows") + ", appended " +
           std::to_string(up.appended) + (up.appended == 1 ? " row" : " rows") + " in " + p + "/" + n;
    if (up.hasTotal) {
      char tot[32];
      std::snprintf(tot, sizeof tot, "%g", up.todayTotal);
      note += "; today's " + sum + ": " + tot;
    }
    return true;
  }

  // The tail view over a whole body (the device reads only the window; on disk the
  // daemon can afford the whole read, and the result is the same text).
  static std::string tailOf(const std::string& name, const std::string& body, size_t rows) {
    const size_t win = nimbus::orch::kTailWindowBytes;
    const size_t start = body.size() > win ? body.size() - win - 1 : 0;
    std::string header;
    const bool csv = name.size() > 4 && name.compare(name.size() - 4, 4, ".csv") == 0;
    if (start > 0 && csv) {
      const size_t nl = body.find('\n');
      if (nl != std::string::npos && nl < 512) header = body.substr(0, nl);
    }
    const nimbus::orch::TailView v = nimbus::orch::tailRows(body.substr(start), start, rows);
    return nimbus::orch::renderTail(v, name, body.size(), header);
  }

  bool read(const std::string& p, const std::string& n, std::string& out) const {
    if (!store_.find(p, n)) return false;
    return fsutil::readFile(bytesPath(p, n), out);
  }

  static std::string str(ArduinoJson::JsonObjectConst a, const char* k) {
    return a[k].is<const char*>() ? std::string(a[k].as<const char*>()) : std::string();
  }

  std::string root_;
  nimbus::orch::FileStore store_;
};

}  // namespace nimbusd
