#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/usage_ledger.h"

// The monthly spend ceiling (lane DEF, owner requirement 21 Oct 2026): the month key in
// the device timezone (an unset clock, a clock that jumps), the counter and the freeze
// over every provider's price table and the router's price state, one notice per
// channel per freeze episode across crossings and reboots, the copy goldens, and the
// process-wide paid-call gate.

using namespace nimbus::orch;

static const char* kGoldenDir = "test/golden";
static const char* kOutDir = "test/golden/out";

void setUp() {
  setenv("TZ", "UTC0", 1);
  tzset();
  installSpendFreeze(nullptr);
  installPendingSpend(nullptr, nullptr);
  resetPaidGateCounts();
}
void tearDown() {}

// ---- helpers ---------------------------------------------------------------------

static time_t utc(int y, int mo, int d, int h, int mi) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  return timegm(&t);
}

static void tz(const char* zone) {
  setenv("TZ", zone, 1);
  tzset();
}

static bool blessMode() {
  const char* e = std::getenv("GOLDEN_UPDATE");
  return e && std::strcmp(e, "1") == 0;
}

static bool readFile(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

static void writeFile(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;   // test/golden/out is gitignored and may be absent
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}

static void checkTextGolden(const char* name, const std::string& current) {
  const std::string goldenPath = std::string(kGoldenDir) + "/" + name;
  if (blessMode()) {
    writeFile(goldenPath, current);
    TEST_MESSAGE((std::string("blessed ") + goldenPath).c_str());
    return;
  }
  std::string blessed;
  if (!readFile(goldenPath, blessed)) {
    writeFile(std::string(kOutDir) + "/" + name, current);
    TEST_FAIL_MESSAGE((std::string("missing golden ") + goldenPath +
                       " - bless with GOLDEN_UPDATE=1").c_str());
    return;
  }
  if (blessed != current) {
    writeFile(std::string(kOutDir) + "/" + name, current);
    TEST_FAIL_MESSAGE((std::string("copy drift vs ") + goldenPath + ":\n" + current).c_str());
  }
}

// Owner-facing copy rules (AGENTS.md section 6): printable ASCII, no em dash, the
// device is "it" (no I / me / my), no exclamation-mark alarm, no ticket numbers.
static void assertPlainCopy(const std::string& s) {
  for (unsigned char c : s) TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 && c < 0x7F, s.c_str());
  TEST_ASSERT_TRUE(s.find('!') == std::string::npos);
  TEST_ASSERT_TRUE(s.find("CUM-") == std::string::npos);
  for (const char* w : {" I ", " me ", " my ", "I'm", "I've"})
    TEST_ASSERT_TRUE_MESSAGE(s.find(w) == std::string::npos, s.c_str());
}

// ---- month key -------------------------------------------------------------------

static void test_month_key_utc_and_unset_clock() {
  TEST_ASSERT_EQUAL_UINT32(202610u, ceilingMonthKey(utc(2026, 10, 31, 23, 59)));
  TEST_ASSERT_EQUAL_UINT32(202611u, ceilingMonthKey(utc(2026, 11, 1, 0, 0)));
  // An unset clock (the epoch, or any time before the device's synced threshold).
  TEST_ASSERT_EQUAL_UINT32(0u, ceilingMonthKey(0));
  TEST_ASSERT_EQUAL_UINT32(0u, ceilingMonthKey(kCeilingClockValid - 1));
  TEST_ASSERT_NOT_EQUAL(0u, ceilingMonthKey(kCeilingClockValid));
  // No zone set at all reads UTC.
  unsetenv("TZ");
  tzset();
  TEST_ASSERT_EQUAL_UINT32(202610u, ceilingMonthKey(utc(2026, 10, 31, 23, 59)));
}

static void test_month_key_follows_the_device_timezone() {
  // Tokyo is nine hours ahead: 15:30 UTC on 31 Oct is 00:30 on 1 Nov there.
  tz("JST-9");
  TEST_ASSERT_EQUAL_UINT32(202611u, ceilingMonthKey(utc(2026, 10, 31, 15, 30)));
  TEST_ASSERT_EQUAL_UINT32(202610u, ceilingMonthKey(utc(2026, 10, 31, 14, 59)));
  // New York is behind (and on daylight time until 1 Nov): 03:30 UTC on 1 Nov is still
  // 31 Oct there.
  tz("EST5EDT,M3.2.0,M11.1.0");
  TEST_ASSERT_EQUAL_UINT32(202610u, ceilingMonthKey(utc(2026, 11, 1, 3, 30)));
  TEST_ASSERT_EQUAL_UINT32(202611u, ceilingMonthKey(utc(2026, 11, 1, 4, 30)));
  // The year boundary.
  tz("UTC0");
  TEST_ASSERT_EQUAL_UINT32(202612u, ceilingMonthKey(utc(2026, 12, 31, 23, 0)));
  TEST_ASSERT_EQUAL_UINT32(202701u, ceilingMonthKey(utc(2027, 1, 1, 0, 0)));
  TEST_ASSERT_EQUAL_UINT32(202701u, nextMonthKey(202612u));
  TEST_ASSERT_EQUAL_UINT32(202611u, nextMonthKey(202610u));
  TEST_ASSERT_EQUAL_UINT32(0u, nextMonthKey(0));
}

// ---- the counter, the rollover, the clock ----------------------------------------

static void test_spend_while_clock_unset_counts_toward_the_first_month() {
  SpendCeiling c;
  c.add(3000000, 0);   // $3 before the clock was set
  TEST_ASSERT_EQUAL_UINT32(0u, c.monthKey);
  c.add(1000000, 202610);
  TEST_ASSERT_EQUAL_UINT32(202610u, c.monthKey);
  TEST_ASSERT_EQUAL_UINT64(4000000ULL, c.spentMicro);   // carried, never dropped
}

static void test_rollover_starts_a_new_count_and_closes_the_freeze_quietly() {
  SpendCeiling c;
  c.add(25000000, 202610);   // over the $20 default
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_TRUE(c.noticeOwed(NoticeChannel::Telegram));
  c.markNotice(NoticeChannel::Telegram);
  TEST_ASSERT_TRUE(c.roll(202611));
  TEST_ASSERT_FALSE(c.frozen);
  TEST_ASSERT_EQUAL_UINT64(0ULL, c.spentMicro);
  // No notice storm at the rollover: nothing is owed on any channel.
  for (int ch = 0; ch < kNoticeChannelCount; ++ch)
    TEST_ASSERT_FALSE(c.noticeOwed((NoticeChannel)ch));
  TEST_ASSERT_FALSE(c.warnOwed());
  TEST_ASSERT_FALSE(c.roll(202611));   // the same month again: nothing moves
}

static void test_a_clock_that_jumps_never_clears_spend_backwards() {
  SpendCeiling c;
  c.add(18000000, 202611);
  // The clock is set back a month (a bad sync corrected): the count moves with it.
  TEST_ASSERT_TRUE(c.roll(202610));
  TEST_ASSERT_EQUAL_UINT32(202610u, c.monthKey);
  TEST_ASSERT_EQUAL_UINT64(18000000ULL, c.spentMicro);
  // An unset clock (0) moves nothing.
  TEST_ASSERT_FALSE(c.roll(0));
  TEST_ASSERT_EQUAL_UINT64(18000000ULL, c.spentMicro);
  // Forward: a later month starts a new count.
  TEST_ASSERT_TRUE(c.roll(202612));
  TEST_ASSERT_EQUAL_UINT64(0ULL, c.spentMicro);
}

static void test_raising_the_ceiling_clears_the_freeze_at_once() {
  SpendCeiling c;
  c.add(20000000, 202610);   // exactly at the ceiling: frozen
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_TRUE(c.setLimit(30000000));
  TEST_ASSERT_FALSE(c.frozen);
  TEST_ASSERT_TRUE(c.ownerSet);
  for (int ch = 0; ch < kNoticeChannelCount; ++ch)
    TEST_ASSERT_FALSE(c.noticeOwed((NoticeChannel)ch));
  // Lowering it under the count opens a new episode.
  TEST_ASSERT_TRUE(c.setLimit(10000000));
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_TRUE(c.noticeOwed(NoticeChannel::Web));
  // Clamps: at least 1 USD, at most 100,000 USD.
  c.setLimit(1);
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kMinLimitMicro, c.limitMicro);
  c.setLimit(UINT64_MAX);
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kMaxLimitMicro, c.limitMicro);
}

static void test_pending_spend_freezes_before_it_is_committed() {
  SpendCeiling c;
  c.add(19000000, 202610);
  TEST_ASSERT_FALSE(c.frozenWith(0));
  TEST_ASSERT_FALSE(c.frozenWith(999999));
  TEST_ASSERT_TRUE(c.frozenWith(1000000));   // the turn in flight reaches $20
  TEST_ASSERT_TRUE(c.frozenWith(UINT64_MAX));  // saturates, never wraps
}

static void test_warning_at_eighty_percent_once_a_month() {
  SpendCeiling c;
  c.add(15990000, 202610);
  TEST_ASSERT_FALSE(c.warnOwed());
  c.add(10000, 202610);   // $16.00 = 80% of $20
  TEST_ASSERT_TRUE(c.warnOwed());
  c.markWarned();
  TEST_ASSERT_FALSE(c.warnOwed());
  c.add(5000000, 202610);   // frozen: the freeze notice replaces any warning
  TEST_ASSERT_FALSE(c.warnOwed());
  c.roll(202611);
  c.add(17000000, 202611);
  TEST_ASSERT_TRUE(c.warnOwed());   // a new month warns again
}

// ---- one notice per channel per episode, across crossings and reboots -------------

// One random step of the walk: spend, a limit edit, a month roll or a reboot (a
// serialize and a deserialize, which must round-trip exactly).
static void randomStep(SpendCeiling& c, uint32_t& month, std::mt19937& rng) {
  switch (rng() % 6) {
    case 0: case 1: c.add((rng() % 8) * 1000000ULL, month); break;
    case 2: c.setLimit((5 + rng() % 30) * 1000000ULL); break;
    case 3:
      if (rng() % 4 == 0) {
        month = nextMonthKey(month);
        c.roll(month);
      }
      break;
    case 4: {
      SpendCeiling r;
      TEST_ASSERT_TRUE(r.deserialize(c.serialize()));
      TEST_ASSERT_EQUAL_STRING(c.serialize().c_str(), r.serialize().c_str());
      c = r;
      break;
    }
    default: break;
  }
}

// The pump: deliver whatever is owed; each channel at most once per episode.
static void pumpNotices(SpendCeiling& c, int (&told)[kNoticeChannelCount]) {
  for (int ch = 0; ch < kNoticeChannelCount; ++ch) {
    if (!c.noticeOwed((NoticeChannel)ch)) continue;
    TEST_ASSERT_TRUE(c.frozen);
    TEST_ASSERT_TRUE(c.markNotice((NoticeChannel)ch));
    TEST_ASSERT_EQUAL_INT(1, ++told[ch]);
    TEST_ASSERT_FALSE(c.markNotice((NoticeChannel)ch));   // a second mark is a no-op
  }
}

static void test_notice_once_per_channel_per_episode_property() {
  // Random walks of spend, limit edits, month rolls and reboots: a channel is told at
  // most once per episode, every open episode has told every channel, and nothing is
  // owed outside one.
  std::mt19937 rng(20261022u);
  for (int run = 0; run < 200; ++run) {
    SpendCeiling c;
    uint32_t month = 202610;
    int told[kNoticeChannelCount] = {0, 0, 0};
    for (int step = 0; step < 60; ++step) {
      const uint32_t before = c.episodes;
      randomStep(c, month, rng);
      if (c.episodes != before || !c.frozen)
        for (int& t : told) t = 0;   // a new episode (or none open)
      pumpNotices(c, told);
      for (int ch = 0; c.frozen && ch < kNoticeChannelCount; ++ch) TEST_ASSERT_EQUAL_INT(1, told[ch]);
    }
  }
}

static void test_reboot_mid_episode_keeps_the_notices_sent() {
  SpendCeiling c;
  c.add(21000000, 202610);
  c.markNotice(NoticeChannel::Telegram);
  SpendCeiling r;
  TEST_ASSERT_TRUE(r.deserialize(c.serialize()));
  TEST_ASSERT_TRUE(r.frozen);
  TEST_ASSERT_FALSE(r.noticeOwed(NoticeChannel::Telegram));   // already told: not again
  TEST_ASSERT_TRUE(r.noticeOwed(NoticeChannel::Web));
  TEST_ASSERT_EQUAL_UINT64(21000000ULL, r.spentMicro);
  TEST_ASSERT_EQUAL_UINT32(202610u, r.monthKey);
}

static void test_file_format_is_strict_and_append_only() {
  SpendCeiling c;
  TEST_ASSERT_FALSE(c.deserialize(""));
  TEST_ASSERT_FALSE(c.deserialize("garbage"));
  TEST_ASSERT_FALSE(c.deserialize("!v|2;1|2"));
  TEST_ASSERT_FALSE(c.deserialize("!v|1;20000000|x"));
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kDefaultLimitMicro, c.limitMicro);   // defaults kept
  // A short (older) line: the missing tail fields read 0.
  TEST_ASSERT_TRUE(c.deserialize("!v|1;30000000|1|202610|5000000"));
  TEST_ASSERT_EQUAL_UINT64(30000000ULL, c.limitMicro);
  TEST_ASSERT_TRUE(c.ownerSet);
  TEST_ASSERT_EQUAL_UINT64(5000000ULL, c.spentMicro);
  TEST_ASSERT_FALSE(c.frozen);
  // A torn line claiming an open episode under the ceiling cannot hold it open, and a
  // count over the ceiling opens one.
  TEST_ASSERT_TRUE(c.deserialize("!v|1;20000000|0|202610|1000000|1|3|7|0|0"));
  TEST_ASSERT_FALSE(c.frozen);
  TEST_ASSERT_TRUE(c.deserialize("!v|1;20000000|0|202610|25000000|0|0|0|0|0"));
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_TRUE(c.noticeOwed(NoticeChannel::Telegram));
  // A bad month is dropped (0 = not known), never trusted.
  TEST_ASSERT_TRUE(c.deserialize("!v|1;20000000|0|202613|0"));
  TEST_ASSERT_EQUAL_UINT32(0u, c.monthKey);
  TEST_ASSERT_TRUE(c.deserialize("!v|1;20000000|0|202610|0|0|0|0|0|3\n"));
  TEST_ASSERT_TRUE(c.migrationNoticeOwed(NoticeChannel::Telegram));
  TEST_ASSERT_TRUE(c.migrationNoticeOwed(NoticeChannel::Web));
  TEST_ASSERT_FALSE(c.migrationNoticeOwed(NoticeChannel::Screen));
}

static void test_migration_notice_once_per_channel() {
  SpendCeiling c;
  c.migrationOwed = 0x03;   // Telegram and the web chat
  TEST_ASSERT_TRUE(c.migrationNoticeOwed(NoticeChannel::Telegram));
  TEST_ASSERT_TRUE(c.markMigrationNotice(NoticeChannel::Telegram));
  TEST_ASSERT_FALSE(c.migrationNoticeOwed(NoticeChannel::Telegram));
  TEST_ASSERT_FALSE(c.markMigrationNotice(NoticeChannel::Telegram));
  SpendCeiling r;
  r.deserialize(c.serialize());   // a reboot before the web chat took it
  TEST_ASSERT_FALSE(r.migrationNoticeOwed(NoticeChannel::Telegram));
  TEST_ASSERT_TRUE(r.migrationNoticeOwed(NoticeChannel::Web));
}

// Migration (the first boot on this release, no ceiling file yet).
static void test_first_boot_new_device_owes_nothing() {
  const SpendCeiling c = SpendCeiling::firstBoot(202610, false, 0, true);
  TEST_ASSERT_EQUAL_UINT64(0ULL, c.spentMicro);
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kDefaultLimitMicro, c.limitMicro);
  TEST_ASSERT_FALSE(c.ownerSet);
  for (int ch = 0; ch < kNoticeChannelCount; ++ch)
    TEST_ASSERT_TRUE(c.nextNotice((NoticeChannel)ch) == SpendCeiling::Notice::None);
}

static void test_first_boot_existing_device_counts_the_month_and_owes_one_notice() {
  SpendCeiling c = SpendCeiling::firstBoot(202610, true, 6600000, false);
  TEST_ASSERT_EQUAL_UINT64(6600000ULL, c.spentMicro);   // this month's estimate, not zero
  TEST_ASSERT_FALSE(c.frozen);
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Web) == SpendCeiling::Notice::Migration);
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Telegram) == SpendCeiling::Notice::None);   // no bot
  TEST_ASSERT_TRUE(c.markDelivered(NoticeChannel::Web, SpendCeiling::Notice::Migration));
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Web) == SpendCeiling::Notice::None);
  SpendCeiling r;
  TEST_ASSERT_TRUE(r.deserialize(c.serialize()));   // a restart: still told
  TEST_ASSERT_TRUE(r.nextNotice(NoticeChannel::Web) == SpendCeiling::Notice::None);
}

// The owner's own device on a premium head: already over the 20 USD default this month,
// so it is paused at once, and the first-boot notice (which says so) is the ONLY message
// about this pause on each channel - never a freeze notice right after it.
static void test_first_boot_over_the_ceiling_sends_one_message_per_channel() {
  SpendCeiling c = SpendCeiling::firstBoot(202610, true, 63200000, true);
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_TRUE(c.frozenWith(0));
  for (NoticeChannel ch : {NoticeChannel::Web, NoticeChannel::Telegram}) {
    TEST_ASSERT_TRUE(c.nextNotice(ch) == SpendCeiling::Notice::Migration);
    c.markDelivered(ch, SpendCeiling::Notice::Migration);
    TEST_ASSERT_TRUE(c.nextNotice(ch) == SpendCeiling::Notice::None);
  }
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Screen) == SpendCeiling::Notice::None);
  // A later crossing (a new episode after a raise) still gets its own notice.
  c.setLimit(70000000);
  c.add(7000000, 202610);
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_TRUE(c.nextNotice(NoticeChannel::Web) == SpendCeiling::Notice::Freeze);
}

// ---- the arithmetic over every price table ---------------------------------------

static uint64_t expectMicro(const BuiltinPrice& p, const TokenDelta& d, uint64_t calls) {
  // A field the row leaves at 0 (an embedding has no output price) is priced at the
  // provider's default row, the ledger's fill rule (never free: fail-closed).
  const BuiltinPrice* dflt = builtinSlotDefault(p.slot);
  const uint64_t in = p.microIn ? p.microIn : (dflt ? dflt->microIn : 0);
  const uint64_t out = p.microOut ? p.microOut : (dflt ? dflt->microOut : 0);
  const uint64_t cr = p.microCacheR ? p.microCacheR : in / 10;
  const uint64_t cw = p.microCacheW ? p.microCacheW : in * 5 / 4;
  return d.in * in / 1000000ULL + d.out * out / 1000000ULL +
         d.cacheR * cr / 1000000ULL + d.cacheW * cw / 1000000ULL +
         calls * p.microPerKCalls / 1000ULL;
}

static void test_every_builtin_row_prices_at_its_own_rate() {
  // One million of each token kind (whole micro-USD at every rate) through the ledger's
  // one formula, for every model row of every provider table.
  UsageLedger L;
  TokenDelta d;
  d.in = 1000000;
  d.out = 1000000;
  d.cacheR = 1000000;
  d.cacheW = 1000000;
  int rows = 0;
  for (const BuiltinPrice& p : kBuiltinPrices) {
    if (std::strcmp(p.family, "*") == 0) continue;
    if (std::strstr(p.family, kLongTierSuffix)) continue;   // a tier key, priced like a row below
    const uint64_t got = L.priceDeltaMicro(p.slot, p.family, d);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(expectMicro(p, d, 0), got, p.family);
    ++rows;
  }
  TEST_ASSERT_TRUE(rows >= 20);
  // The long-context tier row prices at its own (higher) rate.
  const BuiltinPrice* lt = builtinModelPrice("anthropic", "claude-haiku-5-5:long");
  TEST_ASSERT_NOT_NULL(lt);
  TEST_ASSERT_EQUAL_UINT64(expectMicro(*lt, d, 0), L.priceDeltaMicro("anthropic", "claude-haiku-5-5:long", d));
}

static void test_unknown_model_prices_fail_closed_at_the_highest_rate() {
  UsageLedger L;
  TokenDelta d;
  d.in = 1000000;
  d.out = 1000000;
  for (const char* slot : {"anthropic", "openai", "mistral", "zai"}) {
    uint32_t hiIn = 0, hiOut = 0;
    builtinHighestPrice(slot, hiIn, hiOut);
    const uint64_t want = (uint64_t)hiIn + hiOut;
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(want, L.priceDeltaMicro(slot, "a-model-nobody-priced", d), slot);
    // An empty model (spend with no model known) is priced the same fail-closed way.
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(want, L.priceDeltaMicro(slot, "", d), slot);
  }
  // Search calls: Tavily's per-1000-call rate.
  TEST_ASSERT_EQUAL_UINT64(8000ULL, L.priceDeltaMicro("tavily", "", TokenDelta{}, 1));
}

static void test_router_and_owner_prices_win_over_the_builtin_table() {
  UsageLedger L;
  TokenDelta d;
  d.in = 1000000;
  d.out = 1000000;
  // The router's price state for a Cumulo key.
  ModelRate rr;
  std::snprintf(rr.prov, sizeof rr.prov, "cumulo");
  std::snprintf(rr.model, sizeof rr.model, "openai/gpt-5.6");
  rr.microIn = 7000000;
  rr.microOut = 31000000;
  L.models().replaceRouterRates({rr});
  TEST_ASSERT_EQUAL_UINT64(38000000ULL, L.priceDeltaMicro("cumulo", "openai/gpt-5.6", d));
  // Without a router row the key reads its upstream's built-in row.
  TEST_ASSERT_EQUAL_UINT64(35000000ULL, L.priceDeltaMicro("cumulo", "openai/gpt-5.6-sol", d));
  // An owner price beats both.
  TEST_ASSERT_TRUE(L.models().setOwnerRate("cumulo", "openai/gpt-5.6", 1000000, 2000000) ==
                   RateEdit::Ok);
  TEST_ASSERT_EQUAL_UINT64(3000000ULL, L.priceDeltaMicro("cumulo", "openai/gpt-5.6", d));
}

static void test_the_ceiling_counts_every_provider_combined() {
  // $20 across four providers: Anthropic sonnet, OpenAI, Mistral and Z.ai records each
  // count toward the one ceiling, and the record that reaches it opens the freeze.
  UsageLedger L;
  SpendCeiling c;
  TokenDelta d;
  d.in = 1000000;
  d.out = 500000;   // sonnet-5: $2 + $5 = $7
  c.add(L.priceDeltaMicro("anthropic", "claude-sonnet-5", d), 202610);
  d.in = 1000000;
  d.out = 1000000;   // gpt-5.6-terra: $2 + $12 = $14 -> $21 total
  TEST_ASSERT_FALSE(c.frozen);
  c.add(L.priceDeltaMicro("openai", "gpt-5.6-terra", d), 202610);
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_EQUAL_UINT64(21000000ULL, c.spentMicro);
  // A cheap provider's record still counts while frozen.
  c.add(L.priceDeltaMicro("zai", "glm-5.3-flash", d), 202610);
  TEST_ASSERT_EQUAL_UINT64(21650000ULL, c.spentMicro);
}

// ---- the gate ---------------------------------------------------------------------

static bool g_frozen = false;
static bool frozenNow() { return g_frozen; }

static void test_gate_counts_every_seam_and_refuses_while_frozen() {
  installSpendFreeze(&frozenNow);
  g_frozen = false;
  for (int s = 0; s < kSpendSeamCount; ++s) TEST_ASSERT_TRUE(paidCallAllowed((SpendSeam)s));
  g_frozen = true;
  for (int s = 0; s < kSpendSeamCount; ++s) TEST_ASSERT_FALSE(paidCallAllowed((SpendSeam)s));
  for (int s = 0; s < kSpendSeamCount; ++s) {
    TEST_ASSERT_EQUAL_UINT32(2u, paidGateConsults((SpendSeam)s));
    TEST_ASSERT_EQUAL_UINT32(1u, paidGateRefusals((SpendSeam)s));
    TEST_ASSERT_TRUE(std::strlen(spendSeamSlug((SpendSeam)s)) > 1);
  }
  // Uninstalled: never frozen.
  installSpendFreeze(nullptr);
  TEST_ASSERT_TRUE(paidCallAllowed(SpendSeam::Head));
  TEST_ASSERT_TRUE(isSpendFrozenError(std::string("network: ") + kSpendFrozenError));
  TEST_ASSERT_FALSE(isSpendFrozenError("network"));
}

static uint64_t g_pending = 0;
static uint64_t g_priced = 0;
static uint64_t pricer(const TurnSpend& s) {
  UsageLedger L;
  uint64_t m = 0;
  for (size_t i = 0; i < s.n; ++i) m += L.priceDeltaMicro(s.slot[i].host, s.slot[i].model, ledgerDelta(s.slot[i]));
  g_priced += m;
  return m;
}
static void sink(uint64_t micro) { g_pending += micro; }

static void test_pending_usage_is_the_delta_priced() {
  installPendingSpend(&pricer, &sink);
  g_pending = g_priced = 0;
  TokenUsage before, after;
  before.promptTokens = 1000;
  after.promptTokens = 1001000;    // +1M in
  after.completionTokens = 100000; // +100K out
  notePendingUsage("anthropic", "claude-haiku-5-5", before, after);
  // haiku-5-5: $0.10 per 1M in + $0.05 for 100K out.
  TEST_ASSERT_EQUAL_UINT64(150000ULL, g_pending);
  notePendingUsage("anthropic", "claude-haiku-5-5", after, after);   // nothing billed
  TEST_ASSERT_EQUAL_UINT64(150000ULL, g_pending);
}

// The park hears every change of the freeze once (lane K2's pause switch rides it).
static std::vector<bool> g_heard;
static void heard(bool f) { g_heard.push_back(f); }
static void test_freeze_listener_hears_changes_only() {
  g_heard.clear();
  installFreezeListener(&heard);
  noteFreeze(false);
  noteFreeze(false);
  noteFreeze(true);
  noteFreeze(true);
  noteFreeze(false);
  TEST_ASSERT_EQUAL(3, (int)g_heard.size());
  TEST_ASSERT_FALSE(g_heard[0]);
  TEST_ASSERT_TRUE(g_heard[1]);
  TEST_ASSERT_FALSE(g_heard[2]);
  installFreezeListener(&heard);   // a new listener hears the next report, even a repeat
  noteFreeze(false);
  TEST_ASSERT_EQUAL(4, (int)g_heard.size());
  installFreezeListener(nullptr);
  noteFreeze(true);   // nobody listening: nothing to call
  TEST_ASSERT_EQUAL(4, (int)g_heard.size());
}

// The owner's field: whole cents only, clamped, never wrapped by a huge number.
static void test_ceiling_field_parse_is_strict_and_clamped() {
  TEST_ASSERT_EQUAL_UINT64(20000000ULL, parseCeilingCents("2000"));
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kMinLimitMicro, parseCeilingCents("5"));   // 5 cents -> $1
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kMaxLimitMicro, parseCeilingCents("10000000"));
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kMaxLimitMicro, parseCeilingCents("99999999999999"));
  TEST_ASSERT_EQUAL_UINT64(SpendCeiling::kMaxLimitMicro, parseCeilingCents("18446744073709551616000"));
  for (const char* bad : {"", "0", "-5", "+5", "20.5", "abc", "1e9", " 200"})
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0ULL, parseCeilingCents(bad), bad);
}

// ---- copy -------------------------------------------------------------------------

static void test_money_and_dates() {
  TEST_ASSERT_EQUAL_STRING("$20.00", formatUsd(20000000).c_str());
  TEST_ASSERT_EQUAL_STRING("$0.09", formatUsd(99999).c_str());   // rounded down to the cent
  TEST_ASSERT_EQUAL_STRING("$1234.56", formatUsd(1234560000ULL).c_str());
  TEST_ASSERT_EQUAL_STRING("1 November", resetDateText(202610).c_str());
  TEST_ASSERT_EQUAL_STRING("1 January", resetDateText(202612).c_str());
  TEST_ASSERT_EQUAL_STRING("", resetDateText(0).c_str());
}

static CeilingFacts facts() {
  CeilingFacts f;
  f.limitMicro = 20000000;
  f.spentMicro = 20130000;
  f.monthKey = 202610;
  f.dayOfMonth = 22;
  f.headModel = "claude-sonnet-5";
  return f;
}

static void test_copy_goldens() {
  CeilingFacts f = facts();
  const std::string freeze = freezeNoticeText(f);
  const std::string screen = freezeScreenLine(f) + " / " + freezeScreenDetail(f);
  f.spentMicro = 16200000;
  const std::string warn = warnNoticeText(f);
  // Migration: on pace, already over, a premium head (the owner's own device runs
  // Opus), and a device whose clock is not set (no pace, no date).
  f.spentMicro = 6600000;
  const std::string migPace = migrationNoticeText(f);
  f.spentMicro = 63200000;
  f.headModel = "claude-opus-5";
  const std::string migOverOpus = migrationNoticeText(f);
  f.spentMicro = 4400000;
  const std::string migOpusPace = migrationNoticeText(f);
  f.spentMicro = 14300000;   // $0.65 a day on day 22: the ceiling comes before the month ends
  const std::string migSoon = migrationNoticeText(f);
  CeilingFacts unset;
  unset.headModel = "";
  const std::string migUnset = migrationNoticeText(unset);
  const std::string setup = setupDefaultsText(SpendCeiling::kDefaultLimitMicro);
  for (const std::string* s : {&freeze, &screen, &warn, &migPace, &migOverOpus, &migOpusPace,
                               &migSoon, &migUnset, &setup})
    assertPlainCopy(*s);
  TEST_ASSERT_TRUE(freezeScreenLine(f).size() <= 24);
  TEST_ASSERT_TRUE(freezeScreenDetail(f).size() <= 2 * 36);   // two lines of the mic bar
  // What each one must say (the golden pins the words; these pin the facts).
  TEST_ASSERT_TRUE(freeze.find("$20.00") != std::string::npos);
  TEST_ASSERT_TRUE(freeze.find("$20.13") != std::string::npos);
  TEST_ASSERT_TRUE(freeze.find("Assistant > Usage") != std::string::npos);
  TEST_ASSERT_TRUE(freeze.find("1 November") != std::string::npos);
  TEST_ASSERT_TRUE(migOverOpus.find("paused now") != std::string::npos);
  TEST_ASSERT_TRUE(migOverOpus.find("claude-opus-5") != std::string::npos);
  TEST_ASSERT_TRUE(migOverOpus.find("claude-sonnet-5") != std::string::npos);
  TEST_ASSERT_TRUE(migOverOpus.find("2.5x") != std::string::npos);
  TEST_ASSERT_TRUE(migPace.find("claude-sonnet-5") == std::string::npos);   // not premium: no tier line
  TEST_ASSERT_TRUE(migPace.find("a day") != std::string::npos);
  TEST_ASSERT_TRUE(migPace.find("the month ends under the ceiling") != std::string::npos);
  TEST_ASSERT_TRUE(migSoon.find("reached in about 8 days") != std::string::npos);
  TEST_ASSERT_TRUE(migUnset.find("a day") == std::string::npos);
  std::string all = "[freeze]\n" + freeze + "\n[screen]\n" + screen + "\n[warn]\n" + warn +
                    "\n[migration on pace]\n" + migPace + "\n[migration over, Opus head]\n" +
                    migOverOpus + "\n[migration on pace, Opus head]\n" + migOpusPace +
                    "\n[migration, ceiling before the month ends, Opus head]\n" + migSoon +
                    "\n[migration, clock unset]\n" + migUnset + "\n[setup]\n" + setup + "\n";
  checkTextGolden("spend_ceiling_copy.txt", all);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_month_key_utc_and_unset_clock);
  RUN_TEST(test_month_key_follows_the_device_timezone);
  RUN_TEST(test_spend_while_clock_unset_counts_toward_the_first_month);
  RUN_TEST(test_rollover_starts_a_new_count_and_closes_the_freeze_quietly);
  RUN_TEST(test_a_clock_that_jumps_never_clears_spend_backwards);
  RUN_TEST(test_raising_the_ceiling_clears_the_freeze_at_once);
  RUN_TEST(test_pending_spend_freezes_before_it_is_committed);
  RUN_TEST(test_warning_at_eighty_percent_once_a_month);
  RUN_TEST(test_notice_once_per_channel_per_episode_property);
  RUN_TEST(test_reboot_mid_episode_keeps_the_notices_sent);
  RUN_TEST(test_file_format_is_strict_and_append_only);
  RUN_TEST(test_migration_notice_once_per_channel);
  RUN_TEST(test_first_boot_new_device_owes_nothing);
  RUN_TEST(test_first_boot_existing_device_counts_the_month_and_owes_one_notice);
  RUN_TEST(test_first_boot_over_the_ceiling_sends_one_message_per_channel);
  RUN_TEST(test_every_builtin_row_prices_at_its_own_rate);
  RUN_TEST(test_unknown_model_prices_fail_closed_at_the_highest_rate);
  RUN_TEST(test_router_and_owner_prices_win_over_the_builtin_table);
  RUN_TEST(test_the_ceiling_counts_every_provider_combined);
  RUN_TEST(test_gate_counts_every_seam_and_refuses_while_frozen);
  RUN_TEST(test_pending_usage_is_the_delta_priced);
  RUN_TEST(test_freeze_listener_hears_changes_only);
  RUN_TEST(test_ceiling_field_parse_is_strict_and_clamped);
  RUN_TEST(test_money_and_dates);
  RUN_TEST(test_copy_goldens);
  return UNITY_END();
}
