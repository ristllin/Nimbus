#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/provider_caps.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/usage_ledger.h"

// Per-provider monthly limits as hard freezes (lane DEF2, owner request 22 Oct 2026: Lumi
// at Anthropic 50 USD, OpenAI 10, Z.ai 10, Mistral uncapped, a 5 USD daily warning). The
// count per provider over its budget period, the freeze at the limit for that provider
// alone, one notice per channel per episode, the 80 percent warning, the period
// rollover, a raised limit lifting at once, the overall ceiling off when the owner set
// provider limits, the daily warning, the file, and the park's pause per provider.

using namespace nimbus::orch;

void setUp() {
  setenv("TZ", "UTC0", 1);
  tzset();
}
void tearDown() {}

namespace {
const LocalDate kOct22{2026, 10, 22};
const LocalDate kOct31{2026, 10, 31};
const LocalDate kNov1{2026, 11, 1};
constexpr uint64_t kUsd = 1000000ULL;

std::vector<CapRule> lumiRules() {
  return {{"anthropic", 50 * kUsd, 1}, {"openai", 10 * kUsd, 1}, {"zai", 10 * kUsd, 1}};
}

const ProviderCaps::Row& row(const ProviderCaps& p, const char* name) {
  const int at = p.find(name);
  TEST_ASSERT_TRUE_MESSAGE(at >= 0, name);
  return p.rows[at];
}
}  // namespace

// ---- the freeze, per provider -------------------------------------------------------

static void test_a_provider_at_its_limit_freezes_alone() {
  ProviderCaps p;
  p.setRules(lumiRules(), kOct22);
  p.add("anthropic", 49 * kUsd, kOct22);
  p.add("mistral", 500 * kUsd, kOct22);   // uncapped: any amount
  TEST_ASSERT_FALSE(p.frozenWith("anthropic", 0));
  TEST_ASSERT_FALSE(p.frozenWith("mistral", 0));
  TEST_ASSERT_TRUE(p.frozenWith("anthropic", 1 * kUsd));   // its turn in flight crosses it
  p.add("anthropic", 1 * kUsd, kOct22);
  TEST_ASSERT_TRUE(p.frozenWith("anthropic", 0));
  TEST_ASSERT_TRUE(row(p, "anthropic").frozen);
  // Mistral stays uncapped while Anthropic is frozen; OpenAI and Z.ai keep their room.
  TEST_ASSERT_FALSE(p.frozenWith("mistral", 0));
  TEST_ASSERT_FALSE(p.frozenWith("openai", 0));
  TEST_ASSERT_FALSE(p.frozenWith("zai", 0));
  TEST_ASSERT_FALSE(p.frozenWith("custom", 0));   // a provider with no row
  TEST_ASSERT_TRUE(p.anyCap());
}

static void test_raising_the_limit_lifts_at_once_and_lowering_freezes() {
  ProviderCaps p;
  p.setRules(lumiRules(), kOct22);
  p.add("openai", 11 * kUsd, kOct22);
  TEST_ASSERT_TRUE(p.frozenWith("openai", 0));
  std::vector<CapRule> raised = lumiRules();
  raised[1].capMicro = 20 * kUsd;
  TEST_ASSERT_TRUE(p.setRules(raised, kOct22));
  TEST_ASSERT_FALSE(p.frozenWith("openai", 0));
  raised[1].capMicro = 5 * kUsd;   // under the count: frozen again, a new episode
  p.setRules(raised, kOct22);
  TEST_ASSERT_TRUE(p.frozenWith("openai", 0));
  TEST_ASSERT_EQUAL(0, (int)row(p, "openai").noticesSent);
  // The limit removed (no rule): uncapped, the count kept.
  std::vector<CapRule> none;
  p.setRules(none, kOct22);
  TEST_ASSERT_FALSE(p.frozenWith("openai", 0));
  TEST_ASSERT_EQUAL_UINT64(11 * kUsd, row(p, "openai").spentMicro);
  TEST_ASSERT_FALSE(p.anyCap());
}

static void test_the_period_rolls_quietly_and_never_backwards() {
  ProviderCaps p;
  p.setRules(lumiRules(), kOct31);
  p.add("zai", 12 * kUsd, kOct31);
  TEST_ASSERT_TRUE(p.frozenWith("zai", 0));
  p.markDelivered(NoticeChannel::Web, p.nextOwed(NoticeChannel::Web));
  // A clock set back keeps the count (fail-closed).
  p.roll(LocalDate{2026, 9, 30});
  TEST_ASSERT_EQUAL_UINT64(12 * kUsd, row(p, "zai").spentMicro);
  p.roll(kOct31);
  // The next calendar month (reset day 1, device time zone): a new count, no notice.
  TEST_ASSERT_TRUE(p.roll(kNov1));
  TEST_ASSERT_FALSE(p.frozenWith("zai", 0));
  TEST_ASSERT_EQUAL_UINT64(0, row(p, "zai").spentMicro);
  TEST_ASSERT_TRUE(p.nextOwed(NoticeChannel::Web).kind == ProviderCaps::Owed::None);
  // A budget with its own reset day (the 15th) keeps its window across the month end.
  ProviderCaps q;
  q.setRules({{"anthropic", 10 * kUsd, 15}}, LocalDate{2026, 10, 20});
  q.add("anthropic", 8 * kUsd, LocalDate{2026, 10, 20});
  q.roll(kNov1);   // 1 Nov is still in the 15 Oct - 14 Nov window
  TEST_ASSERT_EQUAL_UINT64(8 * kUsd, row(q, "anthropic").spentMicro);
  q.roll(LocalDate{2026, 11, 15});
  TEST_ASSERT_EQUAL_UINT64(0, row(q, "anthropic").spentMicro);
  // An unset clock: the spend carries into the first period the clock reads.
  ProviderCaps u;
  u.setRules(lumiRules(), LocalDate{});
  u.add("anthropic", 3 * kUsd, LocalDate{});
  u.roll(kOct22);
  TEST_ASSERT_EQUAL_UINT64(3 * kUsd, row(u, "anthropic").spentMicro);
}

// ---- notices ------------------------------------------------------------------------

static void test_one_notice_per_channel_per_episode_and_one_warning() {
  ProviderCaps p;
  p.setRules(lumiRules(), kOct22);
  p.add("anthropic", 40 * kUsd, kOct22);   // 80 percent
  ProviderCaps::Owed o = p.nextOwed(NoticeChannel::Telegram);
  TEST_ASSERT_TRUE(o.kind == ProviderCaps::Owed::Warn);
  TEST_ASSERT_TRUE(p.markDelivered(NoticeChannel::Telegram, o));
  TEST_ASSERT_TRUE(p.nextOwed(NoticeChannel::Web).kind == ProviderCaps::Owed::None);   // once
  p.add("anthropic", 10 * kUsd, kOct22);   // the limit
  for (NoticeChannel ch : {NoticeChannel::Telegram, NoticeChannel::Web}) {
    o = p.nextOwed(ch);
    TEST_ASSERT_TRUE(o.kind == ProviderCaps::Owed::Freeze);
    TEST_ASSERT_EQUAL_STRING("anthropic", p.rows[o.row].provider);
    TEST_ASSERT_TRUE(p.markDelivered(ch, o));
    TEST_ASSERT_TRUE(p.nextOwed(ch).kind == ProviderCaps::Owed::None);
  }
  TEST_ASSERT_TRUE(p.nextOwed(NoticeChannel::Screen).kind == ProviderCaps::Owed::None);
  // More spend inside the episode owes nothing more; a second provider's freeze does.
  p.add("anthropic", 5 * kUsd, kOct22);
  TEST_ASSERT_TRUE(p.nextOwed(NoticeChannel::Web).kind == ProviderCaps::Owed::None);
  p.add("zai", 10 * kUsd, kOct22);
  o = p.nextOwed(NoticeChannel::Web);
  TEST_ASSERT_TRUE(o.kind == ProviderCaps::Owed::Freeze);
  TEST_ASSERT_EQUAL_STRING("zai", p.rows[o.row].provider);
}

// The class over random streams: a provider is frozen exactly when it has a limit and its
// count reached it, and no channel hears one episode twice.
static void test_random_streams_hold_the_rule() {
  std::mt19937 rng(22102026);
  const char* provs[] = {"anthropic", "openai", "zai", "mistral"};
  for (int run = 0; run < 50; ++run) {
    ProviderCaps p;
    p.setRules(lumiRules(), kOct22);
    int told[4][2] = {{0}};
    for (int step = 0; step < 200; ++step) {
      const int i = (int)(rng() % 4);
      p.add(provs[i], (uint64_t)(rng() % 900000), kOct22);
      for (int c = 0; c < 2; ++c) {
        const NoticeChannel ch = c ? NoticeChannel::Web : NoticeChannel::Telegram;
        const ProviderCaps::Owed o = p.nextOwed(ch);
        if (o.kind == ProviderCaps::Owed::Freeze) {
          for (int k = 0; k < 4; ++k)
            if (std::strcmp(p.rows[o.row].provider, provs[k]) == 0) ++told[k][c];
          p.markDelivered(ch, o);
        } else if (o.kind == ProviderCaps::Owed::Warn) {
          p.markDelivered(ch, o);
        }
      }
      for (int k = 0; k < 4; ++k) {
        const int at = p.find(provs[k]);
        if (at < 0) continue;
        const ProviderCaps::Row& r = p.rows[at];
        TEST_ASSERT_EQUAL(r.capMicro && r.spentMicro >= r.capMicro, p.frozenWith(provs[k], 0));
      }
    }
    for (int k = 0; k < 4; ++k)
      for (int c = 0; c < 2; ++c) TEST_ASSERT_TRUE(told[k][c] <= 1);
    TEST_ASSERT_FALSE(p.frozenWith("mistral", 0));
  }
}

// ---- the overall ceiling with provider limits ---------------------------------------

static void test_the_overall_ceiling_is_off_when_provider_limits_are_set() {
  SpendCeiling c;
  c.add(25 * kUsd, 202610);
  TEST_ASSERT_TRUE(c.frozenWith(0));   // the 20 USD default holds on a new device
  TEST_ASSERT_TRUE(c.setProviderCaps(true));
  TEST_ASSERT_FALSE(c.active());
  TEST_ASSERT_FALSE(c.frozenWith(5 * kUsd));
  TEST_ASSERT_FALSE(c.warnOwed());
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Web) == SpendCeiling::Notice::None);
  // An overall ceiling the owner chose still applies with provider limits.
  c.setLimit(30 * kUsd);
  TEST_ASSERT_TRUE(c.active());
  TEST_ASSERT_FALSE(c.frozenWith(0));
  c.add(6 * kUsd, 202610);
  TEST_ASSERT_TRUE(c.frozenWith(0));
  // ... and the owner can turn it off explicitly (kept across the file).
  TEST_ASSERT_TRUE(c.setLimit(0));
  TEST_ASSERT_FALSE(c.active());
  TEST_ASSERT_FALSE(c.frozenWith(0));
  SpendCeiling back;
  TEST_ASSERT_TRUE(back.deserialize(c.serialize()));
  TEST_ASSERT_TRUE(back.ownerSet);
  TEST_ASSERT_EQUAL_UINT64(0, back.limitMicro);
  TEST_ASSERT_FALSE(back.active());
  // Off with no provider limits too.
  SpendCeiling solo;
  solo.setLimit(0);
  solo.add(500 * kUsd, 202610);
  TEST_ASSERT_FALSE(solo.frozenWith(0));
}

// The update notice on a device whose earlier file meets provider limits: owed on the web
// chat, and on Telegram only with a bot; it names every limit and says the overall ceiling
// is off for them.
static void test_the_update_notice_names_the_limits() {
  SpendCeiling c;
  c.owesMigration(false);
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Web) == SpendCeiling::Notice::Migration);
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Telegram) == SpendCeiling::Notice::None);
  c.owesMigration(true);
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Telegram) == SpendCeiling::Notice::Migration);
  TEST_ASSERT_TRUE(c.markDelivered(NoticeChannel::Web, SpendCeiling::Notice::Migration));
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Web) == SpendCeiling::Notice::None);
  const std::string s = capsMigrationText(lumiRules(), false, 0);
  TEST_ASSERT_NOT_NULL(std::strstr(s.c_str(), "Anthropic $50.00, OpenAI $10.00, Z.ai $10.00"));
  TEST_ASSERT_NOT_NULL(std::strstr(s.c_str(), "overall monthly ceiling is off"));
}

// A full table never costs a limit: the limited provider takes the row of one no rule
// limits (never a paused one), and an unlimited provider past the table is simply not counted.
static void test_a_full_table_keeps_every_limit() {
  ProviderCaps p;
  const char* names[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8"};
  p.setRules({{"p8", 1 * kUsd, 1}}, kOct22);
  for (const char* n : names) p.add(n, 2 * kUsd, kOct22);
  TEST_ASSERT_EQUAL((int)ProviderCaps::kMaxRows, (int)p.n);
  TEST_ASSERT_TRUE(p.frozenWith("p8", 0));
  TEST_ASSERT_FALSE(p.add("p9", kUsd, kOct22));   // no row: an unlimited count is dropped
  p.setRules({{"p8", 1 * kUsd, 1}, {"anthropic", 50 * kUsd, 1}}, kOct22);
  TEST_ASSERT_TRUE(p.find("anthropic") >= 0);
  TEST_ASSERT_EQUAL_UINT64(50 * kUsd, row(p, "anthropic").capMicro);
  TEST_ASSERT_TRUE(p.frozenWith("p8", 0));        // the paused one kept its row
  TEST_ASSERT_TRUE(p.find("p1") < 0);             // an unlimited one gave its row up
  p.add("anthropic", 51 * kUsd, kOct22);
  TEST_ASSERT_TRUE(p.frozenWith("anthropic", 0));
}

// ---- the daily warning --------------------------------------------------------------

static void test_the_daily_warning_once_a_day_per_channel() {
  DaySpend d;
  const uint32_t today = civilDayKey(2026, 10, 22);
  d.add(6 * kUsd, today);
  TEST_ASSERT_FALSE(d.owed(NoticeChannel::Web));   // no owner threshold: the default rule
  d.setThreshold(5 * kUsd);
  TEST_ASSERT_TRUE(d.owed(NoticeChannel::Web));
  TEST_ASSERT_TRUE(d.owed(NoticeChannel::Telegram));
  TEST_ASSERT_FALSE(d.owed(NoticeChannel::Screen));
  d.mark(NoticeChannel::Web);
  d.add(1 * kUsd, today);
  TEST_ASSERT_FALSE(d.owed(NoticeChannel::Web));   // once a day
  TEST_ASSERT_TRUE(d.owed(NoticeChannel::Telegram));
  d.mark(NoticeChannel::Telegram);
  d.add(1, today + 1);   // the next day: a new count
  TEST_ASSERT_EQUAL_UINT64(1, d.spentMicro);
  TEST_ASSERT_FALSE(d.owed(NoticeChannel::Web));
  d.add(5 * kUsd, today + 1);
  TEST_ASSERT_TRUE(d.owed(NoticeChannel::Web));
  // A raised threshold warns again when reached; 0 hands back to the default rule.
  d.mark(NoticeChannel::Web);
  d.setThreshold(10 * kUsd);
  d.add(5 * kUsd, today + 1);
  TEST_ASSERT_TRUE(d.owed(NoticeChannel::Web));
  d.setThreshold(0);
  TEST_ASSERT_FALSE(d.owed(NoticeChannel::Web));
  // The owner's field keeps a sub-dollar warning (the overall ceiling's floor is 1 USD).
  TEST_ASSERT_EQUAL_UINT64(250000, parseCeilingCents("25", 10000));
  TEST_ASSERT_EQUAL_UINT64(10000, parseCeilingCents("1", 10000));
  TEST_ASSERT_EQUAL_UINT64(kUsd, parseCeilingCents("25"));
  TEST_ASSERT_EQUAL_UINT64(0, parseCeilingCents("-5", 10000));
  TEST_ASSERT_EQUAL_UINT64(0, parseCeilingCents("0.5", 10000));
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kMaxLimitMicro, parseCeilingCents("999999999999999999999", 10000));
}

// ---- the file -----------------------------------------------------------------------

static void test_the_file_keeps_every_part_and_reads_the_earlier_line() {
  SpendCeiling c;
  c.add(7 * kUsd, 202610);
  DaySpend d;
  d.setThreshold(5 * kUsd);
  d.add(2 * kUsd, civilDayKey(2026, 10, 22));
  d.mark(NoticeChannel::Web);
  ProviderCaps p;
  p.setRules(lumiRules(), kOct22);
  p.add("anthropic", 51 * kUsd, kOct22);
  p.markDelivered(NoticeChannel::Telegram, p.nextOwed(NoticeChannel::Telegram));
  p.add("mistral", 3 * kUsd, kOct22);
  const std::string line = serializeCeilingFile(c, d, p);
  SpendCeiling c2;
  DaySpend d2;
  ProviderCaps p2;
  TEST_ASSERT_TRUE(deserializeCeilingFile(line, c2, d2, p2));
  TEST_ASSERT_EQUAL_UINT64(7 * kUsd, c2.spentMicro);
  TEST_ASSERT_EQUAL_UINT64(5 * kUsd, d2.thresholdMicro);
  TEST_ASSERT_EQUAL_UINT64(2 * kUsd, d2.spentMicro);
  TEST_ASSERT_TRUE(d2.told & 2);
  TEST_ASSERT_EQUAL(4, (int)p2.n);   // the three capped providers and Mistral's count
  p2.setRules(lumiRules(), kOct22);   // the cap table comes back from the ledger
  TEST_ASSERT_TRUE(p2.frozenWith("anthropic", 0));
  TEST_ASSERT_TRUE(p2.nextOwed(NoticeChannel::Telegram).kind == ProviderCaps::Owed::None);   // told once
  TEST_ASSERT_TRUE(p2.nextOwed(NoticeChannel::Web).kind == ProviderCaps::Owed::Freeze);
  TEST_ASSERT_EQUAL_UINT64(3 * kUsd, row(p2, "mistral").spentMicro);
  // The earlier build's nine fields still read (its reader stops after them, so it reads
  // this line too).
  SpendCeiling old;
  TEST_ASSERT_TRUE(old.deserialize(line));
  TEST_ASSERT_EQUAL_UINT64(7 * kUsd, old.spentMicro);
  SpendCeiling c3;
  DaySpend d3;
  ProviderCaps p3;
  bool earlier = false;
  TEST_ASSERT_TRUE(deserializeCeilingFile(line, c3, d3, p3, &earlier));
  TEST_ASSERT_FALSE(earlier);
  TEST_ASSERT_TRUE(deserializeCeilingFile(c.serialize(), c3, d3, p3, &earlier));
  TEST_ASSERT_TRUE(earlier);   // the earlier release's file: the store owes the update notice
  TEST_ASSERT_EQUAL(0, (int)p3.n);
  TEST_ASSERT_EQUAL_UINT64(0, d3.thresholdMicro);
  // A bad provider group is skipped, never the file (a bad name, a bad number, a repeat).
  TEST_ASSERT_TRUE(deserializeCeilingFile(
      line + ";p|bad name|1|2|0|0|0;p|custom|x|1|0|0|0;p|zai|1|1|0|0|0", c3, d3, p3));
  TEST_ASSERT_EQUAL(4, (int)p3.n);
  TEST_ASSERT_FALSE(deserializeCeilingFile("garbage", c3, d3, p3));
}

// ---- the park ---------------------------------------------------------------------

// The store hands a provider's freeze to the park with its own cause: every seam of that
// provider closes, the others stay open, and lifting it never releases another cause.
static void test_the_park_pauses_that_provider_on_every_seam() {
  ProviderPark& park = providerPark();
  park.clear();
  park.pause("anthropic", kPauseProviderLimit);
  for (int s = 0; s < (int)kPaidSeamCount; ++s) {
    const ParkVerdict a = park.available("anthropic", (PaidSeam)s, 0, 1000);
    TEST_ASSERT_FALSE(a.open);
    TEST_ASSERT_EQUAL(kPauseProviderLimit, a.paused);
    TEST_ASSERT_TRUE(park.available("mistral", (PaidSeam)s, 0, 1000).open);
  }
  const ParkVerdict v = park.available("anthropic", PaidSeam::Chat, 0, 1000);
  TEST_ASSERT_EQUAL_STRING("paid requests to anthropic are paused: its monthly limit is reached",
                           parkWords("anthropic", v).c_str());
  park.pause("anthropic", kPauseOwner);
  park.resume("anthropic", kPauseProviderLimit);
  TEST_ASSERT_EQUAL(kPauseOwner, park.pausedBy("anthropic"));   // the owner's pause stays
  park.resume("anthropic", kPauseOwner);
  TEST_ASSERT_TRUE(park.available("anthropic", PaidSeam::Chat, 0, 1000).open);
  park.clear();
}

// ---- copy goldens -------------------------------------------------------------------

static std::string slurp(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void test_copy_goldens() {
  CapFacts f;
  f.provider = "anthropic";
  f.capMicro = 50 * kUsd;
  f.spentMicro = 50120000;
  f.periodKey = usagePeriodKey(2026, 10, 22, 1);
  f.resetDay = 1;
  CapFacts w = f;
  w.spentMicro = 40100000;
  std::string out;
  out += "[provider freeze]\n" + providerFreezeText(f) + "\n";
  out += "[provider warning]\n" + providerWarnText(w) + "\n";
  out += "[daily warning]\n" + dailyWarnText(5200000, 5 * kUsd) + "\n";
  out += "[migration, provider limits, overall off]\n" + capsMigrationText(lumiRules(), false, 20 * kUsd) + "\n";
  out += "[migration, provider limits, overall on]\n" + capsMigrationText(lumiRules(), true, 75 * kUsd) + "\n";
  for (char ch : out) TEST_ASSERT_TRUE_MESSAGE(ch == '\n' || (ch >= 0x20 && ch < 0x7f), "printable ASCII");
  const char* path = "test/golden/provider_caps_copy.txt";
  if (getenv("GOLDEN_UPDATE")) {
    std::ofstream(path, std::ios::binary) << out;
    TEST_PASS_MESSAGE("golden written");
    return;
  }
  TEST_ASSERT_EQUAL_STRING(slurp(path).c_str(), out.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_provider_at_its_limit_freezes_alone);
  RUN_TEST(test_raising_the_limit_lifts_at_once_and_lowering_freezes);
  RUN_TEST(test_the_period_rolls_quietly_and_never_backwards);
  RUN_TEST(test_one_notice_per_channel_per_episode_and_one_warning);
  RUN_TEST(test_random_streams_hold_the_rule);
  RUN_TEST(test_the_overall_ceiling_is_off_when_provider_limits_are_set);
  RUN_TEST(test_the_update_notice_names_the_limits);
  RUN_TEST(test_a_full_table_keeps_every_limit);
  RUN_TEST(test_the_daily_warning_once_a_day_per_channel);
  RUN_TEST(test_the_file_keeps_every_part_and_reads_the_earlier_line);
  RUN_TEST(test_the_park_pauses_that_provider_on_every_seam);
  RUN_TEST(test_copy_goldens);
  return UNITY_END();
}
