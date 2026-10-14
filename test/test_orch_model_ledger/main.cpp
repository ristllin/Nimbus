#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/model_ledger.h"

// test_orch_model_ledger - the bounded per-(provider, model) spend and price tables
// behind per-model prices (2026-10). The class guards: every cap holds whatever is
// recorded (spend is folded, never dropped), the text forms round-trip and survive
// garbage, and the web field parser keeps a model holding ':' or '/' intact.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static std::string mname(int i) { return "model-" + std::to_string(i) + "x"; }

static TokenDelta td(uint64_t in, uint64_t out = 0, uint64_t cr = 0, uint64_t cw = 0) {
  TokenDelta d;
  d.in = in;
  d.out = out;
  d.cacheR = cr;
  d.cacheW = cw;
  return d;
}

static uint64_t sumTotIn(const ModelLedger& L, const char* prov) {
  uint64_t s = 0;
  for (const auto& r : L.rows())
    if (std::strcmp(r.prov, prov) == 0) s += r.totIn;
  return s;
}

static void test_record_merges_and_rolls() {
  ModelLedger L;
  L.record("openai", "gpt-5.6", td(100, 20), 24300, 20650);
  L.record("openai", "gpt-5.6", td(50, 10), 24300, 20651);
  TEST_ASSERT_EQUAL(1, (int)L.rows().size());
  const ModelSpend* r = L.findRow("openai", "gpt-5.6");
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_UINT64(150, r->tokIn);
  TEST_ASSERT_EQUAL_UINT64(30, r->tokOut);
  TEST_ASSERT_EQUAL_UINT32(20651, r->lastDay);
  // Unsynced (key 0) keeps the stored period; a new period zeroes the period
  // counters and keeps the all-time totals.
  L.record("openai", "gpt-5.6", td(1, 1), 0, 0);
  TEST_ASSERT_EQUAL_UINT32(24300, r->periodKey);
  TEST_ASSERT_EQUAL_UINT64(151, r->tokIn);
  TEST_ASSERT_EQUAL_UINT32(20651, r->lastDay);   // day 0 never overwrites
  L.record("openai", "gpt-5.6", td(5), 24301, 20680);
  TEST_ASSERT_EQUAL_UINT32(24301, r->periodKey);
  TEST_ASSERT_EQUAL_UINT64(5, r->tokIn);
  TEST_ASSERT_EQUAL_UINT64(0, r->tokOut);
  TEST_ASSERT_EQUAL_UINT64(156, r->totIn);
  TEST_ASSERT_EQUAL_UINT64(31, r->totOut);
  // A stored 0 carries into the first real period.
  ModelLedger C;
  C.record("anthropic", "claude-opus-5", td(70, 7, 3, 4), 0, 0);
  C.record("anthropic", "claude-opus-5", td(30, 3), 24300, 20650);
  TEST_ASSERT_EQUAL_UINT64(100, C.findRow("anthropic", "claude-opus-5")->tokIn);
  TEST_ASSERT_EQUAL_UINT64(3, C.findRow("anthropic", "claude-opus-5")->cacheR);
}

static void test_bad_keys_and_empty_spend_are_ignored() {
  ModelLedger L;
  L.record("openai", "a|b", td(10), 1, 1);
  L.record("OpenAI", "gpt-5.6", td(10), 1, 1);
  L.record("openai", "", td(10), 1, 1);
  L.record("openai", "gpt-5.6", td(0), 1, 1);
  TEST_ASSERT_EQUAL(0, (int)L.rows().size());
  L.record("openai", kOtherModel, td(10), 1, 1);   // the fold row itself is accepted
  TEST_ASSERT_EQUAL(1, (int)L.rows().size());
}

// The 9th named model of one provider folds into its ~other row.
static void test_named_cap_per_provider_folds_to_other() {
  ModelLedger L;
  for (int i = 0; i < 12; ++i) L.record("openai", mname(i).c_str(), td(10, 1), 5, 100);
  size_t named = 0;
  for (const auto& r : L.rows())
    if (std::strcmp(r.model, kOtherModel) != 0) ++named;
  TEST_ASSERT_EQUAL(ModelLedger::kMaxNamedPerProv, named);
  const ModelSpend* o = L.findRow("openai", kOtherModel);
  TEST_ASSERT_NOT_NULL(o);
  TEST_ASSERT_EQUAL_UINT64(40, o->tokIn);       // models 8..11
  TEST_ASSERT_EQUAL_UINT64(120, sumTotIn(L, "openai"));   // nothing dropped
}

// 32 rows full: the oldest row with no spend this period folds into its provider's
// ~other row to make room; with every row busy this period, the new model goes to ~other.
static void test_full_ledger_evicts_oldest_idle_row_into_other() {
  ModelLedger L;
  const char* provs[] = {"openai", "anthropic", "mistral", "zai"};
  int day = 100;
  for (int p = 0; p < 4; ++p)
    for (int i = 0; i < 8; ++i) L.record(provs[p], mname(i).c_str(), td(10), 5, (uint32_t)day++);
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, L.rows().size());
  // Next period: every provider rolled to period 6 (UsageLedger syncs the rows to
  // their providers' periods), so every row is idle and the new model takes the
  // OLDEST (openai model-0, day 100).
  L.syncPeriods([](const void*, const char*) -> uint32_t { return 6; }, nullptr);
  L.record("cumulo", "openai/gpt-5.6", td(7), 6, 200);
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, L.rows().size());
  TEST_ASSERT_NOT_NULL(L.findRow("cumulo", "openai/gpt-5.6"));
  TEST_ASSERT_NULL(L.findRow("openai", mname(0).c_str()));
  TEST_ASSERT_NOT_NULL(L.findRow("openai", kOtherModel));
  TEST_ASSERT_EQUAL_UINT64(80, sumTotIn(L, "openai"));   // folded, not dropped

  // Every row busy THIS period: a new model's spend goes to its provider's ~other.
  ModelLedger B;
  for (int p = 0; p < 4; ++p)
    for (int i = 0; i < 8; ++i) B.record(provs[p], mname(i).c_str(), td(10), 5, 100);
  B.record("openai", "brand-new", td(9), 5, 101);
  TEST_ASSERT_NULL(B.findRow("openai", "brand-new"));
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, B.rows().size());
  TEST_ASSERT_EQUAL_UINT64(89, sumTotIn(B, "openai"));     // kept, under ~other
  TEST_ASSERT_NOT_NULL(B.findRow("openai", kOtherModel));

  // Providers keep their own periods (each has its own reset day): a provider that
  // moved to period 6 never evicts another provider's row that still has spend,
  // whatever that row's period key says. Gate G2: a provider with no row at all
  // still gets one - another provider's smallest row folds whole (period spend and
  // totals) into that provider's ~other row (made from its oldest row when it has
  // none), so the folded spend is priced at that provider's highest price.
  ModelLedger C;
  for (int p = 0; p < 4; ++p)
    for (int i = 0; i < 8; ++i) C.record(provs[p], mname(i).c_str(), td(10), 5, (uint32_t)day++);
  C.record("cumulo", "openai/gpt-5.6", td(7), 6, 300);
  TEST_ASSERT_NOT_NULL(C.findRow("cumulo", "openai/gpt-5.6"));
  TEST_ASSERT_EQUAL_UINT64(7, C.findRow("cumulo", "openai/gpt-5.6")->tokIn);
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, C.rows().size());
  TEST_ASSERT_NULL(C.findRow("openai", mname(0).c_str()));   // became openai's ~other
  TEST_ASSERT_NULL(C.findRow("openai", mname(1).c_str()));   // folded into it
  TEST_ASSERT_EQUAL_UINT64(20, C.findRow("openai", kOtherModel)->tokIn);
  for (int p = 0; p < 4; ++p) {
    uint64_t period = 0;
    for (const auto& r : C.rows())
      if (std::strcmp(r.prov, provs[p]) == 0) period += r.tokIn;
    TEST_ASSERT_EQUAL_UINT64(80, period);              // no period spend dropped
    TEST_ASSERT_EQUAL_UINT64(80, sumTotIn(C, provs[p]));
  }
  // A second new provider takes the room the same way: the smallest row (cumulo's 7)
  // becomes cumulo's ~other (no room freed), then the next smallest and oldest
  // (openai's model-2) folds into openai's ~other row.
  C.record("custom", "llama3.1:8b", td(3), 6, 301);
  TEST_ASSERT_NOT_NULL(C.findRow("custom", "llama3.1:8b"));
  TEST_ASSERT_NULL(C.findRow("cumulo", "openai/gpt-5.6"));
  TEST_ASSERT_EQUAL_UINT64(7, C.findRow("cumulo", kOtherModel)->tokIn);
  TEST_ASSERT_EQUAL_UINT64(30, C.findRow("openai", kOtherModel)->tokIn);
  TEST_ASSERT_EQUAL_UINT64(80, sumTotIn(C, "openai"));
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, C.rows().size());
}

// Tracking (gate G1-G3): a recorded row starts it; spend with no model then files
// under ~other; the spend file keeps the marker and every provider's track, and a
// file from before tracks existed (rows, no marker) still reads as tracking.
static void test_tracking_marker_tracks_and_spend_key_round_trip() {
  ModelLedger L;
  TEST_ASSERT_FALSE(L.tracking());
  TEST_ASSERT_EQUAL_STRING("", L.spendKey(""));
  TEST_ASSERT_EQUAL_STRING("", L.spendKey(nullptr));
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", L.spendKey("gpt-5.6"));
  L.record("openai", "gpt-5.6", td(10), 5, 100);
  TEST_ASSERT_TRUE(L.tracking());
  TEST_ASSERT_EQUAL_STRING(kOtherModel, L.spendKey(""));
  ProvTrack* t = L.trackFor("openai");
  TEST_ASSERT_NOT_NULL(t);
  t->periodKey = 5;
  t->pre = td(1, 2, 3, 4);
  t->lost = td(5, 6, 7, 8);
  t->preTotIn = 9;
  t->preTotOut = 10;
  t->storedTotIn = 11;
  t->storedTotOut = 12;
  t->storedBeforeIn = 13;
  t->storedBeforeOut = 14;
  TEST_ASSERT_EQUAL_PTR(t, L.trackFor("openai"));    // found, not made twice
  TEST_ASSERT_NULL(L.trackFor("Bad Prov"));
  const std::string blob = L.serializeSpend();
  TEST_ASSERT_TRUE(blob.find(";!t;!p|openai|5|1|2|3|4|5|6|7|8|9|10|11|12|13|14;openai|gpt-5.6|") !=
                   std::string::npos);
  ModelLedger R;
  R.deserializeSpend(blob);
  TEST_ASSERT_TRUE(R.tracking());
  TEST_ASSERT_EQUAL_STRING(blob.c_str(), R.serializeSpend().c_str());
  TEST_ASSERT_EQUAL_UINT64(8, R.track("openai")->lost.cacheW);
  TEST_ASSERT_EQUAL_UINT64(12, R.track("openai")->storedTotOut);
  TEST_ASSERT_EQUAL_UINT64(13, R.track("openai")->storedBeforeIn);
  TEST_ASSERT_EQUAL_UINT64(14, R.track("openai")->storedBeforeOut);
  // A track group written before the stored totals existed reads them as 0.
  R.deserializeSpend("!v|1;!p|zai|3|7|0|0|0|0|0|0|0|9|1");
  TEST_ASSERT_EQUAL_UINT64(9, R.track("zai")->preTotIn);
  TEST_ASSERT_EQUAL_UINT64(0, R.track("zai")->storedTotIn);
  TEST_ASSERT_EQUAL_UINT64(0, R.track("zai")->storedBeforeOut);
  // A file with rows and no marker (written before tracks existed) is tracking; an
  // empty file is not; garbage and a duplicate track are skipped.
  R.deserializeSpend("!v|1;openai|gpt-5.6|5|10|0|0|0|10|0|100");
  TEST_ASSERT_TRUE(R.tracking());
  TEST_ASSERT_EQUAL(0, (int)R.tracks().size());
  R.deserializeSpend("");
  TEST_ASSERT_FALSE(R.tracking());
  R.deserializeSpend("!v|1;!p|bad prov|1;!p|zai|3|7;!p|zai|4|8;!q|x");
  TEST_ASSERT_TRUE(R.tracking());
  TEST_ASSERT_EQUAL(1, (int)R.tracks().size());
  TEST_ASSERT_EQUAL_UINT32(3, R.track("zai")->periodKey);
  TEST_ASSERT_EQUAL_UINT64(7, R.track("zai")->pre.in);
  // Tracks roll with their provider like rows: a new period zeroes the period parts
  // (the all-time part stays), a stored 0 carries into the first real period.
  L.syncPeriods([](const void*, const char*) -> uint32_t { return 6; }, nullptr);
  TEST_ASSERT_EQUAL_UINT32(6, L.track("openai")->periodKey);
  TEST_ASSERT_EQUAL_UINT64(0, L.track("openai")->pre.in);
  TEST_ASSERT_EQUAL_UINT64(0, L.track("openai")->lost.in);
  TEST_ASSERT_EQUAL_UINT64(9, L.track("openai")->preTotIn);
  ProvTrack* z = L.trackFor("zai");
  z->pre = td(4);
  L.syncPeriods([](const void*, const char*) -> uint32_t { return 7; }, nullptr);
  TEST_ASSERT_EQUAL_UINT64(4, L.track("zai")->pre.in);
  L.dropTrack("zai");
  TEST_ASSERT_NULL(L.track("zai"));
  // Gate M2: a record stores the spend file before its usgLedger write exactly while
  // tracking is on and no spend file is on flash.
  ModelLedger F;
  TEST_ASSERT_FALSE(F.spendFileFirst());       // not tracking: nothing to show yet
  F.markTracking();
  TEST_ASSERT_TRUE(F.spendFileFirst());
  F.setSpendFileStored(true);
  TEST_ASSERT_FALSE(F.spendFileFirst());
  F.setSpendFileStored(false);
  TEST_ASSERT_TRUE(F.spendFileFirst());
  // "!n" (the latest usgLedger write did not land) round-trips; a load without it
  // clears it.
  F.setCountersBehind(true);
  ModelLedger G;
  G.deserializeSpend(F.serializeSpend());
  TEST_ASSERT_TRUE(G.countersBehind());
  G.deserializeSpend("!v|1;!t");
  TEST_ASSERT_FALSE(G.countersBehind());
  // dropAttribution: the period spend leaves the row, totals included.
  L.record("openai", "gpt-5.6", td(25), 7, 101);
  const size_t i = (size_t)(L.findRow("openai", "gpt-5.6") - L.rows().data());
  L.dropAttribution(i);
  TEST_ASSERT_EQUAL_UINT64(0, L.rows()[i].tokIn);
  TEST_ASSERT_EQUAL_UINT64(10, L.rows()[i].totIn);
}

// A provider at its 8-named cap: a new model takes over the provider's oldest named
// row with no spend this period (its all-time totals fold into ~other), so the cap
// is per period, not for the life of the device.
static void test_named_cap_recycles_a_stale_row() {
  ModelLedger L;
  for (int i = 0; i < 8; ++i) L.record("openai", mname(i).c_str(), td(10), 5, (uint32_t)(100 + i));
  L.record("openai", "gpt-6-astra", td(1000), 6, 200);   // a new month, a 9th model
  const ModelSpend* a = L.findRow("openai", "gpt-6-astra");
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL_UINT64(1000, a->tokIn);
  TEST_ASSERT_EQUAL_UINT64(1000, a->totIn);
  TEST_ASSERT_NULL(L.findRow("openai", mname(0).c_str()));   // oldest lastDay retired
  const ModelSpend* o = L.findRow("openai", kOtherModel);
  TEST_ASSERT_NOT_NULL(o);
  TEST_ASSERT_EQUAL_UINT64(0, o->tokIn);     // no period spend moved
  TEST_ASSERT_EQUAL_UINT64(10, o->totIn);    // model-0's all-time total, kept
  TEST_ASSERT_EQUAL_UINT64(1080, sumTotIn(L, "openai"));
  // All 8 named rows busy this period: the next new model goes to ~other.
  for (int i = 1; i < 8; ++i) L.record("openai", mname(i).c_str(), td(1), 6, 201);
  L.record("openai", "brand-new", td(5), 6, 202);
  TEST_ASSERT_NULL(L.findRow("openai", "brand-new"));
  TEST_ASSERT_EQUAL_UINT64(5, L.findRow("openai", kOtherModel)->tokIn);
}

static void test_spend_round_trip_and_garbage() {
  ModelLedger L;
  L.record("openai", "gpt-5.6", td(100, 20), 24300, 20650);
  L.record("anthropic", "claude-opus-5", td(1, 2, 3, 4), 24300, 20651);
  L.record("cumulo", "anthropic/claude-haiku-4-5", td(9, 8), 24299, 20600);
  const std::string blob = L.serializeSpend();
  TEST_ASSERT_EQUAL(0, (int)blob.find("!v|1;"));
  ModelLedger R;
  R.deserializeSpend(blob);
  TEST_ASSERT_EQUAL(3, (int)R.rows().size());
  TEST_ASSERT_EQUAL_STRING(blob.c_str(), R.serializeSpend().c_str());
  const ModelSpend* a = R.findRow("anthropic", "claude-opus-5");
  TEST_ASSERT_EQUAL_UINT64(3, a->cacheR);
  TEST_ASSERT_EQUAL_UINT64(4, a->cacheW);
  TEST_ASSERT_EQUAL_UINT32(20651, a->lastDay);
  // A short row reads its missing tail as 0; garbage and unsafe keys are skipped;
  // no header is fine; over-range numbers saturate.
  ModelLedger G;
  G.deserializeSpend("openai|gpt-5.6|7|5;;garbage;bad prov|m|1;OPENAI|m|1;openai|bad model|1;"
                     "openai|~bogus|1;openai|gpt-5.6|9|9;zai|glm-5.3|1|99999999999999999999999");
  TEST_ASSERT_EQUAL(2, (int)G.rows().size());   // the duplicate key: first wins
  TEST_ASSERT_EQUAL_UINT64(5, G.findRow("openai", "gpt-5.6")->tokIn);
  TEST_ASSERT_EQUAL_UINT64(0, G.findRow("openai", "gpt-5.6")->totIn);
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, G.findRow("zai", "glm-5.3")->tokIn);
  // Parsing stops at the cap.
  std::string big = "!v|1";
  for (int i = 0; i < 50; ++i) big += ";openai|" + mname(i) + "|1|1";
  G.deserializeSpend(big);
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, G.rows().size());
}

static void test_owner_rates_set_clear_cap_and_round_trip() {
  ModelLedger L;
  TEST_ASSERT_EQUAL((int)RateEdit::Ok, (int)L.setOwnerRate("openai", "gpt-5.6", 4000000, 20000000));
  TEST_ASSERT_EQUAL((int)RateEdit::Ok, (int)L.setOwnerRate("openai", "gpt-5.6", 5000000, 0));
  TEST_ASSERT_EQUAL_UINT32(5000000, L.ownerRate("openai", "gpt-5.6")->microIn);
  TEST_ASSERT_EQUAL((int)RateEdit::Invalid, (int)L.setOwnerRate("openai", "~other", 1, 1));
  TEST_ASSERT_EQUAL((int)RateEdit::Ok, (int)L.setOwnerRate("openai", "gpt-5.6", 0, 0));   // clear
  TEST_ASSERT_NULL(L.ownerRate("openai", "gpt-5.6"));
  TEST_ASSERT_EQUAL((int)RateEdit::Ok, (int)L.setOwnerRate("openai", "never-set", 0, 0));
  for (int i = 0; i < (int)ModelLedger::kMaxOwnerRates; ++i)
    TEST_ASSERT_EQUAL((int)RateEdit::Ok, (int)L.setOwnerRate("custom", mname(i).c_str(), 1, 2));
  TEST_ASSERT_EQUAL((int)RateEdit::Full, (int)L.setOwnerRate("custom", "one-more", 1, 2));
  TEST_ASSERT_EQUAL((int)RateEdit::Ok, (int)L.setOwnerRate("custom", mname(3).c_str(), 9, 9));   // edit ok
  TEST_ASSERT_EQUAL((int)RateEdit::Ok, (int)L.setOwnerRate("custom", mname(3).c_str(), 0, 0));   // frees one
  TEST_ASSERT_EQUAL((int)RateEdit::Ok, (int)L.setOwnerRate("custom", "one-more", 1, 2));
  ModelLedger R;
  R.deserializeOwnerRates(L.serializeOwnerRates());
  TEST_ASSERT_EQUAL_STRING(L.serializeOwnerRates().c_str(), R.serializeOwnerRates().c_str());
  TEST_ASSERT_EQUAL(ModelLedger::kMaxOwnerRates, R.ownerRates().size());
  // A 0:0 row on disk is "unset": not loaded.
  ModelLedger Z;
  Z.deserializeOwnerRates("!v|1;openai|gpt-5.6|0|0;openai|gpt-5.6-luna|200000|1200000");
  TEST_ASSERT_EQUAL(1, (int)Z.ownerRates().size());
}

static void test_durable_owner_edit_reverts_on_a_failed_write() {
  ModelLedger L;
  L.setOwnerRate("openai", "gpt-5.6", 4000000, 20000000);
  const std::string before = L.serializeOwnerRates();
  std::string stored;
  TEST_ASSERT_EQUAL((int)RateEdit::WriteFailed,
                    (int)L.setOwnerRateDurable("openai", "gpt-5.6-luna", 1, 2,
                                               [](const std::string&) { return false; }));
  TEST_ASSERT_EQUAL_STRING(before.c_str(), L.serializeOwnerRates().c_str());
  TEST_ASSERT_EQUAL((int)RateEdit::Ok,
                    (int)L.setOwnerRateDurable("openai", "gpt-5.6-luna", 1, 2, [&](const std::string& b) {
                      stored = b;
                      return true;
                    }));
  TEST_ASSERT_EQUAL_STRING(L.serializeOwnerRates().c_str(), stored.c_str());
}

static void test_router_rates_cap_24_and_round_trip() {
  ModelLedger L;
  std::vector<ModelRate> in;
  for (int i = 0; i < 40; ++i) {
    ModelRate r;
    std::strcpy(r.prov, "cumulo");
    std::snprintf(r.model, sizeof(r.model), "openai/%s", mname(i).c_str());
    r.microIn = 1000 + (uint32_t)i;
    r.microOut = 2000;
    in.push_back(r);
  }
  ModelRate bad;
  std::strcpy(bad.prov, "cumulo");
  std::strcpy(bad.model, "bad|id");
  in.insert(in.begin(), bad);
  L.replaceRouterRates(in);
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRouterRates, L.routerRates().size());
  TEST_ASSERT_NULL(L.routerRate("cumulo", "bad|id"));
  ModelLedger R;
  R.deserializeRouterRates(L.serializeRouterRates());
  TEST_ASSERT_EQUAL_STRING(L.serializeRouterRates().c_str(), R.serializeRouterRates().c_str());
  // A u32 overflow on disk saturates (never wraps to a cheap price).
  R.deserializeRouterRates("!v|1;cumulo|openai/gpt-5.6|99999999999|5");
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, R.routerRate("cumulo", "openai/gpt-5.6")->microIn);
}

static void test_saturating_spend() {
  ModelLedger L;
  L.record("openai", "gpt-5.6", td(UINT64_MAX - 5), 1, 1);
  L.record("openai", "gpt-5.6", td(100), 1, 1);
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, L.findRow("openai", "gpt-5.6")->tokIn);
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, L.findRow("openai", "gpt-5.6")->totIn);
}

// Recording into an existing row (and into a full ledger) never grows the storage:
// the vectors are reserved to their caps on first use.
static void test_storage_is_reserved_once() {
  ModelLedger L;
  L.record("openai", "gpt-5.6", td(1, 1), 1, 1);
  const ModelSpend* base = L.rows().data();
  for (int i = 0; i < 200; ++i)
    L.record("openai", mname(i % 40).c_str(), td(1, 1), 1 + i / 50, 1 + (uint32_t)i);
  TEST_ASSERT_EQUAL_PTR(base, L.rows().data());
  TEST_ASSERT_TRUE(L.rows().size() <= ModelLedger::kMaxRows);
}

// The owner's priceable list: choices + head/sub models (as keys) + spend rows +
// price rows of THIS provider; never ~other, never a nested Cumulo id; bounded.
static void test_editable_model_keys() {
  ModelLedger L;
  L.record("cumulo", "zai/glm-5.3", td(1), 1, 1);
  L.record("cumulo", kOtherModel, td(1), 1, 1);
  L.setOwnerRate("cumulo", "anthropic/claude-haiku-4-5", 1, 1);
  L.setOwnerRate("openai", "gpt-5.6", 1, 1);   // another provider: not listed
  const std::string k = editableModelKeys(
      "cumulo", "gpt-5.6,anthropic/claude-opus-5,mistral/mistral/x",
      "anthropic/claude-opus-5-20251101", "", "", L);
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6,anthropic/claude-opus-5,zai/glm-5.3,"
                           "anthropic/claude-haiku-4-5",
                           k.c_str());
  TEST_ASSERT_TRUE(editableHas(k, "zai/glm-5.3"));
  TEST_ASSERT_TRUE(editableHas(k, "openai/gpt-5.6"));
  TEST_ASSERT_FALSE(editableHas(k, "gpt-5.6"));        // a part of an item is not an item
  TEST_ASSERT_FALSE(editableHas(k, "openai/gpt-5"));
  TEST_ASSERT_FALSE(editableHas(k, ""));
  TEST_ASSERT_EQUAL_STRING("llama3.1:8b",
                           editableModelKeys("custom", "", "llama3.1:8b", "", "llama3.1:8b", L).c_str());
  std::string many;
  for (int i = 0; i < 90; ++i) many += (i ? "," : "") + mname(i);
  const std::string capped = editableModelKeys("openai", many, "", "", "", L);
  TEST_ASSERT_EQUAL((int)kMaxEditable - 1, (int)std::count(capped.begin(), capped.end(), ','));
  TEST_ASSERT_EQUAL_STRING("", editableModelKeys("Bad Prov", many, "", "", "", L).c_str());
}

static void test_parse_model_rate_field() {
  ModelRateField f;
  TEST_ASSERT_TRUE(parseModelRateField("cumulo:4000000:20000000:anthropic/claude-opus-5", f));
  TEST_ASSERT_EQUAL_STRING("cumulo", f.prov.c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5", f.model.c_str());
  TEST_ASSERT_EQUAL_UINT32(4000000, f.microIn);
  TEST_ASSERT_EQUAL_UINT32(20000000, f.microOut);
  TEST_ASSERT_TRUE(parseModelRateField("custom:0:0:llama3.1:8b", f));   // ':' in the model
  TEST_ASSERT_EQUAL_STRING("llama3.1:8b", f.model.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, f.microIn);
  TEST_ASSERT_TRUE(parseModelRateField("openai:4294967295:1:gpt-5.6", f));
  TEST_ASSERT_FALSE(parseModelRateField("openai:4294967296:1:gpt-5.6", f));   // > u32
  TEST_ASSERT_FALSE(parseModelRateField("openai:99999999999:1:gpt-5.6", f));
  TEST_ASSERT_FALSE(parseModelRateField("openai:-1:1:gpt-5.6", f));
  TEST_ASSERT_FALSE(parseModelRateField("openai:1.5:1:gpt-5.6", f));
  TEST_ASSERT_FALSE(parseModelRateField("openai::1:gpt-5.6", f));
  TEST_ASSERT_FALSE(parseModelRateField("openai:1:1:", f));
  TEST_ASSERT_FALSE(parseModelRateField(":1:1:gpt", f));
  TEST_ASSERT_FALSE(parseModelRateField("openai:1:1", f));
  TEST_ASSERT_FALSE(parseModelRateField("", f));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_record_merges_and_rolls);
  RUN_TEST(test_bad_keys_and_empty_spend_are_ignored);
  RUN_TEST(test_named_cap_per_provider_folds_to_other);
  RUN_TEST(test_full_ledger_evicts_oldest_idle_row_into_other);
  RUN_TEST(test_tracking_marker_tracks_and_spend_key_round_trip);
  RUN_TEST(test_named_cap_recycles_a_stale_row);
  RUN_TEST(test_spend_round_trip_and_garbage);
  RUN_TEST(test_owner_rates_set_clear_cap_and_round_trip);
  RUN_TEST(test_durable_owner_edit_reverts_on_a_failed_write);
  RUN_TEST(test_router_rates_cap_24_and_round_trip);
  RUN_TEST(test_saturating_spend);
  RUN_TEST(test_storage_is_reserved_once);
  RUN_TEST(test_editable_model_keys);
  RUN_TEST(test_parse_model_rate_field);
  return UNITY_END();
}
