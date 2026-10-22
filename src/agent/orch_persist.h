#pragma once
#include "nimbus/orch/compact.h"   // FoldStoreIO (v3.6.0 fold state)
#include "nimbus/orch/journal.h"
#include "nimbus/orch/memory.h"

// orch_persist - the DEVICE persistence implementations for the portable
// orchestrator seams (nimbus::orch::MemoryStore / JournalStore). The portable
// core (lib/core) keeps the cap + dedupe + prompt logic host-testable behind these
// interfaces; here we back them with the actual device stores:
//   - LittleFsMemoryStore : the model-memory blob on LittleFS (/data/orchmem.txt),
//     matching Nuage-Solide orch_memory.cpp's path so a device upgrade keeps its
//     memory.
//   - FlashJournalStore : the sub-agent journal in one flash file
//     (/data/agjournal.json, nimbus::orch::FileJournalStore), so in-flight jobs and
//     the finished-job ring survive a restart without spending settings storage. It
//     reads the legacy NVS records (namespace "agjournal", keys "j0".."j5", the
//     Nuage-Solide layout) once to migrate, and erases them only on a boot whose
//     image was already marked valid.
namespace agent {

class LittleFsMemoryStore : public nimbus::orch::MemoryStore {
 public:
  std::string loadModel() override;
  void        saveModel(const std::string& v) override;
  void        clearModel() override;
};

// The device's journal file I/O: LittleFS for the file, the legacy NVS namespace
// read-only (and erased once retired).
class FlashJournalIo : public nimbus::orch::JournalFileIo {
 public:
  bool        readFile(std::string& out) override;
  bool        writeFileAtomic(const std::string& v) override;
  std::string readLegacySlot(int slot) override;
  bool        eraseLegacy() override;
};

// The I/O is a private base listed first, so it is constructed before the store that
// keeps a reference to it (a member would not exist yet when the base is built).
class FlashJournalStore : private FlashJournalIo, public nimbus::orch::FileJournalStore {
 public:
  FlashJournalStore() : nimbus::orch::FileJournalStore(static_cast<FlashJournalIo&>(*this)) {}
  // Load the journal (file, else the legacy NVS records), then retire the legacy
  // copy when the moved-store rule allows. Call once, after LittleFS is mounted.
  void begin();
};

// v3.7.0 RBAC tenant table - /data/tenants.txt. A pre-v3.7 image ignores the
// file, and adoptLegacy() rebuilds it from tgOwners/tgAllow if it is missing,
// so a rollback (or a first boot after upgrade) is never a lockout.
struct LittleFsTenantStoreIO {
  static std::string load();
  static void        save(const std::string& blob);
  // Same write, but REPORTS failure. A role change that did not reach the disk
  // must not answer 200 OK: the in-RAM table would then disagree with what the
  // next boot loads, and a demotion the admin believes landed would silently
  // undo itself on restart.
  static bool saveChecked(const std::string& blob);
};
}  // namespace agent

namespace agent {
// v3.6.0 fold state - /data/chatsum.txt. A pre-v3.6 image never reads this file,
// so an OTA rollback is safe by construction (plan revision 2).
struct LittleFsFoldStoreIO : nimbus::orch::FoldStoreIO {
  std::string load() override;
  void        save(const std::string& blob) override;
};
}  // namespace agent
