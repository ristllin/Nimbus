#include <unity.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "nimbus/orch/food_ledger.h"
#include "nimbus/utf8.h"

// The daily food ledger the prompt carries (food_ledger.h): the canonical names,
// today's rows and totals read the same way the day total reads them, the bounded
// block, the fold's view and the food-message classifier.

using namespace nimbus::orch;

// Unity's double support is off in this build: compare figures exactly as numbers.
#define EQ_NUM(want, got) TEST_ASSERT_TRUE_MESSAGE((double)(want) == (double)(got), #got)

namespace {

const char kHead[] = "date,meal,items,kcal,protein_g";
const LogDay kDay{"2026-10-26", "GMT"};

FoodLedger scan(const std::string& text, bool exists = true) {
  return scanFoodLedger(text, exists, kDay, kFoodLedgerProject, kFoodLedgerOwnerName);
}

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A keyed write the way the playbook makes it (date + meal, kcal summed).
void logRows(std::string& file, const std::string& rows, const std::string& today = "2026-10-26") {
  const CsvUpsert up = planCsvUpsert(file, std::string(kHead) + "\n" + rows, "meal", "kcal", today);
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  if (up.changed) file = up.out;
}

}  // namespace

void test_canonical_names_per_person() {
  TEST_ASSERT_EQUAL_STRING("food-log.csv", foodLedgerName(true, "6098532481").c_str());
  TEST_ASSERT_EQUAL_STRING("food-log.csv", foodLedgerName(true, "").c_str());
  TEST_ASSERT_EQUAL_STRING("food-log-6098532481.csv", foodLedgerName(false, "6098532481").c_str());
  TEST_ASSERT_EQUAL_STRING("food-log-web1x.csv", foodLedgerName(false, "web:1/x").c_str());
  TEST_ASSERT_EQUAL_STRING("", foodLedgerName(false, ":/-").c_str());
  // A long chat id is bounded: the name stays a valid store segment length.
  TEST_ASSERT_TRUE(foodLedgerName(false, std::string(80, '7')).size() <= 9 + 24 + 4);
}

void test_today_rows_and_totals_only() {
  std::string file;
  logRows(file, "2026-10-25,dinner,pasta,800,30\n", "2026-10-25");
  logRows(file, "2026-10-26,breakfast,2 eggs + toast,450,25\n2026-10-26,lunch,salmon sandwich,540,34\n");
  const FoodLedger l = scan(file);
  TEST_ASSERT_TRUE(l.exists);
  TEST_ASSERT_EQUAL_INT(2, l.rowsToday);
  TEST_ASSERT_EQUAL_INT(2, (int)l.rows.size());
  TEST_ASSERT_TRUE(l.hasKcal && l.hasProtein);
  EQ_NUM(990, l.kcal);
  EQ_NUM(59, l.protein);
  TEST_ASSERT_EQUAL_STRING(kHead, l.header.c_str());
  for (const std::string& r : l.rows) TEST_ASSERT_TRUE_MESSAGE(r.rfind("2026-10-26,", 0) == 0, r.c_str());
  const std::string block = renderLedgerBlock(l, false);
  TEST_ASSERT_TRUE_MESSAGE(has(block, "[TODAY'S FOOD LEDGER - food/food-log.csv, 2026-10-26 (GMT)."), block.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(block, "Today so far (2026-10-26): 990 kcal from 2 rows, 59 g protein."),
                           block.c_str());
  TEST_ASSERT_FALSE_MESSAGE(has(block, "pasta"), "yesterday's dinner is not today's ledger");
}

void test_a_correction_never_counts_twice() {
  std::string file;
  logRows(file, "2026-10-26,lunch,brunch plate,865,70\n");
  logRows(file, "2026-10-26,lunch,brunch plate (no oil),745,70\n");   // the owner's correction
  const FoodLedger l = scan(file);
  TEST_ASSERT_EQUAL_INT(1, l.rowsToday);
  EQ_NUM(745, l.kcal);
  TEST_ASSERT_TRUE(has(renderLedgerBlock(l, false), "no oil"));
}

// The rows shown and the total use one reading: over random logs the kcal total
// equals the files tool's day total (csvDayTotal) and every shown row is today's.
void test_property_matches_the_day_total() {
  srand(1026);
  const char* const days[] = {"2026-10-24", "2026-10-25", "2026-10-26"};
  const char* const meals[] = {"breakfast", "lunch", "dinner", "snack-1100", "snack-1600"};
  for (int run = 0; run < 200; run++) {
    std::string file;
    const int writes = 1 + rand() % 12;
    for (int w = 0; w < writes; w++) {
      const std::string day = days[rand() % 3];
      const std::string row = day + "," + meals[rand() % 5] + ",item " + std::to_string(w) + "," +
                              std::to_string(50 + rand() % 900) + "," + std::to_string(rand() % 60) + "\n";
      logRows(file, row, "2026-10-26");
    }
    if (rand() % 4 == 0) file += "# a comment row 2026-10-26,x,y,999,9\n";
    const FoodLedger l = scan(file);
    double want = -1;
    TEST_ASSERT_TRUE(csvDayTotal(file, "kcal", "2026-10-26", want));
    EQ_NUM(want, l.kcal);
    TEST_ASSERT_EQUAL_INT(l.kcalRows, l.rowsToday);
    for (const std::string& r : l.rows) TEST_ASSERT_TRUE_MESSAGE(r.rfind("2026-10-26,", 0) == 0, r.c_str());
  }
}

// ---- known and unknown protein, targets, the second log (the week of 28 Oct) --------

static const char k1Nov[] =
    "date,meal,items,kcal,protein_g\n"
    "2026-10-26,drink,cappuccino,66,3.3\n"
    "2026-10-26,breakfast,berries + yogurt,196,17.5\n"
    "2026-10-26,lunch,eggs + bread,517,27.1\n"
    "2026-10-26,drink2,boba tea,90,0.5\n"
    "2026-10-26,lunch2,steak + mash + sauce,923,\n";

void test_a_blank_protein_is_unknown_not_zero() {
  FoodLedger l = scan(k1Nov);
  EQ_NUM(1792, l.kcal);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 48.4, (float)l.protein);
  TEST_ASSERT_EQUAL_INT(4, l.proteinRows);
  TEST_ASSERT_EQUAL_INT(1, l.proteinUnknown);
  const std::string block = renderLedgerBlock(l, true);
  TEST_ASSERT_TRUE_MESSAGE(has(block, "protein: 48.4 g known from 4 rows, 1 row with no protein figure"), block.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(block, "unknown (at least 48.4 g)"), block.c_str());
  TEST_ASSERT_FALSE_MESSAGE(has(block, "1792 kcal from 5 rows, 48.4 g protein"), "a bare total hides the unknown row");
  // With a target the device does the sum, and "left" is at most, never a number to chase.
  l.targets.protein = 150;
  l.targets.proteinSource = "plan.md";
  l.targets.kcal = 2400;
  l.targets.kcalSource = "plan.md";
  const std::string left = renderLedgerBlock(l, true);
  TEST_ASSERT_TRUE_MESSAGE(has(left, "608 kcal left of 2400"), left.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(left, "protein: at most 101.6 g left of 150"), left.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(left, "less whatever the 1 row with no figure held"), left.c_str());
  // All known: a plain figure.
  FoodLedger known = scan("date,meal,items,kcal,protein_g\n2026-10-26,lunch,x,500,40\n");
  known.targets = l.targets;
  const std::string plain = renderLedgerBlock(known, false);
  TEST_ASSERT_TRUE_MESSAGE(has(plain, "1900 kcal left of 2400"), plain.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(plain, "protein: 110 g left of 150"), plain.c_str());
  TEST_ASSERT_FALSE(has(plain, "at most"));
  // Over the target, the sum says over.
  known.targets.kcal = 400;
  TEST_ASSERT_TRUE(has(renderLedgerBlock(known, false), "100 kcal over 400"));
}

void test_a_kcal_total_that_cannot_be_read_is_no_figure() {
  const FoodLedger l = scan("date,meal,items,kcal,protein_g\n2026-10-26,lunch,x,500,40\n2026-10-26,dinner,y,about 900,40\n");
  const std::string block = renderLedgerBlock(l, true);
  TEST_ASSERT_TRUE_MESSAGE(has(block, "kcal not known (500 counted from 1 row, 1 row unreadable)"), block.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(block, "state no day total until then"), block.c_str());
}

void test_targets_come_from_a_file_with_a_source() {
  const FoodTargets t = scanFoodTargets(
      "date,metric,target,source\n"
      "2026-10-20,kcal,2500,an old plan\n"
      "2026-10-26,kcal,2400,roy-weight-loss/plan.md\n"
      "2026-10-26,protein_g,150,roy-weight-loss/plan.md\n"
      "2026-10-26,fibre_g,30,roy-weight-loss/plan.md\n");
  EQ_NUM(2400, t.kcal);
  TEST_ASSERT_EQUAL_STRING("roy-weight-loss/plan.md", t.kcalSource.c_str());
  EQ_NUM(150, t.protein);
  // No source, no target; junk never becomes one.
  const FoodTargets none = scanFoodTargets("date,metric,target,source\n2026-10-26,protein_g,250,\n2026-10-26,kcal,lots,plan\n");
  EQ_NUM(0, none.protein);
  EQ_NUM(0, none.kcal);
  EQ_NUM(0, scanFoodTargets("").kcal);
  EQ_NUM(0, scanFoodTargets("metric\nkcal\n").kcal);
  // The member's file sits beside the member's ledger, and only that member may write it.
  TEST_ASSERT_EQUAL_STRING("food-targets.csv", foodTargetsName(true, "42").c_str());
  TEST_ASSERT_EQUAL_STRING("food-targets-42.csv", foodTargetsName(false, "42").c_str());
  TEST_ASSERT_TRUE(ledgerNameWritable("food", "food-targets-42.csv", "chat:42", false));
  TEST_ASSERT_FALSE(ledgerNameWritable("food", "food-targets.csv", "chat:42", false));
  TEST_ASSERT_FALSE(ledgerNameWritable("FOOD", "FOOD-TARGETS-7.CSV", "chat:42", false));
}

void test_no_target_stored_sends_the_head_to_a_source() {
  FoodLedger l = scan(k1Nov);
  l.planFiles = {"weight/my-plan.md"};
  std::string block = renderLedgerBlock(l, true);
  TEST_ASSERT_TRUE_MESSAGE(has(block, "[FOOD TARGETS] None is stored."), block.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(block, "files.read weight/my-plan.md"), block.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(block, "food-targets.csv"), block.c_str());
  // The same on the day's first food turn, and not on a turn that is not about food.
  FoodLedger empty = scan(std::string(kHead) + "\n2026-10-25,lunch,x,1,1\n");
  TEST_ASSERT_TRUE(has(renderLedgerBlock(empty, true), "[FOOD TARGETS] None is stored."));
  TEST_ASSERT_FALSE(has(renderLedgerBlock(l, false), "[FOOD TARGETS]"));
  TEST_ASSERT_TRUE(renderLedgerBlock(empty, false).empty());
  // Stored targets end the prompt for a source.
  l.targets.kcal = 2400;
  l.targets.kcalSource = "plan";
  TEST_ASSERT_FALSE(has(renderLedgerBlock(l, true), "[FOOD TARGETS]"));
}

void test_plan_files_are_about_food_or_weight() {
  TEST_ASSERT_TRUE(looksLikeFoodPlan("roy-weight-loss", "roy-12-month-plan-2026-10-14.md"));
  TEST_ASSERT_TRUE(looksLikeFoodPlan("health", "meal_planning.md"));
  TEST_ASSERT_FALSE(looksLikeFoodPlan("work", "q4-plan.md"));
  TEST_ASSERT_FALSE(looksLikeFoodPlan("roy-weight-loss", "research-synthesis.md"));
}

// The second log: the class is every name that says "food log" outside the ledger's place.
void test_one_food_log() {
  const struct {
    const char* project;
    const char* name;
  } second[] = {{"nutrition", "meal_log.csv"}, {"meals", "log.csv"},          {"tracking", "food-log.csv"},
                {"x", "Calorie_Log.CSV"},      {"nutrition", "daily-log.csv"}, {"x", "kcal.csv"}};
  for (const auto& c : second) {
    const std::string why = secondFoodLogRefusal(c.project, c.name, true, "");
    TEST_ASSERT_TRUE_MESSAGE(has(why, "food/food-log.csv"), c.name);
    TEST_ASSERT_TRUE_MESSAGE(has(why, "nothing was written"), c.name);
  }
  // A member is sent to that member's ledger.
  TEST_ASSERT_TRUE(has(secondFoodLogRefusal("nutrition", "meal_log.csv", false, "chat:42"), "food/food-log-42.csv"));
  // The ledger's place, other logs, other kinds of file: untouched.
  TEST_ASSERT_TRUE(secondFoodLogRefusal("food", "food-log.csv", true, "").empty());
  TEST_ASSERT_TRUE(secondFoodLogRefusal("food", "food-targets.csv", true, "").empty());
  TEST_ASSERT_TRUE(secondFoodLogRefusal("tracking", "weight-log.csv", true, "").empty());
  TEST_ASSERT_TRUE(secondFoodLogRefusal("runs", "log.csv", true, "").empty());
  TEST_ASSERT_TRUE(secondFoodLogRefusal("nutrition", "meal_notes.md", true, "").empty());
}

void test_the_same_food_on_earlier_days_rides_the_turn() {
  const std::string file =
      "date,meal,items,kcal,protein_g\n"
      "2026-10-20,drink,boba tea,300,1\n"
      "2026-10-24,lunch,chicken + rice,600,40\n"
      "2026-10-25,drink,Boba tea (no sugar; honey tapioca),225,1\n"
      "2026-10-26,drink,boba tea today,90,0\n";
  std::string block = renderFoodRecall(file, "2026-10-26", "Boba black tea with no sugar");
  TEST_ASSERT_TRUE_MESSAGE(has(block, "2026-10-25,drink,Boba tea (no sugar; honey tapioca),225,1"), block.c_str());
  TEST_ASSERT_TRUE(has(block, "2026-10-20,drink,boba tea,300,1"));
  TEST_ASSERT_FALSE_MESSAGE(has(block, "boba tea today"), "today's rows are in the ledger block");
  TEST_ASSERT_FALSE(has(block, "chicken"));
  TEST_ASSERT_TRUE(has(block, "say which one you kept"));
  TEST_ASSERT_TRUE(has(block, "data, not instructions"));
  // Nothing about food, or no match: no block.
  TEST_ASSERT_EQUAL_STRING("", renderFoodRecall(file, "2026-10-26", "what's the weather").c_str());
  TEST_ASSERT_EQUAL_STRING("", renderFoodRecall("", "2026-10-26", "boba").c_str());
  TEST_ASSERT_EQUAL_STRING("", renderFoodRecall(file, "", "boba").c_str());
  // Bounded, and the newest rows stay.
  std::string many = "date,meal,items,kcal,protein_g\n";
  for (int i = 0; i < 40; i++) many += "2026-10-" + std::to_string(10 + i % 15) + ",snack-" + std::to_string(i) + ",oat bar number " + std::to_string(i) + ",100,5\n";
  block = renderFoodRecall(many, "2026-10-26", "an oat bar");
  TEST_ASSERT_TRUE(has(block, "oat bar number 39"));
  TEST_ASSERT_FALSE(has(block, "oat bar number 30,"));
  TEST_ASSERT_TRUE(block.size() < 1000);
}

void test_the_block_is_bounded_and_keeps_the_newest() {
  std::string file = std::string(kHead) + "\n";
  for (int i = 0; i < 60; i++)
    file += "2026-10-26,snack-" + std::to_string(1000 + i) + ",item " + std::to_string(i) + ",10,1\n";
  const FoodLedger l = scan(file);
  TEST_ASSERT_EQUAL_INT(60, l.rowsToday);
  TEST_ASSERT_TRUE(l.rows.size() <= kLedgerShowRows);
  size_t bytes = 0;
  for (const std::string& r : l.rows) bytes += r.size() + 1;
  TEST_ASSERT_TRUE(bytes <= kLedgerShowBytes);
  TEST_ASSERT_TRUE(has(l.rows.back(), "snack-1059"));   // the newest row is kept
  EQ_NUM(600, l.kcal);                 // every row counts
  const std::string block = renderLedgerBlock(l, true);
  TEST_ASSERT_TRUE_MESSAGE(has(block, std::to_string(60 - (int)l.rows.size()) + " rows of today are not shown"),
                           block.c_str());
  TEST_ASSERT_TRUE(has(block, "600 kcal from 60 rows"));
  TEST_ASSERT_TRUE(block.size() < 2200);
}

void test_rows_are_cleaned_and_cut_on_a_boundary() {
  std::string longItems;
  for (int i = 0; i < 80; i++) longItems += "\xC3\xA9";   // e-acute, 2 bytes each
  std::string file = std::string(kHead) + "\n2026-10-26,lunch,\"" + longItems + "\",500,20\n" +
                     "2026-10-26,dinner,soup\x01x,300,10\n";
  const FoodLedger l = scan(file);
  TEST_ASSERT_EQUAL_INT(2, (int)l.rows.size());
  for (const std::string& r : l.rows) {
    TEST_ASSERT_TRUE(r.size() <= kLedgerRowMax);
    TEST_ASSERT_TRUE(nimbus::utf8Valid(r));
    for (char c : r) TEST_ASSERT_TRUE((unsigned char)c >= 0x20);
  }
}

void test_no_rows_today_names_the_first_call_on_a_food_turn_only() {
  // No file yet: the canonical header and the call that creates it.
  const FoodLedger none = scan("", false);
  TEST_ASSERT_EQUAL_STRING("", renderLedgerBlock(none, false).c_str());
  const std::string b = renderLedgerBlock(none, true);
  TEST_ASSERT_TRUE_MESSAGE(has(b, "no rows yet today"), b.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(b, "\"project\":\"food\",\"name\":\"food-log.csv\",\"key\":\"meal\",\"sum\":\"kcal\""),
                           b.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(b, "date,meal,items,kcal,protein_g\\n2026-10-26,<meal>"), b.c_str());
  TEST_ASSERT_TRUE(has(b, "creates the file"));
  TEST_ASSERT_FALSE(has(b, "Today so far"));
  // An older ledger of another shape: the stored header is the one to send.
  const std::string legacy = "date,time,meal_id,meal_type,item,quantity,quantity_unit,kcal,protein_g\n"
                             "2026-10-25,08:00,b1,breakfast,oats,60,g,230,8\n";
  const std::string c = renderLedgerBlock(scan(legacy), true);
  TEST_ASSERT_TRUE_MESSAGE(has(c, "date,time,meal_id,meal_type,item,quantity,quantity_unit,kcal,protein_g\\n<a row for 2026-10-26"),
                           c.c_str());
  TEST_ASSERT_FALSE(has(c, "creates the file"));
  // Keyed by its own columns: a keyed write on a column the file lacks is refused.
  TEST_ASSERT_TRUE_MESSAGE(has(c, "\"key\":\"meal_id\",\"sum\":\"kcal\""), c.c_str());
  // No meal or kcal column to key by: the owner is told, not sent a call that fails.
  const std::string bare = renderLedgerBlock(scan("date,food,notes\n2026-10-25,oats,x\n"), true);
  TEST_ASSERT_TRUE_MESSAGE(has(bare, "no meal or kcal column to key by"), bare.c_str());
  TEST_ASSERT_FALSE(has(bare, "\"sum\""));   // the keyed call on the ledger; only the targets' own call may follow
}

void test_an_unreadable_ledger_never_shows_a_total() {
  FoodLedger l = scan(std::string(kHead) + "\n2026-10-26,lunch,x,500,20\n");
  l.error = "the card read failed";
  TEST_ASSERT_EQUAL_STRING("", renderLedgerBlock(l, false).c_str());
  const std::string b = renderLedgerBlock(l, true);
  TEST_ASSERT_TRUE_MESSAGE(has(b, "could not be read this turn (the card read failed)"), b.c_str());
  TEST_ASSERT_FALSE(has(b, "500"));
  TEST_ASSERT_EQUAL_STRING("", renderLedgerForFold(l).c_str());
}

void test_no_clock_no_ledger() {
  const FoodLedger l = scanFoodLedger(std::string(kHead) + "\n2026-10-26,lunch,x,500,20\n", true, LogDay{"", ""},
                                      kFoodLedgerProject, kFoodLedgerOwnerName);
  TEST_ASSERT_EQUAL_STRING("", renderLedgerBlock(l, true).c_str());
}

void test_legacy_columns_and_unread_rows() {
  const std::string file = "date,time,meal_id,meal_type,item,quantity,quantity_unit,kcal,protein_g,notes\n"
                           "2026-10-26,08:00,b1,breakfast,skyr,200,g,220,18,\n"
                           "2026-10-26,08:00,b1,breakfast,granola,40,g,~180,4,\n"
                           "2026-10-26,13:00,l1,lunch,sandwich,1,piece,540,34,\n";
  const FoodLedger l = scan(file);
  TEST_ASSERT_EQUAL_INT(3, l.rowsToday);
  EQ_NUM(760, l.kcal);
  TEST_ASSERT_EQUAL_INT(1, l.kcalUnread);
  EQ_NUM(56, l.protein);
  const std::string b = renderLedgerBlock(l, false);
  TEST_ASSERT_TRUE_MESSAGE(has(b, "1 row of today has no plain kcal number"), b.c_str());
  // No kcal column at all: rows still shown, no total invented.
  const FoodLedger n = scan("date,meal,items\n2026-10-26,lunch,soup\n");
  TEST_ASSERT_FALSE(n.hasKcal);
  TEST_ASSERT_EQUAL_INT(1, n.rowsToday);
  TEST_ASSERT_TRUE(has(renderLedgerBlock(n, false), "no kcal column"));
}

void test_the_fold_view_is_dated() {
  std::string file;
  logRows(file, "2026-10-26,breakfast,oats,350,12\n2026-10-26,lunch,chicken salad,520,45\n");
  const std::string f = renderLedgerForFold(scan(file));
  TEST_ASSERT_TRUE_MESSAGE(has(f, "[FOOD LEDGER - food/food-log.csv on 2026-10-26 (GMT)"), f.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(f, "Totals for 2026-10-26: 870 kcal from 2 rows, 57 g protein."), f.c_str());
  TEST_ASSERT_TRUE(has(f, "chicken salad"));
  TEST_ASSERT_EQUAL_STRING("", renderLedgerForFold(scan("", false)).c_str());
}

void test_day_row_sink_sees_every_row_of_the_day() {
  CsvDayTally t("kcal", "2026-10-26");
  std::vector<std::string> seen;
  t.setDayRowSink([&seen](const std::string& r) { seen.push_back(r); });
  for (const char* r : {kHead, "2026-10-25,dinner,x,100,1", "2026-10-26,lunch,y,200,2",
                        "#superseded: 2026-10-26,lunch,z,300,3", "2026-10-26,dinner,w,~50,1"})
    t.feed(r, true);
  TEST_ASSERT_EQUAL_INT(2, (int)seen.size());   // the counted row and the unread one; never a comment
  EQ_NUM(200, t.total().total);
  TEST_ASSERT_EQUAL_INT(1, t.total().unread);
}

void test_food_messages_are_recognised() {
  // The 26 Oct evening, as the owner wrote it.
  for (const char* m : {"2/3 chiinese pear 5-6 cherry tomatoes 1/5 of a bell pepper 1 carrot",
                        "2 eggs, 2 chicken breasts so maybe 250g? no butter for brunch. eggs with olive oil",
                        "give me the full list by meal, main components and kcal for each to see the calc",
                        "Breakfast: oats 60g", "how much protein left today?", "Had a 300ml latte",
                        "Dude... Tell me how much grams of pasta to max out the calories for today",
                        "Lets plan dinner"})
    TEST_ASSERT_TRUE_MESSAGE(looksFoodRelated(m), m);
  for (const char* m : {"here it is again", "brine", "I think you calc is off",
                        "How long should I rest between heavy sets?", "restart the device",
                        "is it raining at the office?", "the 250 GB card is full", "set a wakeup at 6",
                        "", "egged on by the team"})
    TEST_ASSERT_FALSE_MESSAGE(looksFoodRelated(m), m);
}

void test_food_intent_is_read_only_when_plain() {
  for (const char* m : {"Breakfast: porridge, 420 kcal", "I had a chicken wrap for lunch", "Lunch was soup",
                        "just ate an apple", "Snack: a protein yoghurt, 210 kcal and 20 g protein."})
    TEST_ASSERT_EQUAL_MESSAGE(int(FoodIntent::Eaten), int(foodIntentOf(m)), m);
  for (const char* m : {"Just tell me how much pasta and cheese to max out on calories for today.",
                        "Plan my dinner. About 1,000 kcal left.", "what's my total?", "give me the full list by meal"})
    TEST_ASSERT_EQUAL_MESSAGE(int(FoodIntent::Ask), int(foodIntentOf(m)), m);
  for (const char* m : {"You invented things I didn't say we have.", "That's not what I said, there is no rice"})
    TEST_ASSERT_EQUAL_MESSAGE(int(FoodIntent::Complaint), int(foodIntentOf(m)), m);
  // A meal reported in a complaint's words is still a meal: it has to be logged.
  for (const char* m : {"That's wrong, I had the pasta at lunch", "I don't have rice, I had pasta"})
    TEST_ASSERT_EQUAL_MESSAGE(int(FoodIntent::Eaten), int(foodIntentOf(m)), m);
  TEST_ASSERT_TRUE(renderFoodIntentHint(FoodIntent::Complaint).find("opens with one short sentence owning it") != std::string::npos);
  for (const char* m : {"brine", "I had pasta, how much is left?", "2 eggs, 2 chicken breasts so maybe 250g"})
    TEST_ASSERT_EQUAL_MESSAGE(int(FoodIntent::Unclear), int(foodIntentOf(m)), m);
  TEST_ASSERT_TRUE(renderFoodIntentHint(FoodIntent::Eaten).find("write its row with files.append") != std::string::npos);
  TEST_ASSERT_TRUE(renderFoodIntentHint(FoodIntent::Ask).find("write no row") != std::string::npos);
  // A plan only from foods the owner named (a Mistral head planned chicken and rice nobody had).
  TEST_ASSERT_TRUE(renderFoodIntentHint(FoodIntent::Ask).find("none named: ask what they have") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", renderFoodIntentHint(FoodIntent::Unclear).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_canonical_names_per_person);
  RUN_TEST(test_today_rows_and_totals_only);
  RUN_TEST(test_a_correction_never_counts_twice);
  RUN_TEST(test_property_matches_the_day_total);
  RUN_TEST(test_a_blank_protein_is_unknown_not_zero);
  RUN_TEST(test_a_kcal_total_that_cannot_be_read_is_no_figure);
  RUN_TEST(test_targets_come_from_a_file_with_a_source);
  RUN_TEST(test_no_target_stored_sends_the_head_to_a_source);
  RUN_TEST(test_plan_files_are_about_food_or_weight);
  RUN_TEST(test_one_food_log);
  RUN_TEST(test_the_same_food_on_earlier_days_rides_the_turn);
  RUN_TEST(test_the_block_is_bounded_and_keeps_the_newest);
  RUN_TEST(test_rows_are_cleaned_and_cut_on_a_boundary);
  RUN_TEST(test_no_rows_today_names_the_first_call_on_a_food_turn_only);
  RUN_TEST(test_an_unreadable_ledger_never_shows_a_total);
  RUN_TEST(test_no_clock_no_ledger);
  RUN_TEST(test_legacy_columns_and_unread_rows);
  RUN_TEST(test_the_fold_view_is_dated);
  RUN_TEST(test_day_row_sink_sees_every_row_of_the_day);
  RUN_TEST(test_food_messages_are_recognised);
  RUN_TEST(test_food_intent_is_read_only_when_plain);
  return UNITY_END();
}
