#pragma once
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

  void registerTools(nimbus::orch::ToolRegistry& reg) {
    reg.add("files.list",
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

    reg.add("artifact.save",
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

    reg.add("files.read", "Read a stored file's contents.",
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
