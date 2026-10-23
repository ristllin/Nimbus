#pragma once
#include <string>

#include "nimbus/orch/owner_prefs.h"   // OwnerPrefs
#include "nimbus/orch/prefs_tools.h"   // PrefsToolContext

// prefs_subsystem - the device's standing-rules glue (lane J): the store over
// /data/prefs.txt on the internal flash (LittleFS on every tier, zero NVS
// keys), the tool context the prefs.edit tool and the /prefs command share,
// and the first-boot seed that lifts the owner's always/never rules out of
// memory and lists them once.
//
// Concurrency: the store is touched by the turn task and the AsyncTCP web
// task, so every access runs under the shared memory::Lock (recursive). The
// store is small (<= 12 rules); nothing here holds the lock across I/O beyond
// the one atomic file write of a <= 2 KB blob.
namespace agent {
namespace prefs {

// Load the store (idempotent; call once after LittleFS is mounted).
void begin();

// The one store. Access it under memory::Lock (withStore does that for you).
nimbus::orch::OwnerPrefs& store();

// Run fn with the store under the lock. false = the file area is not mounted:
// nothing ran.
bool withStore(const std::function<void(nimbus::orch::OwnerPrefs&)>& fn);

// The device-wired context: scheduled turns refuse, a turn that read outside
// content adds pending, the owner is notified through Telegram.
nimbus::orch::PrefsToolContext context();

// The [STANDING RULES] body for one chat's role ("" when the rules are not
// the owner's business - a member or guest turn never sees them).
std::string blockForRole(bool ownerFacing);

// The device-enforced filters for one chat's role (the delivery seam reads
// this; 0 = none).
uint32_t filtersForRole(bool ownerFacing);

// The first-boot seed: when /data/prefs.txt does not exist yet, lift the
// owner's always/never rules out of long-term memory (pinned entries and any
// entry that reads as a standing rule or default) and the scratchpad's long
// tier, add them as active rules, and send the owner one plain-words listing.
// Marks the file seeded even when nothing qualified, so it never runs again.
// `devName` (the device's own name) sharpens the classifier; "" is fine.
void seedFirstBoot(const std::string& devName);

}  // namespace prefs
}  // namespace agent
