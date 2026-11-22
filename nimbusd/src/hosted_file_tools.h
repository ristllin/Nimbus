#pragma once
#include <ArduinoJson.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema, kWriteContentMax(Short)
#include "nimbus/orch/file_store.h"
#include "nimbus/orch/file_tools.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/digest.h"          // RecentFile, RecentFilesBlock::Source
#include "posix_files.h"
#include "posix_fs.h"
#include "posix_platform.h"

// hosted_file_tools - engine 2's plain file tools on nimbusd (lane E2-HOST, from lane RB-N2b's
// first binding without its crash repair; ruling 112): the library's FileTools (lib/core
// orch_file_tools.cpp, the same rules and result texts as the device) over the instance's own
// file store, PosixFiles. ONE store and ONE .index: engine 1's files.*, the web Files panel and
// these tools all read and write PosixFiles' index under its one lock (PosixFiles::mutex()).
//
//   nimbusd::HostedFileTools ft(rig.files(), {offsetFn, nullptr});
//   ft.attach(plainRig.registry());                    // the five tools, before boot
//   bindings.recentFiles = ft.recentFilesFor(who);     // EngineDeps::recentFiles
//
// The storage rule (ruling 112): no code here truncates, drops or re-assigns stored bytes
// because the index is behind. An append lands at the size ON DISK (O_APPEND), as the old
// engine's append does, and the index then catches up to that size; only this call's own short
// write is cut back (to the size the file had before it). A new file whose path already holds
// bytes the index does not know is refused, never overwritten. The tools refuse, in one plain
// line, a name that collides with the atomic writer's temporary file (<name>.tmp) and a project
// the store evicts as regenerable (chat, chat-*). Bytes live where PosixFiles keeps them
// (<root>/<project>/<name>); the tools' part file is <root>/<project>/.<name>.tools-part (a
// hidden name, which no tool and no index entry can take); the swap is rename(). Calls are
// serialized per store.
namespace nimbusd {

namespace orch = nimbus::orch;

class HostedFilesIo : public orch::FileToolsIo {
 public:
  using OffsetFn = std::function<int32_t(uint32_t epoch)>;   // minutes east of UTC at that instant
  using SkillFn = std::function<bool(const std::string& id, std::string& out)>;

  HostedFilesIo(PosixFiles& files, OffsetFn offset, SkillFn skill)
      : files_(files), offset_(std::move(offset)), skill_(std::move(skill)) {}

  // One hold. After the library's step, an index entry an append found behind the bytes on
  // disk catches up to their size (the library wrote indexed + appended).
  bool locked(const std::function<void()>& fn) override {
    std::lock_guard<std::recursive_mutex> g(files_.mutex());
    fn();
    catchUp();
    return true;
  }
  orch::FileStore& index() override { return files_.toolsIndex(); }

  bool readAt(const std::string& p, const std::string& n, uint64_t offset, size_t len, std::string& out) override {
    out.clear();
    const int fd = ::open(files_.toolsPath(p, n).c_str(), O_RDONLY);
    if (fd < 0) return false;
    out.resize(len);
    size_t got = 0;
    bool ok = true;
    while (got < len) {
      const ssize_t r = ::pread(fd, &out[got], len - got, off_t(offset + got));
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) ok = false;   // a read fault is said, never taken for the end of the file
      if (r <= 0) break;
      got += size_t(r);
    }
    ::close(fd);
    out.resize(got);
    return ok;
  }

  // At the size on disk, whatever the index says. All or nothing for THIS call: a short write
  // is cut back to the size the file had before it, never further.
  bool append(const std::string& p, const std::string& n, std::string_view bytes) override {
    if (!fsutil::mkdirs(files_.toolsRoot() + "/" + p)) return false;
    const orch::FileEntry* e = index().find(p, n);
    const std::string path = files_.toolsPath(p, n);
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      return false;
    }
    const off_t before = st.st_size;
    const size_t land = tearAt && tearAt->at < bytes.size() ? tearAt->at : bytes.size();
    const bool ok = fsutil::writeAll(fd, std::string(bytes.substr(0, land))) && land == bytes.size() && ::fsync(fd) == 0;
    if (!ok && !(tearAt && tearAt->crash)) (void)::ftruncate(fd, before);   // only this call's own bytes
    ::close(fd);
    const uint64_t indexed = e ? e->bytes : 0;
    if (ok && uint64_t(before) != indexed) behind_.push_back(Behind{p, n, uint64_t(before) + bytes.size()});
    return ok;
  }

  bool partWrite(const std::string& p, const std::string& n, std::string_view bytes, bool first) override {
    if (!fsutil::mkdirs(files_.toolsRoot() + "/" + p)) return false;
    const int fd = ::open(part(p, n).c_str(), O_CREAT | O_WRONLY | (first ? O_TRUNC : O_APPEND), 0644);
    if (fd < 0) return false;
    const bool ok = fsutil::writeAll(fd, std::string(bytes)) && ::fsync(fd) == 0;
    ::close(fd);
    return ok;
  }
  // rename() replaces the file in one step: no moment where it is gone. A path that holds bytes
  // the index does not know is never overwritten (ruling 112 item 3): the swap is refused and
  // the file stays as it is.
  bool partSwap(const std::string& p, const std::string& n) override {
    struct stat st;
    if (!index().find(p, n) && ::stat(files_.toolsPath(p, n).c_str(), &st) == 0) return false;
    if (std::rename(part(p, n).c_str(), files_.toolsPath(p, n).c_str()) != 0) return false;
    fsutil::syncDir(files_.toolsRoot() + "/" + p);
    return true;
  }
  void partDiscard(const std::string& p, const std::string& n) override { std::remove(part(p, n).c_str()); }
  bool persistIndex() override { return files_.toolsPersistIndex(); }
  bool roomFor(const std::string& p, const std::string& n, uint64_t addBytes, std::string& err) override {
    return files_.toolsRoomFor(p, n, addBytes, err);
  }
  uint32_t now() override { return vclock::epoch32(); }   // the daemon's one clock (posix_platform.h)
  bool skillBody(const std::string& id, std::string& out) override { return skill_ && skill_(id, out); }
  // The listed time in the owner's zone, the library's format.
  std::string timeText(uint32_t epoch) override {
    if (!epoch || !offset_) return orch::FileToolsIo::timeText(epoch);
    const int64_t local = int64_t(epoch) + int64_t(offset_(epoch)) * 60;
    return orch::FileToolsIo::timeText(local > 0 ? uint32_t(local) : 0);
  }

  // Test seam: the next appends land only `at` bytes, then fail; crash: and nothing after them
  // runs (the process died), so the torn tail stays on the volume.
  struct Tear {
    size_t at = 0;
    bool crash = false;
  };
  std::unique_ptr<Tear> tearAt;

  std::string part(const std::string& p, const std::string& n) const {
    return files_.toolsRoot() + "/" + p + "/." + n + ".tools-part";
  }

 private:
  // An append that found the index behind (or ahead of) the bytes on disk: the size the file
  // has now. Applied when the hold ends, after the library's own index entry.
  struct Behind {
    std::string p, n;
    uint64_t size = 0;
  };
  std::vector<Behind> behind_;
  void catchUp() {
    for (const Behind& b : behind_) {
      const orch::FileEntry* cur = index().find(b.p, b.n);
      if (!cur || cur->bytes == b.size) continue;
      orch::FileEntry e = *cur;
      e.bytes = uint32_t(b.size);
      e.hash = 0;   // the bytes the index never saw: the hash is unknown, not guessed
      std::string err;
      index().add(e, err);
    }
    behind_.clear();
  }

  PosixFiles& files_;
  OffsetFn offset_;
  SkillFn skill_;
};

class HostedFileTools {
 public:
  struct Deps {
    HostedFilesIo::OffsetFn utcOffsetMin;   // null: UTC
    HostedFilesIo::SkillFn skillBody;       // skills/<id>.md; null: none
  };
  static constexpr const char* kNames[] = {"read_file", "write_file", "append_file", "edit_file", "list_files"};
  // The one line for a name the store keeps for itself (ruling 112 item 4).
  static constexpr const char* kReservedName =
      "That name is kept for this instance's own temporary files (a name ending in .tmp, or the chat and chat-* "
      "folders); choose another name.";

  explicit HostedFileTools(PosixFiles& files, Deps d = {})
      : files_(files), io_(files, std::move(d.utcOffsetMin), std::move(d.skillBody)) {}
  HostedFileTools(const HostedFileTools&) = delete;
  HostedFileTools& operator=(const HostedFileTools&) = delete;

  // A changing tool's path that collides with the store's own files or with an evicted project.
  static bool reservedPath(const std::string& path) {
    const size_t slash = path.find('/');
    const std::string project = slash == std::string::npos ? std::string() : path.substr(0, slash);
    const std::string name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    const bool tmp = name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0;
    return tmp || project == "chat" || project.rfind("chat-", 0) == 0;
  }

  // Register the five tools on a chat's registry, before boot, with the manifest's description and
  // schema (plain::toolDecl, plain::declSchema with the same shortHost bit as the request facts).
  // The registry must not outlive this object.
  void attach(orch::ToolRegistry& reg, bool shortHost = false) {
    orch::FileToolsConfig cfg;
    cfg.contentCap = shortHost ? agent::plain::kWriteContentMaxShort : agent::plain::kWriteContentMax;
    tools_.reset(new orch::FileTools(io_, cfg));
    inner_.reset(new orch::ToolRegistry());
    const orch::FileToolDeclFn decl = [shortHost](const char* t) {
      const auto* d = agent::plain::toolDecl(t);
      return d ? orch::FileToolDecl{d->description, agent::plain::declSchema(*d, shortHost)} : orch::FileToolDecl{};
    };
    tools_->registerInto(*inner_, decl);
    for (const char* t : kNames) {
      const orch::FileToolDecl d = decl(t);
      const bool changes = std::string(t) != "read_file" && std::string(t) != "list_files";
      reg.add(
          t, d.description,
          [this, t, changes](ArduinoJson::JsonObjectConst a, const orch::Principal& who) {
            if (changes && a["path"].is<const char*>() && reservedPath(a["path"].as<const char*>()))
              return orch::ToolResult::fail(kReservedName);
            std::lock_guard<std::mutex> g(callMu_);   // one call at a time on this store's part files
            return inner_->dispatch(t, a, who);
          },
          d.schema);
    }
  }

  // EngineDeps::recentFiles for one person: the files that person may read, with their write times.
  orch::tx::RecentFilesBlock::Source recentFilesFor(const orch::Principal& who) {
    return [this, who](std::vector<orch::tx::RecentFile>& out) {
      out.clear();
      std::lock_guard<std::recursive_mutex> g(files_.mutex());
      orch::FileStore& s = files_.toolsIndex();
      const bool shared = who.owner || who.perms().readShared;
      for (const orch::FileEntry* e : s.list()) {
        if (!s.readableBy(*e, who.ns, who.owner, shared)) continue;
        out.push_back({orch::plainCopy(e->project) + "/" + orch::plainCopy(e->name), e->createdAt});
      }
      return true;
    };
  }

  orch::FileTools* tools() { return tools_.get(); }
  HostedFilesIo& io() { return io_; }

 private:
  PosixFiles& files_;
  HostedFilesIo io_;
  std::unique_ptr<orch::FileTools> tools_;
  std::unique_ptr<orch::ToolRegistry> inner_;   // the library's own registration, dispatched under callMu_
  std::mutex callMu_;
};

}  // namespace nimbusd
