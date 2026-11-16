#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"   // modelCtxTokens
#include "nimbus/orch/model_window.h"   // legacyCtxTokens
#include "nimbus/orch/request_budget.h"   // requestBudgetFor - lane B's derived marks
#include "nimbus/orch/episodic.h"

// The history's day floor (lane CTX, conversation.h HistoryBudget): the owner's
// current local day stays verbatim through the overflow shift and the fold, up to
// its cap. On 26 Oct a 128K head (budget 5242 B, low water 2621 B) kept about ten
// messages, and the 15:03 fold took the morning's meal estimates, so at 18:35 the
// head asked for weights it had estimated itself.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kDay = 1792972800u;   // 2026-10-26 00:00 UTC (Mon)
constexpr uint32_t kHourToday = kDay / 3600u;

// A chat built row by row, each row stamped with its own hour.
struct Chat {
  std::vector<EpisodicMessage> rows;   // oldest first
  int seq = 0;

  std::string add(uint32_t hour, const std::string& role, const std::string& text, const std::string& tags) {
    EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = "123";
    m.tsHours = hour;
    m.role = role;
    m.kind = MsgKind::Message;
    m.text = text;
    m.tags = tags;
    rows.push_back(m);
    return m.id;
  }
  // One exchange at `hour`: the owner's message and the reply, about `bytes` long.
  void turn(uint32_t hour, const std::string& what, size_t bytes = 400) {
    const std::string u = add(hour, "user", what, "from:Roy");
    add(hour, "assistant", "re " + what + " " + std::string(bytes, 'x'), "fx:none,out:reply,turn:" + u);
  }
  std::vector<agent::HistTurn> turns(const std::string& after = "", const agent::OffsetAt& zone = {}) const {
    agent::HistoryOptions o;
    o.startAfter = after;
    o.utcOffsetAt = zone;
    return agent::historyTurns(std::vector<EpisodicMessage>(rows.rbegin(), rows.rend()), o);
  }
};

agent::HistoryBudget headBudget(size_t high, bool dayFloor, size_t dayBytes = agent::kHistoryDayBytesMax) {
  agent::HistoryBudget b;
  b.highBytes = high;
  if (dayFloor) {
    b.dayStartUnix = kDay;
    b.dayBytes = dayBytes;
  }
  return b;
}

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// Yesterday evening, then a day of food chat from 08:00 to 18:00.
Chat dayOfFood(int todayTurns, size_t bytes = 400) {
  Chat c;
  for (int i = 0; i < 6; i++) c.turn(kHourToday - 6 + (i / 3), "yesterday " + std::to_string(i), bytes);
  for (int i = 0; i < todayTurns; i++) c.turn(kHourToday + 8 + (uint32_t)(i * 10 / todayTurns), "today " + std::to_string(i), bytes);
  return c;
}

}  // namespace

// The 128K head's window on a busy day: every turn of today survives the shift;
// yesterday's do not. Counter-test: without the floor the morning is gone.
static void test_the_whole_day_survives_the_overflow_shift() {
  const ContextBudget mistral = deriveBudget(modelCtxTokens("mistral", "mistral-large-latest"), {});
  TEST_ASSERT_EQUAL(5242, (int)mistral.recentConvBytes);   // the 26 Oct figure
  const Chat c = dayOfFood(20);
  const agent::HistoryWindow w = agent::fitHistory(c.turns(), "", headBudget(mistral.recentConvBytes, true), 0);
  TEST_ASSERT_TRUE(w.shifted);
  for (int i = 0; i < 20; i++) TEST_ASSERT_TRUE_MESSAGE(has(w.block, "today " + std::to_string(i) + "\n"), w.block.c_str());
  TEST_ASSERT_FALSE(has(w.block, "yesterday 0"));
  TEST_ASSERT_TRUE(w.bytes > mistral.recentConvBytes);   // the day is kept past high water
  // Counter-test: the old budget keeps a few turns of the evening only.
  const agent::HistoryWindow old = agent::fitHistory(c.turns(), "", headBudget(mistral.recentConvBytes, false), 0);
  TEST_ASSERT_FALSE(has(old.block, "today 0\n"));
  TEST_ASSERT_TRUE(old.bytes <= mistral.recentConvBytes);
}

// The floor is capped: a day longer than its cap keeps its newest turns within it.
static void test_the_day_floor_is_capped() {
  const Chat c = dayOfFood(60, 600);   // about 40 KB of today
  const agent::HistoryWindow w = agent::fitHistory(c.turns(), "", headBudget(5242, true, 16384), 0);
  TEST_ASSERT_TRUE(w.bytes <= 16384 + 1200);   // the cap, plus the newest turn's own rendering
  TEST_ASSERT_TRUE(has(w.block, "today 59\n"));
  TEST_ASSERT_FALSE(has(w.block, "today 0\n"));
  TEST_ASSERT_EQUAL_UINT32(16384, (uint32_t)agent::historyDayBytes(128000));
  TEST_ASSERT_EQUAL_UINT32(16384, (uint32_t)agent::historyDayBytes(200000));
  TEST_ASSERT_EQUAL_UINT32(8000, (uint32_t)agent::historyDayBytes(32000));
  TEST_ASSERT_EQUAL_UINT32(2048, (uint32_t)agent::historyDayBytes(8192));
}

// No clock, no floor: the window is byte-identical to the budget without one.
static void test_no_clock_is_byte_identical() {
  const Chat c = dayOfFood(20);
  agent::HistoryBudget off = headBudget(5242, false);
  agent::HistoryBudget noClock = headBudget(5242, true);
  noClock.dayStartUnix = agent::localDayStartUnix(0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, noClock.dayStartUnix);
  TEST_ASSERT_EQUAL_STRING(agent::fitHistory(c.turns(), "", off, 0).block.c_str(),
                           agent::fitHistory(c.turns(), "", noClock, 0).block.c_str());
  // A clock on another day than the rows: no floor either (rows from the future
  // after a clock change, or a chat idle since yesterday).
  agent::HistoryBudget other = headBudget(5242, true);
  other.dayStartUnix = kDay + 3 * 86400;
  TEST_ASSERT_EQUAL_STRING(agent::fitHistory(c.turns(), "", off, 0).block.c_str(),
                           agent::fitHistory(c.turns(), "", other, 0).block.c_str());
}

// Followed turn by turn through a day (the soft start persisted, as the engine
// does): the window shifts once (yesterday leaves) and is append-only after that.
static void test_no_thrash_through_a_day() {
  Chat c;
  for (int i = 0; i < 6; i++) c.turn(kHourToday - 5, "yesterday " + std::to_string(i));
  std::string soft, prev;
  int shifts = 0, breaks = 0;
  for (int i = 0; i < 30; i++) {
    c.turn(kHourToday + 8 + (uint32_t)(i / 3), "today " + std::to_string(i));
    const agent::HistoryWindow w = agent::fitHistory(c.turns(soft), soft, headBudget(5242, true), 0);
    if (w.shifted) {
      shifts++;
      soft = w.softStart;
    }
    const std::string body = w.block.substr(std::strlen(agent::kHistoryHeader));
    if (!prev.empty() && body.compare(0, prev.size(), prev) != 0) breaks++;
    prev = body;
  }
  TEST_ASSERT_EQUAL(1, shifts);
  TEST_ASSERT_TRUE(breaks <= 1);
  TEST_ASSERT_TRUE(has(prev, "today 0\n") && has(prev, "today 29\n"));
}

// Past its cap the day sheds a quarter at a time (hysteresis): a shift is followed
// by turns that only append, never a shift on every message (each one rewrites the
// cached history prefix). Counter-measure: the window stays within the cap.
static void test_past_the_cap_the_day_sheds_a_quarter_at_a_time() {
  Chat c;
  for (int i = 0; i < 6; i++) c.turn(kHourToday - 5, "yesterday " + std::to_string(i));
  const size_t cap = 8192;
  std::string soft;
  int shifts = 0, lastShift = -10, backToBack = 0;
  for (int i = 0; i < 60; i++) {
    c.turn(kHourToday + 8 + (uint32_t)(i / 6), "today " + std::to_string(i), 600);
    const agent::HistoryWindow w = agent::fitHistory(c.turns(soft), soft, headBudget(5242, true, cap), 0);
    TEST_ASSERT_TRUE(has(w.block, "today " + std::to_string(i) + "\n"));
    TEST_ASSERT_TRUE_MESSAGE(w.bytes <= cap + 800, std::to_string(w.bytes).c_str());
    if (!w.shifted) continue;
    shifts++;
    if (i - lastShift == 1) backToBack++;
    lastShift = i;
    soft = w.softStart;
  }
  TEST_ASSERT_TRUE(shifts >= 5);                        // the day did pass its cap, again and again
  TEST_ASSERT_TRUE_MESSAGE(shifts <= 20, std::to_string(shifts).c_str());   // about 1 in 3 turns, not 1 in 1
  TEST_ASSERT_EQUAL(0, backToBack);
}

// The fold never takes the owner's current day; a manual /compact still takes all.
static void test_the_fold_keeps_today() {
  const Chat c = dayOfFood(20);
  const auto t = c.turns();
  const agent::HistoryBudget b = headBudget(5242, true);
  const agent::FoldSlice s = agent::foldSlice(t, b.lowBytes(), 65536, false, 0, b);
  TEST_ASSERT_TRUE(has(s.digest, "yesterday 0"));
  TEST_ASSERT_FALSE_MESSAGE(has(s.digest, "today 0\n"), s.digest.c_str());
  // Counter-test: the slice without the floor takes most of today.
  const agent::FoldSlice old = agent::foldSlice(t, b.lowBytes(), 65536, false, 0);
  TEST_ASSERT_TRUE(has(old.digest, "today 0\n"));
  // Only today in the history: nothing to fold.
  Chat only;
  for (int i = 0; i < 20; i++) only.turn(kHourToday + 9, "today " + std::to_string(i));
  TEST_ASSERT_TRUE(agent::foldSlice(only.turns(), b.lowBytes(), 65536, false, 0, b).digest.empty());
  TEST_ASSERT_EQUAL(20, (int)agent::foldSlice(only.turns(), b.lowBytes(), 1 << 20, true, 0, b).turns);
}

// A clock-change day is one day: each turn's day is the one its row was stored on, in
// the zone at that time (lane HIST). London leaves summer time at 01:00 UTC on
// 25 Oct 2026, so 00:30 local that morning is 23:30 UTC the day before; at 18:00 the
// floor keeps it. Counter-test: read with today's offset it was yesterday's.
static void test_a_clock_change_day_is_one_day() {
  const uint32_t day25 = kDay - 86400u, change = day25 + 3600u;
  const agent::OffsetAt london = [change](uint32_t t) { return t < change ? 60 : 0; };
  Chat c;
  for (int i = 0; i < 3; i++) c.turn(day25 / 3600u - 4, "yesterday " + std::to_string(i));   // 21:00 BST on the 24th
  c.turn(day25 / 3600u - 1, "early 0");                                                       // 00:xx BST on the 25th
  for (int i = 0; i < 20; i++) c.turn(day25 / 3600u + 9 + (uint32_t)i / 3, "today " + std::to_string(i));
  agent::HistoryBudget b = headBudget(5242, true);
  b.dayStartUnix = agent::localDayStartUnix(day25 + 18 * 3600u, 0);   // the offset in force at 18:00
  const agent::HistoryWindow w = agent::fitHistory(c.turns("", london), "", b, 0);
  TEST_ASSERT_TRUE(w.shifted);
  TEST_ASSERT_TRUE_MESSAGE(has(w.block, "early 0\n"), w.block.c_str());
  TEST_ASSERT_TRUE(has(w.block, "today 0\n") && has(w.block, "today 19\n"));
  TEST_ASSERT_FALSE(has(w.block, "yesterday 2\n"));
  // Without the zone the early turn reads as 23:00 UTC on the 24th: not today.
  const agent::HistoryWindow utc = agent::fitHistory(c.turns(), "", b, 0);
  TEST_ASSERT_FALSE(has(utc.block, "early 0\n"));
}

// The local day's first second, east and west of UTC, and before the clock is set.
static void test_local_day_start() {
  const uint64_t noonUtc = kDay + 12 * 3600;
  TEST_ASSERT_EQUAL_UINT32(kDay, agent::localDayStartUnix(noonUtc, 0));
  TEST_ASSERT_EQUAL_UINT32(kDay - 3600, agent::localDayStartUnix(noonUtc, 60));        // BST
  TEST_ASSERT_EQUAL_UINT32(kDay - 330 * 60, agent::localDayStartUnix(noonUtc, 330));   // +05:30
  TEST_ASSERT_EQUAL_UINT32(kDay + 5 * 3600, agent::localDayStartUnix(noonUtc, -300));  // -05:00
  // 23:30 UTC on the 26th is the 27th in London summer time (+60).
  TEST_ASSERT_EQUAL_UINT32(kDay + 86400 - 3600, agent::localDayStartUnix(kDay + 23 * 3600 + 1800, 60));
  TEST_ASSERT_EQUAL_UINT32(0, agent::localDayStartUnix(3600, 60));   // boot-relative: no clock
}

// ---- lane H: the derived window (fold before shift; nothing moves at midnight) --------

namespace {
// A head with room: lane B's derived marks for Mistral Large 4 (360 KB, history 206/165/111 KB).
agent::HistoryBudget derivedBudget(uint32_t dayStart = kDay) {
  const ContextBudget cb = deriveRequestContext(requestBudgetFor("mistral", "mistral-large-4-0", false), {});
  agent::HistoryBudget b;
  b.highBytes = cb.recentConvBytes;
  b.highRows = cb.historyRows;
  b.dayStartUnix = dayStart;
  b.derived = !cb.floorBranch;
  b.maxBytes = cb.historyMaxBytes;
  b.softBytes = cb.historySoftBytes;
  b.keepBytes = cb.historyKeepBytes;
  b.messageCap = cb.historyMessageCap;
  return b;
}
}  // namespace

// The window grows append-only to historyMax with no shift at all; past it the oldest turns
// are hidden down to the soft mark, never below it, and the newest turn stays.
static void test_derived_window_grows_to_max_then_shifts_to_soft() {
  const agent::HistoryBudget b = derivedBudget();
  TEST_ASSERT_TRUE(b.derived);
  TEST_ASSERT_EQUAL_UINT32(206080, (uint32_t)b.maxBytes);
  Chat c;
  std::string soft, prev;
  int shifts = 0, breaks = 0, firstShiftAt = -1;
  for (int i = 0; i < 260; i++) {
    c.turn(kHourToday - 200 + (uint32_t)i, "turn " + std::to_string(i), 1200);
    const agent::HistoryWindow w = agent::fitHistory(c.turns(soft), soft, b, 0);
    TEST_ASSERT_TRUE(has(w.block, "turn " + std::to_string(i) + "\n"));
    TEST_ASSERT_TRUE_MESSAGE(w.bytes <= b.maxBytes, std::to_string(w.bytes).c_str());
    if (w.shifted) {
      shifts++;
      if (firstShiftAt < 0) firstShiftAt = i;
      TEST_ASSERT_TRUE(w.bytes <= b.softBytes);
      soft = w.softStart;
    }
    const std::string body = w.block.empty() ? std::string() : w.block.substr(std::strlen(agent::kHistoryHeader));
    if (!prev.empty() && body.compare(0, prev.size(), prev) != 0) breaks++;
    prev = body;
  }
  TEST_ASSERT_TRUE_MESSAGE(firstShiftAt > 100, std::to_string(firstShiftAt).c_str());   // about 206 KB of turns first
  TEST_ASSERT_TRUE(shifts >= 1 && shifts <= 3);   // a shift takes a fifth of the window, not a turn
  TEST_ASSERT_EQUAL(shifts, breaks);               // between shifts the history only appends
}

// Day boundary (plan v3, lane H): a 23:50 estimate followed by three more exchanges is in the
// 00:05 request on a head with room. Counter-test: v5.1.1's 128K window (5,242 B, the day
// floor of a day that has just begun) shifts it out.
static void test_the_2350_estimate_is_in_the_0005_request() {
  Chat c;
  for (int i = 0; i < 30; i++) c.turn(kHourToday - 4, "evening " + std::to_string(i), 500);
  c.turn(kHourToday - 1, "ESTIMATE bowl 290 g, about 330 kcal", 600);   // 23:50
  for (int i = 0; i < 3; i++) c.turn(kHourToday - 1, "after " + std::to_string(i), 1000);
  const uint32_t tomorrow = kDay + 86400u;   // the 00:05 turn's day
  const agent::HistoryWindow w = agent::fitHistory(c.turns(), "", derivedBudget(tomorrow), 0);
  TEST_ASSERT_FALSE(w.shifted);
  TEST_ASSERT_TRUE(has(w.block, "ESTIMATE bowl 290 g"));
  const ContextBudget legacy = deriveBudget(legacyCtxTokens("mistral", "mistral-large-4-0"), {});
  const agent::HistoryWindow old = agent::fitHistory(c.turns(), "", headBudget(legacy.recentConvBytes, true), 0);
  TEST_ASSERT_TRUE(old.shifted);
  TEST_ASSERT_FALSE(has(old.block, "ESTIMATE bowl 290 g"));
}

// Nothing moves at local midnight: the same turns give the same window whatever day the clock
// says, and with no clock at all (the derived window keeps no day floor).
static void test_derived_window_does_not_move_at_midnight_or_without_a_clock() {
  const Chat c = dayOfFood(40, 1500);
  const std::string at2359 = agent::fitHistory(c.turns(), "", derivedBudget(kDay), 0).block;
  TEST_ASSERT_EQUAL_STRING(at2359.c_str(), agent::fitHistory(c.turns(), "", derivedBudget(kDay + 86400u), 0).block.c_str());
  TEST_ASSERT_EQUAL_STRING(at2359.c_str(), agent::fitHistory(c.turns(), "", derivedBudget(0), 0).block.c_str());
}

// A long day folds today's own oldest turns beyond keep: the fold keeps the newest
// historyKeepBytes (111 KB), not the day, so a day past keep is summarized oldest first and
// its newest part stays verbatim.
static void test_a_long_day_folds_todays_oldest_beyond_keep() {
  const agent::HistoryBudget b = derivedBudget();
  Chat c;
  for (int i = 0; i < 120; i++) c.turn(kHourToday + 6 + (uint32_t)i / 10, "today " + std::to_string(i), 1500);
  const auto t = c.turns();
  const agent::FoldSlice s = agent::foldSlice(t, b.keepBytes, 53792, false, 0);
  TEST_ASSERT_TRUE(has(s.digest, "today 0\n"));
  TEST_ASSERT_FALSE(has(s.digest, "today 119\n"));
  size_t tail = 0;   // what stays verbatim: at least the keep mark's worth of today
  for (size_t i = s.turns; i < t.size(); i++) tail += agent::renderTurnLines(t[i], i ? &t[i - 1] : nullptr, 0).size();
  TEST_ASSERT_TRUE(tail >= b.keepBytes - 2000);
}

// The seed's coverage line (lane H): said only while live rows after the summary are not in
// the request, or the summary is more than a day old; it names the rows the way the [LEFT OUT]
// note does (count, local time range, the call that reads them back).
static void test_the_coverage_line() {
  agent::ConvSpan none;
  TEST_ASSERT_EQUAL_STRING("", agent::historyCoverageLine(none, kDay - 3600, kDay, 0).c_str());   // fresh summary
  TEST_ASSERT_EQUAL_STRING("", agent::historyCoverageLine(none, 0, kDay, 0).c_str());             // no summary
  agent::ConvSpan hidden;
  hidden.afterId = "m000017d9";
  hidden.beforeId = "m00001c97";
  hidden.toId = "m00001c96";
  hidden.rows = 412;
  hidden.fromHours = kHourToday - 13 * 24 + 18;   // Tue 13 Oct 18:00 (UTC)
  hidden.toHours = kHourToday + 14;
  const std::string line = agent::historyCoverageLine(hidden, kDay - 13 * 86400 + 15 * 3600, kDay + 16 * 3600, 60);
  TEST_ASSERT_TRUE_MESSAGE(line.rfind("[CONVERSATION COVERAGE] 412 earlier message(s) of this chat from ", 0) == 0, line.c_str());
  TEST_ASSERT_TRUE(has(line, " to Mon 26 Oct 15:59 are in neither your conversation summary nor this request."));
  TEST_ASSERT_TRUE(has(line, "memory_episodic {\"kind\":\"message\",\"before\":\"m00001c97\"}"));
  TEST_ASSERT_TRUE(has(line, "Your conversation summary was last updated 13 days ago"));
  TEST_ASSERT_TRUE(line.back() == '\n');
  agent::ConvSpan unread;
  unread.afterId = "m000017d9";
  unread.beforeId = "m00001cb0";
  unread.uncounted = true;
  const std::string u = agent::historyCoverageLine(unread, 0, kDay, 0);
  TEST_ASSERT_TRUE_MESSAGE(has(u, "Earlier messages of this chat are in neither"), u.c_str());
  TEST_ASSERT_TRUE(has(u, "(the device is still reading them from its card)"));
  // A summary a day and a half old alone: the age, in hours.
  TEST_ASSERT_TRUE(has(agent::historyCoverageLine(none, kDay, kDay + 36 * 3600, 0), "last updated 36 hours ago"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_whole_day_survives_the_overflow_shift);
  RUN_TEST(test_the_day_floor_is_capped);
  RUN_TEST(test_no_clock_is_byte_identical);
  RUN_TEST(test_no_thrash_through_a_day);
  RUN_TEST(test_past_the_cap_the_day_sheds_a_quarter_at_a_time);
  RUN_TEST(test_the_fold_keeps_today);
  RUN_TEST(test_a_clock_change_day_is_one_day);
  RUN_TEST(test_local_day_start);
  RUN_TEST(test_derived_window_grows_to_max_then_shifts_to_soft);
  RUN_TEST(test_the_2350_estimate_is_in_the_0005_request);
  RUN_TEST(test_derived_window_does_not_move_at_midnight_or_without_a_clock);
  RUN_TEST(test_a_long_day_folds_todays_oldest_beyond_keep);
  RUN_TEST(test_the_coverage_line);
  return UNITY_END();
}
