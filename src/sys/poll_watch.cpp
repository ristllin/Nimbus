#include "poll_watch.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_attr.h>   // RTC_NOINIT_ATTR
#include <esp_debug_helpers.h>
#include <esp_memory_utils.h>
#include <esp_private/freertos_debug.h>   // vTaskGetSnapshot: the frame a switched-out task saved
#include <esp_system.h>
#include <esp_rom_sys.h>   // esp_rom_delay_us

#include <atomic>
#include <string>

#if __has_include(<xtensa_context.h>) && __has_include(<esp_cpu_utils.h>)
#include <esp_cpu_utils.h>    // esp_cpu_process_stack_pc
#include <xtensa_context.h>   // XtExcFrame: what a switched-out task saved
#define NIMBUS_POLLWATCH_FRAMES 1
#endif

#include "agent_log.h"
#include "nimbus/sys/breadcrumb.h"   // crc32
#include "nimbus/sys/poll_guard.h"

namespace nimbus::pollwatch {
namespace {

using sys::PollLiveness;
using sys::PollStall;
using sys::PollStallRecord;
using sys::RunState;

RTC_NOINIT_ATTR sys::PollStallStore s_stallRtc;
PollStallRecord g_rec(s_stallRtc);
PollLiveness g_live;
std::atomic<TaskHandle_t> g_task{nullptr};
bool g_booted = false;
bool g_bootAfterStall = false;
uint8_t g_streakAtBoot = 0;
uint32_t g_lastCheckMs = 0;

uint32_t buildId() {
  const esp_app_desc_t* d = esp_app_get_description();
  return sys::crc32(d->app_elf_sha256, sizeof(d->app_elf_sha256));
}

RunState runState(TaskHandle_t t) {
  if (!t) return RunState::Gone;
  switch (eTaskGetState(t)) {
    case eRunning:   return RunState::Running;
    case eReady:     return RunState::Ready;
    case eBlocked:   return RunState::Blocked;
    case eSuspended: return RunState::Suspended;
    default:         return RunState::Gone;
  }
}

// Nothing in the firmware suspends a task (test_ota_poll_guard pins that class), so the
// frame is read at a moment the task is off its CPU: a blocked task stays that way, and a
// spinning one at priority 3 is preempted by the radio and TCPIP tasks of its core every
// few milliseconds. Polled tightly for at most 100 ms on the loop task.
bool offCpu(TaskHandle_t t) {
  for (int i = 0; i < 5000; ++i) {
    const eTaskState st = eTaskGetState(t);
    if (st == eDeleted || st == eInvalid) return false;
    if (st != eRunning) return true;
    esp_rom_delay_us(20);
  }
  return false;
}

// The stuck task's PC and caller chain, read from the frame it saved when it was last
// switched out (the moment above). It may run again while the frame is read, so every
// step is checked (a sane stack pointer, an executable PC) before it is followed: a frame
// that moved yields a short or empty chain, never a bad read.
size_t readFrames(TaskHandle_t t, uint32_t& pc, uint32_t* bt) {
  pc = 0;
#ifdef NIMBUS_POLLWATCH_FRAMES
  if (!offCpu(t)) return 0;
  TaskSnapshot_t snap;
  if (vTaskGetSnapshot(t, &snap) != pdTRUE || !snap.pxTopOfStack) return 0;
  const XtExcFrame* f = reinterpret_cast<const XtExcFrame*>(snap.pxTopOfStack);
  if (!esp_stack_ptr_is_sane((uint32_t)f)) return 0;
  esp_backtrace_frame_t fr = {(uint32_t)f->pc, (uint32_t)f->a1, (uint32_t)f->a0, f};
  if (!esp_ptr_executable((void*)esp_cpu_process_stack_pc(fr.pc))) return 0;
  pc = esp_cpu_process_stack_pc(fr.pc);
  size_t n = 0;
  while (n < sys::kPollBacktraceMax) {
    bt[n++] = esp_cpu_process_stack_pc(fr.pc);
    if (fr.next_pc == 0 || !esp_stack_ptr_is_sane(fr.sp) || !esp_backtrace_get_next_frame(&fr)) break;
  }
  return n;
#else
  (void)t;
  (void)bt;
  return 0;
#endif
}

[[noreturn]] void restartFor(const PollStall& s, TaskHandle_t t, uint32_t limitMs) {
  uint32_t pc = 0;
  uint32_t bt[sys::kPollBacktraceMax] = {};
  size_t n = 0;
  if (t && !s.gone) n = readFrames(t, pc, bt);
  g_rec.noteStall(s);
  // The durable line takes the card lock for at most 8 ms: when the stuck task holds the
  // card, the line stays in the RAM tail only, and the reason below still survives in the
  // core dump and comes back as the next boot's crash lines.
  agent::alogcf(errlog::cat::kBoot, "%s", sys::pollStallLogLine(s, g_rec.streak(), limitMs, true).c_str());
  const std::string why = sys::pollStallReason(s, pc, bt, n);
  agent::alogcf(errlog::cat::kBoot, "%s", why.c_str());
  // A panic, not esp_restart(): the poison guard counts a try against the Telegram work
  // in flight (a message that stalls the device twice is skipped) and the core dump
  // keeps every task's stack, the stuck one included.
  esp_system_abort(why.c_str());
}

}  // namespace

void boot() {
  if (g_booted) return;
  g_booted = true;
  g_bootAfterStall = g_rec.boot(buildId());
  g_streakAtBoot = g_rec.streak();
  if (g_bootAfterStall)
    agent::alogcf(errlog::cat::kBoot, "telegram: this boot follows a poll-task stall restart (%u in a row)",
                  (unsigned)g_streakAtBoot);
}

void arm(TaskHandle_t task) {
  boot();
  g_task.store(task, std::memory_order_relaxed);
  g_live.arm(millis());
}

void disarm() {
  g_live.disarm();
  g_task.store(nullptr, std::memory_order_relaxed);   // never ask a deleted task its state
}

void beat(const char* step) {
  const TaskHandle_t t = g_task.load(std::memory_order_relaxed);
  if (t && xTaskGetCurrentTaskHandle() == t) g_live.beat(millis(), step);
}

void tick() {
  const uint32_t now = millis();
  if (g_lastCheckMs && now - g_lastCheckMs < 1000) return;
  g_lastCheckMs = now;
  if (!g_live.armed()) return;
  // A healthy half hour ends a streak of guard restarts.
  if (g_rec.streak() && now >= sys::kPollHealthyMs && g_live.silentMs(now) < sys::kPollSampleAfterMs)
    g_rec.noteHealthy();
  const TaskHandle_t t = g_task.load(std::memory_order_relaxed);
  const uint32_t limitMs = g_rec.limitMs();
  PollStall s;
  if (!g_live.check(now, runState(t), limitMs, s)) return;
  restartFor(s, t, limitMs);
}

bool bootAfterStall() { return g_bootAfterStall; }
uint8_t streakAtBoot() { return g_streakAtBoot; }
uint32_t silentS() { return g_live.armed() ? g_live.silentMs(millis()) / 1000u : 0; }
const char* lastStep() { return g_live.lastStep(); }

}  // namespace nimbus::pollwatch
