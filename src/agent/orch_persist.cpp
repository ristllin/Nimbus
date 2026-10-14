#include "orch_persist.h"
#include "../sys/agent_log.h"
#include "fs_atomic.h"          // writeFileAtomic - tmp + rename, honors erasing()

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

// Device persistence for the portable orchestrator seams. Paths + NVS layout match
// Nuage-Solide (orch_memory.cpp / journal.cpp) so state survives a firmware upgrade.
// Whole-blob LittleFS writes go through writeFileAtomic (fs_atomic.h).

namespace agent {

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
