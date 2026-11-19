// test_stamp_replay - the message stamp on the engines (RB-REC5, ruling 42 item 2(c)).
//
// 1. One function, the same bytes: for the same instant, zone and day start, engine 1's
//    per-turn clock line (a real TurnEngine turn over a scripted head) carries exactly
//    orch::day::stamp's bytes, and engine 2's tx::messageStamp is the same bytes in
//    brackets. nimbusd and the harness lab run engine 1's TurnEngine with their own
//    bindings: nimbusd/tests/test_hosted_stamp.cpp and the lab's selftest pin theirs.
// 2. Replays in three domains with identical expectations (ruling 41: no domain in the
//    core, food is one validation family of three): a late food entry, a late expense and
//    a late training session, each sent after midnight and before the day start, read as
//    belonging to the running day. The scripted head is a reader that follows only what
//    the request says (the stamp's own date, or the day its clause names), so a stamp
//    without the clause, or with the wrong day, files the entry on the wrong day.
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/engine.h"
#include "nimbus/orch/day_start.h"
#include "nimbus/orch/tx/item.h"

using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace day = nimbus::orch::day;
namespace tx = nimbus::orch::tx;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

namespace {

uint64_t utc(int y, int mo, int d, int h, int mi) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  return (uint64_t)timegm(&t);
}

// The text after "Current time: " up to its ".\n" in whatever the head was sent.
std::string clockLine(const std::string& sent) {
  const std::string key = "Current time: ";
  const size_t at = sent.find(key);
  if (at == std::string::npos) return std::string();
  const size_t end = sent.find(".\n", at);
  return sent.substr(at + key.size(), end == std::string::npos ? std::string::npos : end - at - key.size());
}

// The reader: the day an entry sent now belongs to, from the stamp alone.
std::string dayFromStamp(const std::string& stamp) {
  const size_t c = stamp.find(" (day ");
  return c == std::string::npos ? stamp.substr(0, 10) : stamp.substr(c + 6, 10);
}

// An engine-1 turn over a scripted head that records what it was sent.
struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  int32_t offsetMin = 0;
  uint16_t startMin = day::kDefaultStartMin;
  bool wireStart = true;
  std::string now = "set";   // what the host's composeInputs says (empty = clock unsynced)
  std::string sent;          // instructions + inputs of the last request
  std::unique_ptr<TurnEngine> eng;

  Rig() {
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.hosts.add("anthropic", [this](std::string&, const std::string& instructions, const std::string& inputs,
                                    std::string& out, std::string&, const agent::HeadTools*,
                                    nimbus::orch::TokenUsage*) {
      sent = instructions + "\n" + inputs;
      out = "{\"reply\":\"noted\",\"memory\":\"\",\"ask\":\"\"}";
      return true;
    });
    d.utcOffsetAt = [this](uint32_t) { return offsetMin; };
    d.utcOffsetMin = [this] { return offsetMin; };
    d.dayStartMin = [this] { return wireStart ? startMin : day::kDefaultStartMin; };
    d.composeInputs = [this](const std::string&) {
      agent::ComposeInputs in;
      if (!now.empty()) in.now = hostNow();
      return in;
    };
    eng.reset(new TurnEngine(std::move(d)));
  }
  // The device's localTimeStr shape at the rig's instant and offset.
  std::string hostNow() const {
    const time_t t = (time_t)(plat.epoch + (int64_t)offsetMin * 60);
    struct tm g;
    gmtime_r(&t, &g);
    char buf[40];
    strftime(buf, sizeof buf, "%Y-%m-%d %a %H:%M ZON", &g);
    return buf;
  }
  std::string turnAt(uint64_t epoch, const std::string& text) {
    plat.epoch = epoch;
    sent.clear();
    eng->runTurn(text, "1001", text);
    return sent;
  }
};

std::string sharedStamp(uint64_t epoch, int32_t off, uint16_t start) {
  char buf[day::kFullCap];
  day::stamp(epoch, off, start, buf, sizeof buf);
  return buf;
}

std::string engine2Stamp(uint64_t epoch, int16_t off, uint16_t start) {
  char buf[tx::kStampMaxBytes];
  TEST_ASSERT_TRUE(tx::messageStamp(epoch, off, start, buf, sizeof buf).ok());
  const std::string s = buf;
  TEST_ASSERT_TRUE(s.size() >= 2 && s.front() == '[' && s.back() == ']');
  return s.substr(1, s.size() - 2);
}

}  // namespace

// ---- one function, the same bytes ---------------------------------------------------------

void test_engine_1_and_engine_2_carry_the_same_stamp_bytes() {
  struct Case {
    uint64_t epoch;
    int32_t off;
    uint16_t start;
  } cases[] = {
      {utc(2026, 11, 19, 0, 30), 0, 240},     {utc(2026, 11, 19, 3, 59), 0, 240},
      {utc(2026, 11, 19, 4, 0), 0, 240},      {utc(2026, 11, 19, 8, 10), 0, 240},
      {utc(2026, 11, 19, 0, 30), 0, 0},       {utc(2026, 12, 31, 23, 30), 60, 240},
      {utc(2028, 2, 29, 23, 30), 60, 240},    {utc(2026, 3, 29, 1, 30), 60, 240},
      {utc(2026, 10, 25, 1, 30), 0, 240},     {utc(2026, 11, 19, 8, 0), -480, 240},
      {utc(2026, 11, 18, 19, 0), 330, 270},   {utc(2026, 11, 19, 11, 30), 840, 1439},
      {utc(2026, 11, 19, 14, 0), -720, 1},
  };
  Rig r;
  for (const Case& c : cases) {
    r.offsetMin = c.off;
    r.startMin = c.start;
    const std::string want = sharedStamp(c.epoch, c.off, c.start);
    const std::string sent = r.turnAt(c.epoch, "hello");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), clockLine(sent).c_str(), sent.substr(0, 200).c_str());
    TEST_ASSERT_EQUAL_STRING(want.c_str(), engine2Stamp(c.epoch, (int16_t)c.off, c.start).c_str());
    // The cached date line agrees with the stamp's date and names the zone.
    const std::string dateLine = "Current date: " + want.substr(0, 14) + ", time zone ZON (";
    TEST_ASSERT_TRUE_MESSAGE(sent.find(dateLine) != std::string::npos, dateLine.c_str());
  }
}

// The host says the clock is not synced: no stamp, the honest UNKNOWN line, never a date.
void test_no_clock_on_engine_1_is_the_unknown_line() {
  Rig r;
  r.now.clear();
  const std::string sent = r.turnAt(utc(2026, 11, 19, 0, 30), "hello");
  TEST_ASSERT_TRUE(sent.find("Current date-time: UNKNOWN (device clock not synced)") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", clockLine(sent).c_str());
  TEST_ASSERT_TRUE(sent.find("2026-11-1") == std::string::npos);
  // The host says synced but the platform clock was never set: the plain lines, no stamp.
  Rig b;
  const std::string s2 = b.turnAt(5000, "hello");
  TEST_ASSERT_TRUE(s2.find("(day ") == std::string::npos);
  TEST_ASSERT_TRUE(s2.find("Current time: 01:23 ZON.") != std::string::npos);
}

// An unwired day start is the default, 04:00.
void test_an_unwired_day_start_is_04_00() {
  Rig r;
  r.wireStart = false;
  r.startMin = 0;
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)",
                           clockLine(r.turnAt(utc(2026, 11, 19, 0, 30), "hi")).c_str());
}

// ---- three domains, identical expectations -------------------------------------------------

void test_late_entries_in_three_domains_read_as_the_running_day() {
  struct Domain {
    const char* family;
    const char* text;
  } domains[] = {
      {"food", "log a late snack: toast with butter"},
      {"expense", "log an expense: taxi home 18.40"},
      {"training", "log a training session: 5 km run, 31 minutes"},
  };
  struct Moment {
    uint64_t epoch;
    const char* wantDay;
  } moments[] = {
      {utc(2026, 11, 18, 23, 50), "2026-11-18"},   // before midnight: the calendar day
      {utc(2026, 11, 19, 0, 30), "2026-11-18"},    // after midnight: still the running day
      {utc(2026, 11, 19, 2, 15), "2026-11-18"},
      {utc(2026, 11, 19, 3, 59), "2026-11-18"},
      {utc(2026, 11, 19, 4, 0), "2026-11-19"},     // the day start: the new day
      {utc(2026, 11, 19, 9, 0), "2026-11-19"},
      {utc(2027, 1, 1, 0, 45), "2026-12-31"},      // New Year's night belongs to the old year
      {utc(2028, 3, 1, 1, 0), "2028-02-29"},       // and a leap day keeps its night
  };
  std::vector<std::string> perDomain[3];
  for (int i = 0; i < 3; ++i) {
    Rig r;
    for (const Moment& m : moments) {
      const std::string got = dayFromStamp(clockLine(r.turnAt(m.epoch, domains[i].text)));
      char msg[160];
      std::snprintf(msg, sizeof msg, "%s at %llu", domains[i].family, (unsigned long long)m.epoch);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(m.wantDay, got.c_str(), msg);
      // Engine 2's stamp gives the reader the same day.
      TEST_ASSERT_EQUAL_STRING_MESSAGE(m.wantDay, dayFromStamp(engine2Stamp(m.epoch, 0, 240)).c_str(), msg);
      perDomain[i].push_back(got);
    }
  }
  // Identical expectations: the three families file every moment on the same day.
  TEST_ASSERT_TRUE(perDomain[0] == perDomain[1] && perDomain[1] == perDomain[2]);
}

// Counter-test: without the clause (a 00:00 day start, the stamp's calendar date only) the
// same reader files the 00:30 entry on the new calendar day. The replay above is red if the
// clause is lost.
void test_counter_without_the_clause_the_late_entry_lands_on_the_next_day() {
  Rig r;
  r.startMin = 0;
  const std::string got = dayFromStamp(clockLine(r.turnAt(utc(2026, 11, 19, 0, 30), "log a late snack")));
  TEST_ASSERT_EQUAL_STRING("2026-11-19", got.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_engine_1_and_engine_2_carry_the_same_stamp_bytes);
  RUN_TEST(test_no_clock_on_engine_1_is_the_unknown_line);
  RUN_TEST(test_an_unwired_day_start_is_04_00);
  RUN_TEST(test_late_entries_in_three_domains_read_as_the_running_day);
  RUN_TEST(test_counter_without_the_clause_the_late_entry_lands_on_the_next_day);
  return UNITY_END();
}
