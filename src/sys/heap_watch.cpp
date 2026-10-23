#include "heap_watch.h"

#include <Arduino.h>
#include <esp_attr.h>          // RTC_NOINIT_ATTR, IRAM_ATTR
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "agent_log.h"

// The orchestrator's turn flag. Forward-declared so this sys-level unit carries no
// include dependency on the agent subsystem (the tls_arbiter pattern).
namespace agent { namespace orchestrator { bool turnInFlight(); } }

namespace nimbus::heapwatch {

// The portable caps names must match the IDF bits.
static_assert(MALLOC_CAP_DMA == (1u << 3) && MALLOC_CAP_INTERNAL == (1u << 11) &&
                  MALLOC_CAP_SPIRAM == (1u << 10) && MALLOC_CAP_DEFAULT == (1u << 12) &&
                  MALLOC_CAP_8BIT == (1u << 2) && MALLOC_CAP_32BIT == (1u << 1) &&
                  MALLOC_CAP_EXEC == (1u << 0) && MALLOC_CAP_IRAM_8BIT == (1u << 13) &&
                  MALLOC_CAP_RTCRAM == (1u << 15),
              "capsString() names drifted from esp_heap_caps.h");

namespace {

// The failure ring. RTC no-init memory survives a panic, watchdog, software or USB
// reset and deep sleep (not a power cycle: the CRC rejects that garbage). ~236 B.
RTC_NOINIT_ATTR FailRing s_ring;
portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

bool s_installed = false;
// PSRAM, not internal: the previous run's records until pump() reports them, and the
// drain's working copy of the ring (~236 B each).
FailRing* s_prev = nullptr;
FailRing* s_snap = nullptr;
uint32_t s_drained = 0;    // ring records already turned into log lines
LineBudget s_budget;
uint32_t s_unshown = 0;    // lines the budget or the ring wrap kept out, not yet reported

Activity s_act;
portMUX_TYPE s_stateMux = portMUX_INITIALIZER_UNLOCKED;   // ladder + resting window
Ladder s_ladder;
RestingLargest s_resting;
uint32_t s_lastPumpMs = 0;
bool s_pumped = false;

std::atomic<bool> s_monitorOn{false};
std::atomic<int> s_tlsLive{0};
std::atomic<uint32_t> s_tlsOverlaps{0};

uint32_t intFree() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
uint32_t intLargest() { return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); }

// The hook. It runs in the allocating context: any task, or an interrupt, right after
// the heap gave up (the heap lock is already released). It must not allocate, log or
// block, and must stay in IRAM: everything it calls is IRAM-resident in this build
// (FreeRTOS and the heap are not placed in flash; the ring push is header-inline and
// table-free). In an interrupt it skips the heap walk and records "isr".
void IRAM_ATTR onAllocFailed(size_t size, uint32_t caps, const char* /*function*/) {
  const bool isr = xPortInIsrContext();
  uint32_t freeB = 0, largest = 0;
  if (!isr) {
    freeB = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  }
  const char* task = isr ? "isr" : pcTaskGetName(nullptr);
  const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  portENTER_CRITICAL_SAFE(&s_ringMux);
  ringPush(s_ring, FailSample{ms, (uint32_t)size, caps, freeB, largest}, task);
  portEXIT_CRITICAL_SAFE(&s_ringMux);
}

void snapshotRing(FailRing& out) {
  portENTER_CRITICAL(&s_ringMux);
  out = s_ring;
  portEXIT_CRITICAL(&s_ringMux);
}

// Once per boot: the failures the previous run recorded before it reset (the
// evidence a panic leaves when no log line made it to the card).
void reportPrevious() {
  if (!s_prev) return;
  FailRecord recs[kRingSize];
  uint32_t lost = 0;
  const size_t n = ringSince(*s_prev, 0, recs, lost);
  agent::alogf("heap: %u alloc failure(s) before the last reset (newest %u follow)",
               (unsigned)s_prev->total, (unsigned)n);
  for (size_t i = 0; i < n; ++i) agent::alogf("%s (before reset)", failLine(recs[i]).c_str());
  heap_caps_free(s_prev);
  s_prev = nullptr;
}

// Turn new ring records into log lines, a few per minute; the count stays exact.
void drainRing(uint32_t now) {
  if (!s_snap) return;
  if (s_ring.total == s_drained && !s_unshown) return;   // nothing new (one aligned read)
  FailRing& snap = *s_snap;
  snapshotRing(snap);
  if (snap.total == s_drained) {
    if (s_unshown && s_budget.take(now)) {
      agent::alogf("heap: %u more alloc failure line(s) not shown (rate limit or ring wrap); "
                   "count=%u", (unsigned)s_unshown, (unsigned)snap.total);
      s_unshown = 0;
    }
    return;
  }
  FailRecord recs[kRingSize];
  uint32_t lost = 0;
  const size_t n = ringSince(snap, s_drained, recs, lost);
  s_drained = snap.total;
  s_unshown += lost;
  for (size_t i = 0; i < n; ++i) {
    if (s_budget.take(now)) agent::alogf("%s count=%u", failLine(recs[i]).c_str(), (unsigned)snap.total);
    else ++s_unshown;
  }
}

uint32_t sampleTags() {
  uint32_t tags = s_act.sample();
  if (agent::orchestrator::turnInFlight()) tags |= 1u << kTurn;
  return tags;
}

// The 1 Hz half: the ladder on the all-time minimum (a monitor window reads its
// local minimum instead, which can only add a true new low), and an idle sample for
// the resting block.
void sampleOnce(uint32_t now) {
  const uint32_t tags = sampleTags();
  const uint32_t minFree = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const bool idle = tags == 0;
  const uint32_t largestNow = idle ? intLargest() : 0;
  portENTER_CRITICAL(&s_stateMux);
  const LadderStep st = ladderNote(s_ladder, minFree, tags, now);
  const Ladder copy = s_ladder;
  if (idle) s_resting.note(largestNow);
  portEXIT_CRITICAL(&s_stateMux);
  if (st.log) agent::alogf("%s", ladderLine(copy, st.was, intFree(), intLargest()).c_str());
}

}  // namespace

void install() {
  if (s_installed) return;
  // The previous run's records, if this reset kept RTC memory (power-on fails the CRC).
  auto psram = [] {
    void* p = heap_caps_malloc(sizeof(FailRing), MALLOC_CAP_SPIRAM);
    return static_cast<FailRing*>(p ? p : heap_caps_malloc(sizeof(FailRing), MALLOC_CAP_8BIT));
  };
  s_snap = psram();
  if (ringValid(s_ring) && s_ring.total > 0 && (s_prev = psram()) != nullptr) *s_prev = s_ring;
  ringReset(s_ring);
  s_drained = 0;
  heap_caps_register_failed_alloc_callback(onAllocFailed);
  s_installed = true;
}

void pump(uint32_t nowMs) {
  if (!s_installed) return;
  if (s_pumped && (uint32_t)(nowMs - s_lastPumpMs) < 1000) return;
  s_pumped = true;
  s_lastPumpMs = nowMs;
  reportPrevious();
  sampleOnce(nowMs);
  drainRing(nowMs);   // up to 8 records a second are kept; the budget allows 6 lines a minute
}

uint32_t allocFails() {
  portENTER_CRITICAL(&s_ringMux);
  const uint32_t n = s_ring.total;
  portEXIT_CRITICAL(&s_ringMux);
  return n;
}

bool lastFail(FailRecord& out) {
  portENTER_CRITICAL(&s_ringMux);
  const bool any = s_ring.total > 0;
  if (any) out = s_ring.rec[(s_ring.total - 1) % kRingSize];
  portEXIT_CRITICAL(&s_ringMux);
  return any;
}

void enter(Tag t) { s_act.enter(t); }
void leave(Tag t) { s_act.leave(t); }
void touch(Tag t) { s_act.touch(t); }

uint32_t restingLargest() {
  const uint32_t live = intLargest();
  portENTER_CRITICAL(&s_stateMux);
  const uint32_t r = s_resting.resting(live);
  portEXIT_CRITICAL(&s_stateMux);
  return r;
}

SpawnVerdict spawnGate(uint32_t stackBytes, const char* who) {
  const uint32_t freeB = intFree();
  const SpawnVerdict v = spawnVerdict(stackBytes, freeB, agent::orchestrator::turnInFlight());
  if (v != SpawnVerdict::Ok)
    agent::alogf("%s: worker not started (%s; internal free %u, needs %u)", who ? who : "worker",
                 spawnVerdictName(v), (unsigned)freeB, (unsigned)(stackBytes + kSpawnMarginBytes));
  return v;
}

bool monitorStart() {
  bool expected = false;
  if (!s_monitorOn.compare_exchange_strong(expected, true)) return false;
  heap_caps_monitor_local_minimum_free_size_start();
  return true;
}

uint32_t monitorStop(bool started) {
  if (!started) return UINT32_MAX;
  const uint32_t localMin = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  heap_caps_monitor_local_minimum_free_size_stop();   // restores the all-time minimum
  s_monitorOn = false;
  return localMin;
}

void tlsIoBegin(const char* who) {
  if (s_tlsLive.fetch_add(1) > 0) {
    const uint32_t n = s_tlsOverlaps.fetch_add(1) + 1;
    agent::alogf("tls: overlap %s (count=%u)", who ? who : "?", (unsigned)n);
  }
}

void tlsIoEnd() {
  int v = s_tlsLive.load();
  while (v > 0 && !s_tlsLive.compare_exchange_weak(v, v - 1)) {
  }
}

uint32_t tlsOverlaps() { return s_tlsOverlaps.load(); }

uint32_t lowWater() {
  portENTER_CRITICAL(&s_stateMux);
  const uint32_t v = s_ladder.low;
  portEXIT_CRITICAL(&s_stateMux);
  return v;
}

// ---- the boot ladder's loopTask stack floor ----------------------------------

static bool s_bootStackFault = false;

bool bootStackFloorCheck() {
  const TaskHandle_t t = xTaskGetHandle("loopTask");
  const uint32_t min = t ? (uint32_t)uxTaskGetStackHighWaterMark(t) : 0;
  s_bootStackFault = loopStackUnderFloor(min);
  agent::alogf("%s", loopStackFloorLine(min, t != nullptr).c_str());
  return s_bootStackFault;
}

bool bootStackFault() { return s_bootStackFault; }

void stateInto(JsonObject mem) {
  portENTER_CRITICAL(&s_stateMux);
  const Ladder l = s_ladder;
  portEXIT_CRITICAL(&s_stateMux);
  // Before the first 1 Hz sample the mark is the live all-time minimum.
  const uint32_t low =
      l.low == UINT32_MAX ? (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) : l.low;
  mem["lowWater"] = low;
  mem["lowWaterTags"] = tagsString(l.lowTags);
  mem["lowWaterUp"] = l.lowUpMs / 1000;
  mem["allocFails"] = allocFails();
  FailRecord last;
  if (lastFail(last)) {
    char b[120];
    snprintf(b, sizeof(b), "size=%u caps=%s task=%.7s up=%us", (unsigned)last.size,
             capsString(last.caps).c_str(), last.task, (unsigned)(last.ms / 1000));
    mem["allocFailLast"] = b;
  }
  mem["restingLargest"] = restingLargest();
  mem["tlsOverlaps"] = tlsOverlaps();
  mem["bootStackFault"] = s_bootStackFault;   // the boot ladder's loopTask floor check
  // Stack high-water marks (bytes: StackType_t is uint8_t on this port) for the
  // tasks the stack-health suite reads: the main loop, the display render task and
  // the voice release watcher.
  if (TaskHandle_t t = xTaskGetHandle("loopTask")) mem["loopStackMin"] = (uint32_t)uxTaskGetStackHighWaterMark(t);
  if (TaskHandle_t t = xTaskGetHandle("tft")) mem["panelStackMin"] = (uint32_t)uxTaskGetStackHighWaterMark(t);
  if (TaskHandle_t t = xTaskGetHandle("vrel")) mem["vrelStackMin"] = (uint32_t)uxTaskGetStackHighWaterMark(t);
}

}  // namespace nimbus::heapwatch
