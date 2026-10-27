#include <unity.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/loops.h"   // daysFromCivil / civilFromDays - the oracle's calendar

// Conversation history days follow the owner's local time exactly.
//
// The history tells each turn's local day from the second the device stored its first row
// (the at: tag; the hour for older rows) and the UTC offset that was in force THEN: not the
// offset in force now, and not the hour with a whole-hour guess. This suite pins the class:
// DST changes in both directions, half-hour and 45-minute zones (positive and negative),
// the local midnight edge to the second, the repeat fold across local midnight, the
// append-only property across a clock change, and the older rows that keep only an hour.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

uint32_t utc(int y, unsigned mo, unsigned d, unsigned h = 0, unsigned mi = 0, unsigned s = 0) {
  return daysFromCivil(y, mo, d) * 86400u + h * 3600u + mi * 60u + s;
}

// 01:00 UTC on the last Sunday of March or October (both months have 31 days).
uint32_t lastSundayChange(int year, unsigned month) {
  const uint32_t last = daysFromCivil(year, month, 31);
  return (last - (last + 4) % 7) * 86400u + 3600u;   // 1970-01-01 was a Thursday
}

// Europe/London: GMT (+0), BST (+60) between the two changes.
int32_t london(uint32_t t) {
  const int year = civilFromDays(t / 86400u).year;
  return t >= lastSundayChange(year, 3) && t < lastSundayChange(year, 10) ? 60 : 0;
}

// The UTC instant a local day starts in London (the first t whose local day is `day`).
uint32_t londonMidnight(uint32_t day) {
  const uint32_t gmt = day * 86400u;
  return london(gmt) == 0 ? gmt : gmt - 3600u;
}

// A chat built row by row the way the device stores it: every row's hour, and the at: tag
// the engine writes on an owner row (`stamp`; false = a row from before that tag).
struct Chat {
  std::vector<EpisodicMessage> rows;   // oldest first
  int seq = 0;

  std::string add(const std::string& role, const std::string& text, uint32_t sec, const std::string& tags,
                  MsgKind kind = MsgKind::Message, const std::string& blob = "") {
    EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = "123";
    m.tsHours = sec / 3600u;
    m.role = role;
    m.kind = kind;
    m.text = text;
    m.tags = tags;
    m.blobPath = blob;
    rows.push_back(m);
    return m.id;
  }
  std::string user(const std::string& text, uint32_t sec, bool stamp = true, const std::string& extra = "") {
    std::string tags = "from:Roy";
    if (!extra.empty()) tags += "," + extra;
    if (stamp) tags += ",at:" + std::to_string(sec);
    return add("user", text, sec, tags);
  }
  void reply(const std::string& turnId, const std::string& text, uint32_t sec) {
    add("assistant", text, sec, "fx:none,out:reply,turn:" + turnId);
  }
  std::vector<EpisodicMessage> newestFirst() const {
    return std::vector<EpisodicMessage>(rows.rbegin(), rows.rend());
  }
  std::vector<agent::HistTurn> turns(const agent::OffsetAt& zone) const {
    agent::HistoryOptions o;
    o.utcOffsetAt = zone;
    return agent::historyTurns(newestFirst(), o);
  }
  std::string block(const agent::OffsetAt& zone) const {
    agent::HistoryBudget b;
    b.highBytes = 1u << 20;
    b.highRows = 1u << 16;
    return agent::fitHistory(turns(zone), "", b, 0).block;
  }
};

size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string all;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) all.append(buf, n);
  std::fclose(f);
  return all;
}

// One turn per time in `t`, each its own owner message "m<i>".
void userAt(Chat& c, const uint32_t* t, size_t n) {
  for (size_t i = 0; i < n; i++) c.user("m" + std::to_string(i), t[i]);
}

}  // namespace

// BST ends 25 Oct 2026 01:00 UTC. A row at 23:30 UTC on the 24th is 00:30 BST on the 25th:
// it opens Sunday even though the clock now reads GMT, and the repeated 01:30 hour adds no
// second Sunday marker.
static void test_day_markers_follow_the_offset_at_each_rows_time_when_bst_ends() {
  Chat c;
  const uint32_t t[] = {utc(2026, 10, 24, 22, 30), utc(2026, 10, 24, 23, 30), utc(2026, 10, 25, 0, 30),
                        utc(2026, 10, 25, 1, 30),  utc(2026, 10, 25, 23, 30), utc(2026, 10, 26, 0, 0)};
  userAt(c, t, 6);
  const std::string w = c.block(london);
  TEST_ASSERT_TRUE_MESSAGE(has(w, "(Sat 24 Oct)\n- user: m0"), w.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(w, "(Sun 25 Oct)\n- user: m1"), w.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(w, "(Mon 26 Oct)\n- user: m5"), w.c_str());
  TEST_ASSERT_EQUAL(1, (int)count(w, "(Sat 24 Oct)"));
  TEST_ASSERT_EQUAL(1, (int)count(w, "(Sun 25 Oct)"));
  TEST_ASSERT_EQUAL(1, (int)count(w, "(Mon 26 Oct)"));
  // Counter-test: the offset in force now (GMT) told to every row, as before, puts m1 on Saturday.
  const std::string now = c.block(agent::fixedZone(0));
  TEST_ASSERT_FALSE(has(now, "(Sun 25 Oct)\n- user: m1"));
  TEST_ASSERT_TRUE(has(now, "(Sun 25 Oct)\n- user: m2"));
}

// BST starts 29 Mar 2026 01:00 UTC. 23:30 UTC on the 28th is 23:30 GMT: still Saturday even
// though the clock now reads BST; 22:59:59 UTC on the 29th is the last second of Sunday.
static void test_day_markers_follow_the_offset_at_each_rows_time_when_bst_starts() {
  Chat c;
  const uint32_t t[] = {utc(2026, 3, 28, 22, 30), utc(2026, 3, 28, 23, 30), utc(2026, 3, 29, 0, 30),
                        utc(2026, 3, 29, 1, 30),  utc(2026, 3, 29, 22, 59, 59), utc(2026, 3, 29, 23, 0, 0)};
  userAt(c, t, 6);
  const std::string w = c.block(london);
  TEST_ASSERT_TRUE_MESSAGE(has(w, "(Sat 28 Mar)\n- user: m0"), w.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(w, "(Sun 29 Mar)\n- user: m2"), w.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(w, "(Mon 30 Mar)\n- user: m5"), w.c_str());
  TEST_ASSERT_EQUAL(1, (int)count(w, "(Sat 28 Mar)"));
  TEST_ASSERT_EQUAL(1, (int)count(w, "(Sun 29 Mar)"));
  TEST_ASSERT_EQUAL(1, (int)count(w, "(Mon 30 Mar)"));
  // Counter-test: the offset now (BST) told to every row puts m1 on Sunday.
  const std::string now = c.block(agent::fixedZone(60));
  TEST_ASSERT_TRUE(has(now, "(Sun 29 Mar)\n- user: m1"));
}

// The class: every local midnight in London around both changes, to the second.
static void test_every_london_midnight_around_a_change_is_exact() {
  const uint32_t firstDays[] = {daysFromCivil(2026, 3, 26), daysFromCivil(2026, 10, 22)};
  for (const uint32_t first : firstDays) {
    for (uint32_t day = first; day < first + 8; day++) {
      const uint32_t edge = londonMidnight(day);
      TEST_ASSERT_EQUAL_INT32(0, (int)(edge % 60));
      Chat c;
      c.user("before", edge - 1);
      c.user("after", edge);
      const auto t = c.turns(london);
      TEST_ASSERT_EQUAL(2, (int)t.size());
      char msg[48];
      snprintf(msg, sizeof(msg), "day %u", (unsigned)day);
      TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)day - 1, t[0].localDay, msg);
      TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)day, t[1].localDay, msg);
    }
  }
}

// Half-hour and 45-minute zones, both signs: the day flips on the second the local clock
// passes midnight, not on the hour.
static void test_day_markers_resolve_to_the_second_in_odd_zones() {
  struct Zone { const char* name; int32_t off; };
  const Zone zones[] = {{"India +5:30", 330},      {"Nepal +5:45", 345},     {"Chatham +12:45", 765},
                        {"Newfoundland -3:30", -210}, {"Marquesas -9:30", -570}, {"US Pacific -8", -480},
                        {"UTC", 0},                {"Kiritimati +14", 840}};
  const uint32_t day = daysFromCivil(2026, 10, 26);   // Monday
  for (const Zone& z : zones) {
    const uint32_t edge = day * 86400u - (uint32_t)((int64_t)z.off * 60);
    Chat c;
    c.user("before", edge - 1);
    c.user("after", edge);
    const auto t = c.turns(agent::fixedZone(z.off));
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)t.size(), z.name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)day - 1, t[0].localDay, z.name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)day, t[1].localDay, z.name);
    const std::string w = c.block(agent::fixedZone(z.off));
    TEST_ASSERT_TRUE_MESSAGE(has(w, "(Sun 25 Oct)\n- user: before"), z.name);
    TEST_ASSERT_TRUE_MESSAGE(has(w, "(Mon 26 Oct)\n- user: after"), z.name);
  }
  // India: 18:29:59 UTC is 23:59:59 on Sunday and 18:30:00 is 00:00:00 on Monday. The older
  // hour-truncated rule placed both in hour 18 and told them the same day.
  Chat c;
  c.user("a", utc(2026, 10, 25, 18, 29, 59));
  c.user("b", utc(2026, 10, 25, 18, 30, 0));
  const std::string w = c.block(agent::fixedZone(330));
  TEST_ASSERT_TRUE_MESSAGE(has(w, "(Sun 25 Oct)\n- user: a"), w.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(w, "(Mon 26 Oct)\n- user: b"), w.c_str());
}

// An older row keeps only its hour: it is told at the hour's first second with the offset
// then (a whole-hour zone is exact; a half-hour zone's midnight hour is best effort and
// deterministic). The sent: tag of an older Telegram row refines it when it agrees with the
// hour, and a delivery hours late keeps the day it arrived.
static void test_rows_without_seconds_fall_back_to_the_hour() {
  const uint32_t h18 = utc(2026, 10, 25, 18, 40, 0);   // 00:10 Monday in India
  Chat hourOnly;
  hourOnly.user("a", h18, false);
  hourOnly.user("b", h18 + 60, false);
  const auto t = hourOnly.turns(agent::fixedZone(330));
  TEST_ASSERT_EQUAL_INT32((int32_t)daysFromCivil(2026, 10, 25), t[0].localDay);   // hour start 23:30 Sunday
  TEST_ASSERT_EQUAL_INT32(t[0].localDay, t[1].localDay);
  // The older Telegram row: sent: names the second inside its hour.
  Chat tg;
  tg.user("a", h18, false, "sent:" + std::to_string(h18));
  TEST_ASSERT_EQUAL_INT32((int32_t)daysFromCivil(2026, 10, 26), tg.turns(agent::fixedZone(330))[0].localDay);
  // A sent: far from the stored hour is a late delivery: the arrival hour decides the day.
  Chat late;
  late.user("a", utc(2026, 10, 26, 10, 0, 0), false, "sent:" + std::to_string(utc(2026, 10, 25, 20, 58, 0)));
  TEST_ASSERT_EQUAL_INT32((int32_t)daysFromCivil(2026, 10, 26), late.turns(agent::fixedZone(0))[0].localDay);
}

// The at: tag refines the stored hour; it never overrules it, and a row stored before the
// clock was set has no date at all.
static void test_at_tag_refines_the_hour_and_never_overrules_it() {
  Chat c;
  const uint32_t noon = utc(2026, 10, 26, 12, 0, 0);
  c.add("user", "ok", noon, "from:Roy,at:" + std::to_string(noon + 3500));       // same hour: used
  c.add("user", "next hour", noon + 4000, "from:Roy,at:" + std::to_string(noon + 3500 + 4000));
  c.add("user", "wild", noon + 7200, "from:Roy,at:" + std::to_string(noon + 30u * 3600u));   // 30 h off: ignored
  c.add("user", "garbage", noon + 7300, "from:Roy,at:99999999999999");
  const auto t = c.turns(agent::fixedZone(0));
  TEST_ASSERT_EQUAL(4, (int)t.size());
  for (const auto& turn : t) TEST_ASSERT_EQUAL_INT32((int32_t)daysFromCivil(2026, 10, 26), turn.localDay);
  // 23:59:30 UTC in UTC+1 is 00:59:30 the next day; an at: 30 hours later would flip it.
  Chat edge;
  const uint32_t late = utc(2026, 10, 26, 22, 30, 0);
  edge.add("user", "x", late, "from:Roy,at:" + std::to_string(late + 30u * 3600u));
  TEST_ASSERT_EQUAL_INT32((int32_t)daysFromCivil(2026, 10, 26), edge.turns(agent::fixedZone(0))[0].localDay);
  // A stamp past 32 bits saturates; it never wraps into a plausible time (here 2^32 + 23:30 UTC
  // would otherwise move a row stored in hour 22 to the next local day at UTC+1).
  Chat wrap;
  const uint32_t h22 = utc(2026, 10, 26, 22, 10);
  wrap.add("user", "w", h22, "from:Roy,at:" + std::to_string(4294967296ull + utc(2026, 10, 26, 23, 30)));
  TEST_ASSERT_EQUAL_INT32((int32_t)daysFromCivil(2026, 10, 26), wrap.turns(agent::fixedZone(60))[0].localDay);
  // Stored before the clock was set: boot-relative hour, no date, no marker.
  Chat boot;
  boot.add("user", "early", 5 * 3600u, "from:Roy,at:" + std::to_string(utc(2026, 10, 26, 12)));
  TEST_ASSERT_EQUAL_INT32(agent::kDayNone, boot.turns(agent::fixedZone(60))[0].localDay);
  TEST_ASSERT_FALSE(has(boot.block(agent::fixedZone(60)), "\n("));   // no day-marker line
}

// A late item shows when it was sent in the owner's clock AT THAT TIME: sent at 21:58 BST
// reads 21:58 after the clocks went back, not 20:58.
static void test_late_label_uses_the_offset_when_it_was_sent() {
  Chat c;
  const std::string a = c.user("evening", utc(2026, 10, 24, 20, 0, 0));
  c.reply(a, "Good evening.", utc(2026, 10, 24, 21, 5, 0));
  const uint32_t sent = utc(2026, 10, 24, 20, 58, 0);                      // 21:58 BST
  c.user("parcel", utc(2026, 10, 25, 8, 0, 0), true, "sent:" + std::to_string(sent));   // read in GMT
  const std::string w = c.block(london);
  TEST_ASSERT_TRUE_MESSAGE(has(w, "- user: [sent 21:58] parcel"), w.c_str());
  TEST_ASSERT_TRUE(has(w, "(Sun 25 Oct)\n- user: [sent 21:58] parcel"));
}

// Two identical messages a minute apart either side of LOCAL midnight are two messages
// (BST: 23:59:30 and 00:00:20), though they share a UTC day; in winter the same UTC times
// are 22:59 and 23:00 on one local day and fold.
static void test_repeat_fold_never_crosses_local_midnight() {
  const uint32_t a = utc(2026, 10, 24, 22, 59, 30);
  const uint32_t b = utc(2026, 10, 24, 23, 0, 20);
  Chat summer;
  summer.user("dinner 600", a);
  summer.user("dinner 600", b);
  TEST_ASSERT_EQUAL(2, (int)summer.turns(london).size());
  TEST_ASSERT_EQUAL(1, (int)summer.turns(agent::fixedZone(0)).size());   // the UTC day: one
  Chat winter;
  winter.user("dinner 600", utc(2026, 1, 5, 22, 59, 30));
  winter.user("dinner 600", utc(2026, 1, 5, 23, 0, 20));
  const auto t = winter.turns(london);
  TEST_ASSERT_EQUAL(1, (int)t.size());
  TEST_ASSERT_EQUAL(2, (int)t[0].repeats);
  // The two forms of the rule agree, and the fold input of a chat with no turn history too.
  const auto rows = summer.newestFirst();
  TEST_ASSERT_FALSE(agent::isRepeatRow(rows[0], rows[1], agent::OffsetAt(london)));
  TEST_ASSERT_TRUE(agent::isRepeatRow(rows[0], rows[1], 0));
  TEST_ASSERT_FALSE(agent::isRepeatRow(rows[0], rows[1], 60));
  TEST_ASSERT_EQUAL(2, (int)agent::collapseRepeats(rows, london).size());
  const auto folded = agent::collapseRepeats(rows, agent::OffsetAt());
  TEST_ASSERT_EQUAL(1, (int)folded.size());
  TEST_ASSERT_EQUAL_STRING("dinner 600 (x2)", folded[0].text.c_str());
}

// The clocks go back between the two messages (01:59:30 BST, then 01:00:20 GMT): different
// offsets, one local day, still one repeated message.
static void test_repeat_fold_across_the_clock_change_inside_one_day() {
  Chat c;
  c.user("dinner 600", utc(2026, 10, 25, 0, 59, 30));
  c.user("dinner 600", utc(2026, 10, 25, 1, 0, 20));
  const auto t = c.turns(london);
  TEST_ASSERT_EQUAL(1, (int)t.size());
  TEST_ASSERT_EQUAL(2, (int)t[0].repeats);
}

// Rows with only an hour fold when they share it, unless that hour holds a local midnight
// (half-hour zones): then the day cannot be shown to match.
static void test_hour_only_repeats_do_not_fold_across_a_possible_midnight() {
  const uint32_t h18 = utc(2026, 10, 25, 18, 0, 0);   // 23:30-00:29 in India
  const uint32_t h17 = utc(2026, 10, 25, 17, 0, 0);   // 22:30-23:29 in India
  const uint32_t h23 = utc(2026, 10, 25, 23, 0, 0);   // 00:00-00:59 at UTC+1
  struct Case { uint32_t hour; int32_t off; int turns; };
  const uint32_t h22 = utc(2026, 10, 25, 22, 0, 0);   // 23:00-23:59 at UTC+1: ends a second before midnight
  const Case cases[] = {{h18, 330, 2}, {h17, 330, 1}, {h18, 345, 2}, {h18 - 3600u, 345, 1},
                        {h23, 60, 1},  {h22, 60, 1},  {h18, 0, 1},   {h18, -210, 1},
                        {utc(2026, 10, 25, 3), -210, 2}};
  for (const Case& k : cases) {
    Chat c;
    c.user("dinner 600", k.hour + 5, false);
    c.user("dinner 600", k.hour + 3000, false);
    char msg[48];
    snprintf(msg, sizeof(msg), "hour %u off %d", (unsigned)(k.hour / 3600u), (int)k.off);
    TEST_ASSERT_EQUAL_MESSAGE(k.turns, (int)c.turns(agent::fixedZone(k.off)).size(), msg);
  }
}

// A run of unanswered repeats is chained row by row (each compared with the row just newer than
// it), so a growing run is always one group and an older group never re-splits: a message 50 s
// after the last one joins the newest group or starts another, and nothing else changes. The
// older rule measured from the newest row, so the run split anew at every message
// ('ping', 'ping (x2)', 'ping (x3)', then 'ping' + 'ping (x3)').
static void test_a_run_of_repeats_stays_one_group_and_never_re_splits() {
  const uint32_t t0 = utc(2026, 10, 26, 10, 0, 0);
  for (int n = 1; n <= 8; n++) {   // 50 s apart: 350 s end to end, far past the 120 s window
    Chat c;
    for (int i = 0; i < n; i++) c.user("ping", t0 + 50u * (uint32_t)i);
    const auto turns = c.turns(agent::fixedZone(0));
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)turns.size(), std::to_string(n).c_str());
    TEST_ASSERT_EQUAL((int)n, (int)turns[0].repeats);
    const auto rows = agent::collapseRepeats(c.newestFirst(), agent::fixedZone(0));
    TEST_ASSERT_EQUAL(1, (int)rows.size());
    if (n > 1) TEST_ASSERT_EQUAL_STRING(("ping (x" + std::to_string(n) + ")").c_str(), rows[0].text.c_str());
  }
  // A gap past the window ends the chain; the run before it is closed and never changes again.
  const uint32_t at[] = {0, 50, 100, 230, 280, 330, 380};   // groups: 3 and 4
  Chat c;
  std::vector<std::string> before;
  for (const uint32_t s : at) {
    c.user("ping", t0 + s);
    const auto turns = c.turns(agent::fixedZone(0));
    std::vector<std::string> now;
    for (size_t k = 0; k < turns.size(); k++)
      now.push_back(agent::renderTurnLines(turns[k], k ? &turns[k - 1] : nullptr, 0));
    const size_t closed = now.size() > before.size() ? before.size() : (before.empty() ? 0 : before.size() - 1);
    for (size_t k = 0; k < closed; k++) TEST_ASSERT_EQUAL_STRING(before[k].c_str(), now[k].c_str());
    before = now;
  }
  const auto last = c.turns(agent::fixedZone(0));
  TEST_ASSERT_EQUAL(2, (int)last.size());
  TEST_ASSERT_EQUAL(3, (int)last[0].repeats);
  TEST_ASSERT_EQUAL(4, (int)last[1].repeats);
}

// Days never run backwards. A turn whose first row has no stamp (an on-device voice row, a reply
// nobody asked for) is told at the start of its hour, which in a half-hour zone is the day before
// the stamped turn that precedes it: 18:40 UTC is 00:10 the next day in India, 18:00 UTC is 23:30.
static void test_days_never_run_backwards_for_a_turn_without_a_stamp() {
  const uint32_t a = utc(2026, 10, 26, 18, 40, 0);   // Tue 27 Oct 00:10 in India
  Chat c;
  c.user("stamped one", a);
  c.add("user", "[voice note] ok", a + 300, "from:Roy", MsgKind::Audio, "/mem/blobs/00000000abc00007.ogg");
  c.add("assistant", "unprompted", a + 400, "fx:none,out:reply,turn:m7fffffff");   // no owner row asked
  c.user("stamped two", a + 900);
  const auto turns = c.turns(agent::fixedZone(330));
  TEST_ASSERT_EQUAL(4, (int)turns.size());
  const int32_t tue = (int32_t)daysFromCivil(2026, 10, 27);
  for (const auto& t : turns) TEST_ASSERT_EQUAL_INT32(tue, t.localDay);
  const std::string w = c.block(agent::fixedZone(330));
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)count(w, "(Tue 27 Oct)"), w.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)count(w, "(Mon 26 Oct)"), w.c_str());
  // The class: whatever mix of stamped and unstamped turns, the days never decrease.
  Chat mix;
  uint32_t s = utc(2026, 10, 25, 12, 0, 0);
  for (int i = 0; i < 60; i++, s += 1777u) {
    if (i % 3 == 1) mix.add("user", "[voice note]", s, "from:Roy", MsgKind::Audio, "/mem/blobs/00000000abc0" + std::to_string(10 + i) + ".ogg");
    else mix.user("m" + std::to_string(i), s, i % 3 != 2);
  }
  for (const int32_t off : {330, 345, -210, 60, 0}) {
    const auto t = mix.turns(agent::fixedZone(off));
    for (size_t k = 1; k < t.size(); k++)
      TEST_ASSERT_TRUE_MESSAGE(t[k].localDay >= t[k - 1].localDay, std::to_string(off).c_str());
  }
  // A turn stored before the clock was set stays undated and does not lift the floor.
  Chat boot;
  boot.user("early", 5 * 3600u, false);
  boot.user("later", a);
  const auto bt = boot.turns(agent::fixedZone(0));
  TEST_ASSERT_EQUAL_INT32(agent::kDayNone, bt[0].localDay);
}

// What may fold: the 120 s window is closed at its edge (120 s folds, 121 s does not), a replay
// keeps the sender's time (same sent:, minutes apart in store time, still one message), the
// day marker follows the second the device stored the row (at:) even when sent: disagrees, and
// a garbled stamp tells nothing (it neither folds rows an hour apart nor sets a day).
static void test_repeat_window_edge_and_stamp_sources() {
  const uint32_t t0 = utc(2026, 10, 26, 10, 0, 0);
  Chat edge;
  edge.user("dinner 600", t0);
  edge.user("dinner 600", t0 + 120);
  TEST_ASSERT_EQUAL(1, (int)edge.turns(agent::fixedZone(0)).size());
  Chat past;
  past.user("dinner 600", t0);
  past.user("dinner 600", t0 + 121);
  TEST_ASSERT_EQUAL(2, (int)past.turns(agent::fixedZone(0)).size());
  Chat replay;   // one Telegram message served again 20 minutes later: same sent:, different at:
  replay.user("dinner 600", t0, true, "sent:" + std::to_string(t0 - 30));
  replay.user("dinner 600", t0 + 1200, true, "sent:" + std::to_string(t0 - 30));
  TEST_ASSERT_EQUAL(1, (int)replay.turns(agent::fixedZone(0)).size());
  Chat both;   // stored 22:59:59 UTC, sent 23:00:01 UTC: UTC+1 puts the day at the store time
  both.user("m", utc(2026, 10, 25, 22, 59, 59), true, "sent:" + std::to_string(utc(2026, 10, 25, 23, 0, 1)));
  TEST_ASSERT_EQUAL_INT32((int32_t)daysFromCivil(2026, 10, 25), both.turns(agent::fixedZone(60))[0].localDay);
  Chat garbled;   // a stamp past 32 bits saturates: rows an hour apart are two messages
  garbled.add("user", "dinner 600", t0, "from:Roy,at:99999999999");
  garbled.add("user", "dinner 600", t0 + 3600, "from:Roy,at:99999999999");
  TEST_ASSERT_EQUAL(2, (int)garbled.turns(agent::fixedZone(0)).size());
  Chat early;   // a stamp from before the clock was set tells nothing either
  early.add("user", "dinner 600", t0, "from:Roy,at:5000");
  early.add("user", "dinner 600", t0 + 3600, "from:Roy,at:5000");
  TEST_ASSERT_EQUAL(2, (int)early.turns(agent::fixedZone(0)).size());
}

// The offset function the device wires, against the real zone rules of the C library (the
// device applies its zone setting as a POSIX TZ string): London's two changes in 2026 to the
// second, then odd zones in winter and summer, and a time before the clock was ever set.
static void test_the_process_zone_rule_gives_the_offset_in_force_at_each_time() {
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
  tzset();
  uint32_t bad = 0;
  for (uint32_t t = utc(2025, 12, 31, 23); t < utc(2027, 1, 1, 1) && !bad; t += 1021)
    if (agent::utcOffsetAtUnix(t) != london(t)) bad = t;
  for (const uint32_t change : {lastSundayChange(2026, 3), lastSundayChange(2026, 10)})
    for (uint32_t t = change - 2; t < change + 2 && !bad; t++)
      if (agent::utcOffsetAtUnix(t) != london(t)) bad = t;
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, bad, "London offset differs from the rule's oracle at this time");
  struct Zone { const char* tz; int32_t jan; int32_t jul; };
  const Zone zones[] = {{"IST-5:30", 330, 330},
                        {"<+0545>-5:45", 345, 345},
                        {"NST3:30NDT,M3.2.0,M11.1.0", -210, -150},
                        {"<+1245>-12:45<+1345>,M9.1.0/2:45,M4.1.0/3:45", 825, 765},
                        {"PST8PDT,M3.2.0,M11.1.0", -480, -420},
                        {"UTC0", 0, 0}};
  for (const Zone& z : zones) {
    setenv("TZ", z.tz, 1);
    tzset();
    TEST_ASSERT_EQUAL_INT32_MESSAGE(z.jan, agent::utcOffsetAtUnix(utc(2026, 1, 15, 12)), z.tz);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(z.jul, agent::utcOffsetAtUnix(utc(2026, 7, 15, 12)), z.tz);
  }
  TEST_ASSERT_EQUAL_INT32(0, agent::utcOffsetAtUnix(5));   // before the clock was set
  unsetenv("TZ");
  tzset();
}

// The class property: a closed turn's rendered bytes never change. A clock change between
// two turns (BST ending at 01:00 UTC) used to re-day every earlier row, because one offset,
// the one now, was applied to all of them. Every prefix of the chat renders as a byte prefix
// of the next.
static void test_history_stays_append_only_across_a_clock_change() {
  const uint32_t start = utc(2026, 10, 24, 18, 0, 0);
  const int n = 16;   // hourly, to 09:00 UTC on the 25th: the change falls between rows 7 and 8
  std::string prev;
  int brokenNow = 0;
  std::string prevNow;
  Chat c;
  for (int i = 0; i < n; i++) {
    const uint32_t t = start + (uint32_t)i * 3600u + 1800u;
    const std::string u = c.user("m" + std::to_string(i), t);
    c.reply(u, "a" + std::to_string(i), t + 30u);
    const std::string w = c.block(london);
    TEST_ASSERT_TRUE_MESSAGE(w.compare(0, prev.size(), prev) == 0, w.c_str());
    prev = w;
    // Counter-test: the offset in force at the newest row, told to every row (the older rule).
    const std::string old = c.block(agent::fixedZone(london(t)));
    if (!prevNow.empty() && old.compare(0, prevNow.size(), prevNow) != 0) brokenNow++;
    prevNow = old;
  }
  TEST_ASSERT_TRUE_MESSAGE(brokenNow > 0, "the one-offset rule must break the prefix at the clock change");
}

// Older callers keep working: a turn nobody resolved is told from its hour with the fixed
// offset the render functions are given.
static void test_unresolved_turns_use_the_fixed_offset() {
  agent::HistTurn t;
  t.user = "hello";
  t.assistant = "hi";
  t.tsHours = utc(2026, 10, 25, 23, 0, 0) / 3600u;   // 23:00 UTC Sunday
  TEST_ASSERT_EQUAL_INT32(agent::kDayUnresolved, t.localDay);
  TEST_ASSERT_TRUE(has(agent::renderTurnLines(t, nullptr, 0), "(Sun 25 Oct)\n"));
  TEST_ASSERT_TRUE(has(agent::renderTurnLines(t, nullptr, 60), "(Mon 26 Oct)\n"));
  agent::HistTurn early = t;
  early.tsHours = 12;   // boot-relative
  TEST_ASSERT_FALSE(has(agent::renderTurnLines(early, nullptr, 60), "("));
  // historyTurns' own resolution wins over the render offset (the two never disagree on the device).
  Chat c;
  c.user("x", utc(2026, 10, 25, 23, 0, 0));
  agent::HistoryOptions o;
  o.utcOffsetMin = 0;
  const auto turns = agent::historyTurns(c.newestFirst(), o);
  TEST_ASSERT_EQUAL_INT32((int32_t)daysFromCivil(2026, 10, 25), turns[0].localDay);
  TEST_ASSERT_TRUE(has(agent::renderTurnLines(turns[0], nullptr, 60), "(Sun 25 Oct)\n"));
}

// localZone: the per-time function replaces the fixed offset when given.
static void test_local_zone_prefers_the_per_time_offset() {
  agent::HistoryOptions o;
  o.utcOffsetMin = 60;
  TEST_ASSERT_EQUAL_INT32(60, agent::localZone(o)(utc(2026, 7, 1)));
  o.utcOffsetAt = [](uint32_t t) { return t < 1000 ? 330 : -480; };
  TEST_ASSERT_EQUAL_INT32(330, agent::localZone(o)(5));
  TEST_ASSERT_EQUAL_INT32(-480, agent::localZone(o)(utc(2026, 7, 1)));
  TEST_ASSERT_EQUAL_INT32(45, agent::fixedZone(45)(utc(2026, 7, 1)));
}

namespace {
std::tm tmAt(int yday, int hour, int min) {
  std::tm t;
  memset(&t, 0, sizeof(t));
  t.tm_yday = yday;
  t.tm_hour = hour;
  t.tm_min = min;
  return t;
}
}  // namespace

// The device derives the offset from localtime_r and gmtime_r of one instant; the day and
// year wrap are where a hand-rolled version slips (no tm_gmtoff on the ESP32).
static void test_utc_offset_from_broken_down_times() {
  struct Case { std::tm local, utc; int32_t off; };
  const Case cases[] = {
      {tmAt(297, 1, 30), tmAt(297, 0, 30), 60},        // BST, same day
      {tmAt(297, 6, 15), tmAt(297, 0, 30), 345},       // Nepal: minutes borrow
      {tmAt(297, 10, 15), tmAt(297, 4, 45), 330},      // India: 10:15 against 04:45
      {tmAt(297, 21, 30), tmAt(297, 23, 45), -135},    // same day, behind: 21:30 against 23:45
      {tmAt(298, 0, 30), tmAt(297, 19, 0), 330},       // local is already tomorrow
      {tmAt(296, 23, 30), tmAt(297, 4, 0), -270},      // local is still yesterday
      {tmAt(364, 21, 30), tmAt(0, 2, 30), -300},       // local Dec 31, UTC already Jan 1
      {tmAt(0, 5, 30), tmAt(364, 23, 45), 345},        // local Jan 1, UTC still Dec 31
      {tmAt(0, 5, 30), tmAt(365, 23, 45), 345},        // ... in a leap year
      {tmAt(100, 12, 0), tmAt(100, 12, 0), 0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char msg[24];
    snprintf(msg, sizeof(msg), "case %u", (unsigned)i);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(cases[i].off, agent::utcOffsetFromTm(cases[i].local, cases[i].utc), msg);
  }
}

// The host rig always hands the engine a clock and a zone, so a device that never wired them
// would pass every test above while its history stayed hour-only (a missing
// Platform::nowEpoch on the device is exactly that). Pin the device seam.
static void test_the_device_wires_the_clock_and_the_zone_the_history_reads() {
  const std::string src = readFile("src/agent/orchestrator.cpp");
  for (const char* wire : {"d.platform.nowEpoch =", "d.utcOffsetMin =", "d.utcOffsetAt ="})
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)count(src, wire), wire);
  // The fold input of a chat without the turn-structured history tells the same local days.
  const size_t fold = src.find("agent::collapseRepeats(");
  TEST_ASSERT_TRUE(fold != std::string::npos);
  TEST_ASSERT_TRUE(has(src.substr(fold, 220), "agent::utcOffsetAtUnix"));
  // The engine reads the clock it was given, and only that.
  const std::string eng = readFile("lib/harness/src/engine.cpp");
  TEST_ASSERT_TRUE(has(eng, "d.platform.nowEpoch ? d.platform.nowEpoch() : 0"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_device_wires_the_clock_and_the_zone_the_history_reads);
  RUN_TEST(test_day_markers_follow_the_offset_at_each_rows_time_when_bst_ends);
  RUN_TEST(test_day_markers_follow_the_offset_at_each_rows_time_when_bst_starts);
  RUN_TEST(test_every_london_midnight_around_a_change_is_exact);
  RUN_TEST(test_day_markers_resolve_to_the_second_in_odd_zones);
  RUN_TEST(test_rows_without_seconds_fall_back_to_the_hour);
  RUN_TEST(test_at_tag_refines_the_hour_and_never_overrules_it);
  RUN_TEST(test_late_label_uses_the_offset_when_it_was_sent);
  RUN_TEST(test_repeat_fold_never_crosses_local_midnight);
  RUN_TEST(test_repeat_fold_across_the_clock_change_inside_one_day);
  RUN_TEST(test_hour_only_repeats_do_not_fold_across_a_possible_midnight);
  RUN_TEST(test_a_run_of_repeats_stays_one_group_and_never_re_splits);
  RUN_TEST(test_days_never_run_backwards_for_a_turn_without_a_stamp);
  RUN_TEST(test_repeat_window_edge_and_stamp_sources);
  RUN_TEST(test_the_process_zone_rule_gives_the_offset_in_force_at_each_time);
  RUN_TEST(test_history_stays_append_only_across_a_clock_change);
  RUN_TEST(test_unresolved_turns_use_the_fixed_offset);
  RUN_TEST(test_local_zone_prefers_the_per_time_offset);
  RUN_TEST(test_utc_offset_from_broken_down_times);
  UNITY_END();
  return 0;
}
