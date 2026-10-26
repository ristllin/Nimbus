#pragma once
#include <ArduinoJson.h>

#include <cstdint>

#include "nimbus/heap_watch.h"

// heap_watch (device glue) - the internal-SRAM evidence the device keeps. The pure
// pieces (ring, ladder, tags, spawn gate) live in lib/core nimbus/heap_watch.h and
// are host-tested; this file wires them to the ESP-IDF heap.
//
//   install()   setup(), right after the PSRAM routing: registers the failed-
//               allocation hook and takes the previous run's records out of the
//               RTC ring (they survive a panic, watchdog or software reset).
//   pump(now)   loop(): at 1 Hz, the low-water ladder, the resting-block sample and
//               the ring drain into rate-limited log lines.
//   Scope       a one-line activity tag at a call site (relay dial, vision, ...).
//   spawnGate() a worker task asks before xTaskCreate (never during a turn, and only
//               with room for its stack plus 16 KB).
//
// No new task and no NVS: the ring lives in RTC no-init memory, everything else in RAM.
namespace nimbus::heapwatch {

void install();
void pump(uint32_t nowMs);

// Failed allocations (any task) since boot. Exact: the hook counts every one, the
// rate limit applies only to the log lines.
uint32_t allocFails();
// The newest failure this boot; false when there was none.
bool lastFail(FailRecord& out);

// Activity tags. Prefer Scope; touch() marks a tag for the next sample only.
void enter(Tag t);
void leave(Tag t);
void touch(Tag t);
class Scope {
 public:
  explicit Scope(Tag t) : t_(t) { enter(t_); }
  ~Scope() { end(); }
  // End the activity before the scope does (idempotent).
  void end() {
    if (live_) leave(t_);
    live_ = false;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Tag t_;
  bool live_ = true;
};

// The largest internal block at rest: the minimum of the last idle samples (no turn,
// no tagged activity), or the live value before the first idle sample.
uint32_t restingLargest();

// The worker-spawn gate (sites: provider verify, cloud key mint, voice catalog, safety
// report). Logs a refusal with its reason. Ok means the caller may xTaskCreate now.
SpawnVerdict spawnGate(uint32_t stackBytes, const char* who);
// The boot form (the orchestrator boot task): no turn exists yet, so it asks only
// for the stack plus kBootSpawnMarginBytes. A null `who` refuses without a log line
// (the boot's retries log once, not every attempt). An overload, not a new name: the
// L40 source guard requires every worker spawn site to call spawnGate(.
SpawnVerdict spawnGate(uint32_t stackBytes, const char* who, bool atBoot);

// PSRAM-first small allocations for the length of a turn (the L24 fix,
// nimbus/harness/turn_memory.h): on drops the general-malloc spill limit to 0, so
// every plain malloc/new goes to PSRAM first; off restores the boot limit
// (kSmallAllocInternalLimit). A no-op on a unit without PSRAM. Explicit
// MALLOC_CAP_INTERNAL/DMA allocations (Wi-Fi, lwIP, drivers) are never affected.
void psramFirst(bool on);

// The local-minimum monitor around one transport exchange (inside the TLS work slot
// only: it is one global monitor). monitorStart() returns false when another exchange
// already holds it; pass that result to monitorStop(), which returns the local minimum
// free (UINT32_MAX when this exchange did not hold the monitor).
bool monitorStart();
uint32_t monitorStop(bool started);

// The concurrent work-TLS detector (the bench proof of the single-slot rule): the
// transport's exchange, the relay's dial and each relay big-frame chunk mark their TLS
// I/O; a mark while another is live counts one overlap (and logs "tls: overlap").
// With the relay inside the work slot it stays 0; FAULT relay_noslot makes it move.
void tlsIoBegin(const char* who);
void tlsIoEnd();
uint32_t tlsOverlaps();

// The low-water mark (UINT32_MAX before the first sample).
uint32_t lowWater();

// The orchestrator boot task's deepest point: it reports its own free-stack
// watermark (bytes) and stack size just before it ends.
void noteOrchBootStack(uint32_t minFreeBytes, uint32_t stackBytes);

// The boot ladder's stack floors (release-v451x bisect): reads the loopTask
// high-water mark once, after the orchestrator boot, and the orchboot watermark
// that task reported, writes one floor line each (healthy AND fault - the bench leg
// greps them) and records the verdict for /api/state. Returns true when either is
// under its floor (a BOOT FAULT).
bool bootStackFloorCheck();
// The verdict bootStackFloorCheck() recorded (false before it ran).
bool bootStackFault();

// /api/state.mem: lowWater, lowWaterTags, lowWaterUp (s), allocFails, allocFailLast,
// restingLargest, tlsOverlaps, bootStackFault, and the stack high-water marks the
// stack-health suite reads (loopStackMin, panelStackMin, vrelStackMin, and
// orchBootStackMin, the boot task's watermark when it reported one).
void stateInto(JsonObject mem);

}  // namespace nimbus::heapwatch
