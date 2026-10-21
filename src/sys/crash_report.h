#pragma once
// crash_report - the device seam for boot and crash forensics (crash forensics F2/F5,
// crash UX row 42). The portable halves are host-tested in lib/core:
// nimbus/sys/reset_reason.h, nimbus/sys/boot_record.h, nimbus/crash_summary.h and
// nimbus/orch/turn_journal.h.
//
// Boot order (main.cpp setup()):
//   1. bootRecord() right after the OTA boot guard and NVS open: the first durable line
//      of every boot ("boot: n=? fw=... reason=<slug>(0x<raw>) t=... prev=..."). It
//      waits in the durable log's early backlog until storage attaches.
//   2. reportOnce() runs when storage attaches (errlog::onAttached, the SD mount or the
//      LittleFS mount in memory::begin()), and once more at the end of setup() for a
//      unit with no storage: reads the core dump once, emits crash:, crash-bt: and
//      crash-why:, dedupes through /log/crash.seen. It never erases the image.
//   3. collectTurnJournal() once LittleFS is mounted, then queueCrashNotice() after the
//      Telegram task starts (Orchestrator mode): after a crash boot, one plain-words
//      message to the owner naming the request that was in flight (/crash/turn.json),
//      sent through the reply queue. A crash that cut a background turn (a routine or
//      background task) is held and sent once at the owner's next turn instead
//      (nimbus/orch/restart_notice.h); a crash during an owner request still sends at boot.
//
// Everything runs on the boot task or the turn task: no new task, no TLS, no NVS writes.

#include <Arduino.h>      // String (the test-build console hook)
#include <ArduinoJson.h>

#include <cstdint>
#include <string>

#include "nimbus/sys/breadcrumb.h"

namespace nimbus::crash {

// RTC breadcrumb (F4). captureBreadcrumb() runs first thing in setup(): it reads where
// the previous run stopped (the boot line's prev= field) and starts this run's record.
// stage() records a boot stage or turn step; breadcrumbTick() (loop) refreshes the
// uptime and heap low-water at most every 5 s.
void captureBreadcrumb();
void stage(nimbus::sys::Stage s, const char* mark = nullptr);
void breadcrumbTick();

// Capture this boot's facts and emit the boot line. `orch` = Orchestrator mode.
void bootRecord(bool orch);

// Read the core dump (once) and emit the crash lines. Idempotent.
void reportOnce();

// This boot's reset reason (esp_reset_reason_t value) and raw ROM code.
int resetReason();
uint32_t rawResetCode();
bool crashBoot();   // isCrashReset(resetReason())

// " crash=tg_poll@0x4201abcd \"...\"" for the ev:boot row ("" when there is no dump).
std::string bootRowSuffix();

// The /api/state "boot" object: n, reason, raw, t, crash{...} (null when no dump).
void fillBootState(JsonObject o);

// Crash UX journal, written on the turn task.
void turnStarted(int source, const std::string& chat, const std::string& textHead);
void turnRound(int round);
void turnEnded();
// The turn's first user text, noted before turnStarted fires (only its head is kept).
void noteTurnText(const std::string& text);

// Read and clear /crash/turn.json (every boot, before the turn task starts, so a
// journal left by a power cut never names a later crash and a new turn's journal is
// never taken for the old one).
void collectTurnJournal();
// After a crash boot: queue the owner notice through the reply queue (call once the
// Telegram task is up), naming the turn collectTurnJournal() found. A crash that cut
// a background turn is held and sent once at the owner's next turn; anything else
// sends at boot (nimbus/orch/restart_notice.h).
void queueCrashNotice();

#ifdef NIMBUS_TEST
// Test builds only: the test console's `PANIC <kind> arm=turn [round=N]` hook
// (Hooks::panicArmTurn). Arms a panic (nullptr store, abort, or a tg_poll stack
// overflow) that fires at the transport's body write in round N of the next turn; the
// arm is dropped if that turn ends first. Spec and decision: nimbus/sys/panic_arm.h.
// Returns the console reply. A release gate checks no production image links these.
String armTurnPanic(const String& spec);
// The transport's hook, right before the body write: fires an armed panic.
void testPanicPoint();
#endif

}  // namespace nimbus::crash
