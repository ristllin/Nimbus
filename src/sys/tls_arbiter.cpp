#include "tls_arbiter.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "nimbus/sys/web_guard.h"   // slotWaitMs: the web task's bounded slot wait
#include "web_task.h"                 // onWebTask

// agent::store::tlsSlots() - boot-latched slot count, and agent::memory::noteNetworkStart
// - the card-lock check every TLS request passes. Forward-declared so this sys-level TU
// carries no include dependency on the agent subsystem.
namespace agent { namespace store { int tlsSlots(); } }
namespace agent { namespace memory { void noteNetworkStart(uintptr_t caller); } }

// Ported from Nuage-Solide src/tls_arbiter.cpp (Head Orchestrator v2), extended
// for the S3 + 8 MB PSRAM (v2.0.0): the work slot is now a COUNTING semaphore.
// With mbedTLS calloc routed to PSRAM and the sub-4 KB malloc churn spilled to
// PSRAM (main.cpp), two concurrent work-TLS sessions fit comfortably - the
// remaining internal cost per session is lwIP sockets + task-stack transients.
// Default is 1 slot (store::tlsSlots(), NVS `tlsSlots`, web-tunable 1..2) - the
// classic single-slot survival mode. MEASURED 2026-08-05: a 2nd concurrent
// work-TLS beside a heavy fan-out turn collapsed the head turn's largest
// contiguous internal block below what its send needed and the turn failed;
// serializing to 1 makes it complete. 2 trades that reliability for throughput
// (an STT upload no longer serializes behind a turn) - for a board with headroom.
//
// The slot count is latched at begin() (boot/mode-switch): a live change would
// strand or mint permits on a semaphore with takers in flight. The web UI says
// "applies after reboot".

namespace agent {
namespace arbiter {

static SemaphoreHandle_t g_sem = nullptr;
static int g_slots = 1;

void begin() {
  if (g_sem) return;               // idempotent (mode switch may call twice)
  g_slots = store::tlsSlots();     // 1..2, default 1 (clamped in store)
  g_sem = xSemaphoreCreateCounting(g_slots, g_slots);
}

// Every work TLS session starts here, so this is where two rules are checked
// (nimbus/sys/web_guard.h): a request started while holding the card lock is counted
// and named, and the web task (watched by the task watchdog) waits for the slot at
// most kWebSlotWaitMs - a turn holds it for minutes.
bool acquireWork(uint32_t timeoutMs) {
  agent::memory::noteNetworkStart((uintptr_t)__builtin_return_address(0));
  if (!g_sem) return true;         // arbiter not started => no contention
  const uint32_t waitMs = nimbus::sys::slotWaitMs(nimbus::sys::onWebTask(), timeoutMs);
  return xSemaphoreTake(g_sem, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void releaseWork() {
  if (g_sem) xSemaphoreGive(g_sem);
}

int slots() { return g_sem ? g_slots : 0; }

}  // namespace arbiter
}  // namespace agent
