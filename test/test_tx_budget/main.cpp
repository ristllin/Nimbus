// test_tx_budget - engine 2's per-head budget in tokens (PRD section 2 "Budget", amended by owner
// rulings 81 and 82 and ruling 83; lane RB-T4). Layer L1, free.
//
// The class rules, every head the released table names:
//   - the default profile is full, and no fallback resolves to anything smaller;
//   - every head's default body, due mark, keep, digest block, inline message cap and bulk-whole cap
//     is at least its released v5.1.2 row (test/support/fixtures/budgets_v5_1_2.json);
//   - 85,000 B only where v5.1.2 itself is at 85,000 B; a floor head is a class, not a 0;
//   - the aim is in tokens: about 100,000, or the head's own lower limit; due at the aim less the
//     next request's growth, keep about 70% of it;
//   - both routed rows exist behind the one switch;
//   - the generated table (docs/reference/transcript-budgets.md) is this code's output.
// Every checker is counter-tested first.
#include <unity.h>
#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../support/recorded_models.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/harness/plain/compact.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/rules.h"
#include "budget_table.h"

namespace tx = nimbus::orch::tx;
using nimbus::orch::BudgetLink;
using nimbus::orch::RequestBudget;
using nimbus::orch::requestBudgetFor;

void setUp() {}
void tearDown() {}

namespace {

// ---- the released rows -----------------------------------------------------------------------

struct V512 {
  std::string link, host, model;
  uint32_t body = 0, summary = 0, messageCap = 0, histMax = 0, soft = 0, keep = 0, trigger = 0, toolResult = 0;
};

const std::vector<V512>& v512() {
  static std::vector<V512> rows;
  static JsonDocument doc;
  if (!rows.empty()) return rows;
  TEST_ASSERT_FALSE(deserializeJson(doc, recorded::readFixture("test/support/fixtures/budgets_v5_1_2.json")));
  for (JsonObjectConst r : doc["models"].as<JsonArrayConst>()) {
    V512 v;
    v.link = r["link"] | "";
    v.host = r["provider"] | "";
    v.model = r["id"] | "";
    v.body = r["wireBodyBytes"] | 0u;
    v.summary = r["chatSummaryBytes"] | 0u;
    v.messageCap = r["historyMessageCap"] | 0u;
    v.histMax = r["historyMaxBytes"] | 0u;
    v.soft = r["historySoftBytes"] | 0u;
    v.keep = r["historyKeepBytes"] | 0u;
    v.trigger = r["foldTriggerBytes"] | 0u;
    v.toolResult = r["toolResultBytes"] | 0u;
    rows.push_back(v);
  }
  TEST_ASSERT_TRUE(rows.size() >= 230);   // RB-BG's fixture: every released row
  return rows;
}

BudgetLink linkOf(const V512& v) { return v.link == "server" ? BudgetLink::Server : BudgetLink::Device; }
tx::HeadKey keyOf(const V512& v) { return tx::HeadKey{v.host, v.model, v.host == "cumulo"}; }

// The fold mark v5.1.2 compared against: its trigger (a floor row's counts raw bytes since the last
// fold), unless the trigger does not fit beside the fixed part and the digest block of the released
// body (a tiny window): then what the released window carried.
uint32_t foldMark(const V512& v) {
  return v.trigger + tx::kFixedOverheadCeiling + tx::kFullDigestAndFilesBytes <= v.body ? v.trigger
                                                                                         : std::max(v.histMax, v.soft);
}

// 85,000 B only where v5.1.2 itself is at 85,000 B.
bool floorByAccident(const tx::HeadBudget& b, const V512& v) {
  return b.bodyCap == tx::kFloorHeadBodyCap && v.body != tx::kFloorHeadBodyCap;
}

// Empty when `b` carries at least the released row; else the first field that is short.
std::string shortOf(const tx::HeadBudget& b, const V512& v) {
  struct F {
    const char* name;
    uint32_t have, was;
  };
  const F fs[] = {{"body", b.bodyCap, v.body},
                  {"due mark", b.marks.high, foldMark(v)},
                  {"keep", b.marks.low, v.keep},
                  {"digest block", b.digestBlock, v.summary},
                  {"message cap", b.messageCap, v.messageCap},
                  {"bulk whole", b.bulkWholeCap, v.toolResult}};
  for (const F& f : fs)
    if (f.have < f.was) return std::string(f.name) + " " + std::to_string(f.have) + " < " + std::to_string(f.was);
  return "";
}

tx::HeadBudget full(const V512& v, bool lift = false) {
  tx::HeadBudget b;
  const tx::Status s = tx::budgetFor(keyOf(v), tx::Profile::Full, linkOf(v), lift, b);
  TEST_ASSERT_TRUE_MESSAGE(s.ok(), (v.host + "/" + v.model).c_str());
  return b;
}

tx::HeadBudget direct(const char* host, const char* model, bool routed = false, bool lift = false) {
  tx::HeadBudget b;
  TEST_ASSERT_TRUE(tx::budgetFor(tx::HeadKey{host, model, routed}, tx::Profile::Full, BudgetLink::Device, lift, b).ok());
  return b;
}

// ---- 1. the default is full, everywhere ------------------------------------------------------

void test_full_is_value_4_and_the_default_everywhere() {
  TEST_ASSERT_EQUAL(4, (int)tx::Profile::Full);
  TEST_ASSERT_EQUAL_STRING("full", tx::profileSlug(tx::Profile::Full));
  TEST_ASSERT_EQUAL((int)tx::Profile::Full, (int)tx::kDefaultProfile);
  TEST_ASSERT_EQUAL((int)tx::Profile::Full, (int)tx::HeadBudget{}.profile);
  TEST_ASSERT_EQUAL((int)tx::Profile::Full, (int)agent::plain::TurnCtx{}.profile);
  TEST_ASSERT_EQUAL_STRING("full", tx::profileSlug((tx::Profile)9));   // a damaged value
  // Every fallback of a stored slug resolves to full; each named profile round-trips.
  for (const char* junk : {"", "Full", "lean ", "medium", "wide2", "\xff"})
    TEST_ASSERT_EQUAL_MESSAGE((int)tx::Profile::Full, (int)tx::profileOf(junk), junk);
  for (tx::Profile p : {tx::Profile::Tight, tx::Profile::Lean, tx::Profile::Standard, tx::Profile::Wide, tx::Profile::Full})
    TEST_ASSERT_EQUAL((int)p, (int)tx::profileOf(tx::profileSlug(p)));
}

void test_the_smaller_profiles_are_fixed_and_full_is_per_head() {
  tx::ProfileMarks m;
  TEST_ASSERT_TRUE(tx::profileMarks(tx::Profile::Tight, m).ok());
  TEST_ASSERT_EQUAL_UINT32(24000, m.high);
  TEST_ASSERT_EQUAL_UINT32(12000, m.low);
  TEST_ASSERT_TRUE(tx::profileMarks(tx::Profile::Wide, m).ok());
  TEST_ASSERT_EQUAL_UINT32(100000, m.high);
  TEST_ASSERT_EQUAL_UINT32(50000, m.low);
  TEST_ASSERT_TRUE(tx::profileMarks(tx::Profile::Full, m).err == tx::Err::InvalidArgument);
}

// ---- 2. no head under its released row -------------------------------------------------------

void test_the_row_checker_catches_one_byte_short_in_every_field() {
  V512 v;
  v.body = 250000, v.summary = 6144, v.messageCap = 12288, v.histMax = 118080, v.soft = 94464, v.keep = 59040,
  v.trigger = 94464, v.toolResult = 8192;
  tx::HeadBudget b;
  b.bodyCap = 250000, b.marks = {94464, 59040}, b.digestBlock = 6144, b.messageCap = 12288, b.bulkWholeCap = 8192;
  TEST_ASSERT_EQUAL_STRING("", shortOf(b, v).c_str());
  uint32_t* fields[] = {&b.bodyCap, &b.marks.high, &b.marks.low, &b.digestBlock, &b.messageCap, &b.bulkWholeCap};
  for (uint32_t* f : fields) {
    *f -= 1;
    TEST_ASSERT_FALSE(shortOf(b, v).empty());
    *f += 1;
  }
  // A floor row's fold mark is its 49,152 B trigger; a tiny window's is what its window carried.
  v.body = 85000, v.histMax = 11141, v.soft = 5570, v.trigger = 49152;
  TEST_ASSERT_EQUAL_UINT32(49152, foldMark(v));
  v.body = 8000, v.histMax = 4096, v.soft = 2048;
  TEST_ASSERT_EQUAL_UINT32(4096, foldMark(v));
  // The floor-by-accident checker: an 85,000 B body only where the released row is 85,000 B.
  b.bodyCap = tx::kFloorHeadBodyCap;
  v.body = 360000;
  TEST_ASSERT_TRUE(floorByAccident(b, v));
  v.body = tx::kFloorHeadBodyCap;
  TEST_ASSERT_FALSE(floorByAccident(b, v));
}

void test_no_head_default_is_under_its_v5_1_2_row() {
  for (const V512& v : v512()) {
    const tx::HeadBudget b = full(v);
    const std::string what = v.link + ":" + v.host + "/" + v.model + " " + shortOf(b, v);
    TEST_ASSERT_TRUE_MESSAGE(shortOf(b, v).empty(), what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE((int)tx::Profile::Full, (int)b.profile, what.c_str());
    // 85,000 B only where v5.1.2 itself is at 85,000 B.
    TEST_ASSERT_FALSE_MESSAGE(floorByAccident(b, v), what.c_str());
  }
}

// The engine keeps owner and assistant text inline up to its tier-1 caps and a stored result inline up to
// kBulkWholeMax when there is no bulk storage: each at least the largest released row.
// An owner's smaller engine-1 history profile never lowers engine 2's floors (they are the head's
// own v5.1.2 row, read with no profile).
void test_engine_1s_profile_never_lowers_engine_2s_floors() {
  const tx::HeadBudget before = direct("anthropic", "claude-sonnet-5-5");
  nimbus::orch::installHistoryProfile(nimbus::orch::HistoryProfile::Tiny);
  const tx::HeadBudget after = direct("anthropic", "claude-sonnet-5-5");
  nimbus::orch::installHistoryProfile(nimbus::orch::kDefaultHistoryProfile);
  TEST_ASSERT_EQUAL_UINT32(before.releasedKeep, after.releasedKeep);
  TEST_ASSERT_EQUAL_UINT32(before.releasedFold, after.releasedFold);
  TEST_ASSERT_EQUAL_UINT32(before.marks.low, after.marks.low);
  TEST_ASSERT_EQUAL_UINT32(59040, after.releasedKeep);
}

void test_the_engines_inline_caps_hold_every_released_row() {
  for (const V512& v : v512()) {
    TEST_ASSERT_TRUE(tx::kTier1OwnerText >= v.messageCap && tx::kTier1AssistantText >= v.messageCap);
    TEST_ASSERT_TRUE(tx::kInlineMessageCap >= v.messageCap);
    TEST_ASSERT_TRUE(tx::kBulkWholeMax >= v.toolResult);
  }
}

void test_the_aim_is_tokens_due_before_it_and_keeps_70_percent() {
  for (const V512& v : v512()) {
    const tx::HeadBudget b = full(v);
    const RequestBudget r = requestBudgetFor(v.host, v.model, v.host == "cumulo", linkOf(v), false);
    const std::string what = v.link + ":" + v.host + "/" + v.model;
    TEST_ASSERT_TRUE_MESSAGE(b.tokens.aim <= tx::kAimClampTokens, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.tokens.aim >= r.targetTokens, what.c_str());   // never lower than v5.1.2 aimed
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.tokens.aim - tx::nextRequestReserve(b.tokens.aim), b.tokens.due, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.tokens.keep < b.tokens.due && b.tokens.due < b.tokens.aim, what.c_str());
    // No mark is past what the body carries (an underflow would show here).
    TEST_ASSERT_TRUE_MESSAGE(b.marks.high <= b.bodyCap, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.tokens.aim * tx::kKeepPercent / 100, b.tokens.keep, what.c_str());
    // The body carries the aim on the head's wire: the request at the aim fits.
    TEST_ASSERT_TRUE_MESSAGE(tx::bodyBytesFor(b, b.tokens.aim) <= b.bodyCap, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.marks.low < b.marks.high, what.c_str());
  }
}

void test_the_named_head_classes() {
  tx::HeadBudget b = direct("anthropic", "claude-sonnet-5-5");
  TEST_ASSERT_EQUAL_UINT32(100000, b.tokens.aim);
  TEST_ASSERT_EQUAL_UINT32(96000, b.tokens.due);
  TEST_ASSERT_EQUAL_UINT32(70000, b.tokens.keep);
  TEST_ASSERT_EQUAL((int)tx::AimBound::Clamp, (int)b.bound);
  TEST_ASSERT_FALSE(b.floorHead);
  TEST_ASSERT_EQUAL_UINT32(tx::kWarmBulkBytes, b.warmBulk);
  b = direct("anthropic", "claude-haiku-5-5");   // its price tier is 100,000 tokens
  TEST_ASSERT_EQUAL_UINT32(90000, b.tokens.aim);
  TEST_ASSERT_EQUAL((int)tx::AimBound::Tier, (int)b.bound);
  b = direct("openai", "gpt-5.6-luna");
  TEST_ASSERT_EQUAL_UINT32(100000, b.tokens.aim);
  TEST_ASSERT_EQUAL_UINT32(360000, b.bodyCap);
  b = direct("zai", "glm-5.3");   // v5.1.2's read-window fit on the device link
  TEST_ASSERT_EQUAL((int)tx::AimBound::Latency, (int)b.bound);
  TEST_ASSERT_EQUAL_UINT32(188000, b.bodyCap);
  // Floor heads are a class: routed with the lift off, custom.
  b = direct("cumulo", "gpt-5.6", true);
  TEST_ASSERT_TRUE(b.floorHead);
  TEST_ASSERT_EQUAL_UINT32(tx::kFloorHeadBodyCap, b.bodyCap);
  TEST_ASSERT_EQUAL_UINT32(tx::kWarmBulkBytesFloor, b.warmBulk);
  b = direct("custom", "llama-3-70b");
  TEST_ASSERT_TRUE(b.floorHead);
  // A held (unqualified) head gets its full budget: qualification never sizes the window.
  b = direct("mistral", "mistral-large-4-0");
  TEST_ASSERT_EQUAL_UINT32(360000, b.bodyCap);
  TEST_ASSERT_FALSE(b.floorHead);
  TEST_ASSERT_EQUAL_UINT32(100000, b.tokens.aim);
  // A key the table cannot read is refused, never sized as a smaller head.
  TEST_ASSERT_TRUE(tx::budgetFor(tx::HeadKey{}, tx::Profile::Full, b).err == tx::Err::InvalidArgument);
}

// ---- 3. both routed rows, one switch ---------------------------------------------------------

void test_both_routed_rows_behind_the_one_switch() {
  for (const char* m : {"gpt-5.6", "openai/gpt-5.6", "anthropic/claude-sonnet-5-5", "zai/glm-5.3"}) {
    const tx::HeadBudget off = direct("cumulo", m, true, false), on = direct("cumulo", m, true, true);
    const RequestBudget lifted = requestBudgetFor("cumulo", m, true, BudgetLink::Device, true);
    TEST_ASSERT_TRUE_MESSAGE(off.floorHead, m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(tx::kFloorHeadBodyCap, off.bodyCap, m);
    TEST_ASSERT_FALSE_MESSAGE(on.floorHead, m);
    TEST_ASSERT_TRUE_MESSAGE(on.bodyCap >= lifted.wireBodyBytes, m);
    TEST_ASSERT_TRUE_MESSAGE(on.tokens.aim > off.tokens.aim && on.marks.high > off.marks.high, m);
  }
  // The installed form follows the one switch (off until RB-CUV reports).
  tx::HeadBudget installed;
  TEST_ASSERT_TRUE(tx::budgetFor(tx::HeadKey{"cumulo", "gpt-5.6", true}, tx::Profile::Full, installed).ok());
  TEST_ASSERT_EQUAL_UINT32(direct("cumulo", "gpt-5.6", true, nimbus::orch::kRoutedBudgetLift).bodyCap, installed.bodyCap);
}

// ---- 4. an owner's smaller profile ----------------------------------------------------------

void test_an_owner_choice_lowers_only_the_marks_and_never_raises_them() {
  for (const V512& v : v512()) {
    const tx::HeadBudget f = full(v);
    for (tx::Profile p : {tx::Profile::Tight, tx::Profile::Lean, tx::Profile::Standard, tx::Profile::Wide}) {
      tx::HeadBudget b;
      TEST_ASSERT_TRUE(tx::budgetFor(keyOf(v), p, linkOf(v), false, b).ok());
      tx::ProfileMarks m;
      TEST_ASSERT_TRUE(tx::profileMarks(p, m).ok());
      const std::string what = v.host + "/" + v.model + " " + tx::profileSlug(p);
      TEST_ASSERT_EQUAL_MESSAGE((int)p, (int)b.profile, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(std::min(m.high, f.marks.high), b.marks.high, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(std::min(m.low, f.marks.low), b.marks.low, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(f.bodyCap, b.bodyCap, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(f.tokens.aim, b.tokens.aim, what.c_str());
    }
  }
}

// ---- 5. digests settle at the head's profile -------------------------------------------------

void test_the_digest_block_at_full_holds_every_released_summary() {
  TEST_ASSERT_TRUE(tx::blockCapFor(tx::Profile::Full) >= tx::kFullDigestBlockBytes);
  for (const V512& v : v512()) TEST_ASSERT_TRUE(tx::blockCapFor(tx::Profile::Full) >= v.summary);
  // The out-of-range value is full's, never lean's.
  TEST_ASSERT_EQUAL_UINT32(tx::blockCapFor(tx::Profile::Full), tx::blockCapFor((tx::Profile)9));
}

// ---- 5b. the arena is sized to full (ruling 83 item 8) ------------------------------------------

// Arena bytes a transcript takes once its resident (plain::residentBytes) reaches `resident`, built
// of turns of the given shape: a t item, the owner's text, `tools` calls with receipts of
// `receipt` bytes, the reply, the e item.
struct ArenaUse {
  size_t used = 0, resident = 0;
  bool refused = false;
};
ArenaUse fillTo(size_t resident, size_t owner, size_t reply, int tools, size_t receipt) {
  tx::TxArena a;
  TEST_ASSERT_TRUE(a.init(tx::kActiveChatArenaBytes * 2, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  uint64_t q = 0;
  uint32_t turn = 0;
  auto put = [&](tx::Item it) {
    it.q = ++q, it.turn = turn, it.t = 1795000000 + q;
    const tx::Status st = a.append(it);
    TEST_ASSERT_TRUE_MESSAGE(st.ok(), (std::string(tx::errSlug(st.err)) + " at " + st.where + " used " + std::to_string(a.usedBytes()) + " resident " + std::to_string(agent::plain::residentBytes(a, 0)) + " kind " +
                                       std::to_string((int)it.kind)).c_str());
  };
  while (agent::plain::residentBytes(a, 0) < resident) {
    ++turn;
    tx::Item t;
    t.kind = tx::ItemKind::TurnOpen, t.profile = "full", t.head = "openai/gpt-5.6-luna";
    put(t);
    tx::Item u;
    u.kind = tx::ItemKind::User, u.x = std::string(owner, 'o'), u.m = turn;
    put(u);
    for (int k = 0; k < tools; ++k) {
      tx::Item c;
      c.kind = tx::ItemKind::Call, c.id = "call_" + std::to_string(q), c.n = "read_file";
      c.args = R"({"path":"notes/day.md","offset":0})";
      put(c);
      tx::Item r;
      r.kind = tx::ItemKind::Result, r.id = c.id, r.n = c.n, r.x = std::string(receipt, 'f');
      r.bytes = (uint32_t)receipt;
      put(r);
    }
    tx::Item as;
    as.kind = tx::ItemKind::Assistant, as.x = std::string(reply, 'a'), as.fin = true;
    put(as);
    tx::Item e;
    e.kind = tx::ItemKind::TurnEnd, e.end.cause = "reply", e.end.promptTokens = 90000;
    put(e);
  }
  return ArenaUse{a.usedBytes(), agent::plain::residentBytes(a, 0), false};
}

size_t arenaNeedAtFull(bool print);

void test_the_arena_holds_the_largest_head_at_full_with_a_turn_of_growth() {
  TEST_ASSERT_TRUE_MESSAGE(arenaNeedAtFull(true) <= tx::kActiveChatArenaBytes, "the arena does not hold the largest head at full");
}

// The most arena the largest default (the head with the highest due mark, plus one request's growth
// on its wire) takes over four turn mixes.
size_t arenaNeedAtFull(bool print) {
  // The largest default: the head with the highest due mark, plus one request's growth on its wire.
  uint32_t high = 0, growth = 0;
  std::string who;
  for (const V512& v : v512()) {
    const tx::HeadBudget b = full(v);
    if (v.link != "device" || b.marks.high <= high) continue;
    high = b.marks.high, who = v.host + "/" + v.model;
    growth = tx::bodyBytesFor(b, tx::kNextRequestReserveTokens + 1000) - tx::bodyBytesFor(b, 1000);
  }
  struct Mix {
    const char* name;
    size_t owner, reply;
    int tools;
    size_t receipt;
  } mixes[] = {{"chat", 300, 900, 0, 0}, {"one tool", 250, 500, 1, 1200}, {"three tools", 200, 400, 3, 2048},
               {"short turns", 60, 120, 1, 200}};
  size_t need = 0;
  for (const Mix& m : mixes) {
    const ArenaUse u = fillTo(high + growth, m.owner, m.reply, m.tools, m.receipt);
    char line[200];
    std::snprintf(line, sizeof line, "%s at full (H %u + growth %u B), %s turns: arena %zu of %zu B", who.c_str(), high,
                  growth, m.name, u.used, tx::kActiveChatArenaBytes);
    if (print) TEST_MESSAGE(line);
    need = std::max(need, u.used);
  }
  return need;
}

// ---- 6. the generated table ------------------------------------------------------------------

// ---- 7. measured figures against their rows (ruling 83 item 7, amended DoD item 9) --------------

// A measured figure contradicts its row when it is more than 15% away from it.
bool contradicts(double measured, double row) { return row <= 0 || std::fabs(measured - row) > 0.15 * row; }

void test_the_contradiction_checker_is_15_percent_either_side() {
  TEST_ASSERT_FALSE(contradicts(96000, 96000));
  TEST_ASSERT_FALSE(contradicts(96000 * 1.15, 96000));
  TEST_ASSERT_FALSE(contradicts(96000 * 0.85, 96000));
  TEST_ASSERT_TRUE(contradicts(96000 * 1.151, 96000));
  TEST_ASSERT_TRUE(contradicts(96000 * 0.849, 96000));
  TEST_ASSERT_TRUE(contradicts(1, 0));
}

void test_no_measured_figure_contradicts_its_row() {
  const JsonDocument& m = budgettable::measured();
  // The wire ratio and the growth reserve in the code are the measured ones.
  TEST_ASSERT_EQUAL_INT((int)tx::kAnthropicBodyMilliBytesPerToken, m["wire_fit"]["milli_bytes_per_token"] | 0);
  TEST_ASSERT_EQUAL_INT(tx::kAnthropicBodyInterceptBytes, m["wire_fit"]["intercept_bytes"] | 0);
  const uint32_t grew = std::max<uint32_t>(m["next_request_growth"]["in_turn_max"] | 0u, m["next_request_growth"]["turn_to_turn_max"] | 0u);
  TEST_ASSERT_TRUE(grew > 0 && tx::kNextRequestReserveTokens >= grew && tx::kNextRequestReserveTokens < grew + 1000);
  // claude-sonnet-5-5 at full: the requests the arm sent at the due mark and just after a compaction.
  const tx::HeadBudget b = direct("anthropic", "claude-sonnet-5-5");
  JsonArrayConst points = m["full_arm"]["compactions"];
  TEST_ASSERT_TRUE_MESSAGE(points.size() > 0, "the full arm never reached the due mark");
  uint32_t lowestAfter = UINT32_MAX;
  for (JsonObjectConst p : points) {
    const uint32_t before = p["before"] | 0u, after = p["after"] | 0u;
    TEST_ASSERT_FALSE_MESSAGE(contradicts(before, b.tokens.due), ("at the due mark: " + std::to_string(before)).c_str());
    // The owner's band for a request at the due mark (ruling 82: around 100,000 tokens).
    TEST_ASSERT_TRUE_MESSAGE(before >= 90000 && before <= 105000 && before <= b.tokens.aim, std::to_string(before).c_str());
    lowestAfter = std::min(lowestAfter, after);
  }
  TEST_ASSERT_FALSE_MESSAGE(contradicts(lowestAfter, b.tokens.keep), ("after a compaction: " + std::to_string(lowestAfter)).c_str());
  // The cost formula reproduces the measured Sonnet turn.
  const budgettable::Head* sonnet = nullptr;
  for (const budgettable::Head& h : budgettable::heads())
    if (std::string(h.host) == "anthropic" && std::string(h.model) == "claude-sonnet-5-5") sonnet = &h;
  TEST_ASSERT_NOT_NULL(sonnet);
  const budgettable::CostBlocks c = budgettable::costOf(*sonnet);
  const double measuredTurn = (m["full_arm"]["blended_turn_usd"] | 0.0) + (m["full_arm"]["background_usd_per_turn"] | 0.0);
  TEST_ASSERT_FALSE_MESSAGE(contradicts(c.turnUsd(), measuredTurn), "USD a turn");
}

// PRD section 2: the cost sums every block. Each is present (the measured input covers the fixed part
// and the digest block), and taking any one out changes the month.
void test_every_block_is_in_the_sum() {
  for (const budgettable::Head& h : budgettable::heads()) {
    const budgettable::CostBlocks c = budgettable::costOf(h);
    TEST_ASSERT_TRUE_MESSAGE(c.fixedTokens > 0 && c.digestTokens > 0 && c.conversationTokens > 0, h.name);
    const double month = c.monthUsd(12);
    double budgettable::CostBlocks::*usd[] = {&budgettable::CostBlocks::inputUsd, &budgettable::CostBlocks::outputUsd,
                                              &budgettable::CostBlocks::backgroundUsd, &budgettable::CostBlocks::otherUsdMonth};
    for (auto f : usd) {
      budgettable::CostBlocks less = c;
      less.*f = 0;
      TEST_ASSERT_TRUE_MESSAGE(less.monthUsd(12) < month, h.name);
    }
  }
}

void test_the_generated_table_is_this_codes_output() {
  const std::string want = budgettable::render(arenaNeedAtFull(false));
  const char* path = "docs/reference/transcript-budgets.md";
  if (std::getenv("GOLDEN_UPDATE")) {
    std::ofstream(path) << want;
  }
  TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), recorded::readFixture(path).c_str(),
                                   "re-run with GOLDEN_UPDATE=1 and read the diff");
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_full_is_value_4_and_the_default_everywhere);
  RUN_TEST(test_the_smaller_profiles_are_fixed_and_full_is_per_head);
  RUN_TEST(test_the_row_checker_catches_one_byte_short_in_every_field);
  RUN_TEST(test_no_head_default_is_under_its_v5_1_2_row);
  RUN_TEST(test_engine_1s_profile_never_lowers_engine_2s_floors);
  RUN_TEST(test_the_engines_inline_caps_hold_every_released_row);
  RUN_TEST(test_the_aim_is_tokens_due_before_it_and_keeps_70_percent);
  RUN_TEST(test_the_named_head_classes);
  RUN_TEST(test_both_routed_rows_behind_the_one_switch);
  RUN_TEST(test_an_owner_choice_lowers_only_the_marks_and_never_raises_them);
  RUN_TEST(test_the_digest_block_at_full_holds_every_released_summary);
  RUN_TEST(test_the_arena_holds_the_largest_head_at_full_with_a_turn_of_growth);
  RUN_TEST(test_the_contradiction_checker_is_15_percent_either_side);
  RUN_TEST(test_no_measured_figure_contradicts_its_row);
  RUN_TEST(test_every_block_is_in_the_sum);
  RUN_TEST(test_the_generated_table_is_this_codes_output);
  return UNITY_END();
}
