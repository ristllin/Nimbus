#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/body_budget.h"
#include "nimbus/orch/claims.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/embed_pick.h"
#include "nimbus/orch/failure_chain.h"
#include "nimbus/orch/file_append.h"
#include "nimbus/orch/file_store.h"
#include "nimbus/orch/food_figures.h"
#include "nimbus/orch/food_label.h"
#include "nimbus/orch/food_ledger.h"
#include "nimbus/orch/food_portions.h"
#include "nimbus/orch/ledger_migrate.h"
#include "nimbus/orch/loop_delivery.h"
#include "nimbus/orch/loops.h"
#include "nimbus/orch/msg_batch.h"
#if __has_include("nimbus/orch/open_questions.h")
#include "nimbus/orch/open_questions.h"
#define OIR_HAVE_OPENQ 1
#endif
#include "nimbus/orch/owner_prefs.h"
#include "nimbus/orch/owner_voice.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/reply_guard.h"
#include "nimbus/orch/spawn_claim.h"
#include "nimbus/orch/sub_provenance.h"
#include "nimbus/orch/turn_effects.h"

// test_owner_regressions - the deterministic guards for coverage/OWNER-ISSUES-REGISTER.md
// (lane REGR). One test per register row that has a lib/core seam; every fixture is synthetic,
// shaped like the real case with the same numbers class. No personal text from the evidence.
//
// Rows whose fix landed while this lane was in flight (OIR-19/OIR-42 lane UPSERT 0cefb9d3,
// OIR-43 lane FSCAP fd75210d) carried a runtime self-flip: while the unfixed behavior was
// detected the test reported EXPECTED-FAIL via TEST_IGNORE_MESSAGE; the train removed the
// ignore when the owning lane landed, so the real assertions run unconditionally.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

struct FakeIO : PrefsStoreIO {
  bool exists = false;
  std::string blob;
  bool load(std::string& out) override {
    if (!exists) return false;
    out = blob;
    return true;
  }
  bool saveChecked(const std::string& b) override {
    exists = true;
    blob = b;
    return true;
  }
};

struct FoldIO : FoldStoreIO {
  std::string blob;
  std::string load() override { return blob; }
  void save(const std::string& b) override { blob = b; }
};

bool anyClaimGap(const std::string& reply, const TurnEffects* fx) {
  ClaimEvidence ev;
  ev.fx = fx;
  return !claimGaps(reply, scanClaims(reply), ev).empty();
}

bool hasGapKind(const std::vector<FoodGap>& gaps, FoodGapKind k) {
  for (const auto& g : gaps)
    if (g.kind == k) return true;
  return false;
}

FoodFacts facts(double kcal, double protein, int unknown = 0) {
  FoodFacts f;
  f.have = true;
  f.kcal = kcal;
  f.rows = 4;
  f.protein = protein;
  f.proteinUnknown = unknown;
  return f;
}

const char* kLedgerHeader = "date,meal,items,kcal,protein_g\n";

// OIR-01: a routine's fire request carries the routine's prompt verbatim; nothing
// else is appended to what the owner asked the routine to do.
void test_oir_01_routine_fire_request_carries_prompt() {
  LoopRecord l;
  l.id = "lpe50e9c";
  l.name = "commute brief";
  l.prompt = "Send the commute brief: trains, weather, one calendar line.";
  l.chatId = "owner";
  DeliveryWorld w = deliveryWorld("", "", "owner");
  Delivery d = resolveDelivery(l.chatId, w, true);
  LoopFireRequest fr = buildFireRequest(l, d, 1762000000);
  TEST_ASSERT_EQUAL_STRING(l.prompt.c_str(), fr.prompt.c_str());
}

// OIR-02: "Stored on the device" with no save effect this turn is a claim gap.
void test_oir_02_contradictory_save_status() {
  TurnEffects fx;
  TEST_ASSERT_TRUE(anyClaimGap("Stored on the device.", &fx));
  TEST_ASSERT_TRUE(anyClaimGap("Saved to the device's meal log.", &fx));
  TEST_ASSERT_FALSE(anyClaimGap("Here is the answer.", &fx));
}

// OIR-04: a reply total that disagrees with the owner's own figures is flagged.
void test_oir_04_owner_figures_win() {
  const auto gaps = foodFigureGaps("Today's total is 1,005 kcal.", facts(1045, 0), "1045");
  TEST_ASSERT_TRUE(hasGapKind(gaps, FoodGapKind::WrongDayTotal));
}

// OIR-05: the receipt sums every row of the day (1,611), and a reply stating 1,461
// is a WrongDayTotal gap.
void test_oir_05_day_total_receipt() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-10-28,breakfast,oats,120,8\n"
                         "2026-10-28,snack,bar,131,3\n"
                         "2026-10-28,lunch,plate,355,20\n"
                         "2026-10-28,dinner,plate,1005,40\n";
  double t = -1;
  TEST_ASSERT_TRUE(csvDayTotal(ex, "kcal", "2026-10-28", t));
  TEST_ASSERT_EQUAL_INT(1611, (int)t);
  const auto gaps = foodFigureGaps("Today's total is 1,461 kcal.", facts(1611, 0), "");
  TEST_ASSERT_TRUE(hasGapKind(gaps, FoodGapKind::WrongDayTotal));
}

// OIR-07: "finalised at 2,526" against a ledger whose rows give 2,676 is flagged.
void test_oir_07_unconfirmed_finalised() {
  const auto gaps = foodFigureGaps("Today's total is 2,526 kcal.", facts(2676, 0), "");
  TEST_ASSERT_TRUE(hasGapKind(gaps, FoodGapKind::WrongDayTotal));
}

// OIR-08/OIR-34/OIR-39: an open item is raised once, re-opened when the owner returns
// to it, and dropped when the owner pairs a drop cue with its subject.
#ifdef OIR_HAVE_OPENQ
void test_oir_08_open_question_raise_once_and_drop() {
  FakeIO io;
  OpenQuestions q;
  q.begin(&io);
  std::string id;
  TEST_ASSERT_EQUAL(int(OpenQErr::Ok), int(q.add("Photograph the skyr pot label", 5, &id)));
  // First mention in a reply marks it raised (a change is logged).
  const std::string first = observeOpenQ(q, "", "Did you photograph the skyr pot label yet?");
  TEST_ASSERT_FALSE(first.empty());
  TEST_ASSERT_TRUE(q.find(1)->raised);
  // The same question again changes nothing (raised once, not nagged).
  TEST_ASSERT_TRUE(observeOpenQ(q, "", "Did you photograph the skyr pot label yet?").empty());
  // The owner returns to it: re-opened.
  observeOpenQ(q, "What about the skyr pot label?", "");
  TEST_ASSERT_FALSE(q.find(1)->raised);
  // The owner drops it: removed.
  TEST_ASSERT_TRUE(openQOwnerDrops("Duolingo lesson", "Forget duolingo, no reason to ask"));
  observeOpenQ(q, "Forget the skyr pot label, I will send it", "");
  TEST_ASSERT_EQUAL_INT(0, (int)q.count());
}
#endif  // OIR_HAVE_OPENQ

// OIR-09: the day total counts the morning cappuccino row too (534, not 486).
void test_oir_09_total_includes_every_row() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-10-29,breakfast,plate,486,20\n"
                         "2026-10-29,drink,cappuccino,48,3\n";
  double t = -1;
  TEST_ASSERT_TRUE(csvDayTotal(ex, "kcal", "2026-10-29", t));
  TEST_ASSERT_EQUAL_INT(534, (int)t);
}

// OIR-10: a target row without a source counts for nothing; an invented
// "we adjusted it to 2,500" is flagged against the plan's 2,400.
void test_oir_10_target_needs_source() {
  const FoodTargets t = scanFoodTargets("date,metric,target,source\n2026-10-29,kcal,2500,\n");
  TEST_ASSERT_EQUAL_INT(0, (int)t.kcal);
  TEST_ASSERT_FALSE(unsourcedFoodTargets("we adjusted it to 2,500 kcal", "2400").empty());
}

// OIR-03: a second vision read that disagrees with the head's is surfaced with both
// values, never averaged; an agreeing second read is quiet.
void test_oir_03_second_read_disagreement_surfaced() {
  const std::string head =
      "Nutritional values Per 100g Per serving\n"
      "Energy kJ/kcal 750/179 982/235\n"
      "Fat 5.6g 7.3g\n"
      "Carbohydrate 0.9g 1.2g\n"
      "Protein 31.3g 41.0g\n"
      "Net 131 g";
  const std::string misread =
      "Nutritional values Per 100g Per serving\n"
      "Energy kJ/kcal 750/179 982/235\n"
      "Fat 5.6g 7.3g\n"
      "Carbohydrate 0.9g 1.2g\n"
      "Protein 31.3g 4.3g\n"
      "Net 131 g";
  const auto d = labelReadDisagreements(head, misread);
  TEST_ASSERT_EQUAL_INT(1, (int)d.size());
  TEST_ASSERT_NOT_NULL(strstr(d[0].field.c_str(), "protein per serving"));
  TEST_ASSERT_EQUAL_FLOAT(41.0, d[0].head);
  TEST_ASSERT_EQUAL_FLOAT(4.3, d[0].second);
  const std::string note = secondReadNote(head, misread, "openai");
  TEST_ASSERT_FALSE(note.empty());
  TEST_ASSERT_NOT_NULL(strstr(note.c_str(), "41"));
  TEST_ASSERT_NOT_NULL(strstr(note.c_str(), "4.3"));
  TEST_ASSERT_NOT_NULL(strstr(note.c_str(), "never averaged"));
  TEST_ASSERT_TRUE(secondReadNote(head, head, "openai").empty());
}

// OIR-12: a gram amount converts through kcal-per-100 g, never counts as kcal
// (350 g at 68 kcal/100 g is 238 kcal, not 350).
void test_oir_12_grams_convert_through_kcal100() {
  PortionFood boba;
  boba.name = "boba tea";
  boba.kcal100 = 68;
  boba.fixedG = 350;
  const PortionPlan p = sizePortions({boba}, 1000);
  TEST_ASSERT_TRUE_MESSAGE(p.ok, p.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, (int)p.lines.size());
  TEST_ASSERT_EQUAL_FLOAT(350, p.lines[0].grams);
  TEST_ASSERT_EQUAL_FLOAT(238, p.lines[0].kcal);
  TEST_ASSERT_TRUE(p.lines[0].kcal < 300);   // never the gram figure itself
}

// OIR-13/OIR-21: a "next turn" promise is detected and carries a correction line.
void test_oir_13_later_promise_tracked() {
  size_t b = 0, e = 0;
  const std::string reply = "I'll finish the calculation and update the log next turn.";
  TEST_ASSERT_TRUE(laterPromiseSpan(reply, &b, &e));
  TEST_ASSERT_TRUE(e > b);
  TEST_ASSERT_FALSE(std::string(kLaterPromiseLine).empty());
  TEST_ASSERT_FALSE(laterCorrection(reply.substr(b, e - b)).empty());
}

// OIR-14: yesterday's rows never enter today's total (465, not 2,989).
void test_oir_14_day_rollover() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-10-29,dinner,plate,2524,120\n";
  const std::string nw = "2026-10-30,snack,skyr,342,20\n"
                         "2026-10-30,coffee,black coffee,75,2\n"
                         "2026-10-30,drink,cappuccino,48,3\n";
  const CsvUpsert up = planCsvUpsert(ex, std::string(kLedgerHeader) + nw, "meal", "kcal", "2026-10-30");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(465, (int)up.todayTotal);
}

// OIR-15: a misread per-100 g column fails the label's own cross-column arithmetic.
void test_oir_15_label_cross_column() {
  const LabelRead r = parseLabelRead(
      "Nutritional values Per 100g Per serving\n"
      "Energy kJ/kcal 750/179 982/235\n"
      "Fat 5.6g 7.3g\n"
      "Carbohydrate 0.9g 1.2g\n"
      "Protein 31.3g 4.3g\n"
      "Net 131 g");
  const auto doubts = checkLabelRead(r);
  TEST_ASSERT_FALSE(doubts.empty());
}

// OIR-17: a sub-agent result with no live task and no connector call is unverified,
// and the note says so.
void test_oir_17_subagent_invented_items() {
  TEST_ASSERT_TRUE(subResultUnverified(true, 0, false, 0));
  TEST_ASSERT_TRUE(subResultUnverified(false, 0, true, 0));
  TEST_ASSERT_FALSE(liveDataNotCheckedNote().empty());
}

// OIR-18: the receipt gives 4,006 for the rows the reply itself listed as 3,989.
void test_oir_18_receipt_vs_reply_rows() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-10-30,lunch,chicken sandwich,900,40\n"
                         "2026-10-30,snack,black beans,300,15\n"
                         "2026-10-30,drink,cappuccino,150,6\n"
                         "2026-10-30,snack,raspberries,50,1\n"
                         "2026-10-30,dinner,plate,2606,90\n";
  double t = -1;
  TEST_ASSERT_TRUE(csvDayTotal(ex, "kcal", "2026-10-30", t));
  TEST_ASSERT_EQUAL_INT(4006, (int)t);
  const auto gaps = foodFigureGaps("Today's total is 3,989 kcal.", facts(4006, 0), "");
  TEST_ASSERT_TRUE(hasGapKind(gaps, FoodGapKind::WrongDayTotal));
}

// OIR-16: a two-letter fragment ("bb") yields no food words, so it can never match
// "black beans" as the same food - a near-match is not a match, and no recall or
// provenance backs an invented item.
void test_oir_16_near_match_is_not_a_food_match() {
  TEST_ASSERT_TRUE(foodWordsOf("bb").empty());
  TEST_ASSERT_FALSE(sameFoodWords(foodWordsOf("bb"), foodWordsOf("black beans")));
  TEST_ASSERT_FALSE(foodWordIn("bb", foodWordsOf("black beans")));
  // Control: the same food named two ways still matches.
  TEST_ASSERT_TRUE(sameFoodWords(foodWordsOf("boba tea"), foodWordsOf("another boba tea, no milk")));
}

// OIR-19 (fixed, lane UPSERT 0cefb9d3): a keyed write whose key names a meal slot must never
// replace a row describing a different item. The new item appends with its own key (UPSERT
// TASK item 1: only a correction of the same item replaces), so the replace count is 0 and
// both rows count in the day total (457 + 445 = 902).
void test_oir_19_silent_replace_fixed() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-11-04,dinner,smoked brisket 150 g and bread 58 g,457,40.2\n";
  const std::string nw = "2026-11-04,dinner,Frive Smoky Baharat Mango Chicken 70% of 406 g pack,445,26.0\n";
  const CsvUpsert up = planCsvUpsert(ex, std::string(kLedgerHeader) + nw, "meal", "kcal", "2026-11-04");
  TEST_ASSERT_TRUE(up.ok);
  TEST_ASSERT_EQUAL_INT(0, up.replaced);
  TEST_ASSERT_EQUAL_INT(1, up.appended);
  TEST_ASSERT_EQUAL_INT(902, (int)up.todayTotal);
}

// OIR-20: "now correctly keyed" with no such effect this turn is a claim gap.
void test_oir_20_false_claim_keyed() {
  TurnEffects fx;
  TEST_ASSERT_TRUE(anyClaimGap("I updated the meal log.", &fx));
}

// OIR-22: "38.3 g protein left" implies a 250 g target no source holds.
void test_oir_22_unsourced_protein_target() {
  FoodFacts f = facts(1890, 118.1);
  f.targetsKnown = true;
  f.proteinTarget = 150;
  const auto gaps = foodFigureGaps("38.3 g protein left for today.", f, "150 160");
  TEST_ASSERT_TRUE(hasGapKind(gaps, FoodGapKind::UnsourcedTarget));
}

// OIR-23: an output cut carries a retry note naming the cut, never silence.
void test_oir_23_cut_retry_note() {
  const std::string note = cutRetryNote("output cut", "partial answer");
  TEST_ASSERT_FALSE(note.empty());
}

// OIR-24: repeated fold failures pause the thrash (FoldDue::ThrashPaused), so a
// 400 loop cannot re-send the identical request forever.
void test_oir_24_compact_thrash_pause() {
  FoldIO io;
  FoldStore st;
  st.begin(&io);
  for (int i = 0; i < 5; i++) st.noteMessage("c1", 2000);   // over the 1000-byte threshold
  st.applyFold("c1", "summary", 1700000000);                 // a completed fold
  st.noteMessage("c1", 2000);                                // refills within kThrashTurns
  TEST_ASSERT_EQUAL(int(FoldDue::ThrashPaused), int(st.evaluateDue("c1", 1000, 200)));
}

// OIR-25/OIR-33: the body-budget error round-trips (85,000 B budget, 90,519 B body).
void test_oir_25_body_budget_roundtrip() {
  const std::string err = bodyBudgetError(85000, 90519);
  size_t budget = 0, size = 0;
  TEST_ASSERT_TRUE(parseBodyBudgetError(err, budget, size));
  TEST_ASSERT_EQUAL_size_t(85000, budget);
  TEST_ASSERT_EQUAL_size_t(90519, size);
}

// OIR-27: the built-in day sum over a five-row day is the hand sum (1,878), never 256.
void test_oir_27_day_sum_receipt_multirow() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-11-04,breakfast,skyr and granola,533,32.5\n"
                         "2026-11-04,drink,cappuccino,66,3.3\n"
                         "2026-11-04,lunch,shoyu chicken thighs,557,39.7\n"
                         "2026-11-04,snack,waffles,277,5.7\n"
                         "2026-11-04,dinner,brisket and bread,445,26.0\n";
  double t = -1;
  TEST_ASSERT_TRUE(csvDayTotal(ex, "kcal", "2026-11-04", t));
  TEST_ASSERT_EQUAL_INT(1878, (int)t);
}

// OIR-28: a reply citing a date 18 days old is stale and flagged.
void test_oir_28_stale_status() {
  int age = 0;
  const std::string note = staleStatusIn("Your Duolingo streak, last active 2026-10-14, is 14 days.",
                                         "2026-11-01", 7, &age);
  TEST_ASSERT_FALSE(note.empty());
  TEST_ASSERT_TRUE(age >= 18);
}

// OIR-30: "need ~102 g more" over a day with an unknown-protein row is flagged.
void test_oir_30_protein_unknown() {
  const auto gaps = foodFigureGaps("you need at least ~102 g more protein today", facts(1792, 48.4, 1), "");
  TEST_ASSERT_TRUE(hasGapKind(gaps, FoodGapKind::ProteinLeftUnknown));
}

// OIR-31: the same food on an earlier day is recalled, so its figures stay consistent.
void test_oir_31_cross_day_recall() {
  const std::string file = std::string(kLedgerHeader) +
                           "2026-10-29,drink,boba tea no milk,225,0.5\n"
                           "2026-11-01,drink,boba tea no milk,90,0.5\n";
  const std::string recall = renderFoodRecall(file, "2026-11-01", "another boba tea, no milk");
  TEST_ASSERT_FALSE(recall.empty());
  TEST_ASSERT_NOT_NULL(strstr(recall.c_str(), "225"));
}

// OIR-32: a reply claiming a spawn says so, and the not-started line exists for the
// case where none started.
void test_oir_32_spawn_claim() {
  TEST_ASSERT_TRUE(replyClaimsSpawn("I started a sub-agent to look that up."));
  TEST_ASSERT_FALSE(std::string(kSpawnNotStartedLine).empty());
}

// OIR-33: a raw findings dump is capped at the budget, never delivered whole.
void test_oir_33_raw_dump_capped() {
  const std::string big(200000, 'x');
  const std::string out = sanitizeFailText(big.c_str(), big.size(), 85000);
  TEST_ASSERT_TRUE(out.size() <= 85000);
}

// OIR-36: when the standing-rules list is full, a new active rule makes room
// (evicts) instead of being refused into a memory write.
void test_oir_36_rules_full_makes_room() {
  FakeIO io;
  OwnerPrefs p;
  p.begin(&io);
  for (int i = 0; i < 12; i++) {
    std::string id;
    TEST_ASSERT_EQUAL(int(PrefErr::Ok),
                      int(p.add("rule number " + std::to_string(i), PrefOrigin::Web, 1, false, &id)));
  }
  std::vector<PrefEntry> evicted;
  std::string id;
  TEST_ASSERT_EQUAL(int(PrefErr::Ok),
                    int(p.add("Always assume the whole pack unless I say otherwise", PrefOrigin::Chat, 2,
                              false, &id, &evicted)));
  TEST_ASSERT_FALSE(evicted.empty());
}

// OIR-37: plumbing (artifact paths, memory ids) is detected and corrected.
void test_oir_37_plumbing() {
  const std::string t = "Stored as commute-brief/result-job0009.md (memory m84cc98c8), read back confirmed.";
  TEST_ASSERT_TRUE(hasPlumbing(t));
  TEST_ASSERT_FALSE(plumbingCheckText(t).empty());
  TEST_ASSERT_FALSE(hasPlumbing("Here is your commute brief."));
}

// OIR-39: a standing rule with a stop marker ("never ask about duolingo") blocks the
// routine whose name or prompt raises that topic, and never blocks an unrelated one.
void test_oir_39_never_ask_blocks_the_routine() {
  const std::vector<std::string> rules = {"never ask about duolingo"};
  const StandingBlock hit = standingBlocksFire(rules, "check the Duolingo streak", "Duolingo streak check");
  TEST_ASSERT_TRUE(hit.blocked);
  TEST_ASSERT_EQUAL_STRING("never ask about duolingo", hit.rule.c_str());
  TEST_ASSERT_FALSE(standingBlocksFire(rules, "check the gym streak", "gym check").blocked);
  TEST_ASSERT_FALSE(standingBlocksFire({"always log meals"}, "log today's meals").blocked);
}

// OIR-40: a message arriving past the batch caps is Deferred (re-served), never
// silently dropped.
void test_oir_40_queued_message_deferred_not_dropped() {
  MessageBatcher b;
  bool sawDeferred = false;
  for (int i = 0; i < 200; i++) {
    const BatchAdd r = b.add("c1", "owner", "message " + std::to_string(i), i + 1);
    if (r == BatchAdd::Deferred) {
      sawDeferred = true;
      break;
    }
  }
  TEST_ASSERT_TRUE(sawDeferred);
}

// OIR-41: a Mistral Studio connector is never promised to a Mistral head turn;
// it is SubsessionsOnly (the call rides a spawned sub-agent).
void test_oir_41_mistral_connector_scope() {
  ConnectorInfo c;
  c.name = "gmail";
  c.prov = "mistral";
  c.kind = "connector";
  c.enabled = true;
  c.auth = 1;
  ProviderState ps;
  ps.mistralKeyed = true;
  ps.currentHost = "mistral";
  ps.mistralHeadCarriesStudio = false;
  TEST_ASSERT_EQUAL(int(CapScope::SubsessionsOnly), int(connectorScope(c, ps)));
}

// OIR-42 (fixed, lane UPSERT 0cefb9d3): after a second dinner is logged, the day total must
// be 2,335 (both rows), not 1,878 (the replaced row lost).
void test_oir_42_upsert_total_fixed() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-11-04,breakfast,skyr and granola,533,32.5\n"
                         "2026-11-04,drink,cappuccino,66,3.3\n"
                         "2026-11-04,lunch,shoyu chicken thighs,557,39.7\n"
                         "2026-11-04,snack,waffles,277,5.7\n"
                         "2026-11-04,dinner,smoked brisket 150 g and bread 58 g,457,40.2\n";
  const std::string nw = "2026-11-04,dinner,Frive Smoky Baharat Mango Chicken 70% of 406 g pack,445,26.0\n";
  const CsvUpsert up = planCsvUpsert(ex, std::string(kLedgerHeader) + nw, "meal", "kcal", "2026-11-04");
  TEST_ASSERT_TRUE(up.ok);
  TEST_ASSERT_EQUAL_INT(2335, (int)up.todayTotal);
}

// OIR-43 (fixed, lane FSCAP fd75210d): the store's 256-entry count cap must not be a limit
// an owner can hit while the card has room. The 257th entry is accepted.
void test_oir_43_store_count_cap_fixed() {
  FileStore st;
  for (int i = 0; i < 256; i++) {
    FileEntry e;
    e.project = "mem";
    e.name = "file" + std::to_string(i) + ".txt";
    e.bytes = 10;
    std::string err;
    TEST_ASSERT_TRUE_MESSAGE(st.add(e, err), err.c_str());
  }
  FileEntry e;
  e.project = "mem";
  e.name = "file256.txt";
  e.bytes = 10;
  std::string err;
  const bool ok = st.add(e, err);
  TEST_ASSERT_TRUE(ok);
}

// OIR-44: "Both rows are in today's ledger" with no write effect is a claim gap.
void test_oir_44_write_drop_claim() {
  TurnEffects fx;
  TEST_ASSERT_TRUE(anyClaimGap("Both rows are stored in today's ledger.", &fx));
}

// OIR-46: a parked embedding key is never picked.
void test_oir_46_embed_parked_not_used() {
  TEST_ASSERT_FALSE(embedKeyServes(true, 1, "", true, "openai"));
  TEST_ASSERT_TRUE(embedKeyServes(true, 1, "", false, "openai"));
}

// OIR-47: an HTTP 402 parks as billing, with words a person can act on.
void test_oir_47_park_402_billing() {
  TEST_ASSERT_EQUAL(int(ParkKind::Billing), int(parkKindFor(402, "insufficient credits for call")));
  ParkVerdict v;
  v.open = false;
  v.kind = ParkKind::Billing;
  v.httpStatus = 402;
  TEST_ASSERT_FALSE(parkWords("mistral", v).empty());
  TEST_ASSERT_EQUAL_STRING("billing", parkKindSlug(ParkKind::Billing));
}

// OIR-48: a second food log outside the one ledger is refused with the one call to use.
void test_oir_48_second_ledger_refused() {
  TEST_ASSERT_FALSE(secondFoodLogRefusal("nutrition", "meal_log.csv", true, "owner").empty());
  TEST_ASSERT_TRUE(secondFoodLogRefusal("food", "food-log.csv", true, "owner").empty());
}

// OIR-49: a migration that cannot map legacy rows reports them (dropped > 0), so the
// owner can be told instead of the rows vanishing silently.
void test_oir_49_migration_reports_dropped() {
  const std::string legacy = "lunch 900 kcal\n???\n???\ndinner 1600 kcal\n";
  const LedgerMigration m = planLedgerFold(legacy, legacy.size(), false, "", 0);
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_TRUE(m.dropped >= 2);
  TEST_ASSERT_FALSE(m.sourceFullyFolded());
}

// OIR-50: "you told me" with no backing owner message is an owner-action gap.
void test_oir_50_defended_hallucination() {
  ClaimGap gap;
  TEST_ASSERT_TRUE(ownerActionGap("You told me about the 4 fries and the pack you photographed.",
                                  "4 frives", &gap));
  TEST_ASSERT_FALSE(ownerActionGap("You told me about the 4 frives.", "4 frives", &gap));
}

// OIR-50, lane MLQ: challenged ("I never said that"), mistral-large-4-0 took the claim back
// by quoting its own line, 'I was wrong to say "as you told me."', which repeats the words
// the owner disputes. A disputed turn gets the [DISPUTED CLAIM] line, and on it a quoted
// owner-action phrase is checked unless the quote is the owner's own words.
void test_oir_50_disputed_quote_is_checked() {
  const std::string owner = "I never said that. Show me where I supposedly said it.";
  TEST_ASSERT_TRUE(disputesAttribution(owner));
  TEST_ASSERT_TRUE(disputesAttribution("I didn\xE2\x80\x99t say that"));   // the phone's apostrophe
  // Fix pass: the plain "Where did I tell you I ate a burger?" reads as a recall question
  // too ("Where did I tell you I parked?"), so the dispute form is the "ever" one.
  TEST_ASSERT_TRUE(disputesAttribution("Where did I ever tell you I ate a burger?"));
  TEST_ASSERT_FALSE(disputesAttribution("Where did I tell you I parked?"));
  TEST_ASSERT_FALSE(disputesAttribution("What did I have for dinner yesterday?"));
  // Not disputes: news the owner had not shared, a request to be shown something.
  TEST_ASSERT_FALSE(disputesAttribution("I didn't tell you, but I got the job!"));
  TEST_ASSERT_FALSE(disputesAttribution("Show me where the file is."));
  TEST_ASSERT_TRUE(renderOwnerVoiceHint(owner).find("[DISPUTED CLAIM]") != std::string::npos);
  TEST_ASSERT_TRUE(renderOwnerVoiceHint("Log my lunch").find("[DISPUTED CLAIM]") == std::string::npos);
  const std::string words = normalizedWords(owner);
  const char* quoted = "I was wrong to say \"as you told me.\"";
  ClaimGap gap;
  TEST_ASSERT_TRUE(ownerActionGap(quoted, words, &gap, /*disputed=*/true));
  TEST_ASSERT_TRUE(claimCorrection({gap}, TurnEffects()).find("leave the phrase out") != std::string::npos);
  TEST_ASSERT_FALSE(ownerActionGap(quoted, words, nullptr, /*disputed=*/false));   // as before
  // The owner quoted back is still theirs.
  const std::string mine = normalizedWords("you told me the shop opens at nine");
  TEST_ASSERT_FALSE(ownerActionGap("You wrote \"you told me the shop opens at nine\" earlier.", mine, nullptr, true));
}

// OIR-06, lane MLQ: an earlier owner message that got no reply ("I love tech trivia, ask me
// sometime.") was answered inside a food reply. Every human turn carries [REPLY SCOPE]; the
// reply-scope check finds the sentence that takes the earlier message up, unless this
// message brings it up itself.
void test_oir_06_unanswered_message_not_taken_up() {
  TEST_ASSERT_TRUE(renderOwnerVoiceHint("Log my lunch").rfind("[REPLY SCOPE]", 0) == 0);
  const std::vector<std::string> earlier = {"(Sun 8 Nov) I love tech trivia, ask me sometime."};
  const std::string now = "Log my lunch: a chicken bowl, 620 kcal.";
  const std::string reply = "Logged: lunch, chicken bowl, 620 kcal. I'll quiz you on tech trivia sometime!";
  size_t b = 0, e = 0;
  TEST_ASSERT_EQUAL(0, unansweredTopicSpan(reply, now, earlier, &b, &e));
  TEST_ASSERT_EQUAL_STRING(" I'll quiz you on tech trivia sometime!", reply.substr(b, e - b).c_str());
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("Logged: lunch, chicken bowl, 620 kcal.", now, earlier));
  // This message brings it up: a shared topic word, or a pointer back.
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan(reply, "One trivia question, then log my lunch.", earlier));
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan(reply, "Also add what I mentioned earlier.", earlier));
  // The OIR-16 shape: the owner points back at the unanswered message, so the reply may take it up.
  const std::vector<std::string> bb = {"I also had some bb with my lunch earlier, I will tell you about it later."};
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("Logged the chicken, the blueberries and the bb.",
                                            "Log 450 g of chicken. And please also add the bb I mentioned earlier.", bb));
  // Common words never count as a topic.
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("Thanks, that is logged for today.", now, {"thanks for today"}));
}

// OIR-50, lane MLQ fix pass (review of 77473103): a dispute is a denial of something the
// head said they said, never a recall question or someone else's words. Each case below
// read as a dispute in the first version, which then told the head it was wrong and banned
// "you mentioned" from a reply that could answer the question from the record.
void test_oir_50_recall_questions_are_not_disputes() {
  const char* const recall[] = {
      "When did I tell you about my dentist appointment?",
      "Where did I say the meeting was?",
      "Where did I tell you my sister's name?",
      "When did I tell you that I was moving?",
      "My boss never said that, she said Friday.",
      "I never sent you the invoice, can you remind me tomorrow?",
      "Is that not what I said yesterday about the gym?",
      "I never photographed the receipt, remind me tonight.",
  };
  for (const char* m : recall) TEST_ASSERT_FALSE_MESSAGE(disputesAttribution(m), m);
  const char* const disputes[] = {
      "I never said that. Show me where I supposedly said it.",
      "Where did I say that?",
      "When did I say that?!",
      "where did i tell you that",
      "When did I ever tell you I ate a burger?",
      "I didn't say that",
      "That's not what I said.",
      "I never sent you that photo.",
      "I never said I had fries.",
  };
  for (const char* m : disputes) TEST_ASSERT_TRUE_MESSAGE(disputesAttribution(m), m);
}

// OIR-50, lane MLQ fix pass: the owner-action correction named only the phrase it found, so
// the rewrite swapped "you told me" for "something you said" and the claim check fired again;
// the device's own tail then said "something you said" too. The correction names every
// attribution phrase, and the tail passes the very check that appends it.
void test_oir_50_owner_action_tail_keeps_its_own_rule() {
  const std::string words = normalizedWords("I never said that. Show me where I supposedly said it.");
  ClaimGap gap;
  TEST_ASSERT_TRUE(ownerActionGap("I was wrong to present it as something you said, sorry about the burger.", words,
                                  &gap, /*disputed=*/true));
  const std::string fix = claimCorrection({gap}, TurnEffects());
  for (const char* p : {"\"you told me\"", "\"you said\"", "\"you mentioned\"", "\"you photographed\""})
    TEST_ASSERT_TRUE_MESSAGE(fix.find(p) != std::string::npos, fix.c_str());
  const std::string tail = claimTailLine(gap);
  TEST_ASSERT_FALSE_MESSAGE(ownerActionGap(tail, words, nullptr, /*disputed=*/true), tail.c_str());
  for (const char* p : {"you said", "you told me", "you mentioned"})
    TEST_ASSERT_TRUE_MESSAGE(tail.find(p) == std::string::npos, tail.c_str());
}

// OIR-06, lane MLQ fix pass: the check holds back a remark the owner made in passing; it
// never holds back what they are owed. Each negative is a case the first version flagged
// (generic word overlap with any unanswered message), each positive a reply that took the
// remark up on a live leg.
void test_oir_06_owed_answers_and_reported_facts_are_in_scope() {
  // The per-turn line says so too: an earlier question shown with [no reply] is owed.
  TEST_ASSERT_TRUE(renderOwnerVoiceHint("Log my lunch").find("[no reply] still gets its answer") != std::string::npos);
  // An earlier question that got no reply is owed its answer, pinged or not.
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("Yes, I am here. Your protein target is 150 g.", "Hello? Are you there?",
                                            {"What is my protein target?"}));
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("Logged the pasta. And your protein target? I'll check: 150 g.",
                                            "Log my dinner: pasta 700 kcal", {"What is my protein target?"}));
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("Logged the pasta. I'll set the reminder for the dentist too.",
                                            "Log my dinner: pasta 700 kcal", {"Please remind me about the dentist at 5."}));
  // A ping asks after the earlier message, whatever it was.
  const std::vector<std::string> salad = {"I had a chicken salad for lunch."};
  for (const char* ping : {"Hello?", "Are you there?", "Did you get my last message", "??", "Any update?"})
    TEST_ASSERT_EQUAL_MESSAGE(-1, unansweredTopicSpan("Sorry, I'm here. I'll log the chicken salad now.", ping, salad),
                              ping);
  // A recap asks about a span that holds it.
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("You have had chicken salad and a latte so far, 870 kcal. I'll keep adding.",
                                            "How many calories have I had today?", salad));
  // A sentence that reports a fact does not take the remark up.
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("You have had chicken salad and a latte, 870 kcal.",
                                            "What is my total now?", salad));
  // A sentiment word is not a topic.
  const std::vector<std::string> trivia = {"I love tech trivia, ask me sometime."};
  const std::string dinner = "Log my dinner: pasta 700 kcal";
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("Logged your dinner: pasta, 700 kcal. Love the choice!", dinner, trivia));
  TEST_ASSERT_EQUAL(-1, unansweredTopicSpan("Logged your dinner. Would you love a dessert idea?", dinner, trivia));
  // The class still holds: a promise, an acknowledgement, an offer, a question, an aside.
  const char* const taken[] = {
      "Logged. I'll remember you love tech trivia.",
      "Logged. Noted on tech trivia.",
      "Logged. Want me to ask you a tech trivia question now?",
      "Logged. Good tech trivia offer, I will take you up on that sometime!",
      "Logged. And since you love tech trivia, here's one: what was the first computer virus called?",
      "Logged. By the way, tech trivia sounds like fun.",
  };
  for (const char* r : taken) TEST_ASSERT_EQUAL_MESSAGE(0, unansweredTopicSpan(r, dinner, trivia), r);
  // A request deferred to "sometime" is a remark too, even as a question.
  TEST_ASSERT_EQUAL(0, unansweredTopicSpan("Logged. I'll quiz you on trivia sometime!", dinner,
                                           {"Could you quiz me on trivia sometime?"}));
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_oir_01_routine_fire_request_carries_prompt);
  RUN_TEST(test_oir_02_contradictory_save_status);
  RUN_TEST(test_oir_03_second_read_disagreement_surfaced);
  RUN_TEST(test_oir_04_owner_figures_win);
  RUN_TEST(test_oir_05_day_total_receipt);
  RUN_TEST(test_oir_06_unanswered_message_not_taken_up);
  RUN_TEST(test_oir_06_owed_answers_and_reported_facts_are_in_scope);
  RUN_TEST(test_oir_07_unconfirmed_finalised);
#ifdef OIR_HAVE_OPENQ
  RUN_TEST(test_oir_08_open_question_raise_once_and_drop);
#endif
  RUN_TEST(test_oir_09_total_includes_every_row);
  RUN_TEST(test_oir_10_target_needs_source);
  RUN_TEST(test_oir_12_grams_convert_through_kcal100);
  RUN_TEST(test_oir_13_later_promise_tracked);
  RUN_TEST(test_oir_14_day_rollover);
  RUN_TEST(test_oir_15_label_cross_column);
  RUN_TEST(test_oir_16_near_match_is_not_a_food_match);
  RUN_TEST(test_oir_17_subagent_invented_items);
  RUN_TEST(test_oir_18_receipt_vs_reply_rows);
  RUN_TEST(test_oir_19_silent_replace_fixed);
  RUN_TEST(test_oir_20_false_claim_keyed);
  RUN_TEST(test_oir_22_unsourced_protein_target);
  RUN_TEST(test_oir_23_cut_retry_note);
  RUN_TEST(test_oir_24_compact_thrash_pause);
  RUN_TEST(test_oir_25_body_budget_roundtrip);
  RUN_TEST(test_oir_27_day_sum_receipt_multirow);
  RUN_TEST(test_oir_28_stale_status);
  RUN_TEST(test_oir_30_protein_unknown);
  RUN_TEST(test_oir_31_cross_day_recall);
  RUN_TEST(test_oir_32_spawn_claim);
  RUN_TEST(test_oir_33_raw_dump_capped);
  RUN_TEST(test_oir_36_rules_full_makes_room);
  RUN_TEST(test_oir_37_plumbing);
  RUN_TEST(test_oir_39_never_ask_blocks_the_routine);
  RUN_TEST(test_oir_40_queued_message_deferred_not_dropped);
  RUN_TEST(test_oir_41_mistral_connector_scope);
  RUN_TEST(test_oir_42_upsert_total_fixed);
  RUN_TEST(test_oir_43_store_count_cap_fixed);
  RUN_TEST(test_oir_44_write_drop_claim);
  RUN_TEST(test_oir_46_embed_parked_not_used);
  RUN_TEST(test_oir_47_park_402_billing);
  RUN_TEST(test_oir_48_second_ledger_refused);
  RUN_TEST(test_oir_49_migration_reports_dropped);
  RUN_TEST(test_oir_50_defended_hallucination);
  RUN_TEST(test_oir_50_disputed_quote_is_checked);
  RUN_TEST(test_oir_50_recall_questions_are_not_disputes);
  RUN_TEST(test_oir_50_owner_action_tail_keeps_its_own_rule);
  return UNITY_END();
}
