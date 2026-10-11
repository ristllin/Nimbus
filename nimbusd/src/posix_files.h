#pragma once
#include <ArduinoJson.h>
#include <unistd.h>

#include <ctime>
#include <string>

#include "nimbus/orch/file_store.h"
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

    addOwnerOnly(reg, "files.read", "Read a stored file's contents.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name");
              std::string body;
              if (!read(p, n, body))
                return nimbus::orch::ToolResult::fail("no such file: " + p + "/" + n);
              return nimbus::orch::ToolResult::ok(body);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"}},"required":["project","name"]})");
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

  bool save(const std::string& p, const std::string& n, const std::string& text,
            std::string& err) {
    if (!nimbus::orch::FileStore::validSegment(p, 24) ||
        !nimbus::orch::FileStore::validSegment(n, 48)) {
      err = "invalid project or name";
      return false;
    }
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
    if (!store_.add(e, err)) return false;
    fsutil::writeFileAtomic(indexPath(), store_.dump());
    return true;
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
