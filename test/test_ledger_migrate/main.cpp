#include <unity.h>

#include <cstdio>
#include <string>

#include "nimbus/orch/file_append.h"
#include "nimbus/orch/food_ledger.h"
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

// The fold replaces the ledger and deletes the legacy file, so it runs only on text read whole.
static void test_the_fold_stands_down_on_anything_not_read_whole() {
  const std::string legacy =
      "date,meal,items,kcal,protein_g\n2026-10-31,lunch,soup,300,20\n";
  const std::string ledger = "date,meal,items,kcal,protein_g\n2026-11-01,breakfast,eggs,200,12\n2026-11-01,lunch,rice,500,9\n";
  // Whole on both sides: it folds and the ledger stays byte for byte.
  LedgerMigration whole = planLedgerFold(legacy, legacy.size(), true, ledger, ledger.size());
  TEST_ASSERT_TRUE(whole.ok);
  TEST_ASSERT_EQUAL_INT(1, whole.folded);
  TEST_ASSERT_EQUAL_INT(0, whole.out.rfind(ledger, 0));
  // No ledger yet: nothing to lose.
  TEST_ASSERT_TRUE(planLedgerFold(legacy, legacy.size(), false, "", 0).ok);
  // A short read of the ledger, an empty read of a ledger that has bytes, a ledger over the
  // read cap, a short read of the legacy file: nothing is written and nothing is deleted.
  const std::string prefix = ledger.substr(0, ledger.size() - 12);
  for (const LedgerMigration& m : {planLedgerFold(legacy, legacy.size(), true, prefix, ledger.size()),
                                   planLedgerFold(legacy, legacy.size(), true, "", ledger.size()),
                                   planLedgerFold(legacy, legacy.size(), true, ledger, ledger.size() + 300000),
                                   planLedgerFold(legacy.substr(0, 20), legacy.size(), true, ledger, ledger.size())}) {
    TEST_ASSERT_FALSE(m.ok);
    TEST_ASSERT_EQUAL_INT(0, m.folded);
    TEST_ASSERT_TRUE(m.out.empty());
    TEST_ASSERT_FALSE(m.err.empty());
  }
}

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

// A model writing standard CSV quotes an item that holds a comma. The quote keeps the
// columns where they are in every schema, and the ledger row quotes it again so that
// every reader of the ledger sees one item and the true figures.
static void test_a_quoted_item_keeps_its_comma_and_its_columns() {
  const LedgerMigration m = planLedgerMigration(
      "2026-10-30T12:00:00,lunch,\"Rice, beans\",200,400,20\n"          // ts, meal, item, grams, kcal, protein
      "2026-10-30T19:00:00,dinner,Pasta,150,500,18\n"
      "2026-10-30 08:30,\"Eggs, toast\",2 pieces,300,16\n"              // ts, item, amount, kcal, protein
      "2026-10-29 21:00,\"Tea, \"\"chai\"\" style\",60,2\n"             // ts, item, kcal, protein
      "2026-10-29,breakfast, \"Oats, milk\" ,250,310,11\n");            // blanks around the quotes
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL_MESSAGE(5, m.folded, m.out.c_str());
  TEST_ASSERT_EQUAL(0, m.dropped);
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-10-30,lunch,\"Rice, beans\",400,20\n"), m.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-10-30,dinner,Pasta,500,18\n"), m.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-10-30,breakfast,\"Eggs, toast\",300,16\n"), m.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-10-29,evening,\"Tea, \"\"chai\"\" style\",60,2\n"), m.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-10-29,breakfast,\"Oats, milk\",310,11\n"), m.out.c_str());

  // The day's readers take every row: nothing is left out of the total.
  LogDay day;
  day.date = "2026-10-30";
  day.zone = "GMT";
  const FoodLedger l = scanFoodLedger(m.out, true, day, kLedgerProject, kLedgerName);
  TEST_ASSERT_EQUAL(3, l.kcalRows);
  TEST_ASSERT_EQUAL(0, l.kcalUnread);
  TEST_ASSERT_TRUE(l.kcal > 1199.9 && l.kcal < 1200.1);
  TEST_ASSERT_TRUE(l.protein > 53.9 && l.protein < 54.1);
  // A keyed correction finds the row by its meal.
  const CsvUpsert fix = planCsvUpsert(m.out, std::string(kLedgerHeader) + "\n2026-10-30,lunch,Rice,350,15\n",
                                      "meal", "kcal", "2026-10-30");
  TEST_ASSERT_TRUE_MESSAGE(fix.ok, fix.err.c_str());
  TEST_ASSERT_EQUAL(1, fix.replaced);
}

// A comma the item did not quote shifts every later column. A row that shifted shows it
// in its quantity cell, and it drops: grams must never be read as kcal.
static void test_an_unquoted_comma_never_shifts_the_figures() {
  const char* shifted[] = {
      "2026-10-30T12:00:00,lunch,Rice, beans,200,400,20\n",    // the meal-word schema
      "2026-10-30T12:00:00,lunch,Rice, 2 eggs,200,400,20\n",   // a digit after the comma's blank
      "2026-10-30 08:30,Rice, beans,200,400,20\n",             // the amount schema
      "2026-10-30 08:30,Rice, beans,400,20\n",                 // the shortest schema
      "2026-10-30T12:00:00,lunch,\"Rice, beans,200,400,20\n",  // a quote that never closes
      "2026-10-30T12:00:00,lunch,\"Rice\" beans,200,400,20\n", // text after a closing quote
  };
  for (const char* row : shifted) {
    const LedgerMigration m = planLedgerMigration(row);
    TEST_ASSERT_TRUE(m.ok);
    TEST_ASSERT_EQUAL_MESSAGE(0, m.folded, row);
    TEST_ASSERT_EQUAL_MESSAGE(1, m.dropped, row);
    TEST_ASSERT_EQUAL_MESSAGE(1, m.droppedData, row);
  }
  // An item that needs no quotes stays as it was, a parenthesized comma included, and a
  // comma the parentheses cannot shield (they never close) is quoted on the way out.
  const LedgerMigration kept = planLedgerMigration(
      "2026-10-30,lunch,Tea (no sugar, honey),100,50,1\n"
      "2026-10-30,dinner,\"Soup (large, hot\",100,70,2\n"
      "2026-10-30,breakfast,\"Oats (large\",100,30,3\n");   // no comma: the readers' open paren shields nothing
  TEST_ASSERT_EQUAL(3, kept.folded);
  TEST_ASSERT_TRUE_MESSAGE(contains(kept.out, "2026-10-30,breakfast,Oats (large,30,3\n"), kept.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(kept.out, "2026-10-30,lunch,Tea (no sugar, honey),50,1\n"), kept.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(kept.out, "2026-10-30,dinner,\"Soup (large, hot\",70,2\n"), kept.out.c_str());
  CsvDayTally tally("kcal", "2026-10-30");
  RowSplitter rows([&](const std::string& row, bool complete) {
    tally.feed(row, complete);
    return true;
  });
  rows.push(kept.out.data(), kept.out.size());
  rows.end();
  TEST_ASSERT_EQUAL(3, tally.total().rows);
  TEST_ASSERT_EQUAL(0, tally.total().unread);
  TEST_ASSERT_TRUE(tally.total().total > 149.9 && tally.total().total < 150.1);
}

// A row whose protein cell exceeds its kcal cell cannot be a meal's figures: protein is
// 4 kcal/g, so protein_g <= kcal/4 <= kcal. A four-field row of shape <ts, item, amount,
// kcal> (the amount schema without its protein cell) lands exactly there - grams read as
// kcal - so it drops and counts, and the source is kept: the misread never reaches the
// ledger and the raw row is never deleted.
static void test_a_row_whose_protein_exceeds_its_kcal_drops_and_keeps_the_source() {
  const LedgerMigration m = planLedgerMigration("2026-10-31 20:15,Chicken breast,200,330\n");
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(0, m.folded);
  TEST_ASSERT_EQUAL(1, m.dropped);
  TEST_ASSERT_EQUAL(1, m.droppedData);
  TEST_ASSERT_FALSE(m.sourceFullyFolded());
  TEST_ASSERT_EQUAL_STRING("date,meal,items,kcal,protein_g\n", m.out.c_str());
  // A physically possible row still folds (coffee: 64 kcal, 3.2 g protein).
  const LedgerMigration ok = planLedgerMigration("2026-10-30 19:27,Coffee,64,3.2\n");
  TEST_ASSERT_EQUAL(1, ok.folded);
  TEST_ASSERT_TRUE(contains(ok.out, "2026-10-30,dinner,Coffee,64,3.2"));
  // ...and so does a row whose figures sit exactly on the bound (protein == kcal only at 0).
  const LedgerMigration zero = planLedgerMigration("2026-10-30 19:27,Water,0,0\n");
  TEST_ASSERT_EQUAL(1, zero.folded);
  // A five-field amount row is a different, unambiguous schema: it folds as before.
  const LedgerMigration five = planLedgerMigration("2026-10-31 20:15,Chicken breast,200,330,40\n");
  TEST_ASSERT_EQUAL(1, five.folded);
  TEST_ASSERT_TRUE(contains(five.out, "2026-10-31,evening,Chicken breast,330,40"));
}

// The source file goes only when every row of figures reached the ledger: a dropped data
// row has no other copy. A header row holds no figures, so it never keeps the file.
static void test_the_source_goes_only_when_every_data_row_folded() {
  const LedgerMigration clean = planLedgerMigration(
      "timestamp,meal,item,grams,kcal,protein\n"
      "2026-10-30,lunch,Soup,300,260,14\n");
  TEST_ASSERT_EQUAL(1, clean.folded);
  TEST_ASSERT_EQUAL(0, clean.dropped);   // a first row that names the columns is read, not dropped
  TEST_ASSERT_EQUAL(0, clean.droppedData);
  TEST_ASSERT_TRUE(clean.sourceFullyFolded());
  // A header further down is dropped, and a total row is no meal: neither keeps the file.
  const LedgerMigration stray = planLedgerMigration(
      "2026-10-29,lunch,Rice,200,300,8\n"
      "timestamp,meal,item,grams,kcal,protein\n"
      "2026-10-30,lunch,Soup,300,260,14\n");
  TEST_ASSERT_EQUAL(1, stray.dropped);
  TEST_ASSERT_EQUAL(0, stray.droppedData);
  TEST_ASSERT_TRUE(stray.sourceFullyFolded());
  const LedgerMigration totals = planLedgerMigration(
      "key,date,items,kcal,protein_g\n"
      "2026-11-01-lunch,2026-11-01,soup,300,10\n"
      "2026-11-01-total,2026-11-01,total,300,10\n");
  TEST_ASSERT_EQUAL(1, totals.folded);
  TEST_ASSERT_EQUAL(1, totals.dropped);
  TEST_ASSERT_EQUAL(0, totals.droppedData);
  TEST_ASSERT_TRUE(totals.sourceFullyFolded());
  TEST_ASSERT_TRUE(planLedgerMigration(kFixture).sourceFullyFolded());   // its two drops are headers

  const LedgerMigration lost = planLedgerMigration(
      "2026-10-30,lunch,Soup,300,260,14\n"
      "2026-10-30 19:00,Rice, beans,200,400,20\n");
  TEST_ASSERT_EQUAL(1, lost.folded);
  TEST_ASSERT_EQUAL(1, lost.droppedData);
  TEST_ASSERT_FALSE(lost.sourceFullyFolded());
  TEST_ASSERT_FALSE(planLedgerMigration("not a csv row at all\n").sourceFullyFolded());
  TEST_ASSERT_FALSE(planLedgerMigration("").sourceFullyFolded());
}

// A member can create nutrition/meal_log.csv in their own namespace (the name is not
// reserved). It must never become the owner's ledger: only a file the owner controls
// folds. Every owner a stored entry can carry, the shape of the index on a real device.
static void test_only_the_owners_legacy_log_folds() {
  FileStore store{FileStore::Limits{}};
  struct Row { const char* owner; bool folds; };
  const Row rows[] = {
      {"", true},             // a file from before namespaces is the owner's
      {kOwnerNs, true},
      {"chat:12345", false},  // a member or guest
      {kSharedNs, false},
      {kMcpNs, false},
  };
  for (const Row& r : rows) {
    FileEntry e;
    e.project = "nutrition";
    e.name = "meal_log.csv";
    e.owner = r.owner;
    TEST_ASSERT_EQUAL_MESSAGE(r.folds, legacyLogFoldable(store, e), r.owner);
  }
}

// A kept source folds once. The ledger existing cannot be the sign any more (a ledger beside a
// legacy file is what the fold is for), so without the mark a row the ledger removed since
// would come back at every boot.
static void test_a_kept_source_folds_once_even_with_a_ledger() {
  const std::string legacy =
      "2026-10-29,lunch,Boba,300,225,1\n"
      "2026-10-30 19:00,Rice, beans,200,400,20\n";   // a row that cannot be read keeps the file
  const LedgerMigration first = planLedgerMigration(legacy, "date,meal,items,kcal,protein_g\n");
  TEST_ASSERT_EQUAL(1, first.folded);
  TEST_ASSERT_FALSE(first.sourceFullyFolded());
  const std::string marked = markLegacyFolded(legacy);
  TEST_ASSERT_EQUAL_INT(0, (int)marked.find(legacy));
  TEST_ASSERT_FALSE(legacyFoldedAlready(legacy));
  TEST_ASSERT_TRUE(legacyFoldedAlready(marked));
  // The owner took the boba out of the ledger: the next boot writes nothing.
  const LedgerMigration again = planLedgerMigration(marked, "date,meal,items,kcal,protein_g\n");
  TEST_ASSERT_TRUE(again.ok);
  TEST_ASSERT_EQUAL(0, again.folded);
  TEST_ASSERT_TRUE(again.out.empty());
  TEST_ASSERT_FALSE(again.sourceFullyFolded());
  // Also with the ledger gone, and with a source that has no last line end.
  TEST_ASSERT_EQUAL(0, planLedgerMigration(marked).folded);
  TEST_ASSERT_TRUE(legacyFoldedAlready(markLegacyFolded("2026-10-29,lunch,Boba,300,225,1")));
}

// The device wiring of the two rules above, in the order they must run: the owner check
// before a byte is read, and the remove of the source only past the fully-folded check.
static void test_the_fold_checks_the_owner_and_keeps_an_unfolded_source() {
  std::FILE* f = std::fopen("src/agent/files_subsystem.cpp", "rb");
  if (!f) { TEST_FAIL_MESSAGE("src/agent/files_subsystem.cpp unreadable"); return; }
  std::string src;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
  std::fclose(f);
  const size_t fn = src.find("void migrateLegacyMealLog()");
  TEST_ASSERT_TRUE(fn != std::string::npos);
  const size_t owner = src.find("legacyLogFoldable(g_store,", fn);
  const size_t read = src.find("readWhole(kLegacyProject, kLegacyName, kFoldCap)", fn);
  const size_t save = src.find("saveText(nimbus::orch::kLedgerProject", fn);
  const size_t whole = src.find("m.sourceFullyFolded()", fn);
  const size_t marks = src.find("markLegacyFolded(legacy)", fn);
  const size_t markfail = src.find("could not be marked as folded", fn);
  const size_t remove = src.find("remove(pathOf(kLegacyProject, kLegacyName)", fn);
  TEST_ASSERT_TRUE(owner != std::string::npos && read != std::string::npos);
  TEST_ASSERT_TRUE(save != std::string::npos && whole != std::string::npos && remove != std::string::npos);
  TEST_ASSERT_TRUE(marks != std::string::npos && markfail != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(owner < read, "the owner check must run before the read");
  // The fold-once mark is durable BEFORE the ledger is written: a kept source that is not
  // marked would fold again at every boot and re-add rows the owner removed since, and a
  // failed mark write must leave nothing written (the fold retries at the next boot).
  TEST_ASSERT_TRUE_MESSAGE(whole < marks && marks < save,
                           "the kept source is marked before the ledger is saved");
  TEST_ASSERT_TRUE_MESSAGE(markfail < save, "a failed mark write returns before any ledger write");
  TEST_ASSERT_TRUE_MESSAGE(save < remove, "the source goes only past the fully-folded check");
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
// 31 Oct: the head rebuilt the legacy file with a header, keyed by hand, next to the designed
// ledger, and logged every later meal there. Those rows fold in too: dated keys, a bare comma
// in the items, a total row, a blank protein (unknown, never 0 g), and the ledger's own rows
// keep their keys.
static void test_a_headered_legacy_file_folds_into_an_existing_ledger() {
  const std::string legacy =
      "key,date,items,kcal,protein_g\n"
      "2026-11-01-drink,2026-11-01,cappuccino,66,3.3\n"
      "2026-11-01-lunch,2026-11-01,3 eggs, 1 bun,517,27.1\n"
      "2026-11-01-lunch2,2026-11-01,steak, mash, sauce,923,\n"
      "2026-11-01-drink,2026-11-01,cappuccino large,90,4\n"
      "2026-11-01-total,2026-11-01,total,1200,50\n"
      "2026-10-30-lunch,2026-10-30,stale copy,1,1\n";
  const std::string ledger = "date,meal,items,kcal,protein_g\n2026-10-30,lunch,the real lunch,600,30\n";
  const LedgerMigration m = planLedgerMigration(legacy, ledger);
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-10-30,lunch,the real lunch,600,30\n"), m.out.c_str());
  TEST_ASSERT_FALSE_MESSAGE(contains(m.out, "stale copy"), m.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-11-01,drink,cappuccino large,90,4\n"), m.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-11-01,lunch,3 eggs; 1 bun,517,27.1\n"), m.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(contains(m.out, "2026-11-01,lunch2,steak; mash; sauce,923,\n"), m.out.c_str());
  TEST_ASSERT_FALSE_MESSAGE(contains(m.out, "1200"), "a total row is no meal");
  const FoodLedger l = scanFoodLedger(m.out, true, LogDay{"2026-11-01", "GMT"}, kFoodLedgerProject, kFoodLedgerOwnerName);
  TEST_ASSERT_EQUAL_INT(90 + 517 + 923, (int)l.kcal);
  TEST_ASSERT_EQUAL_INT(1, l.proteinUnknown);
  // The ledger stays byte for byte: a quoted comma, two rows of one meal.
  const std::string kept = "date,meal,items,kcal,protein_g\n2026-10-30,lunch,\"chicken, rice\",600,30\n2026-10-30,lunch,berries,57,1\n";
  const LedgerMigration both = planLedgerMigration(legacy, kept);
  TEST_ASSERT_EQUAL_INT(0, (int)both.out.find(kept));
  TEST_ASSERT_FALSE_MESSAGE(contains(both.out, "stale copy"), both.out.c_str());
  // A ledger in other columns takes nothing, and loses nothing.
  const LedgerMigration other = planLedgerMigration(legacy, "date,time,food,kcal\n2026-10-30,12:00,soup,300\n");
  TEST_ASSERT_TRUE(other.ok);
  TEST_ASSERT_EQUAL_INT(0, other.folded);
  // A meal column, and a time for the row with no meal.
  const LedgerMigration named = planLedgerMigration(
      "timestamp,meal,food,kcal,protein\n2026-11-01 12:30,lunch,soup,300,10\n", std::string());
  TEST_ASSERT_TRUE_MESSAGE(contains(named.out, "2026-11-01,lunch,soup,300,10\n"), named.out.c_str());
}

static void test_begin_wires_the_one_time_migration() {
  std::FILE* f = std::fopen("src/agent/files_subsystem.cpp", "rb");
  if (!f) { TEST_FAIL_MESSAGE("src/agent/files_subsystem.cpp unreadable"); return; }
  std::string src;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
  std::fclose(f);
  TEST_ASSERT_TRUE(contains(src, "migrateLegacyMealLog()"));
  // The device goes through the gate that checks both files were read whole, never around it.
  TEST_ASSERT_TRUE(contains(src, "planLedgerFold"));
  TEST_ASSERT_FALSE(contains(src, "planLedgerMigration("));
  TEST_ASSERT_TRUE(contains(src, "kLedgerProject"));
  // A legacy file is folded once, into the ledger when one exists, and then removed.
  TEST_ASSERT_TRUE(contains(src, "g_store.find(nimbus::orch::kLedgerProject, "
                                 "nimbus::orch::kLedgerName)"));
  // The one food log: files.append and artifact.save both ask before writing.
  TEST_ASSERT_TRUE(contains(src, "secondFoodLogRefusal(args.project, args.name, who.owner, who.ns)"));
  TEST_ASSERT_TRUE(contains(src, "secondFoodLogRefusal(project, name, who.owner, who.ns)"));
  // The header bootstrap (candidate 3b): an append creates a CSV only when the
  // store holds none, and only then must it carry the header row first.
  TEST_ASSERT_TRUE(contains(src, "const bool csvCreate = size == 0 && fileKindIsCsv(a.name);"));
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

// A migrated file IS the designed ledger: same place, same header, and every reader of
// the ledger takes its rows, a comma inside parentheses included (main's reader counted
// such a row as wider than the header and left it out of every total).
static void test_a_migrated_ledger_is_read_by_the_ledger_readers() {
  TEST_ASSERT_EQUAL_STRING(kFoodLedgerProject, kLedgerProject);
  TEST_ASSERT_EQUAL_STRING(kFoodLedgerOwnerName, kLedgerName);
  TEST_ASSERT_EQUAL_STRING(kFoodLedgerHeader, kLedgerHeader);
  const LedgerMigration m = planLedgerMigration(
      "2026-10-29 21:55,Boba tea (no sugar, honey tapioca),1 cup,300,1\n"
      "2026-10-29 12:30,lunch,Soup,300,260,14\n");
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL(2, m.folded);
  TEST_ASSERT_TRUE(m.out.find("(no sugar, honey tapioca)") != std::string::npos);
  CsvDayTally tally("kcal", "2026-10-29");
  RowSplitter rows([&](const std::string& row, bool complete) {
    tally.feed(row, complete);
    return true;
  });
  rows.push(m.out.data(), m.out.size());
  rows.end();
  TEST_ASSERT_TRUE_MESSAGE(tally.ok(), tally.whyNot().c_str());
  TEST_ASSERT_EQUAL(2, tally.total().rows);
  TEST_ASSERT_EQUAL(0, tally.total().unread);
  TEST_ASSERT_TRUE(tally.total().total > 559.9 && tally.total().total < 560.1);
  // A keyed correction of the paren row replaces it (the meal name the fold gave it).
  const CsvUpsert fix = planCsvUpsert(m.out, std::string(kLedgerHeader) + "\n2026-10-29,evening,Boba tea,250,1\n",
                                      "meal", "kcal", "2026-10-29");
  TEST_ASSERT_TRUE_MESSAGE(fix.ok, fix.err.c_str());
  TEST_ASSERT_EQUAL(1, fix.replaced);
  TEST_ASSERT_EQUAL(0, fix.appended);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_fixture_folds_to_the_final_figures);
  RUN_TEST(test_the_fold_stands_down_on_anything_not_read_whole);
  RUN_TEST(test_same_item_rows_collapse_to_the_last);
  RUN_TEST(test_the_last_generation_wins);
  RUN_TEST(test_distinct_items_of_one_meal_coexist);
  RUN_TEST(test_meal_from_the_time_of_day_and_never_guessed);
  RUN_TEST(test_the_designed_ledger_wins_its_keys);
  RUN_TEST(test_unmappable_rows_drop_and_count);
  RUN_TEST(test_a_comma_inside_parentheses_stays_in_the_item);
  RUN_TEST(test_a_quoted_item_keeps_its_comma_and_its_columns);
  RUN_TEST(test_an_unquoted_comma_never_shifts_the_figures);
  RUN_TEST(test_a_row_whose_protein_exceeds_its_kcal_drops_and_keeps_the_source);
  RUN_TEST(test_the_source_goes_only_when_every_data_row_folded);
  RUN_TEST(test_a_kept_source_folds_once_even_with_a_ledger);
  RUN_TEST(test_only_the_owners_legacy_log_folds);
  RUN_TEST(test_the_fold_checks_the_owner_and_keeps_an_unfolded_source);
  RUN_TEST(test_a_migrated_ledger_is_read_by_the_ledger_readers);
  RUN_TEST(test_a_headered_legacy_file_folds_into_an_existing_ledger);
  RUN_TEST(test_begin_wires_the_one_time_migration);
  return UNITY_END();
}
