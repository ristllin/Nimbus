#include "orch_persist.h"
#include "../sys/agent_log.h"
#include "../sys/config_nvs.h"  // notePersist: the legacy journal erase is reported
#include "../sys/ota_update.h"  // imageValidAtBoot - when the legacy journal may go
#include "fs_atomic.h"          // writeFileAtomic - tmp + rename, honors erasing()

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <nvs.h>         // the legacy journal records: read-only, quiet when absent

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

// ---- FlashJournalStore --------------------------------------------------------
// The journal lives in /data/agjournal.json (nimbus::orch::FileJournalStore). The
// legacy NVS records are only READ here (once, to migrate) and erased once retired:
// no journal write ever spends settings-storage entries again.

static const char* kJournalPath = "/data/agjournal.json";
static const char* kJournalNs = "agjournal";

static void slotKey(int slot, char out[4]) {
  out[0] = 'j';
  out[1] = (char)('0' + slot);
  out[2] = 0;
}

bool FlashJournalIo::readFile(std::string& out) {
  if (!flashMounted() || !LittleFS.exists(kJournalPath)) return false;   // no [E] open line
  File f = LittleFS.open(kJournalPath, FILE_READ);
  if (!f) return false;
  String s = f.readString();
  f.close();
  out.assign(s.c_str(), s.length());
  return true;
}

bool FlashJournalIo::writeFileAtomic(const std::string& v) {
  LittleFS.mkdir("/data");
  return agent::writeFileAtomic(kJournalPath, v, "agjournal");
}

// Read-only through the IDF API, which (unlike Preferences) logs nothing for a
// namespace or key that is absent: a fresh device reads all six here on every boot
// until its first sub-agent creates the file.
std::string FlashJournalIo::readLegacySlot(int slot) {
  nvs_handle_t h;
  if (nvs_open(kJournalNs, NVS_READONLY, &h) != ESP_OK) return "";   // nothing to migrate
  char key[4]; slotKey(slot, key);
  std::string v;
  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 1) {
    v.resize(len);
    if (nvs_get_str(h, key, &v[0], &len) == ESP_OK) v.resize(len - 1);   // drop the NUL
    else v.clear();
  }
  nvs_close(h);
  return v;
}

// An erase only frees entries, so it lands on a full partition too. A namespace that
// was never created is left uncreated (a read-write open would add its entry).
bool FlashJournalIo::eraseLegacy() {
  nvs_handle_t h;
  if (nvs_open(kJournalNs, NVS_READONLY, &h) != ESP_OK) return true;   // nothing left
  nvs_close(h);
  Preferences p;
  if (!p.begin(kJournalNs, /*readOnly=*/false)) return false;
  const bool ok = nimbus::sys::notePersist(p.clear(), kJournalNs);
  p.end();
  return ok;
}

void FlashJournalStore::begin() {
  using nimbus::orch::JournalSource;
  const JournalSource src = load();
  if (src == JournalSource::Legacy)
    alogf("journal: moved the sub-agent records to flash (%s)",
          durable() ? "saved" : "not saved yet - kept in settings storage");
  // Running records an earlier release left in NVS after this file (a rollback, then an
  // update back): added to the file; the NVS copy goes on a later boot.
  if (src == JournalSource::Merged)
    alogf("journal: added running records an earlier release left in settings storage (%s)",
          durable() ? "saved" : "not saved yet");
  if (src == JournalSource::File && retireLegacy(otaupd::imageValidAtBoot()))
    alog("journal: legacy settings-storage copy removed (the file is in use)");
}

}  // namespace agent
