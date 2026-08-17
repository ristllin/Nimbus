#include <unity.h>

#include <ArduinoJson.h>

#include "nimbus/orch/loops.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// --- civil math ------------------------------------------------------------

static void test_civil_roundtrip() {
  // 2026-07-14 -> day number -> back.
  uint32_t d = daysFromCivil(2026, 7, 14);
  CivilTime c = civilFromDays(d);
  TEST_ASSERT_EQUAL_INT(2026, c.year);
  TEST_ASSERT_EQUAL_INT(7, c.month);
  TEST_ASSERT_EQUAL_INT(14, c.day);
  // epoch day 0 = 1970-01-01 = Thursday (wday 4).
  CivilTime e = civilFromDays(0);
  TEST_ASSERT_EQUAL_INT(1970, e.year);
  TEST_ASSERT_EQUAL_INT(1, e.month);
  TEST_ASSERT_EQUAL_INT(1, e.day);
  TEST_ASSERT_EQUAL_INT(4, e.wday);
  // 2026-07-14 is a Tuesday (wday 2).
  TEST_ASSERT_EQUAL_INT(2, c.wday);
}

// --- interval ---------------------------------------------------------------

static void test_next_interval() {
  SchedSpec s; s.kind = SchedKind::Interval; s.intervalSec = 3600;
  TEST_ASSERT_EQUAL_UINT64(1000 + 3600, nextIntervalEpoch(s, 1000));
}

static void test_is_due() {
  TEST_ASSERT_FALSE(isDue(0, 100));          // nextRun 0 = never
  TEST_ASSERT_FALSE(isDue(200, 100));        // future
  TEST_ASSERT_TRUE(isDue(100, 100));         // exactly due
  TEST_ASSERT_TRUE(isDue(100, 150));         // overdue
}

// --- wall clock (Daily / Weekly), strictly-after + no-backfill -------------

static CivilTime at(int y, int mo, int d, int h, int mi) {
  CivilTime c; c.year = y; c.month = mo; c.day = d; c.hour = h; c.min = mi; c.sec = 0;
  c.wday = (int)((daysFromCivil(y, mo, d) + 4) % 7);
  return c;
}

static void test_daily_same_day_vs_tomorrow() {
  SchedSpec s; s.kind = SchedKind::Daily; s.minuteOfDay = 8 * 60 + 30;  // 08:30
  // now 07:00 -> fires today 08:30
  CivilTime n1 = nextWallClockLocal(s, at(2026, 7, 14, 7, 0));
  TEST_ASSERT_EQUAL_INT(14, n1.day); TEST_ASSERT_EQUAL_INT(8, n1.hour); TEST_ASSERT_EQUAL_INT(30, n1.min);
  // now 08:30 exactly -> NOT strictly after -> tomorrow 08:30 (no double-fire)
  CivilTime n2 = nextWallClockLocal(s, at(2026, 7, 14, 8, 30));
  TEST_ASSERT_EQUAL_INT(15, n2.day); TEST_ASSERT_EQUAL_INT(8, n2.hour);
  // now 09:00 -> tomorrow (across midnight, month boundary sanity)
  CivilTime n3 = nextWallClockLocal(s, at(2026, 7, 31, 9, 0));
  TEST_ASSERT_EQUAL_INT(8, n3.month); TEST_ASSERT_EQUAL_INT(1, n3.day);  // -> Aug 1
}

static void test_weekly_next_matching_day() {
  SchedSpec s; s.kind = SchedKind::Weekly; s.minuteOfDay = 9 * 60;  // 09:00
  s.weekMask = (1u << 1) | (1u << 3);  // Mon + Wed
  // 2026-07-14 is Tuesday 07:00 -> next Mon/Wed at 09:00 is Wed 2026-07-15
  CivilTime n = nextWallClockLocal(s, at(2026, 7, 14, 7, 0));
  TEST_ASSERT_EQUAL_INT(15, n.day);  // Wednesday
  TEST_ASSERT_EQUAL_INT(3, n.wday);
  TEST_ASSERT_EQUAL_INT(9, n.hour);
}

// --- parseSpec -------------------------------------------------------------

static bool parse(const char* json, SchedSpec& out, std::string& err) {
  JsonDocument d; deserializeJson(d, json);
  LoopCaps caps;
  return parseSpec(d.as<JsonObjectConst>(), caps, out, err);
}

static void test_parse_valid_and_invalid() {
  SchedSpec s; std::string err;
  TEST_ASSERT_TRUE(parse(R"({"kind":"interval","every_seconds":3600})", s, err));
  TEST_ASSERT_EQUAL_UINT32(3600, s.intervalSec);
  // below the 300 s floor -> rejected
  TEST_ASSERT_FALSE(parse(R"({"kind":"interval","every_seconds":60})", s, err));
  // daily
  TEST_ASSERT_TRUE(parse(R"({"kind":"daily","at":"08:30"})", s, err));
  TEST_ASSERT_EQUAL_INT(SchedKind::Daily, (int)s.kind);
  TEST_ASSERT_EQUAL_UINT16(510, s.minuteOfDay);
  // bad time
  TEST_ASSERT_FALSE(parse(R"({"kind":"daily","at":"25:00"})", s, err));
  // weekly needs days
  TEST_ASSERT_FALSE(parse(R"({"kind":"weekly","at":"09:00","days":[]})", s, err));
  TEST_ASSERT_TRUE(parse(R"({"kind":"weekly","at":"09:00","days":["mon","fri"]})", s, err));
  TEST_ASSERT_EQUAL_UINT8((1u << 1) | (1u << 5), s.weekMask);
  // unknown kind
  TEST_ASSERT_FALSE(parse(R"({"kind":"hourly"})", s, err));
}

// --- evaluate() decision table ---------------------------------------------

static LoopRecord dueLoop() {
  LoopRecord l;
  l.id = "lp000001"; l.enabled = true; l.approved = true;
  l.sched.kind = SchedKind::Interval; l.sched.intervalSec = 3600;
  l.nextRun = 100;  // due at now>=100
  return l;
}

static void test_evaluate_decisions() {
  LoopCaps caps; DeviceCounters dev;
  const uint64_t now = 200;

  // happy path: due, enabled, approved, allowed, under caps -> Fire
  TEST_ASSERT_EQUAL_INT(FireDecision::Fire, (int)evaluate(dueLoop(), dev, caps, now, true, true));

  { LoopRecord l = dueLoop(); l.enabled = false;
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedDisabled, (int)evaluate(l, dev, caps, now, true, true)); }
  { LoopRecord l = dueLoop(); l.approved = false;
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedUnapproved, (int)evaluate(l, dev, caps, now, true, true)); }
  { LoopRecord l = dueLoop(); l.consecFails = caps.maxConsecFails;
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedConsecFails, (int)evaluate(l, dev, caps, now, true, true)); }
  { LoopRecord l = dueLoop(); l.sched.kind = SchedKind::Daily;   // needs a clock
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedNoClock, (int)evaluate(l, dev, caps, now, false, true)); }
  { LoopRecord l = dueLoop(); l.nextRun = 500;                   // not yet due
    TEST_ASSERT_EQUAL_INT(FireDecision::SkipNotDue, (int)evaluate(l, dev, caps, now, true, true)); }
  { LoopRecord l = dueLoop();                                    // chat revoked
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedChatRevoked, (int)evaluate(l, dev, caps, now, true, false)); }
  { LoopRecord l = dueLoop(); l.firesToday = (uint16_t)caps.maxFiresPerDay;
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedLoopFires, (int)evaluate(l, dev, caps, now, true, true)); }
  { LoopRecord l = dueLoop(); l.tokensToday = caps.maxTokensPerDay;
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedLoopTokens, (int)evaluate(l, dev, caps, now, true, true)); }
  { DeviceCounters d2; d2.firesInWindow = (uint16_t)caps.devFiresWindow;
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedRateWindow, (int)evaluate(dueLoop(), d2, caps, now, true, true)); }
  { DeviceCounters d2; d2.tokensToday = caps.devTokensPerDay;
    TEST_ASSERT_EQUAL_INT(FireDecision::BlockedDeviceTokens, (int)evaluate(dueLoop(), d2, caps, now, true, true)); }
}

// --- clock-safe rollover (the reboot-wipe regression) ----------------------

static void test_clock_safe_rollover() {
  LoopRecord l; l.firesToday = 5; l.tokensToday = 9000; l.lastSyncedDay = 20649;

  // invalid clock -> NEVER roll (reboot mid-day must not wipe the ceiling)
  rollDayIfNeeded(l, 0, false);
  TEST_ASSERT_EQUAL_UINT16(5, l.firesToday);
  TEST_ASSERT_EQUAL_UINT32(20649, l.lastSyncedDay);

  // same wall-day, valid clock -> keep (a reboot rejoining the same day)
  rollDayIfNeeded(l, 20649, true);
  TEST_ASSERT_EQUAL_UINT16(5, l.firesToday);

  // a genuine new wall-day -> reset
  rollDayIfNeeded(l, 20650, true);
  TEST_ASSERT_EQUAL_UINT16(0, l.firesToday);
  TEST_ASSERT_EQUAL_UINT32(0, l.tokensToday);
  TEST_ASSERT_EQUAL_UINT32(20650, l.lastSyncedDay);

  // first-ever sync (lastSyncedDay 0) -> adopt, don't wipe carried counters
  LoopRecord fresh; fresh.firesToday = 3; fresh.lastSyncedDay = 0;
  rollDayIfNeeded(fresh, 20650, true);
  TEST_ASSERT_EQUAL_UINT16(3, fresh.firesToday);
  TEST_ASSERT_EQUAL_UINT32(20650, fresh.lastSyncedDay);
}

// --- fire-result state machine ---------------------------------------------

// ---- timezone-change day adoption (adoptLocalDay) ---------------------------
// A tz edit moves the local CALENDAR, not time. The counters must survive the
// move in both directions and the next REAL midnight must still roll them.

static void test_adopt_westward_then_real_midnight_rolls() {
  LoopRecord l; l.firesToday = 3; l.tokensToday = 4000; l.lastSyncedDay = 20650;
  adoptLocalDay(l, 20649);                 // e.g. UTC -> US/Pacific near midnight
  TEST_ASSERT_EQUAL_UINT32(20649, l.lastSyncedDay);
  TEST_ASSERT_EQUAL_UINT16(3, l.firesToday);      // budget-neutral
  TEST_ASSERT_EQUAL_UINT32(4000, l.tokensToday);
  rollDayIfNeeded(l, 20649, true);         // still the same (new) local day
  TEST_ASSERT_EQUAL_UINT16(3, l.firesToday);
  rollDayIfNeeded(l, 20650, true);         // the next REAL local midnight
  TEST_ASSERT_EQUAL_UINT16(0, l.firesToday);
  TEST_ASSERT_EQUAL_UINT32(0, l.tokensToday);
  TEST_ASSERT_EQUAL_UINT32(20650, l.lastSyncedDay);
}

static void test_adopt_eastward_keeps_counters() {
  LoopRecord l; l.firesToday = 6; l.tokensToday = 12000; l.lastSyncedDay = 20650;
  adoptLocalDay(l, 20651);                 // eastward: the local day jumped ahead
  TEST_ASSERT_EQUAL_UINT32(20651, l.lastSyncedDay);
  TEST_ASSERT_EQUAL_UINT16(6, l.firesToday);      // NO free daily budget
  rollDayIfNeeded(l, 20651, true);         // same adopted day -> no roll
  TEST_ASSERT_EQUAL_UINT16(6, l.firesToday);
  rollDayIfNeeded(l, 20652, true);         // real next midnight -> rolls
  TEST_ASSERT_EQUAL_UINT16(0, l.firesToday);
}

// Documents the hazard adoptLocalDay exists for: without adoption, a westward
// tz change leaves lastSyncedDay AHEAD of the calendar and the strict-`>` roll
// never fires at the (earlier) new day number - counters stall.
static void test_westward_without_adopt_stalls() {
  LoopRecord l; l.firesToday = 3; l.lastSyncedDay = 20650;
  rollDayIfNeeded(l, 20649, true);         // westward day, no adoption
  TEST_ASSERT_EQUAL_UINT16(3, l.firesToday);      // did NOT roll
  TEST_ASSERT_EQUAL_UINT32(20650, l.lastSyncedDay);  // frame still ahead
}

static void test_on_fire_result() {
  LoopRecord l;
  TokenUsage u; u.add(1000, 200);

  onFireResult(l, true, u, 0xABCD, 500);
  TEST_ASSERT_EQUAL_INT(LastResult::Ok, (int)l.lastResult);
  TEST_ASSERT_EQUAL_UINT32(1200, l.tokensToday);
  TEST_ASSERT_EQUAL_UINT8(0, l.consecFails);
  TEST_ASSERT_EQUAL_UINT8(0, l.repeatRun);       // first reply, no repeat yet

  onFireResult(l, true, u, 0xABCD, 600);          // identical reply -> repeat++
  TEST_ASSERT_EQUAL_UINT8(1, l.repeatRun);
  TEST_ASSERT_EQUAL_UINT32(2400, l.tokensToday);  // accumulates

  onFireResult(l, false, u, 0, 700);              // failure -> consecFails++, repeat untouched
  TEST_ASSERT_EQUAL_INT(LastResult::Fail, (int)l.lastResult);
  TEST_ASSERT_EQUAL_UINT8(1, l.consecFails);
  TEST_ASSERT_EQUAL_UINT8(1, l.repeatRun);

  onFireResult(l, true, u, 0x1234, 800);          // new reply -> repeat resets, consecFails resets
  TEST_ASSERT_EQUAL_UINT8(0, l.consecFails);
  TEST_ASSERT_EQUAL_UINT8(0, l.repeatRun);

  // semantic-repeat gate
  LoopRecord r; r.repeatRun = 5;
  TEST_ASSERT_TRUE(isSemanticRepeat(r, 5));
  TEST_ASSERT_FALSE(isSemanticRepeat(r, 6));
}

// --- persistence round-trip -------------------------------------------------

static void test_persistence_roundtrip() {
  std::vector<LoopRecord> loops;
  LoopRecord a; a.id = "lpaaaa01"; a.name = "digest"; a.prompt = "summarize my day";
  a.chatId = "12345"; a.sched.kind = SchedKind::Daily; a.sched.minuteOfDay = 1290;  // 21:30
  a.createdBy = CreatedBy::Agent; a.approved = false; a.enabled = true;
  a.nextRun = 1800000000ULL; a.firesToday = 2; a.tokensToday = 5000; a.consecFails = 1;
  a.lastReplyHash = 0xDEADBEEFCAFEULL; a.repeatRun = 1; a.lastSyncedDay = 20650;
  LoopRecord b; b.id = "lpbbbb02"; b.name = "watch"; b.prompt = "check the build";
  b.sched.kind = SchedKind::Interval; b.sched.intervalSec = 7200;
  loops.push_back(a); loops.push_back(b);

  std::string json = dumpLoops(loops);
  std::vector<LoopRecord> back;
  TEST_ASSERT_TRUE(loadLoops(json, back));
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)back.size());

  const LoopRecord& r = back[0];
  TEST_ASSERT_EQUAL_STRING("lpaaaa01", r.id.c_str());
  TEST_ASSERT_EQUAL_STRING("summarize my day", r.prompt.c_str());
  TEST_ASSERT_EQUAL_INT(SchedKind::Daily, (int)r.sched.kind);
  TEST_ASSERT_EQUAL_UINT16(1290, r.sched.minuteOfDay);
  TEST_ASSERT_EQUAL_INT(CreatedBy::Agent, (int)r.createdBy);
  TEST_ASSERT_FALSE(r.approved);
  TEST_ASSERT_EQUAL_UINT64(1800000000ULL, r.nextRun);
  TEST_ASSERT_EQUAL_UINT32(5000, r.tokensToday);
  TEST_ASSERT_EQUAL_UINT64(0xDEADBEEFCAFEULL, r.lastReplyHash);
  TEST_ASSERT_EQUAL_UINT32(20650, r.lastSyncedDay);
  TEST_ASSERT_EQUAL_UINT32(7200, back[1].sched.intervalSec);

  // empty input = zero loops, not an error
  std::vector<LoopRecord> none;
  TEST_ASSERT_TRUE(loadLoops("", none));
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)none.size());
}


// ---- W20: one-shot wakeups --------------------------------------------------

// The once spec has its OWN bounds (2 min..7 days) and is exempt from the
// recurring 5-min-x-6h interval floor - "check back in 30 minutes" is its job.
static void test_once_spec_parses_with_own_bounds() {
  LoopCaps caps;
  SchedSpec sp; std::string err;
  JsonDocument d;
  d["kind"] = "once"; d["in_seconds"] = 1800;
  TEST_ASSERT_TRUE(parseSpec(d.as<ArduinoJson::JsonObjectConst>(), caps, sp, err));
  TEST_ASSERT_EQUAL((int)SchedKind::Once, (int)sp.kind);
  TEST_ASSERT_EQUAL(1800, sp.intervalSec);
  d["in_seconds"] = 60;                          // below the 2-min floor
  TEST_ASSERT_FALSE(parseSpec(d.as<ArduinoJson::JsonObjectConst>(), caps, sp, err));
  d["in_seconds"] = 8u * 24 * 3600;              // past 7 days: that's a routine
  TEST_ASSERT_FALSE(parseSpec(d.as<ArduinoJson::JsonObjectConst>(), caps, sp, err));
}

// Approval policy in one place: agent RECURRING loops await the owner; an agent
// ONCE wakeup is auto-approved (bounded single fire under the same governor).
// ⚠ Mutation-sensitive: flipping autoApproved to `!byAgent` alone goes red here.
static void test_once_wakeups_skip_owner_approval() {
  TEST_ASSERT_TRUE(autoApproved(SchedKind::Once, /*byAgent=*/true));
  TEST_ASSERT_FALSE(autoApproved(SchedKind::Interval, true));
  TEST_ASSERT_FALSE(autoApproved(SchedKind::Daily, true));
  TEST_ASSERT_FALSE(autoApproved(SchedKind::Weekly, true));
  TEST_ASSERT_TRUE(autoApproved(SchedKind::Interval, false));   // owner: always
  TEST_ASSERT_TRUE(autoApproved(SchedKind::Once, false));
}

// After its single fire a wakeup retires - except ONE short retry when the turn
// itself failed (a silently lost wakeup breaks the model's follow-up promise);
// the second failure retires it.
static void test_once_after_fire_retires_with_one_retry() {
  LoopRecord l;
  l.sched.kind = SchedKind::Once;
  TEST_ASSERT_EQUAL((int)OnceAfterFire::Retire, (int)onceAfterFire(l, true));
  l.consecFails = 1;                            // first failure just recorded
  TEST_ASSERT_EQUAL((int)OnceAfterFire::RetryShort, (int)onceAfterFire(l, false));
  l.consecFails = 2;                            // second failure
  TEST_ASSERT_EQUAL((int)OnceAfterFire::Retire, (int)onceAfterFire(l, false));
  l.sched.kind = SchedKind::Interval;           // recurring loops: not ours
  TEST_ASSERT_EQUAL((int)OnceAfterFire::NotOnce, (int)onceAfterFire(l, false));
}

// W22: /remind's duration parser. Bare number = minutes; s/m/h/d units;
// malformed -> -1; over 7 days -> just past the ceiling so the caller rejects.
static void test_parse_duration_secs() {
  using nimbus::orch::parseDurationSecs;
  TEST_ASSERT_EQUAL(30 * 60, parseDurationSecs("30m"));
  TEST_ASSERT_EQUAL(30 * 60, parseDurationSecs("30"));        // bare = minutes
  TEST_ASSERT_EQUAL(45, parseDurationSecs("45s"));
  TEST_ASSERT_EQUAL(2 * 3600, parseDurationSecs("2h"));
  TEST_ASSERT_EQUAL(24 * 3600, parseDurationSecs("1d"));
  TEST_ASSERT_EQUAL(90 * 60, parseDurationSecs("90MIN"));     // case-insensitive
  // Malformed / empty / unknown unit -> -1.
  TEST_ASSERT_EQUAL(-1, parseDurationSecs(""));
  TEST_ASSERT_EQUAL(-1, parseDurationSecs("soon"));
  TEST_ASSERT_EQUAL(-1, parseDurationSecs("m"));              // no number
  TEST_ASSERT_EQUAL(-1, parseDurationSecs("30w"));            // unknown unit
  TEST_ASSERT_EQUAL(-1, parseDurationSecs("0m"));             // zero rejected
  // Over the 7-day ceiling saturates to just past it (never overflows) so the
  // /remind bounds check refuses it deterministically.
  TEST_ASSERT_EQUAL(7 * 24 * 3600 + 1, parseDurationSecs("8d"));
  TEST_ASSERT_EQUAL(7 * 24 * 3600 + 1, parseDurationSecs("99999999999d"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_civil_roundtrip);
  RUN_TEST(test_next_interval);
  RUN_TEST(test_is_due);
  RUN_TEST(test_daily_same_day_vs_tomorrow);
  RUN_TEST(test_weekly_next_matching_day);
  RUN_TEST(test_parse_valid_and_invalid);
  RUN_TEST(test_evaluate_decisions);
  RUN_TEST(test_clock_safe_rollover);
  RUN_TEST(test_adopt_westward_then_real_midnight_rolls);
  RUN_TEST(test_adopt_eastward_keeps_counters);
  RUN_TEST(test_westward_without_adopt_stalls);
  RUN_TEST(test_on_fire_result);
  RUN_TEST(test_persistence_roundtrip);
  RUN_TEST(test_once_spec_parses_with_own_bounds);
  RUN_TEST(test_once_wakeups_skip_owner_approval);
  RUN_TEST(test_once_after_fire_retires_with_one_retry);
  RUN_TEST(test_parse_duration_secs);
  return UNITY_END();
}
