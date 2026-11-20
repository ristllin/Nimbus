#include "orchestrator_common.h"

#include "../sys/agent_log.h"
#include "orch_persist.h"   // LittleFsTenantStoreIO
#include "store.h"
#include "nimbus/orch/loop_delivery.h"  // telegramOwnerOf (roleOfChat's rule, host-tested)

// orchestrator_common - the engine-neutral half of the device seam (lane RB-SEAM1a, no
// behaviour change): tenants, job glue, ring arcs, voice, status getters and reply sinks,
// moved here from orchestrator.cpp so a second engine can be bound beside the old one.
// orchestrator.cpp keeps the old engine's half; what the two share is declared in
// orchestrator_common.h. The public surface is orchestrator.h, unchanged.

namespace agent {
namespace orchestrator {

// v3.7.0 RBAC: the tenant table (roles + quotas). Unlike the rest of the
// orchestrator state this genuinely has TWO writers - the web surface
// (/api/tenant, AsyncTCP) and the assistant's tenant.* tools (tg_poll, or
// AsyncTCP again via POST /mcp) - and both need a SYNCHRONOUS answer, because
// "that is the only admin" must come back as a 409 rather than land silently a
// second later. So it gets a real mutex instead of the staged-flag pattern: a
// push_back reallocating the vector under a concurrent find() is a
// use-after-free, not a skewed counter.
//
// Every access goes through the tenant* helpers below, which return values, not
// references - nothing may hold a pointer into the table across a task switch.
nimbus::orch::TenantStore g_tenants;
// STATIC allocation, not lazy creation. "if (!mux) mux = create()" is itself a
// race: two tasks reaching it together both create one, each takes a different
// mutex, and the lock silently stops excluding anything - the exact bug this
// lock exists to prevent, now invisible. A static mutex needs no heap and
// exists before the first task runs.
static StaticSemaphore_t g_tenantMuxBuf;
SemaphoreHandle_t g_tenantMux =
    xSemaphoreCreateRecursiveMutexStatic(&g_tenantMuxBuf);
// TenantLock is in orchestrator_common.h (roleOfChat, in orchestrator.cpp, takes it too).

// The whole principal for a chat (namespace, role and quota) from the tenant table:
// the one answer to "who is this chat" for the turn (ApplyDeps) and for the
// sub-agents it starts (JobEngine), so the two can never disagree (CUM-828).
nimbus::orch::Principal chatPrincipal(const std::string& chatId) {
  nimbus::orch::Quota q;
  tenantQuotaOf(chatId, q);
  return nimbus::orch::principalForRole(chatId, roleOfChat(String(chatId.c_str())), q);
}


// ---- v3.7.0 tenant table ----------------------------------------------------
static std::vector<std::string> splitIds(const String& csv) {
  std::vector<std::string> out;
  int start = 0;
  while (start < (int)csv.length()) {
    int end = csv.indexOf(',', start);
    if (end < 0) end = csv.length();
    String one = csv.substring(start, end);
    one.trim();
    if (one.length()) out.push_back(std::string(one.c_str()));
    start = end + 1;
  }
  return out;
}

void loadTenants() {
  TenantLock lk;
  g_tenants.load(LittleFsTenantStoreIO::load());
  // First boot after the upgrade (or after a rollback wrote nothing): rebuild
  // from the legacy lists so nobody's access changes by surprise and the device
  // always has an admin.
  if (g_tenants.all().empty()) {
    g_tenants.adoptLegacy(splitIds(store::telegramOwners()),
                          splitIds(store::telegramAllowlist()));
    LittleFsTenantStoreIO::save(g_tenants.dump());
    alogf("rbac: adopted %u legacy tenants (%u admin)",
          (unsigned)g_tenants.all().size(), (unsigned)g_tenants.adminCount());
  }
}

void persistTenants() {
  TenantLock lk;
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump()))
    alog("rbac: tenant table did not persist - roles may revert on restart");
}


// ---- tenant mutations (locked; every surface goes through these) -------------
// Each takes the lock, mutates, persists, and returns a plain value. Nothing
// hands out a reference into the table, so a caller on another task cannot hold
// a pointer across a reallocation.

std::vector<nimbus::orch::Tenant> tenantSnapshot(size_t* adminsOut) {
  TenantLock lk;
  if (adminsOut) *adminsOut = g_tenants.adminCount();
  return g_tenants.all();                      // by value, under the lock
}

bool tenantQuotaOf(const std::string& chatId, nimbus::orch::Quota& out) {
  TenantLock lk;
  const nimbus::orch::Tenant* t = g_tenants.find(chatId);
  if (!t) return false;
  out = t->quota;
  return true;
}

bool tenantSetRole(const std::string& chatId, nimbus::orch::Role r, std::string& err) {
  TenantLock lk;
  if (!g_tenants.setRole(chatId, r, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}

void tenantApproved(const std::string& chatId, bool telegramOwner) {
  TenantLock lk;
  std::string err;
  if (!nimbus::orch::noteApproved(g_tenants, chatId, telegramOwner, err)) {
    if (!err.empty()) alogf("rbac: approved %s without a row: %s", chatId.c_str(), err.c_str());
    return;
  }
  if (LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) return;
  g_tenants.load(LittleFsTenantStoreIO::load());   // RAM never disagrees with storage
  alogf("rbac: approved %s, its row did not persist", chatId.c_str());
}

bool tenantSetQuota(const std::string& chatId, const nimbus::orch::Quota& q,
                    std::string& err) {
  TenantLock lk;
  if (!g_tenants.setQuota(chatId, q, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}

bool tenantRemove(const std::string& chatId, std::string& err) {
  TenantLock lk;
  // Removing the ROW revokes access and frees a slot; it deliberately does NOT
  // erase what the person stored. An admin can still read those namespaces, and
  // silently destroying data as a side effect of a role change would be a nasty
  // surprise. Deleting the data is a separate, explicit act.
  if (!g_tenants.remove(chatId, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}


}  // namespace orchestrator
}  // namespace agent
