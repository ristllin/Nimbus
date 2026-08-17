#include "orch_persist.h"
#include "../sys/agent_log.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

// Device persistence for the portable orchestrator seams. Paths + NVS layout match
// Nuage-Solide (orch_memory.cpp / journal.cpp) so state survives a firmware upgrade.

namespace agent {

// Write a whole small blob so that a power loss can never leave a TRUNCATED one.
//
// LittleFS FILE_WRITE truncates on open, so the previous content is gone the
// instant the file is opened and only reappears if every byte lands. Losing
// power in that window used to cost the device every chat's fold summary, or -
// since v3.7.0 - every tenant's role, which is worse: a device with no admin
// row is a device nobody can administer. Writing to a sibling .tmp and renaming
// makes the swap atomic, so a reader sees either the old blob or the new one.
//
// The short write is also now reported rather than silently accepted; a full
// filesystem previously looked exactly like a successful save.
static bool writeFileAtomic(const char* path, const std::string& blob, const char* tag) {
  std::string tmp = std::string(path) + ".tmp";
  {
    File f = LittleFS.open(tmp.c_str(), FILE_WRITE);
    if (!f) { alogf("%s: LittleFS open failed", tag); return false; }
    const size_t n = f.write((const uint8_t*)blob.data(), blob.size());
    f.close();
    if (n != blob.size()) {
      alogf("%s: short write %u/%u - keeping the previous copy",
            tag, (unsigned)n, (unsigned)blob.size());
      LittleFS.remove(tmp.c_str());
      return false;
    }
  }
  // NO remove-before-rename. littlefs's rename commits over an existing target
  // as part of the same operation, so removing first only opens a window where
  // the file is ABSENT - strictly worse than the truncate it replaced. For
  // tenants.txt that window is a security hole: a crash inside it leaves no
  // table, adoptLegacy rebuilds from the allowlist, and a revoked chat comes
  // back as a User.
  if (!LittleFS.rename(tmp.c_str(), path)) {
    alogf("%s: rename failed", tag);
    LittleFS.remove(tmp.c_str());
    return false;
  }
  return true;
}

// ---- LittleFsMemoryStore ----------------------------------------------------

static const char* kMemPath = "/data/orchmem.txt";

std::string LittleFsMemoryStore::loadModel() {
  File f = LittleFS.open(kMemPath, FILE_READ);
  if (!f) return "";
  String s = f.readString();
  f.close();
  return std::string(s.c_str(), s.length());
}

void LittleFsMemoryStore::saveModel(const std::string& v) {
  writeFileAtomic(kMemPath, v, "orchmem");
}

static const char* kFoldPath = "/data/chatsum.txt";
static const char* kTenantPath = "/data/tenants.txt";

std::string LittleFsTenantStoreIO::load() {
  File f = LittleFS.open(kTenantPath, FILE_READ);
  if (!f) return "";
  String s = f.readString();
  f.close();
  return std::string(s.c_str(), s.length());
}

bool LittleFsTenantStoreIO::saveChecked(const std::string& blob) {
  return writeFileAtomic(kTenantPath, blob, "tenants");
}
void LittleFsTenantStoreIO::save(const std::string& blob) {
  // Kept for the interface; callers that can report failure use saveChecked.
  writeFileAtomic(kTenantPath, blob, "tenants");
}

std::string LittleFsFoldStoreIO::load() {
  File f = LittleFS.open(kFoldPath, FILE_READ);
  if (!f) return "";
  String s = f.readString();
  f.close();
  return std::string(s.c_str(), s.length());
}

void LittleFsFoldStoreIO::save(const std::string& blob) {
  writeFileAtomic(kFoldPath, blob, "chatsum");
}

void LittleFsMemoryStore::clearModel() {
  LittleFS.remove(kMemPath);
}

// ---- NvsJournalStore --------------------------------------------------------

static Preferences s_prefs;
static bool        s_prefsOpen = false;
static const char* kJournalNs = "agjournal";

static void slotKey(int slot, char out[4]) {
  out[0] = 'j';
  out[1] = (char)('0' + slot);
  out[2] = 0;
}

void NvsJournalStore::begin() {
  if (s_prefsOpen) return;
  s_prefsOpen = s_prefs.begin(kJournalNs, false);
  if (!s_prefsOpen) alog("journal: NVS open failed");
}

std::string NvsJournalStore::get(int slot) {
  if (!s_prefsOpen) return "";
  char key[4]; slotKey(slot, key);
  String v = s_prefs.getString(key, "");
  return std::string(v.c_str(), v.length());
}

void NvsJournalStore::put(int slot, const std::string& v) {
  if (!s_prefsOpen) return;
  char key[4]; slotKey(slot, key);
  s_prefs.putString(key, v.c_str());
}

void NvsJournalStore::remove(int slot) {
  if (!s_prefsOpen) return;
  char key[4]; slotKey(slot, key);
  s_prefs.remove(key);
}

void NvsJournalStore::clearNs() {
  if (!s_prefsOpen) return;
  s_prefs.clear();   // wipe the whole agjournal namespace
}

}  // namespace agent
