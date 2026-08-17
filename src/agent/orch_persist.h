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
//   - NvsJournalStore : the per-slot journal in NVS via Preferences (namespace
//     "agjournal", keys "j0".."j5"), matching Nuage-Solide journal.cpp so in-flight
//     jobs survive a reboot AND a firmware upgrade (the compact JSON blob is the
//     same shape).
namespace agent {

class LittleFsMemoryStore : public nimbus::orch::MemoryStore {
 public:
  std::string loadModel() override;
  void        saveModel(const std::string& v) override;
  void        clearModel() override;
};

class NvsJournalStore : public nimbus::orch::JournalStore {
 public:
  void begin();   // open the Preferences namespace (idempotent)
  std::string get(int slot) override;
  void        put(int slot, const std::string& v) override;
  void        remove(int slot) override;
  void        clearNs() override;
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
