// test_self_wakeup - the owner design (9 Nov ruling, binding; 11 Nov ruling on
// approval, binding): a background turn that has something worth telling the
// owner DURING QUIET HOURS arms ONE self-wakeup (a Once loop record, id prefix
// "sw") due at the end of quiet hours, instead of messaging at night or holding
// a note for the owner's next message (the Lumi v5.0.0 bug: a 03:31Z commute
// brief held and delivered at 12:33Z glued to an unrelated message).
//
// Pinned here (pure core, lib/core/orch/self_wakeup.h + the harness class rule):
//   * the quiet-hours window class (overnight [22:00, quietEnd) wrapping midnight);
//   * the exact 03:31 -> 09:00 case and the due-within-24 h bound on every arm;
//   * at most ONE pending self-wakeup per kind; not quiet hours => nothing armed
//     AND nothing held; empty note => nothing armed;
//   * the armed record's shape: Once, auto-approved (11 Nov ruling), enabled,
//     listed with the routines, prompt = the note envelope, nextRun within 24 h;
//   * the note envelope round-trips through dumpLoops/loadLoops (a restart keeps
//     the pending self-wakeup - loops.json is the persistence);
//   * the wake-time re-check: stale (window passed) or already delivered by a
//     routine (dedup key in another loop's lastReplyHash) => dropped; else the
//     delivery text says what it is and when it was noticed ("Noticed at HH:MM:"),
//     with no "nightly memory upkeep" prefix;
//   * the self-wakeup turn's surface: every tool-bearing field dropped, reply
//     and ask kept (restrictSelfWakeupTurn);
//   * buildFireRequest marks a self-wakeup record (req.selfWakeup) so the
//     executor renders its own framing;
//   * wakeKindForSource over EVERY TurnSource (a source added to the enum fails
//     the class test until classified).
#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/harness/turn_progress.h"   // wakeKindForSource / backgroundSource
#include "nimbus/orch/loop_delivery.h"       // buildFireRequest
#include "nimbus/orch/loops.h"
#include "nimbus/orch/self_wakeup.h"
#include "nimbus/orch/turn.h"

namespace orch = nimbus::orch;

void setUp() {}
void tearDown() {}

// 03:31 local on day 7, with 30 s into the minute (epoch = 7*86400 + 211*60 + 30).
static const uint64_t kAt0331 = 7ull * 86400u + 211ull * 60u + 30ull;
static const uint16_t kMin0331 = 211;
static const uint16_t kQuietEnd0900 = 540;   // the default: 09:00 local

static orch::WakeNote sampleNote(orch::WakeKind kind, const std::string& text) {
  orch::WakeNote n;
  n.kind = kind;
  n.text = text;
  n.noticedEpoch = kAt0331;
  n.noticedMinute = kMin0331;
  n.notAfterEpoch = 0;
  n.dedupKey = text.empty() ? 0 : orch::fnv64(text);
  return n;
}

// ---- the quiet-hours window class -------------------------------------------
static void test_quiet_hours_window_wraps_midnight() {
  // [22:00, 09:00) wrapping midnight: quiet from 22:00 up to 08:59, not from 09:00.
  TEST_ASSERT_TRUE(orch::inQuietHours(22 * 60, kQuietEnd0900));        // 22:00: night begins
  TEST_ASSERT_TRUE(orch::inQuietHours(23 * 60 + 59, kQuietEnd0900));   // 23:59
  TEST_ASSERT_TRUE(orch::inQuietHours(0, kQuietEnd0900));              // 00:00
  TEST_ASSERT_TRUE(orch::inQuietHours(3 * 60 + 31, kQuietEnd0900));    // 03:31 (the Lumi case)
  TEST_ASSERT_TRUE(orch::inQuietHours(8 * 60 + 59, kQuietEnd0900));    // 08:59
  TEST_ASSERT_FALSE(orch::inQuietHours(9 * 60, kQuietEnd0900));        // 09:00: quiet ends
  TEST_ASSERT_FALSE(orch::inQuietHours(12 * 60, kQuietEnd0900));       // noon
  TEST_ASSERT_FALSE(orch::inQuietHours(21 * 60 + 59, kQuietEnd0900));  // 21:59
  // A quiet-end at 22:00 is an empty window: never quiet.
  TEST_ASSERT_FALSE(orch::inQuietHours(0, 22 * 60));
  TEST_ASSERT_FALSE(orch::inQuietHours(23 * 60, 22 * 60));
  // Out-of-range inputs clamp, never wrap into a wrong verdict.
  TEST_ASSERT_TRUE(orch::inQuietHours(1500, kQuietEnd0900) == orch::inQuietHours(60, kQuietEnd0900));
  TEST_ASSERT_FALSE(orch::inQuietHours(600, 2000));   // quietEnd clamped to 1439: 10:00 not quiet
}

// ---- the exact 03:31 -> 09:00 case and the due-within-24 h bound -------------
static void test_delay_exact_case_and_24h_bound() {
  // Armed at 03:31:30 with quiet end 09:00: due at 09:00:00 = 19710 s later.
  const uint32_t d = orch::selfWakeupDelaySec(kAt0331, kMin0331, kQuietEnd0900);
  TEST_ASSERT_EQUAL_UINT32((540u - 211u) * 60u - 30u, d);   // 19710 s
  TEST_ASSERT_TRUE(d >= orch::kWakeupMinSec && d <= orch::kSelfWakeupMaxSec);
  // Every minute of the day: the delay is always within [2 min, 24 h].
  for (uint32_t m = 0; m < 1440; m += 7) {
    const uint32_t s = orch::selfWakeupDelaySec(kAt0331, (uint16_t)m, kQuietEnd0900);
    TEST_ASSERT_TRUE(s >= orch::kWakeupMinSec);
    TEST_ASSERT_TRUE(s <= orch::kSelfWakeupMaxSec);
  }
  // Armed one minute before quiet end: the raw delay is 30 s, floored to 2 min.
  const uint32_t edge = orch::selfWakeupDelaySec(kAt0331, 539, kQuietEnd0900);
  TEST_ASSERT_EQUAL_UINT32(orch::kWakeupMinSec, edge);
  // Armed at 21:59 (just before the night begins): due at 09:00 next day = 601 min.
  const uint32_t eve = orch::selfWakeupDelaySec(kAt0331, 21 * 60 + 59, kQuietEnd0900);
  TEST_ASSERT_EQUAL_UINT32((540u + 1440u - (21u * 60u + 59u)) * 60u - 30u, eve);
  TEST_ASSERT_TRUE(eve <= orch::kSelfWakeupMaxSec);
}

// ---- arming: one pending per kind, bounded, auto-approved ---------------------
static void test_arm_one_pending_per_kind_and_record_shape() {
  std::vector<orch::LoopRecord> loops;
  const std::string id1 = orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Upkeep, "brief A"),
                                              kAt0331, kMin0331, kQuietEnd0900);
  TEST_ASSERT_FALSE(id1.empty());
  TEST_ASSERT_EQUAL(1, (int)loops.size());
  const orch::LoopRecord& l = loops[0];
  TEST_ASSERT_TRUE(orch::isSelfWakeupId(l.id));
  TEST_ASSERT_EQUAL_STRING(id1.c_str(), l.id.c_str());
  TEST_ASSERT_EQUAL(orch::SchedKind::Once, l.sched.kind);
  TEST_ASSERT_TRUE(l.enabled);
  TEST_ASSERT_TRUE(l.approved);   // 11 Nov ruling: NO owner approval for a single self-wakeup
  TEST_ASSERT_EQUAL(orch::CreatedBy::Agent, l.createdBy);
  TEST_ASSERT_EQUAL_STRING("Self-wakeup: upkeep", l.name.c_str());
  TEST_ASSERT_TRUE(l.sched.intervalSec >= orch::kWakeupMinSec);
  TEST_ASSERT_TRUE(l.sched.intervalSec <= orch::kSelfWakeupMaxSec);
  TEST_ASSERT_EQUAL_UINT64(kAt0331 + l.sched.intervalSec, l.nextRun);   // due within 24 h
  TEST_ASSERT_TRUE(l.nextRun - kAt0331 <= orch::kSelfWakeupMaxSec);
  // The prompt is the note envelope and parses back to the same note.
  orch::WakeNote back;
  TEST_ASSERT_TRUE(orch::parseWakeNote(l.prompt, back));
  TEST_ASSERT_EQUAL((int)orch::WakeKind::Upkeep, (int)back.kind);
  TEST_ASSERT_EQUAL_STRING("brief A", back.text.c_str());
  TEST_ASSERT_EQUAL_UINT64(kAt0331, back.noticedEpoch);
  TEST_ASSERT_EQUAL(kMin0331, (int)back.noticedMinute);
  TEST_ASSERT_EQUAL_UINT64(orch::fnv64("brief A"), back.dedupKey);
  // A second self-wakeup of the SAME kind is dropped (at most one pending per kind).
  TEST_ASSERT_EQUAL_STRING("", orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Upkeep, "brief B"),
                                                    kAt0331, kMin0331, kQuietEnd0900).c_str());
  TEST_ASSERT_EQUAL(1, (int)loops.size());
  TEST_ASSERT_TRUE(orch::hasPendingSelfWakeup(loops, orch::WakeKind::Upkeep));
  // A different kind arms its own slot.
  const std::string id2 = orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Routine, "routine note"),
                                              kAt0331, kMin0331, kQuietEnd0900);
  const std::string id3 = orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Subagent, "sub note"),
                                              kAt0331, kMin0331, kQuietEnd0900);
  TEST_ASSERT_FALSE(id2.empty());
  TEST_ASSERT_FALSE(id3.empty());
  TEST_ASSERT_TRUE(id1 != id2 && id2 != id3 && id1 != id3);
  TEST_ASSERT_EQUAL(3, (int)loops.size());
  TEST_ASSERT_TRUE(orch::hasPendingSelfWakeup(loops, orch::WakeKind::Routine));
  TEST_ASSERT_TRUE(orch::hasPendingSelfWakeup(loops, orch::WakeKind::Subagent));
  // ... and a second of those kinds is dropped too.
  TEST_ASSERT_EQUAL_STRING("", orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Routine, "x"),
                                                    kAt0331, kMin0331, kQuietEnd0900).c_str());
  TEST_ASSERT_EQUAL(3, (int)loops.size());
}

// ---- not quiet hours: nothing armed, nothing held ----------------------------
static void test_arm_outside_quiet_hours_arms_and_holds_nothing() {
  std::vector<orch::LoopRecord> loops;
  // 10:00 local: outside quiet hours - the note is NOT held either; the caller
  // delivers it by its normal path.
  TEST_ASSERT_EQUAL_STRING("", orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Upkeep, "brief"),
                                                    kAt0331, 600, kQuietEnd0900).c_str());
  TEST_ASSERT_TRUE(loops.empty());
  // 09:00 exactly: quiet hours have ended.
  TEST_ASSERT_EQUAL_STRING("", orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Upkeep, "brief"),
                                                    kAt0331, 540, kQuietEnd0900).c_str());
  TEST_ASSERT_TRUE(loops.empty());
  // An empty note arms nothing even inside quiet hours.
  TEST_ASSERT_EQUAL_STRING("", orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Upkeep, ""),
                                                    kAt0331, kMin0331, kQuietEnd0900).c_str());
  TEST_ASSERT_TRUE(loops.empty());
}

// ---- the envelope and the table survive a restart (loops.json) ---------------
static void test_note_envelope_and_table_round_trip_through_loops_json() {
  std::vector<orch::LoopRecord> loops;
  orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Upkeep, "commute brief: leave by 07:10"),
                      kAt0331, kMin0331, kQuietEnd0900);
  // A routine with a delivered reply (its lastReplyHash is the dedup view).
  orch::LoopRecord routine;
  routine.id = "lp00000a";
  routine.name = "Morning brief";
  routine.sched.kind = orch::SchedKind::Daily;
  routine.lastReplyHash = orch::fnv64("commute brief: leave by 07:10");
  loops.push_back(routine);
  // dumpLoops -> loadLoops is the restart: the pending self-wakeup and the
  // routine's delivered key both survive.
  const std::string json = orch::dumpLoops(loops);
  std::vector<orch::LoopRecord> loaded;
  TEST_ASSERT_TRUE(orch::loadLoops(json, loaded));
  TEST_ASSERT_EQUAL(2, (int)loaded.size());
  TEST_ASSERT_TRUE(orch::hasPendingSelfWakeup(loaded, orch::WakeKind::Upkeep));
  orch::WakeNote back;
  const orch::LoopRecord* sw = nullptr;
  for (const auto& l : loaded) if (orch::isSelfWakeupId(l.id)) sw = &l;
  TEST_ASSERT_NOT_NULL(sw);
  TEST_ASSERT_TRUE(orch::parseWakeNote(sw->prompt, back));
  TEST_ASSERT_EQUAL_STRING("commute brief: leave by 07:10", back.text.c_str());
  TEST_ASSERT_EQUAL_UINT64(kAt0331, back.noticedEpoch);
  // The delivered-keys view: the routine's hash is there, the self-wakeup's own is not.
  const std::vector<uint64_t> keys = orch::deliveredNoteKeys(loaded, sw->id);
  TEST_ASSERT_EQUAL(1, (int)keys.size());
  TEST_ASSERT_EQUAL_UINT64(orch::fnv64("commute brief: leave by 07:10"), keys[0]);
}

// ---- the exact Lumi case end to end: 03:31 note, 09:00 wake -------------------
static void test_exact_case_0331_note_0900_wake() {
  // Armed at 03:31:30 local (quiet end 09:00): the record is due at 09:00:00.
  std::vector<orch::LoopRecord> loops;
  const std::string id = orch::armSelfWakeup(
      loops, sampleNote(orch::WakeKind::Upkeep, "06:00 commute brief: leave by 07:10"),
      kAt0331, kMin0331, kQuietEnd0900);
  TEST_ASSERT_FALSE(id.empty());
  const uint64_t wake = kAt0331 + 19710u;   // 09:00:00 local
  TEST_ASSERT_EQUAL_UINT64(wake, loops[0].nextRun);
  // A still-relevant note is delivered at 09:00, saying what it is and when it
  // was noticed.
  orch::WakeNote n;
  TEST_ASSERT_TRUE(orch::parseWakeNote(loops[0].prompt, n));
  TEST_ASSERT_EQUAL_STRING("Noticed at 03:31:\n06:00 commute brief: leave by 07:10",
                           orch::wakeupRecheck(n, wake, {}).c_str());
  // A stale brief (its window ended at 08:00, e.g. the delays ended) is dropped.
  orch::WakeNote stale = n;
  stale.notAfterEpoch = kAt0331 + (8u * 3600u - 211u * 60u - 30u);   // 08:00:00
  TEST_ASSERT_EQUAL_STRING("", orch::wakeupRecheck(stale, wake, {}).c_str());
  // A brief a routine already delivered (same text) is dropped: no duplicates.
  TEST_ASSERT_EQUAL_STRING("", orch::wakeupRecheck(n, wake, {n.dedupKey}).c_str());
}

// ---- the wake-ups API lists the self-wakeup and the quiet-end setting ----------
static void test_wakeups_api_lists_self_wakeup_and_quiet_end() {
  std::vector<orch::LoopRecord> loops;
  orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Upkeep, "brief"), kAt0331, kMin0331, kQuietEnd0900);
  // quietEnd passed: emitted; default: omitted (older callers unchanged).
  const std::string js = orch::dumpWakeupsApi(loops, false, kAt0331, kQuietEnd0900);
  TEST_ASSERT_TRUE(js.find("\"quietEnd\":540") != std::string::npos);
  TEST_ASSERT_TRUE(js.find("Self-wakeup: upkeep") != std::string::npos);   // listed, cancellable
  TEST_ASSERT_TRUE(js.find("\"approved\":true") != std::string::npos);     // no approval needed
  const std::string jsDef = orch::dumpWakeupsApi(loops, false, kAt0331);
  TEST_ASSERT_TRUE(jsDef.find("quietEnd") == std::string::npos);
}

// ---- the wake-time re-check ---------------------------------------------------
static void test_recheck_drops_stale_and_already_delivered() {
  orch::WakeNote n = sampleNote(orch::WakeKind::Upkeep, "06:00 commute brief: leave by 07:10");
  // Fresh: delivered, saying what it is and when it was noticed.
  const std::string text = orch::wakeupRecheck(n, kAt0331, {});
  TEST_ASSERT_EQUAL_STRING("Noticed at 03:31:\n06:00 commute brief: leave by 07:10", text.c_str());
  TEST_ASSERT_TRUE(text.find("nightly memory upkeep") == std::string::npos);   // the old prefix is gone
  // Stale: its window passed -> dropped.
  orch::WakeNote stale = n;
  stale.notAfterEpoch = kAt0331 + 3600;
  TEST_ASSERT_EQUAL_STRING("", orch::wakeupRecheck(stale, kAt0331 + 3601, {}).c_str());
  TEST_ASSERT_TRUE(orch::wakeNoteStale(stale, kAt0331 + 3601));
  TEST_ASSERT_FALSE(orch::wakeNoteStale(stale, kAt0331 + 3600));   // at the edge: not yet stale
  // A note with no known window (0) is not stale by itself.
  TEST_ASSERT_FALSE(orch::wakeNoteStale(n, kAt0331 + 30ull * 86400u));
  // Already delivered by a routine (its dedup key is in the delivered set) -> dropped.
  TEST_ASSERT_EQUAL_STRING("", orch::wakeupRecheck(n, kAt0331, {orch::fnv64("other"), n.dedupKey}).c_str());
  TEST_ASSERT_TRUE(orch::wakeNoteAlreadyDelivered(n, {n.dedupKey}));
  TEST_ASSERT_FALSE(orch::wakeNoteAlreadyDelivered(n, {orch::fnv64("other")}));
  // Empty note -> dropped.
  TEST_ASSERT_EQUAL_STRING("", orch::wakeupRecheck(sampleNote(orch::WakeKind::Upkeep, ""), kAt0331, {}).c_str());
}

// ---- the self-wakeup turn's surface: no tools that change anything ------------
static void test_restrict_self_wakeup_turn_drops_every_tool_field() {
  orch::Turn t;
  t.reply = "Noticed at 03:31:\n06:00 commute brief: leave by 07:10";
  t.ask = "";
  t.memory = "";
  t.device.push_back(orch::DeviceAction{"{\"reboot\":{}}"});
  t.spawn.push_back(orch::Spawn{});
  t.await_.push_back("job1");
  t.session_ops.push_back(orch::SessionOp{});
  t.mem_write.push_back(orch::MemWrite{});
  t.mem_query.push_back("trains");
  t.scratchpad.present = true;
  t.scratchpad.shortItems.push_back("x");
  const int dropped = orch::restrictSelfWakeupTurn(t);
  TEST_ASSERT_EQUAL(7, dropped);
  TEST_ASSERT_TRUE(t.device.empty() && t.spawn.empty() && t.await_.empty() &&
                   t.session_ops.empty() && t.mem_write.empty() && t.mem_query.empty());
  TEST_ASSERT_FALSE(t.scratchpad.present);
  // The reply (the note's delivery) stays.
  TEST_ASSERT_EQUAL_STRING("Noticed at 03:31:\n06:00 commute brief: leave by 07:10", t.reply.c_str());
  // A clean turn drops nothing.
  orch::Turn clean;
  clean.reply = "ok";
  TEST_ASSERT_EQUAL(0, orch::restrictSelfWakeupTurn(clean));
  TEST_ASSERT_EQUAL_STRING("ok", clean.reply.c_str());
}

// ---- id class ------------------------------------------------------------------
static void test_is_self_wakeup_id_class() {
  TEST_ASSERT_TRUE(orch::isSelfWakeupId("sw00a1b2"));
  TEST_ASSERT_TRUE(orch::isSelfWakeupId("swx"));
  TEST_ASSERT_FALSE(orch::isSelfWakeupId("sw"));        // prefix alone is not an id
  TEST_ASSERT_FALSE(orch::isSelfWakeupId("lp00a1b2"));  // a routine
  TEST_ASSERT_FALSE(orch::isSelfWakeupId("dream"));     // the reserved night
  TEST_ASSERT_FALSE(orch::isSelfWakeupId(""));
  TEST_ASSERT_FALSE(orch::isSelfWakeupId("s"));
}

// ---- buildFireRequest marks the record ------------------------------------------
static void test_build_fire_request_marks_self_wakeup() {
  std::vector<orch::LoopRecord> loops;
  const std::string id = orch::armSelfWakeup(loops, sampleNote(orch::WakeKind::Routine, "note"),
                                             kAt0331, kMin0331, kQuietEnd0900);
  TEST_ASSERT_FALSE(id.empty());
  orch::Delivery d;
  d.verdict = orch::DeliveryVerdict::Deliver;
  d.target = "1001";
  const orch::LoopFireRequest req = orch::buildFireRequest(loops[0], d, kAt0331);
  TEST_ASSERT_TRUE(req.selfWakeup);      // the executor renders the [SELF-WAKEUP] framing
  TEST_ASSERT_TRUE(req.once);            // Once semantics: gone after it fires
  TEST_ASSERT_FALSE(req.ownerReminder);  // agent-armed, not an owner /remind
  TEST_ASSERT_EQUAL_STRING(id.c_str(), req.id.c_str());
  // An ordinary routine is not marked.
  orch::LoopRecord routine;
  routine.id = "lp00000b";
  routine.name = "Morning";
  routine.sched.kind = orch::SchedKind::Daily;
  const orch::LoopFireRequest rreq = orch::buildFireRequest(routine, d, kAt0331);
  TEST_ASSERT_FALSE(rreq.selfWakeup);
  TEST_ASSERT_FALSE(rreq.once);
}

// ---- the kind class over every turn source --------------------------------------
static void test_wake_kind_for_source_class_over_every_turn_source() {
  for (int s = 0; s <= (int)agent::TurnSource::Serial; ++s) {
    const agent::TurnSource src = (agent::TurnSource)s;
    orch::WakeKind kind = orch::WakeKind::Upkeep;
    switch (src) {
      case agent::TurnSource::Owner:
      case agent::TurnSource::Serial:
        // A person started it: it delivers directly, it never arms a wakeup.
        TEST_ASSERT_FALSE(nimbus::harness::wakeKindForSource(src, "turn", kind));
        TEST_ASSERT_FALSE(nimbus::harness::backgroundSource(src));
        break;
      case agent::TurnSource::Loop:
        TEST_ASSERT_TRUE(nimbus::harness::backgroundSource(src));
        TEST_ASSERT_TRUE(nimbus::harness::wakeKindForSource(src, "loop:dream", kind));
        TEST_ASSERT_EQUAL((int)orch::WakeKind::Upkeep, (int)kind);   // the reserved night
        TEST_ASSERT_TRUE(nimbus::harness::wakeKindForSource(src, "loop:lp00000b", kind));
        TEST_ASSERT_EQUAL((int)orch::WakeKind::Routine, (int)kind);
        TEST_ASSERT_TRUE(nimbus::harness::wakeKindForSource(src, "loop", kind));
        TEST_ASSERT_EQUAL((int)orch::WakeKind::Routine, (int)kind);
        break;
      case agent::TurnSource::Synthesis:
        TEST_ASSERT_TRUE(nimbus::harness::backgroundSource(src));
        TEST_ASSERT_TRUE(nimbus::harness::wakeKindForSource(src, "synthesis", kind));
        TEST_ASSERT_EQUAL((int)orch::WakeKind::Subagent, (int)kind);
        break;
      default:
        TEST_FAIL_MESSAGE("a TurnSource was added: classify it in wakeKindForSource");
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_quiet_hours_window_wraps_midnight);
  RUN_TEST(test_delay_exact_case_and_24h_bound);
  RUN_TEST(test_exact_case_0331_note_0900_wake);
  RUN_TEST(test_wakeups_api_lists_self_wakeup_and_quiet_end);
  RUN_TEST(test_arm_one_pending_per_kind_and_record_shape);
  RUN_TEST(test_arm_outside_quiet_hours_arms_and_holds_nothing);
  RUN_TEST(test_note_envelope_and_table_round_trip_through_loops_json);
  RUN_TEST(test_recheck_drops_stale_and_already_delivered);
  RUN_TEST(test_restrict_self_wakeup_turn_drops_every_tool_field);
  RUN_TEST(test_is_self_wakeup_id_class);
  RUN_TEST(test_build_fire_request_marks_self_wakeup);
  RUN_TEST(test_wake_kind_for_source_class_over_every_turn_source);
  return UNITY_END();
}
