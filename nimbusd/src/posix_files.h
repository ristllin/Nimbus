#pragma once
#include <ArduinoJson.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <new>
#include <string>
#include <vector>

#include "posix_platform.h"
#include "nimbus/orch/file_append.h"
#include "nimbus/orch/file_store.h"
#include "nimbus/orch/provider_caps.h"  // LocalDate - the day total's today
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/turn_effects.h"  // kSaveReplacedNote: the save receipt
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

// The page's name for a photo attached in the chat: on its way to one turn, not a file the
// owner keeps. The turn removes it once it has the bytes; one never sent is swept.
constexpr const char* kChatPhotoProject = "chat";
constexpr const char* kChatPhotoPrefix = "att-";
inline bool isChatPhoto(std::string_view project, std::string_view name) {
  return project == kChatPhotoProject && name.rfind(kChatPhotoPrefix, 0) == 0;
}

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
            "Returns one 'project/name (N bytes)' per line. Lane FSCAP: the listing is "
            "bounded per call - 'limit' (default 1000, max 2000) entries after "
            "'offset'; the reply says how many there are in all.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              return filesListTool(a);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("limit":{"type":"number"},"offset":{"type":"number"}}})");

    addOwnerOnly(reg, "artifact.save",
            "Save a text file under a project. Overwrites an existing file of the same name.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name"), t = str(a, "text");
              if (p.empty() || n.empty())
                return nimbus::orch::ToolResult::fail("need 'project' and 'name'");
              std::string err;
              const bool replacing = store_.find(p, n) != nullptr;
              if (!save(p, n, t, err)) return nimbus::orch::ToolResult::fail(err);
              return nimbus::orch::ToolResult::ok("saved " + p + "/" + n + " (" + std::to_string(t.size()) + " bytes)" +
                                                  (replacing ? std::string(", ") + nimbus::orch::kSaveReplacedNote
                                                             : std::string()));
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"}},)"
            R"("required":["project","name"]})");

    addOwnerOnly(reg, "files.read",
            "Read a stored file's contents. Pass 'tail' (a row count) to read only the "
            "last rows of a log or CSV; with 'sum' (a numeric column) the tail also "
            "reports that column's total for today.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) { return readTool(a); },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"tail":{"type":"number"},"sum":{"type":"string"}},"required":["project","name"]})");

    // The device's files.append (file_append.h): the new rows go on the end of the
    // file; nothing stored is read back, replaced or rewritten.
    addOwnerOnly(reg, "files.append",
            "Add rows to the END of a TEXT document (a log, a CSV); creates it when missing. "
            "Up to 4 KB per call. Only the new rows are written: nothing stored is read back, "
            "replaced or rewritten.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) { return appendTool(a); },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"},"text":{"type":"string"}},)"
            R"("required":["project","name","text"]})");
    reg.alsoReads("files.append", {"key", "sum", "replace"});   // read to be refused by name

    addOwnerOnly(reg, "files.delete",
            "Delete a stored file for good: the bytes and the index entry both go. "
            "Pass 'project' and 'name'.",
            [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
              const std::string p = str(a, "project"), n = str(a, "name");
              if (p.empty() || n.empty())
                return nimbus::orch::ToolResult::fail("need 'project' and 'name'");
              if (!nimbus::orch::FileStore::validSegment(p, 24) ||
                  !nimbus::orch::FileStore::validSegment(n, 48))
                return nimbus::orch::ToolResult::fail("invalid project or name");
              if (!remove(p, n))
                return nimbus::orch::ToolResult::fail("no such file: " + p + "/" + n);
              return nimbus::orch::ToolResult::ok("deleted " + p + "/" + n);
            },
            R"({"type":"object","properties":{"project":{"type":"string"},)"
            R"("name":{"type":"string"}},)"
            R"("required":["project","name"]})");
  }

  // files.list, paged (lane FSCAP): at most `limit` entries after `offset`; the
  // reply says the total when the page does not cover it.
  nimbus::orch::ToolResult filesListTool(ArduinoJson::JsonObjectConst a) {
    const std::string proj = str(a, "project");
    size_t limit = a["limit"].is<double>() ? (size_t)a["limit"].as<double>() : 0;
    size_t offset = a["offset"].is<double>() ? (size_t)a["offset"].as<double>() : 0;
    if (limit == 0 || limit > kListPageMax) limit = kListPageDefault;
    std::string out;
    size_t shown = 0;
    for (size_t base = offset; shown < limit; base += kListPageChunk) {
      const auto page = store_.listPage(proj, base, kListPageChunk);
      if (page.empty()) break;
      for (const auto* e : page) {
        if (shown >= limit) break;
        out += nimbus::orch::plainCopy(e->project) + "/" + nimbus::orch::plainCopy(e->name) +
               " (" + std::to_string(e->bytes) + " bytes)\n";
        ++shown;
      }
    }
    if (out.empty()) return nimbus::orch::ToolResult::ok("(no files)");
    const size_t total = store_.countMatching(proj);   // the filter-aware total the page keys on
    if (shown < total)
      out = "(showing " + std::to_string(shown) + " of " + std::to_string(total) +
            " files; pass offset to page)\n" + out;
    return nimbus::orch::ToolResult::ok(out);
  }

  // files.read: the text of a stored file, or the rule's refusal (file_store.h).
  nimbus::orch::ToolResult readTool(ArduinoJson::JsonObjectConst a) {
    const std::string p = str(a, "project"), n = str(a, "name");
    std::string body;
    if (!read(p, n, body)) return nimbus::orch::ToolResult::fail("no such file: " + p + "/" + n);
    if (!nimbus::orch::fileReadableAsText(n, body)) return nimbus::orch::ToolResult::fail(nimbus::orch::kNotTextRefusal);
    if (a["tail"].is<float>() && a["tail"].as<double>() >= 1)
      return nimbus::orch::ToolResult::ok(tailWithTotal(n, body, a));
    return nimbus::orch::ToolResult::ok(body);
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
  // Lane FSCAP: the byte quota is the store's real space limit (the entry count
  // is uncapped by default); the daemon config / tests set it here, as the device
  // sets it from the real card size at mount.
  void setLimits(const nimbus::orch::FileStore::Limits& l) { store_.setLimits(l); }
  const nimbus::orch::FileStore& store() const { return store_; }

  // ---- the web app's Files panel (device /api/files/*, CUM-543) ----
  // The listing in the device's shape (web_files.cpp + files_subsystem listJson).
  // Lane FSCAP: bounded per call - at most `limit` entries after `offset`
  // (0/0 = the default page); the payload's count says the total.
  static constexpr size_t kListPageDefault = 1000;
  static constexpr size_t kListPageMax = 2000;
  static constexpr size_t kListPageChunk = 512;
  size_t listInto(JsonArray arr, const std::string& project, size_t limit = 0,
                  size_t offset = 0) const {
    if (limit == 0 || limit > kListPageMax) limit = kListPageDefault;
    size_t shown = 0;
    for (size_t base = offset; shown < limit; base += kListPageChunk) {
      const auto page = store_.listPage(project, base, kListPageChunk);
      if (page.empty()) break;
      for (const auto* e : page) {
        if (shown >= limit) break;
        JsonObject o = arr.add<JsonObject>();
        o["project"] = nimbus::orch::plainCopy(e->project);
        o["name"] = nimbus::orch::plainCopy(e->name);
        o["bytes"] = e->bytes;
        o["kind"] = nimbus::orch::fileKindName(e->kind);
        o["createdAt"] = e->createdAt;
        ++shown;
      }
    }
    return shown;
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
  // Chat photos no turn ever took (the page closed before the message went): gone after
  // a day. `now` is epoch seconds. Runs on each chat photo's upload.
  void sweepChatPhotos(uint32_t now) {
    std::vector<std::string> stale;
    for (const auto* e : store_.list(kChatPhotoProject))
      if (isChatPhoto(kChatPhotoProject, e->name) && now - e->createdAt > 86400u)
        stale.push_back(nimbus::orch::plainCopy(e->name));
    for (const auto& n : stale) remove(kChatPhotoProject, n);
  }
  // Delete every file in a project; -1 when the project holds none.
  int removeProject(const std::string& p) {
    std::vector<std::string> names;
    for (const auto* e : store_.list(p)) names.push_back(nimbus::orch::plainCopy(e->name));
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

  // Lane FSCAP: evict the oldest regenerable copies (chat-attachment copies only -
  // never an owner artifact or any other project's file) until `addBytes` fits
  // under the byte quota, mirroring the device's reclaimRoomLocked. Returns true
  // when anything was reclaimed; the caller re-checks wouldExceed afterwards.
  bool reclaimRoom(const std::string& p, const std::string& n, uint64_t addBytes) {
    const nimbus::orch::FileEntry* cur = store_.find(p, n);
    const uint64_t replaced = cur ? cur->bytes : 0;
    const uint64_t total = store_.totalBytes();
    const uint64_t quota = store_.limits().maxTotalBytes;
    const uint64_t over = total - replaced + addBytes;
    if (over <= quota) return false;
    const auto plan = store_.planEviction(over - quota);
    if (plan.empty()) return false;
    uint64_t cover = 0;
    for (const auto* e : plan) cover += e->bytes;
    if (cover < over - quota) return false;   // the copies cannot cover the need: evicting them would lose data for a save that still refuses
    // Copy the identities FIRST: remove() erases from the index vector and shifts
    // the rest, so a plan pointer read after a remove names the wrong entry.
    struct Victim { std::string project, name; };
    std::vector<Victim> victims;
    victims.reserve(plan.size());
    for (const auto* e : plan)
      victims.push_back(Victim{nimbus::orch::plainCopy(e->project), nimbus::orch::plainCopy(e->name)});
    for (const auto& v : victims) {
      ::unlink(bytesPath(v.project, v.name).c_str());
      store_.remove(v.project, v.name);
    }
    fsutil::writeFileAtomic(indexPath(), store_.dump());
    return true;
  }

  // Room for `addBytes` more under the byte quota, reclaiming regenerable copies
  // first when the quota is the blocker (lane FSCAP). save/append/upsert share it.
  bool roomFor(const std::string& p, const std::string& n, uint64_t addBytes, std::string& err) {
    if (!store_.wouldExceed(p, n, addBytes, err)) return true;
    if (err == nimbus::orch::FileStore::kErrStoreFull) reclaimRoom(p, n, addBytes);
    return !store_.wouldExceed(p, n, addBytes, err);
  }

  // The store's limits are checked BEFORE the first byte lands: a refused file
  // (past the entry or byte cap) must leave nothing on the volume, since the index
  // is what the Files panel lists and deletes by, and an unindexed file could never
  // be seen or reclaimed from the app. Lane FSCAP: when the BYTE quota is the
  // blocker, the oldest regenerable copies (chat-attachment copies only) are
  // evicted first so the save lands; owner artifacts are never evicted.
  bool save(const std::string& p, const std::string& n, const std::string& text,
            std::string& err) {
    if (!nimbus::orch::FileStore::validSegment(p, 24) ||
        !nimbus::orch::FileStore::validSegment(n, 48)) {
      err = "invalid project or name";
      return false;
    }
    if (!roomFor(p, n, text.size(), err)) return false;
    const bool replacing = store_.find(p, n) != nullptr;
    if (!fsutil::writeFileAtomic(bytesPath(p, n), text)) {
      err = "write failed";
      return false;
    }
    nimbus::orch::FileEntry e;
    e.project = p;
    e.name = n;
    e.bytes = (uint32_t)text.size();
    e.createdAt = vclock::epoch32();
    e.kind = nimbus::orch::fileKindForName(n);
    if (!store_.add(e, err)) {
      if (!replacing) ::unlink(bytesPath(p, n).c_str());   // a late refusal leaves no new file behind
      return false;
    }
    fsutil::writeFileAtomic(indexPath(), store_.dump());
    return true;
  }

  // A stored file's first line (at most 512 B): a CSV's header, for the append's width fact.
  static std::string firstLine(const std::string& path) {
    char buf[512];
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    const size_t got = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    std::string row(buf, got);
    const size_t nl = row.find('\n');
    if (nl != std::string::npos) row.resize(nl);
    if (!row.empty() && row.back() == '\r') row.pop_back();
    return row;
  }

  // The file's size and last byte: all a pure append reads of what is stored. False when
  // a file the index lists cannot be opened or read (the append then writes nothing).
  static bool fileEnd(const std::string& path, bool listed, size_t& size, char& last) {
    size = 0;
    last = '\0';
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return !listed;   // a new file
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    const long end = ok ? std::ftell(f) : -1;
    ok = end >= 0;
    if (ok && end > 0) {
      const int c = std::fseek(f, end - 1, SEEK_SET) == 0 ? std::fgetc(f) : EOF;
      ok = c != EOF;
      last = ok ? (char)c : '\0';
    }
    std::fclose(f);
    size = ok ? (size_t)end : 0;
    return ok;
  }

 public:
  // files.append (file_append.h): the new rows go on the end of the file, whatever it is
  // called or holds - a pure append on every path (owner ruling 41). Nothing stored is read
  // back, replaced or rewritten. A refused allocation (ruling 30) answers kAppendNoMemory
  // and leaves the file's bytes as they were: everything that allocates (the plan, the new
  // index and its dump, the receipt) is built before the first byte lands; after the write
  // only a short write cuts the file back to its old size.
  bool append(const std::string& p, const std::string& n, const std::string& text,
              std::string& err, std::string& note) {
    try {
      return appendOrThrow(p, n, text, err, note);
    } catch (const std::bad_alloc&) {
      err.assign(nimbus::orch::kAppendNoMemory);   // a small string: no allocation
      return false;
    }
  }

 private:
  // One append, everything that allocates built before the first byte lands.
  struct AppendJob {
    std::string path, index, dump, done, shortWrite = "write failed - nothing was added";
    nimbus::orch::AppendPlan plan;
    nimbus::orch::FileStore next;
    size_t size = 0;
    bool existed = false;
  };

  // The reply of a landed append: rows, the new size, the line-end and width facts.
  static std::string appendReceipt(const AppendJob& j, const std::string& p, const std::string& n, uint32_t bytes) {
    std::string out = "appended " + std::to_string(j.plan.rows) + (j.plan.rows == 1 ? " row" : " rows") + " to " + p +
                      "/" + n + " (now " + std::to_string(bytes) + " bytes)";
    if (j.plan.repairedCut) out += "; its last row had no line end, so the new rows start on a fresh line";
    if (nimbus::orch::isCsvName(n)) out += nimbus::orch::csvWidthNote(j.size ? firstLine(j.path) : "", j.plan.bytes);
    return out;
  }

  // Plan the append, check the room, and build the next index: false (err set) refuses it.
  bool prepareAppend(const std::string& p, const std::string& n, const std::string& text, AppendJob& j,
                     std::string& err) {
    if (!nimbus::orch::FileStore::validSegment(p, 24) || !nimbus::orch::FileStore::validSegment(n, 48)) {
      err = "invalid project or name";
      return false;
    }
    const nimbus::orch::FileEntry* old = store_.find(p, n);
    j.existed = old != nullptr;
    j.path = bytesPath(p, n);
    char last = '\0';
    if (!fileEnd(j.path, j.existed, j.size, last)) {
      err = "read failed - nothing was added";
      return false;
    }
    j.plan = nimbus::orch::planAppend(text, j.size, last);
    if (!j.plan.ok) {
      err = j.plan.err;
      return false;
    }
    if (!roomFor(p, n, j.size + j.plan.bytes.size(), err)) return false;
    ::mkdir((root_ + "/" + p).c_str(), 0755);
    nimbus::orch::FileEntry e = old ? *old : nimbus::orch::FileEntry{};
    if (!old) {
      e.project = p;
      e.name = n;
      e.createdAt = vclock::epoch32();
      e.kind = nimbus::orch::fileKindForName(n);
    }
    e.bytes = (uint32_t)(j.size + j.plan.bytes.size());
    e.hash = 0;
    j.next = store_;
    if (!j.next.add(e, err)) return false;
    j.index = indexPath();
    j.dump = j.next.dump();
    j.done = appendReceipt(j, p, n, e.bytes);
    return true;
  }

  bool appendOrThrow(const std::string& p, const std::string& n, const std::string& text,
                     std::string& err, std::string& note) {
    AppendJob j;
    if (!prepareAppend(p, n, text, j, err)) return false;
    // The first byte lands here; nothing below allocates before the bytes are known whole.
    FILE* f = std::fopen(j.path.c_str(), "ab");
    const size_t wrote = f ? std::fwrite(j.plan.bytes.data(), 1, j.plan.bytes.size(), f) : 0;
    const bool flushed = f && std::fflush(f) == 0;
    if (f) std::fclose(f);
    if (wrote != j.plan.bytes.size() || !flushed) {
      if (f && (j.existed || j.size > 0)) (void)::truncate(j.path.c_str(), (off_t)j.size);   // the file as it was
      else if (f) ::unlink(j.path.c_str());   // a file this call created
      err.swap(j.shortWrite);
      return false;
    }
    store_ = std::move(j.next);
    note.swap(j.done);
    try {
      fsutil::writeFileAtomic(j.index, j.dump);
    } catch (const std::bad_alloc&) {
      // The rows landed and the index in memory says so; the index file catches up at the next write.
    }
    return true;
  }

  // files.append's handler: an older key, sum or replace is one error naming them (nothing written).
  nimbus::orch::ToolResult appendTool(ArduinoJson::JsonObjectConst a) {
    const std::string p = str(a, "project"), n = str(a, "name"), t = str(a, "text");
    std::vector<std::string> old;
    for (const char* k : nimbus::orch::kAppendOldArgs)
      if (!a[k].isNull()) old.push_back(k);
    if (!old.empty()) return nimbus::orch::ToolResult::fail(nimbus::orch::appendArgsError(old));
    if (p.empty() || n.empty()) return nimbus::orch::ToolResult::fail("need 'project', 'name' and 'text'");
    std::string err, note;
    if (!append(p, n, t, err, note)) return nimbus::orch::ToolResult::fail(err);
    return nimbus::orch::ToolResult::ok(note);
  }

  // The device's day (the day total of files.read {tail, sum}).
  static std::string todayStr() {
    const nimbus::orch::LocalDate d = nimbus::orch::localDateOf(vclock::now());
    if (!d.valid()) return std::string();
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", d.year, d.month, d.day);
    return buf;
  }

  // files.read {tail, sum}: the tail view, and the day's total when a sum column is named.
  std::string tailWithTotal(const std::string& n, const std::string& body, ArduinoJson::JsonObjectConst a) {
    std::string text = tailOf(n, body, (size_t)a["tail"].as<double>());
    const std::string sum = str(a, "sum");
    if (!sum.empty()) text += nimbus::orch::csvDayTotalLine(body, sum, todayStr(), nimbus::orch::logDayOf(vclock::now(), true).zone);
    return text;
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
