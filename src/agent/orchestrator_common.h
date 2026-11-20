#pragma once
// orchestrator_common.h - PRIVATE to the device seam: what orchestrator_common.cpp (the
// engine-neutral half: tenants, job glue, ring arcs, voice, status getters, reply sinks)
// and orchestrator.cpp (the old engine's half) share. Not for any other file: the public
// surface stays orchestrator.h, unchanged for its callers (lane RB-SEAM1a, no behaviour
// change). Both files include this header, so every shared object is declared once and
// type-checked against its one definition.
#include "orchestrator.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "nimbus/orch/rbac.h"   // TenantStore, Principal

namespace agent {
namespace orchestrator {

// ---- tenants (orchestrator_common.cpp) ------------------------------------------
// The tenant table and its recursive mutex. roleOfChat (orchestrator.cpp) reads the
// table under TenantLock; every other access is a tenant* accessor in the common file.
extern nimbus::orch::TenantStore g_tenants;
extern SemaphoreHandle_t g_tenantMux;
struct TenantLock {
  TenantLock() { xSemaphoreTakeRecursive(g_tenantMux, portMAX_DELAY); }
  ~TenantLock() { xSemaphoreGiveRecursive(g_tenantMux); }
  TenantLock(const TenantLock&) = delete;
  TenantLock& operator=(const TenantLock&) = delete;
};
// The whole principal for a chat (namespace, role and quota) from the tenant table.
nimbus::orch::Principal chatPrincipal(const std::string& chatId);

}  // namespace orchestrator
}  // namespace agent
