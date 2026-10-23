#include "skills.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "memory_subsystem.h"          // dataFs() + haveSd() + Lock (SD serialization)
#include "orchestrator.h"              // telegramForTurn / fileSendForTurn (CUM-523 sides)
#include "../sys/agent_log.h"
#include "nimbus/harness/builtin_skills.h"   // isBuiltinSkill (the un-shadowable ids)

namespace agent::skills {

namespace {

// The store root on each tier: SD when a card is mounted, else LittleFS
// (/data/skills - the flash tier keeps working with no card).
constexpr const char* kSdRoot    = "/mem/skills";
constexpr const char* kFlashRoot = "/data/skills";
// The largest stored skill on the flash tier (LittleFS holds the store there
// alongside everything else; the SD tier has no limit).
constexpr size_t kFlashMaxStored = 8;

// DeviceSkillFs - the five file operations of the portable seam (skill_store.h)
// over the device's FS. Every operation takes the storage lock and stages big
// reads through PSRAM (never a large internal-SRAM block). memory::Lock is
// recursive, so the whole-op locks in the public API below are safe.
class DeviceSkillFs : public agent::SkillFs {
 public:
  bool read(const std::string& path, size_t cap, std::string& out) override {
    agent::memory::Lock g;
    File f = agent::memory::dataFs().open(path.c_str(), FILE_READ);
    if (!f) return false;
    size_t n = f.size();
    if (n > cap) n = cap;
    char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); return false; }
    size_t got = f.read((uint8_t*)buf, n);
    f.close();
    out.assign(buf, got);
    heap_caps_free(buf);
    return true;
  }
  bool writeAtomic(const std::string& path, const std::string& text) override {
    agent::memory::Lock g;
    fs::FS& fs = agent::memory::dataFs();
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) mkdirs(fs, path.substr(0, slash));
    const std::string tmp = path + ".tmp";
    File f = fs.open(tmp.c_str(), FILE_WRITE);
    if (!f) return false;
    const size_t n = f.write((const uint8_t*)text.data(), text.size());
    f.close();
    if (n != text.size()) { fs.remove(tmp.c_str()); return false; }
    fs.remove(path.c_str());   // a reader sees the old file or the new one, never torn
    return fs.rename(tmp.c_str(), path.c_str());
  }
  std::vector<std::string> listDirs(const std::string& dir) override {
    std::vector<std::string> out;
    agent::memory::Lock g;
    File d = agent::memory::dataFs().open(dir.c_str());
    if (!d || !d.isDirectory()) return out;
    for (File e; (e = d.openNextFile());) {
      if (e.isDirectory()) {
        const char* full = e.name();   // may be the full path or the bare name
        const char* slash = strrchr(full, '/');
        out.push_back(slash ? slash + 1 : full);
      }
      e.close();
    }
    d.close();
    return out;
  }
  bool exists(const std::string& path) override {
    agent::memory::Lock g;
    return agent::memory::dataFs().exists(path.c_str());
  }
  bool remove(const std::string& path) override {
    agent::memory::Lock g;
    return agent::memory::dataFs().remove(path.c_str());
  }
  bool rename(const std::string& from, const std::string& to) override {
    agent::memory::Lock g;
    fs::FS& fs = agent::memory::dataFs();
    fs.remove(to.c_str());
    return fs.rename(from.c_str(), to.c_str());
  }
  void removeDir(const std::string& dir) override {   // best effort
    agent::memory::Lock g;
    agent::memory::dataFs().rmdir(dir.c_str());
  }

 private:
  static void mkdirs(fs::FS& fs, const std::string& dir) {
    size_t p = 0;
    while ((p = dir.find('/', p + 1)) != std::string::npos)
      fs.mkdir(dir.substr(0, p).c_str());
    fs.mkdir(dir.c_str());
  }
};

DeviceSkillFs g_fs;
agent::SkillStore* g_store = nullptr;   // constructed on first use (the tier is known then)
agent::AttachTable g_attach;            // RAM only, lost on restart by design

// Device clock as "YYYY-MM-DD HH:MM" ("" before SNTP sync - honest absence
// beats a 1970 stamp).
std::string nowStamp() {
  time_t t = time(nullptr);
  if (t < 1600000000) return std::string();   // clock not synced yet
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900,
           tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  return buf;
}

// The store's config for the ACTIVE tier; constructs the store on first use.
agent::SkillStore& storeFor() {
  if (!g_store) {
    agent::SkillStoreConfig cfg;
    cfg.root = agent::memory::haveSd() ? kSdRoot : kFlashRoot;
    cfg.sourceSlug = agent::memory::haveSd() ? "sd" : "flash";
    cfg.fileCap = 8192;
    cfg.pendingCap = 4;
    cfg.maxStored = agent::memory::haveSd() ? 0 : kFlashMaxStored;
    g_store = new agent::SkillStore(g_fs, cfg, &nowStamp);
  }
  return *g_store;
}

}  // namespace

bool sdAvailable() { return agent::memory::haveSd(); }

agent::SkillStore& store() { return storeFor(); }

// The switch file lives on internal flash on EVERY tier (LittleFS is always
// mounted), so pulling the SD card never changes the owner's trust setting.
bool selfApproveOn() {
  if (LittleFS.totalBytes() == 0) return false;   // not mounted
  File f = LittleFS.open(agent::kSkillSelfApproveFile, FILE_READ);
  if (!f) return false;
  std::string text;
  text.resize(f.size() > 32 ? 32 : f.size());
  const size_t got = text.empty() ? 0 : f.read((uint8_t*)text.data(), text.size());
  f.close();
  text.resize(got);
  return agent::skillSelfApproveFromFile(text);
}

bool setSelfApprove(bool on) {
  if (LittleFS.totalBytes() == 0) return false;   // not mounted: the switch stays as-is
  if (!on) return LittleFS.remove(agent::kSkillSelfApproveFile) || true;
  File f = LittleFS.open(agent::kSkillSelfApproveFile, FILE_WRITE);
  if (!f) return false;
  const size_t n = f.write((const uint8_t*)"1", 1);
  f.close();
  return n == 1;
}

// ---- the engine's attach surface ----------------------------------------------

std::string indexText() { return storeFor().indexText(); }

std::vector<agent::AttachPick> picksFor(const std::string& chatId) {
  // noteTurn (idle expiry) rides the pick: the table expires lazily here, the
  // same rule the engine's RAM tests pin.
  g_attach.noteTurn(chatId, millis());
  return agent::selectAttach(g_attach.stickyFor(chatId), storeFor().list());
}

std::string attachBody(const std::string& id) {
  // CUM-523: a built-in body follows the turn's one sample (the tool surface
  // serves the same side).
  return storeFor().liveBody(id, agent::SkillSide{agent::orchestrator::telegramForTurn(),
                                                  agent::orchestrator::fileSendForTurn()});
}

void noteUsed(const std::string& chat, const std::string& id) {
  g_attach.noteUsed(chat, id, millis());
}

void dropChat(const std::string& chat) { g_attach.dropChat(chat); }

// ---- the tool and web surface ---------------------------------------------------

std::vector<agent::SkillMeta> list() { return storeFor().list(); }

std::string get(const std::string& id, bool* trusted) {
  return storeFor().get(id, agent::SkillSide{agent::orchestrator::telegramForTurn(),
                                             agent::orchestrator::fileSendForTurn()},
                        trusted);
}

std::string spawnCapsule(const std::string& id) { return storeFor().spawnCapsule(id); }

std::string raw(const std::string& id) { return storeFor().raw(id); }

std::string rawNext(const std::string& id) { return storeFor().rawNext(id); }

int pendingCount() { return storeFor().pendingCount(); }

// Every whole-store op holds the storage lock across its read-modify-write
// (recursive, so the fs seam's inner locks re-enter harmlessly). Without this,
// an agent skill.save landing between the read and the write of an approve
// would be overwritten - the approve-then-mutate bypass the store's rules
// exist to stop.
agent::SkillResult save(const std::string& id, const std::string& md,
                        const agent::SkillCaller& who) {
  agent::memory::Lock g;
  const agent::SkillResult r = storeFor().save(id, md, who, selfApproveOn());
  if (r.ok)
    alogf("skills: %s %s rev %d (outcome %s)", who.owner ? "owner" : "assistant",
          id.c_str(), r.rev, agent::skillOutcomeSlug(r.outcome));
  else
    alogf("skills: refused %s by %s (%s)", id.c_str(), who.owner ? "owner" : "assistant",
          r.err.c_str());
  return r;
}

agent::SkillResult approve(const std::string& id, const agent::SkillCaller& who) {
  agent::memory::Lock g;
  return storeFor().approve(id, who);
}
agent::SkillResult deny(const std::string& id, const agent::SkillCaller& who) {
  agent::memory::Lock g;
  return storeFor().deny(id, who);
}
agent::SkillResult discard(const std::string& id, const agent::SkillCaller& who) {
  agent::memory::Lock g;
  return storeFor().discard(id, who);
}
agent::SkillResult remove(const std::string& id, const agent::SkillCaller& who) {
  agent::memory::Lock g;
  return storeFor().remove(id, who);
}

}  // namespace agent::skills
