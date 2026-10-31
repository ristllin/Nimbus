#include <unity.h>

#include <cstdio>
#include <string>

#include "nimbus/orch/ledger_migrate.h"

// The legacy meal-log fold (ledger_migrate.h): v4.6.3's one-time migration of
// an ad-hoc meal log into the designed ledger. The fixture below reproduces
// the structure of the real file the 30 Oct investigation pulled (39 rows, no
// header, five generations of the same days in four schemas, a sub-agent's
// invented generation, phantom rows, shifted-column corrections) with the
// owner's data replaced - the raw file is personal data and stays out of the
// repo; the fold itself was verified against it in the lane's scratch run.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

// The same shape as the investigation's file: schema A/B rows (no meal word,
// amount then kcal then protein), schema C rows (a meal word), schema D rows
// (a plain date + meal word), a stray mid-file header, three generations of
// one drink with three different figures, and the owner's final figures last.
const char* kFixture =
    "2026-10-29T17:28:00,Beef rice bowl,400,520,40.1,62.9,12.7,9.8,2.0\n"
    "2026-10-29 21:56,Tea drink (plain),350,0,88,0,0,drink\n"
    "2026-10-29 21:56,Tea drink (plain),350,225,1,56,0,0,drink\n"
    "2026-10-29 21:56,Tea drink (plain),225,225,1,56,0,0,drink\n"
    "2026-10-30 08:30,Yogurt bowl,150 g,342,13.5,0.3,4.1,0.12\n"
    "2026-10-30 08:30,Berries,150 g,75,1.5,0.5,17.5,0\n"
    "2026-10-30 08:30,Coffee (whole milk),100 ml,48,2.4,2.6,3.6,0.05\n"
    "2026-10-30 13:06,lunch,sandwich,450,675,90.0,27.0,0.0\n"
    "2026-10-30 13:06,lunch,phantom beans,250,285,45.0,1.0,0.0\n"
    "2026-10-30 13:06,lunch,berries,100,57,12.0,1.0,0.0\n"
    "2026-10-30 08:30,Yogurt bowl,150,342,13.5,4.1,0.2,9.0\n"
    "2026-10-30 08:30,Berries,150,75,1.5,17.5,0.5,0.7\n"
    "2026-10-30 08:30,Coffee (whole milk),100,48,2.4,3.6,1.3,0.2\n"
    "2026-10-30 13:06,Sandwich,450,675,90.0,0.0,15.0,0.0\n"
    "2026-10-30 13:06,Phantom beans,250,285,0.0,45.0,1.5,16.0\n"
    "timestamp,meal,item,grams,kcal,protein,carbs,fat,fibre\n"
    "2026-10-30 08:30,breakfast,Yogurt bowl,150,90,16.5,6.0,0.3,0.0\n"
    "2026-10-30 08:30,breakfast,Berries,50,26,0.6,6.0,0.35,3.25\n"
    "2026-10-30 13:06,lunch,Chicken,450,742.5,139.5,0.0,16.2,0.0\n"
    "2026-10-30 13:06,lunch,Phantom beans,250,330,22.25,59.25,1.25,21.75\n"
    "2026-10-30 19:26,Chicken,450,742.5,139.5,0.0,16.2,0.0\n"
    "2026-10-30 19:27,Coffee,64,3.2\n"
    "date,meal,item,grams,kcal,protein\n"
    "2026-10-30,breakfast,Yogurt,150,175,30\n"
    "2026-10-30,lunch,Chicken,450,1100,140\n"
    "2026-10-30,afternoon,Coffee,100,66,3.4\n"
    "2026-10-30 19:37,bread roll,140,0,0,0,0\n"
    "2026-10-30 19:37,bread roll,1,140,4,24,1.5\n"
    "2026-10-30 19:37,spread,5,10,1,0,0\n";

bool contains(const std::string& h, const char* needle) {
  return h.find(needle) != std::string::npos;
}

}  // namespace

// The whole fixture folds to the owner's final figures: every (day, meal)
// keeps ONLY its last generation, the same item inside a generation keeps
// only its last row, and the phantom/duplicate/garbage generations drop.
static void test_the_fixture_folds_to_the_final_figures() {
  const LedgerMigration m = planLedgerMigration(kFixture);
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(6, m.folded);
  TEST_ASSERT_EQUAL(2, m.dropped);   // the two stray header rows
  TEST_ASSERT_EQUAL_STRING(
      "date,meal,items,kcal,protein_g\n"
      "2026-10-29,dinner,Beef rice bowl,520,40.1\n"
      "2026-10-29,evening,Tea drink (plain),225,1\n"
      "2026-10-30,breakfast,Yogurt,175,30\n"
      "2026-10-30,lunch,Chicken,1100,140\n"
      "2026-10-30,afternoon,Coffee,66,3.4\n"
      "2026-10-30,dinner,bread roll; spread,150,5\n",
      m.out.c_str());
}

// The three same-item rows of one generation collapse to the LAST (the crash
// + re-send + correction loop the investigation documented).
static void test_same_item_rows_collapse_to_the_last() {
  const LedgerMigration m = planLedgerMigration(
      "2026-10-29 21:56,Tea drink,350,0,88,0,0,drink\n"
      "2026-10-29 21:56,Tea drink,350,225,1,56,0,0,drink\n"
      "2026-10-29 21:56,Tea drink,225,225,1,56,0,0,drink\n");
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(1, m.folded);
  TEST_ASSERT_TRUE(contains(m.out, "2026-10-29,evening,Tea drink,225,1"));
}

// The LAST generation of a (day, meal) wins outright - the owner's final
// figures, not the sub-agent's invented ones and not the earlier estimates.
static void test_the_last_generation_wins() {
  const LedgerMigration m = planLedgerMigration(
      "2026-10-30 08:30,Yogurt bowl,150 g,342,13.5,0.3,4.1,0.12\n"      // gen 1
      "2026-10-30 08:30,Yogurt bowl,150,90,16.5,6.0,0.3,0.0\n"          // gen 2 (invented)
      "2026-10-30,breakfast,Yogurt,150,175,30\n");                      // gen 3 (the owner)
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(1, m.folded);
  TEST_ASSERT_TRUE(contains(m.out, "2026-10-30,breakfast,Yogurt,175,30"));
  TEST_ASSERT_FALSE(contains(m.out, "342"));
}

// Distinct items of one meal coexist (a meal is not one food); the items join
// with "; ", never a comma - the ledger is comma-separated.
static void test_distinct_items_of_one_meal_coexist() {
  const LedgerMigration m = planLedgerMigration(
      "2026-10-30,breakfast,Yogurt,150,175,30\n"
      "2026-10-30,breakfast,Berries,100,206,2.6\n");
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(1, m.folded);
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-10-30,breakfast,Berries; Yogurt,381,32.6"),
                           m.out.c_str());   // items join in a fixed (alphabetical) order
}

// A row with no meal word takes its slot from the time of day; a row with
// neither a time nor a meal word is dropped (never guessed into a slot).
static void test_meal_from_the_time_of_day_and_never_guessed() {
  const LedgerMigration m = planLedgerMigration(
      "2026-10-29T07:30,Porridge,300,320,12\n"
      "2026-10-29 13:00,Soup,400,260,14\n"
      "2026-10-29 18:30,Pasta,350,520,20\n"
      "2026-10-29 22:30,Late tea,200,90,2\n"
      "2026-10-29,plain date row with no meal word,1,2,3,4,5\n");
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(4, m.folded);
  TEST_ASSERT_EQUAL(1, m.dropped);
  TEST_ASSERT_TRUE(contains(m.out, "2026-10-29,breakfast,Porridge"));
  TEST_ASSERT_TRUE(contains(m.out, "2026-10-29,lunch,Soup"));
  TEST_ASSERT_TRUE(contains(m.out, "2026-10-29,dinner,Pasta"));
  TEST_ASSERT_TRUE(contains(m.out, "2026-10-29,evening,Late tea"));
  TEST_ASSERT_FALSE(contains(m.out, "plain date row"));
}

// An existing designed ledger wins its keys (the legacy is the older source).
static void test_the_designed_ledger_wins_its_keys() {
  const std::string ledger =
      "date,meal,items,kcal,protein_g\n2026-10-30,lunch,Chicken,900,120\n";
  const LedgerMigration m = planLedgerMigration(
      "2026-10-30 13:06,lunch,Chicken,450,1100,140\n", ledger);
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_TRUE(contains(m.out, "2026-10-30,lunch,Chicken,900,120"));
  TEST_ASSERT_FALSE(contains(m.out, "1100"));
}

// A comma inside parentheses does not split a field: the real file's item
// names carry them ("Tea drink (no sugar, honey tapioca)") - a blind split
// truncates the item and misreads every column after it.
static void test_a_comma_inside_parentheses_stays_in_the_item() {
  const LedgerMigration m = planLedgerMigration(
      "2026-10-29 21:56,Tea drink (no sugar, honey tapioca),225,225,1,56,0,0,drink\n");
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(1, m.folded);
  TEST_ASSERT_TRUE(contains(m.out, "2026-10-29,evening,Tea drink (no sugar, honey tapioca),225,1"));
}

// Headers, prose and short rows drop and are counted - never guessed.
static void test_unmappable_rows_drop_and_count() {
  const LedgerMigration m = planLedgerMigration(
      "date,meal,item,kcal\n"
      "not a csv row at all\n"
      "2026-10-30,too few fields,1\n"
      "2026-10-30 08:30,Row with text where kcal belongs,nope,12\n");
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(0, m.folded);
  TEST_ASSERT_EQUAL(4, m.dropped);
}

// The device wiring (a source pin: the device code does not build on host -
// the test_tool_taint/test_store_full pattern): begin() runs the one-time
// migration - the legacy file folds into the designed ledger and is removed.
static void test_begin_wires_the_one_time_migration() {
  std::FILE* f = std::fopen("src/agent/files_subsystem.cpp", "rb");
  if (!f) { TEST_FAIL_MESSAGE("src/agent/files_subsystem.cpp unreadable"); return; }
  std::string src;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
  std::fclose(f);
  TEST_ASSERT_TRUE(contains(src, "migrateLegacyMealLog()"));
  TEST_ASSERT_TRUE(contains(src, "planLedgerMigration"));
  TEST_ASSERT_TRUE(contains(src, "kLedgerProject"));
  // The one-time shape: no migration once the ledger exists.
  TEST_ASSERT_TRUE(contains(src, "g_store.find(nimbus::orch::kLedgerProject, "
                                 "nimbus::orch::kLedgerName)"));
  // The header bootstrap (candidate 3b): an append creates a CSV only when the
  // store holds none, and only then must it carry the header row first.
  TEST_ASSERT_TRUE(contains(src, "const bool csvCreate = size == 0 && fileKindIsCsv(name);"));
  // PRISM v4.6.3 row 1: the size guard runs BEFORE the bounded read, and an
  // oversized legacy log is left entirely alone (no fold, no orphan removal -
  // the read's silent truncation would drop the tail's latest generations).
  const size_t fn = src.find("void migrateLegacyMealLog()");
  const size_t guard = src.find("legacyBytes > kFoldCap", fn);
  const size_t read = src.find("readWhole(kLegacyProject, kLegacyName, kFoldCap)", fn);
  TEST_ASSERT_TRUE(fn != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(guard != std::string::npos && read != std::string::npos &&
                               guard < read,
                           "the size guard must run before the bounded read");
  TEST_ASSERT_TRUE(contains(src, "too large to fold safely, left in place"));
  // A fold that folded nothing also leaves the orphan alone (the old
  // out.empty() check was dead: the planner always emits the header).
  TEST_ASSERT_TRUE(contains(src, "m.folded == 0"));
  TEST_ASSERT_FALSE(contains(src, "m.out.empty()"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_fixture_folds_to_the_final_figures);
  RUN_TEST(test_same_item_rows_collapse_to_the_last);
  RUN_TEST(test_the_last_generation_wins);
  RUN_TEST(test_distinct_items_of_one_meal_coexist);
  RUN_TEST(test_meal_from_the_time_of_day_and_never_guessed);
  RUN_TEST(test_the_designed_ledger_wins_its_keys);
  RUN_TEST(test_unmappable_rows_drop_and_count);
  RUN_TEST(test_a_comma_inside_parentheses_stays_in_the_item);
  RUN_TEST(test_begin_wires_the_one_time_migration);
  return UNITY_END();
}
