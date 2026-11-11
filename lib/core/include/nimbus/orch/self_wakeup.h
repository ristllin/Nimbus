#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/loops.h"
#include "nimbus/orch/turn.h"

// Self-wakeups - the owner design (9 Nov ruling, binding; 11 Nov ruling on
// approval, binding): when a background turn (the nightly upkeep, a routine,
// a sub-agent result) has something worth telling the owner DURING QUIET HOURS,
// it sets ONE self-wakeup through the existing routines scheduler (a Once loop
// record - no new task, no new machinery). The wakeup:
//   - needs NO owner approval (routines do; a single self-wakeup does not -
//     11 Nov ruling): it is created approved, bounded so it cannot be abused:
//     at most ONE pending per kind, due within 24 h (at the end of quiet hours),
//     its only job is to re-check and deliver that one note, one short turn,
//     no tools that change anything;
//   - is listed with the routines (it IS a loop record: /loops, the web Routines
//     tab, /api/wakeups) and the owner can cancel it like any routine;
//   - is gone after it fires (Once semantics: it retires);
//   - survives a restart (the loop table is persisted in /data/loops.json, and
//     the note rides in the record's prompt);
//   - NEVER holds a note for the owner's next message.
// At wake time the note is re-checked before anything is sent: dropped when
// stale (time-sensitive content past its window) or already delivered by a
// routine (no duplicates); otherwise sent in plain words saying what it is and
// when it was noticed.
//
// Pure core: no Arduino, host-tested (test_self_wakeup). The device glue
// (loops_subsystem, dream_subsystem) binds the clock, the quiet-hours setting
// and the scheduler table.

namespace nimbus {
namespace orch {

// --- quiet hours -------------------------------------------------------------
// Quiet hours end at the owner's quiet-hours setting, default 09:00 local
// (minuteOfDay 540). The night begins at 22:00 local (firmware constant); the
// owner's setting is the END. `minuteOfDay` is the local minute of day.
constexpr uint16_t kQuietHoursEndDefault = 540;    // 09:00 local
constexpr uint16_t kQuietHoursStart       = 22 * 60;  // 22:00 local
// A self-wakeup is due within 24 h (the end of quiet hours is a daily minute,
// so the raw delay is at most 1440 min; the clamp bounds it hard).
constexpr uint32_t kSelfWakeupMaxSec = 24u * 3600u;

// Is `minuteOfDay` inside quiet hours (from 22:00 to `quietEndMinute`)? An end
// before 22:00 is the following morning (the overnight window [22:00, end)
// wrapping midnight); an end at or past 22:00 is the same evening ([22:00, end),
// empty when equal). quietEnd is clamped to 0..1439 by the implementation.
bool inQuietHours(uint16_t minuteOfDay, uint16_t quietEndMinute);

// --- the self-wakeup kinds: one pending per kind ------------------------------
// Every background turn source maps to exactly one kind (the class test in
// lib/harness iterates every TurnSource): the nightly upkeep, a routine/loop,
// a sub-agent result (synthesis).
enum class WakeKind : uint8_t { Upkeep = 0, Routine = 1, Subagent = 2 };
constexpr int kWakeKindCount = 3;
const char* wakeKindSlug(WakeKind k);   // "upkeep" | "routine" | "subagent"

// The note a background turn wants the owner to see at the end of quiet hours.
struct WakeNote {
  WakeKind   kind = WakeKind::Upkeep;
  std::string text;           // what it is, in plain words (may be model text -
                              // it is re-checked at wake, never held for the next message)
  uint64_t   noticedEpoch = 0;   // when the background turn noticed it
  uint16_t   noticedMinute = 0;  // local minute of day then ("when it was noticed")
  uint64_t   notAfterEpoch = 0;  // 0 = no known window; else stale after this epoch
  uint64_t   dedupKey = 0;       // fnv64(text): a routine that already delivered it drops it
};

// Self-wakeup records are Once loops with this id prefix (routine ids are
// "lp"+6 hex, the reserved dream id is "dream").
inline constexpr const char* kSelfWakeupIdPrefix = "sw";
bool isSelfWakeupId(const std::string& id);

// The note envelope carried in the record's prompt (JSON, so it survives a
// restart through /data/loops.json and the wake can re-check it).
std::string dumpWakeNote(const WakeNote& n);
bool        parseWakeNote(const std::string& prompt, WakeNote& out);

// Seconds from now (epoch `nowEpoch`, local minute `nowMinute`) until the next
// local occurrence of `quietEndMinute`, clamped to [kWakeupMinSec,
// kSelfWakeupMaxSec] - so the wakeup is always due within 24 h.
uint32_t selfWakeupDelaySec(uint64_t nowEpoch, uint16_t nowMinute, uint16_t quietEndMinute);

// Arm ONE self-wakeup for `note` in `loops` (the routines scheduler's table).
// Returns the new record's id, or "" when nothing was armed:
//   - not quiet hours now (minuteOfDay vs quietEndMinute): the note is not
//     held either - the caller delivers it by its normal path;
//   - a self-wakeup of the same kind is already pending: at most one pending
//     per kind, the new note is dropped (the pending one keeps its slot).
// The record created is a Once loop: approved (no owner approval needed, 11 Nov
// ruling), enabled, named "Self-wakeup: <kind>", prompt = the note envelope,
// intervalSec = the delay to the end of quiet hours. It is listed with the
// routines, cancellable, retiring after it fires, persisted with the table.
std::string armSelfWakeup(std::vector<LoopRecord>& loops, const WakeNote& note,
                          uint64_t nowEpoch, uint16_t nowMinute, uint16_t quietEndMinute);

// Is a self-wakeup of `kind` already pending (enabled Once self-wakeup)?
bool hasPendingSelfWakeup(const std::vector<LoopRecord>& loops, WakeKind kind);

// --- the re-check at wake time ------------------------------------------------
// Stale: the note's own window has passed (notAfterEpoch != 0 and now is past
// it). A note with no known window (0) is not stale by itself - the wake turn
// re-checks time-sensitive content semantically.
bool wakeNoteStale(const WakeNote& n, uint64_t nowEpoch);
// Already delivered by a routine: some other loop's lastReplyHash equals the
// note's dedupKey (a routine that delivered the same text to the owner).
bool wakeNoteAlreadyDelivered(const WakeNote& n, const std::vector<uint64_t>& deliveredKeys);
// The delivered text: plain words saying what it is and when it was noticed.
// "Noticed at HH:MM:\n<text>" - the "when it is from" part of the 11 Nov design
// (the old "From the nightly memory upkeep:" prefix is gone).
std::string wakeupDeliveryText(const WakeNote& n);
// The re-check: "" = drop the note (stale or already delivered), else the
// delivery text. `deliveredKeys` = deliveredNoteKeys(loops, selfId).
std::string wakeupRecheck(const WakeNote& n, uint64_t nowEpoch,
                          const std::vector<uint64_t>& deliveredKeys);

// The delivered-keys view over the loop table: every NON-self-wakeup loop's
// lastReplyHash (routines record fnv64 of their reply on fire) - persisted in
// loops.json, so the duplicate check survives a restart. `exceptId` skips one
// record (the waking self-wakeup itself).
std::vector<uint64_t> deliveredNoteKeys(const std::vector<LoopRecord>& loops,
                                        const std::string& exceptId);

// The self-wakeup turn's surface: its only job is to re-check and deliver that
// one note - one short turn, NO tools that change anything. Drops every
// tool-bearing field (device, spawn, await, session_ops, mem_write, mem_query,
// scratchpad); reply and ask stay. Returns how many items were dropped.
int restrictSelfWakeupTurn(Turn& t);

}  // namespace orch
}  // namespace nimbus
