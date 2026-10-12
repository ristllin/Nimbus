#include "skills.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "memory_subsystem.h"          // dataFs() + haveSd() + Lock (SD serialization)
#include "orchestrator.h"              // telegramForTurn - which side of a built-in to serve
#include "../sys/agent_log.h"
#include "nimbus/harness/builtin_skills.h"   // the built-in capsules (portable, host-tested)
#include "nimbus/harness/skill_md.h"   // portable parseSkillMd (host-tested)

namespace agent::skills {

namespace {

// ---- SD capsules ------------------------------------------------------------
const char*  kSkillsDir  = "/mem/skills";
// File read cap: capsules are small by contract; the spawn injection is further
// capped at 4 KB by the portable composer.
constexpr size_t kFileCap = 8192;

// The Directive/PendingSpawn skill field is char[24] - an id longer than 23
// bytes could never round-trip through a spawn, so refuse it at save time.
constexpr size_t kIdMax = 23;

bool validId(const std::string& id) {
  if (id.empty() || id.size() > kIdMax) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

std::string mdPath(const std::string& id) {
  return std::string(kSkillsDir) + "/" + id + "/SKILL.md";
}

// Read <id>/SKILL.md through a PSRAM staging buffer (never a big internal-SRAM
// block), capped at kFileCap. "" when absent/unreadable. Caller holds no lock;
// this takes memory::Lock around the SD I/O (the one mutex serializing the bus).
std::string readMd(const std::string& id) {
  if (!memory::haveSd() || !validId(id)) return std::string();
  agent::memory::Lock g;
  File f = memory::dataFs().open(mdPath(id).c_str(), FILE_READ);
  if (!f) return std::string();
  size_t n = f.size();
  if (n > kFileCap) n = kFileCap;
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (!buf) { f.close(); return std::string(); }
  size_t got = f.read((uint8_t*)buf, n);
  f.close();
  std::string out(buf, got);
  heap_caps_free(buf);
  return out;
}

}  // namespace

bool sdAvailable() { return memory::haveSd(); }

std::vector<Capsule> list() {
  std::vector<Capsule> out;
  for (size_t i = 0; i < builtinSkillCount(); i++) {
    const BuiltinSkill& b = builtinSkill(i);
    out.push_back(Capsule{b.id, b.title, b.desc, "", "builtin", "builtin", true});
  }
  if (!memory::haveSd()) return out;

  // Scan /mem/skills/<id>/SKILL.md. Directory walk under the Lock; the per-file
  // header read re-takes it recursively (memory::Lock is recursive).
  std::vector<std::string> ids;
  {
    agent::memory::Lock g;
    File dir = memory::dataFs().open(kSkillsDir);
    if (dir && dir.isDirectory()) {
      File e;
      while ((e = dir.openNextFile())) {
        if (e.isDirectory()) {
          const char* full = e.name();          // may be full path or bare name
          const char* slash = strrchr(full, '/');
          std::string id = slash ? slash + 1 : full;
          if (validId(id)) ids.push_back(id);
        }
        e.close();
      }
    }
    if (dir) dir.close();
  }
  for (const auto& id : ids) {
    std::string md = readMd(id);
    if (md.empty()) continue;                   // dir without a SKILL.md
    SkillMd doc = parseSkillMd(md);
    Capsule c;
    c.id       = id;
    c.title    = doc.title.empty() ? id : doc.title;
    c.desc     = doc.desc;   // "" falls back to title in indexText
    c.version  = doc.version;
    c.source   = "sd";
    c.origin   = doc.createdBy;      // "user" | "agent" (parser clamps)
    c.approved = doc.approved;
    // SD wins on an id collision with a built-in (owner override).
    bool replaced = false;
    for (auto& b : out)
      if (b.id == id) { b = c; replaced = true; break; }
    if (!replaced) out.push_back(std::move(c));
  }
  return out;
}

std::string indexText() {
  std::vector<agent::SkillIndexEntry> es;
  for (const auto& c : list()) {
    agent::SkillIndexEntry e;
    e.id      = c.id;
    e.desc    = !c.desc.empty() ? c.desc : c.title;   // desc, else title, else id
    e.pending = !c.approved;
    es.push_back(std::move(e));
  }
  return agent::skillsIndexText(es);
}

std::string get(const std::string& id) {
  // SD first (SD wins on collision), then the built-ins.
  std::string md = readMd(id);
  if (!md.empty()) return parseSkillMd(md).body;
  // CUM-523: a built-in names reply.telegram / files.send only on a turn that was
  // offered them (the turn's one sample; outside a turn, the device's standing answer).
  return builtinSkillBody(id, agent::orchestrator::telegramForTurn(),
                          agent::orchestrator::fileSendForTurn());
}

std::string spawnCapsule(const std::string& id) {
  // Only owner-authored SD capsules inject at spawn, and only when their front
  // matter says inject: spawn|both. Built-ins stay pure provider hints - the
  // pre-P2 behavior - so existing spawn flows are byte-identical.
  std::string md = readMd(id);
  if (md.empty()) return std::string();
  SkillMd doc = parseSkillMd(md);
  if (doc.inject == "context") return std::string();
  // INERT-PENDING (v4.0.0): an unapproved agent capsule never rides a spawn
  // brief - a persistent instruction blob is a prompt-injection channel until
  // the owner approves it (web or /skill approve). skill.get read-back still
  // works; injection is the gated surface.
  if (!doc.approved) return std::string();
  return doc.body;
}

std::string raw(const std::string& id) { return readMd(id); }

bool reservedId(const std::string& id) { return isBuiltinSkill(id); }

int pendingAgentCount() {
  int n = 0;
  for (const auto& c : list())
    if (c.origin == "agent" && !c.approved) n++;
  return n;
}

// Device clock as "YYYY-MM-DD HH:MM" ("" before SNTP sync - honest absence
// beats a 1970 stamp).
static std::string nowStamp() {
  time_t t = time(nullptr);
  if (t < 1600000000) return std::string();   // clock not synced yet
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900,
           tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  return std::string(buf);
}

// Write canonical text atomically (tmp + rename, writeBlobAtomic discipline).
static bool writeMd(const std::string& id, const std::string& text, std::string& err) {
  agent::memory::Lock g;
  fs::FS& fs = memory::dataFs();
  fs.mkdir(kSkillsDir);
  fs.mkdir((std::string(kSkillsDir) + "/" + id).c_str());
  std::string path = mdPath(id), tmp = path + ".tmp";
  File f = fs.open(tmp.c_str(), FILE_WRITE);
  if (!f) { err = "SD open failed"; return false; }
  size_t n = f.write((const uint8_t*)text.data(), text.size());
  f.close();
  if (n != text.size()) { fs.remove(tmp.c_str()); err = "SD write failed"; return false; }
  fs.remove(path.c_str());
  if (!fs.rename(tmp.c_str(), path.c_str())) { err = "SD rename failed"; return false; }
  return true;
}

bool save(const std::string& id, const std::string& md, std::string& err, bool byAgent) {
  if (!validId(id)) { err = "bad id (a-z 0-9 - _ , max 23 chars)"; return false; }
  if (md.empty())   { err = "empty capsule"; return false; }
  if (md.size() > kFileCap) { err = "capsule too large (8 KB max)"; return false; }
  if (!memory::haveSd()) { err = "no SD card - dynamic skills need the SD tier"; return false; }
  if (byAgent && reservedId(id)) {
    err = "'" + id + "' is a built-in skill - pick a different id";
    return false;
  }
  // Parse whatever was handed in, keep the CONTENT fields, SERVER-STAMP the
  // origin, and re-emit canonical front matter - a model-supplied
  // `approved: true` or `created_by: user` never reaches disk (the portable
  // composeSkillMd round-trip is the sanitization, host-tested).
  SkillMd doc = parseSkillMd(md);
  doc.createdBy = byAgent ? "agent" : "user";
  // Agent saves are ALWAYS pending - including re-saves of an approved capsule
  // (approve-then-mutate must not bypass review). Owner saves imply approval.
  doc.approved  = !byAgent;
  doc.createdAt = nowStamp();
  const std::string canon = composeSkillMd(doc);
  if (canon.size() > kFileCap) { err = "capsule too large (8 KB max)"; return false; }
  if (!writeMd(id, canon, err)) return false;
  alogf("skills: saved %s (%u bytes, by=%s%s)", id.c_str(), (unsigned)canon.size(),
        byAgent ? "agent" : "user", byAgent ? " PENDING" : "");
  return true;
}

bool approve(const std::string& id, std::string& err) {
  // Hold the Lock across the whole read-modify-write (it is recursive, so the
  // inner readMd/writeMd re-take it harmlessly). Without this, an agent
  // skill.save landing between our read and our write would be OVERWRITTEN by
  // the approve - an approved capsule with a body the owner never reviewed,
  // exactly the approve-then-mutate bypass the save() stamping exists to stop.
  agent::memory::Lock g;
  std::string md = readMd(id);
  if (md.empty()) { err = "no such skill on SD"; return false; }
  SkillMd doc = parseSkillMd(md);
  if (doc.approved) return true;                 // idempotent
  doc.approved = true;
  if (!writeMd(id, composeSkillMd(doc), err)) return false;
  alogf("skills: approved %s", id.c_str());
  return true;
}

bool remove(const std::string& id, std::string& err, bool byAgent) {
  if (!validId(id)) { err = "bad id"; return false; }
  if (!memory::haveSd()) { err = "no SD card"; return false; }
  // Lock spans the origin check AND the delete (recursive; same reasoning as
  // approve()) - the check must not run against a file another task replaces
  // before the delete lands.
  agent::memory::Lock g;
  if (byAgent) {
    // The model may delete ONLY what it authored; user capsules are the
    // owner's (web UI). Read the origin from disk - never trust the caller.
    std::string md = readMd(id);
    if (md.empty()) { err = "no such skill on SD"; return false; }
    if (parseSkillMd(md).createdBy != "agent") {
      err = "'" + id + "' was created by the owner - only they can delete it";
      return false;
    }
  }
  fs::FS& fs = memory::dataFs();
  if (!fs.exists(mdPath(id).c_str())) { err = "no such skill on SD"; return false; }
  fs.remove(mdPath(id).c_str());
  fs.rmdir((std::string(kSkillsDir) + "/" + id).c_str());   // best-effort
  alogf("skills: removed %s%s", id.c_str(), byAgent ? " (by agent)" : "");
  return true;
}

}  // namespace agent::skills
