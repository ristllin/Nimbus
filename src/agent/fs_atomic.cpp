#include "fs_atomic.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "../sys/agent_log.h"
#include "memory_subsystem.h"   // erasing() - the factory-sweep write barrier

namespace agent {

// Write a whole small blob so that a power loss can never leave a TRUNCATED one.
//
// Losing power in the truncate window used to cost the device every chat's fold
// summary, or - since v3.7.0 - every tenant's role, which is worse: a device with no
// admin row is a device nobody can administer. The short write is also reported
// rather than silently accepted; a full filesystem previously looked exactly like a
// successful save.
bool writeFileAtomic(const char* path, const std::string& blob, const char* tag,
                     bool logFailure) {
  // Factory-erase barrier (release-gate finding 2026-10-04): a persist racing the
  // sweep would recreate tenants/chatsum/orchmem after /data was erased, handing
  // the next owner the old RBAC table. Refusing here is safe: on the success path
  // the device restarts into an empty store, and on the refusal path the caller's
  // in-RAM state persists again on the next normal mutation.
  if (agent::memory::erasing()) return false;
  std::string tmp = std::string(path) + ".tmp";
  {
    File f = LittleFS.open(tmp.c_str(), FILE_WRITE);
    if (!f) {
      if (logFailure) alogf("%s: LittleFS open failed", tag);
      return false;
    }
    const size_t n = f.write((const uint8_t*)blob.data(), blob.size());
    f.close();
    if (n != blob.size()) {
      if (logFailure)
        alogf("%s: short write %u/%u - keeping the previous copy", tag, (unsigned)n,
              (unsigned)blob.size());
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
    if (logFailure) alogf("%s: rename failed", tag);
    LittleFS.remove(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace agent
