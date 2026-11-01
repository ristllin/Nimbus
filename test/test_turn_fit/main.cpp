#include <unity.h>

#include <string>

#include "../support/fake_platform.h"
#include "nimbus/harness/compose.h"
#include "nimbus/harness/head_tools.h"
#include "nimbus/harness/turn_fit.h"

// The ladder's order: each rung runs only when the rungs before it could not give enough,
// and what goes is said.
using agent::ComposeInputs;
using agent::HistMsg;
using agent::TurnFit;
using nimbus::orch::FitScope;

void setUp() { harness_test::LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static ComposeInputs baseInputs() {
  ComposeInputs ci;
  ci.devName = "Nimbus";
  ci.skillsShelf = "SHELF " + std::string(2000, 's');
  ci.standingPrefs = "PREFS " + std::string(500, 'p');
  ci.recalled = {"[90%] RECALLED " + std::string(1500, 'm')};
  ci.foodPlaybook = "PLAYBOOK " + std::string(1500, 'f');
  ci.history.push_back({true, std::string(agent::kSummaryUserHead) + "SUMMARY"});
  ci.history.push_back({false, agent::kSummaryAck});
  for (int i = 0; i < 6; i++) {
    ci.history.push_back({true, "Q" + std::to_string(i) + " " + std::string(1000, 'q')});
    ci.history.push_back({false, "A" + std::to_string(i) + " " + std::string(1000, 'a')});
  }
  return ci;
}

static const char* const kInput =
    "[MEMORY RESULTS] (background)\nMEMBODY\n[FRESH RESULTS] (sub-agents)\n- job1: RESULTHEAD";

static std::string inputWithResults() {
  return std::string(kInput) + std::string(6000, 'r') + " RESULTTAIL\n[SYSTEM]\nsynthesize";
}

static bool hasQ(const ComposeInputs& ci, int i) {
  for (const auto& m : ci.history)
    if (m.text.rfind("Q" + std::to_string(i) + " ", 0) == 0) return true;
  return false;
}

static void test_older_history_goes_first_and_alone() {
  ComposeInputs ci = baseInputs();
  TurnFit fit(ci, inputWithResults(), nullptr);
  const size_t freed = fit.shrink(2500, FitScope::OlderHistory);
  TEST_ASSERT_TRUE(freed >= 2500);
  TEST_ASSERT_FALSE(hasQ(ci, 0));
  TEST_ASSERT_FALSE(hasQ(ci, 1));
  TEST_ASSERT_TRUE(hasQ(ci, 2));
  TEST_ASSERT_TRUE(hasQ(ci, 5));                                       // the newest stays
  TEST_ASSERT_TRUE(ci.history[0].text.find("SUMMARY") != std::string::npos);   // the summary stays
  TEST_ASSERT_FALSE(ci.recalled.empty());                               // nothing else moved
  TEST_ASSERT_FALSE(ci.foodPlaybook.empty());
  TEST_ASSERT_TRUE(fit.inputs().find("RESULTTAIL") != std::string::npos);
  TEST_ASSERT_TRUE(fit.inputs().rfind("[LEFT OUT TO FIT THIS REQUEST] 4 older conversation message(s)", 0) == 0);
  TEST_ASSERT_TRUE(fit.ownerLine().empty());   // older history alone is not the owner's news
  TEST_ASSERT_TRUE(harness_test::LogCapture::contains("turn fit: left out 4 older"));
  TEST_ASSERT_TRUE(fit.instructions().find("Q0 ") == std::string::npos);
  TEST_ASSERT_TRUE(fit.instructions().find("Q5 ") != std::string::npos);
}

static void test_older_history_rung_runs_out() {
  ComposeInputs ci = baseInputs();
  TurnFit fit(ci, inputWithResults(), nullptr);
  TEST_ASSERT_TRUE(fit.shrink(1000000, FitScope::OlderHistory) > 0);
  TEST_ASSERT_TRUE(hasQ(ci, 5));   // still kept
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)fit.shrink(1000, FitScope::OlderHistory));   // nothing left on the rung
}

// What a tool can read back goes before anything it cannot.
static void test_memories_and_playbook_go_before_results() {
  ComposeInputs ci = baseInputs();
  ci.speakerPresent = true;
  TurnFit fit(ci, inputWithResults(), nullptr);
  TEST_ASSERT_TRUE(fit.shrink(1000, FitScope::Ladder) >= 1000);
  TEST_ASSERT_TRUE(ci.recalled.empty());
  TEST_ASSERT_FALSE(ci.foodPlaybook.empty());
  TEST_ASSERT_TRUE(fit.inputs().find("cut to fit") == std::string::npos);
  TEST_ASSERT_TRUE(hasQ(ci, 5));
  TEST_ASSERT_TRUE(fit.ownerLine().empty());
}

// An owner's turn keeps the last exchange and clips the results, both ends kept.
static void test_owner_turn_clips_results_before_the_newest_turn() {
  ComposeInputs ci = baseInputs();
  ci.speakerPresent = true;
  TurnFit fit(ci, inputWithResults(), nullptr);
  TEST_ASSERT_TRUE(fit.shrink(6000, FitScope::Ladder) >= 6000);
  const std::string in = fit.inputs();
  TEST_ASSERT_TRUE(in.find("RESULTHEAD") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("RESULTTAIL") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("cut to fit this request") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("[SYSTEM]\nsynthesize") != std::string::npos);
  TEST_ASSERT_TRUE(hasQ(ci, 5));
  TEST_ASSERT_TRUE(fit.ownerLine().find("part of the results") != std::string::npos);
}

// A synthesis exists for its results: the past turns go first, and that is no news to the owner.
static void test_synthesis_keeps_results_over_the_newest_turn() {
  ComposeInputs ci = baseInputs();
  ci.speakerPresent = false;
  TurnFit fit(ci, inputWithResults(), nullptr);
  TEST_ASSERT_TRUE(fit.shrink(6000, FitScope::Ladder) >= 6000);
  TEST_ASSERT_TRUE(fit.inputs().find("cut to fit") == std::string::npos);
  TEST_ASSERT_FALSE(hasQ(ci, 0));
  TEST_ASSERT_TRUE(fit.ownerLine().empty());
}

// The wire budget's scope: one rung at a time, nothing past the first that had anything.
static void test_next_rung_stops_at_the_first_that_gives() {
  ComposeInputs ci = baseInputs();
  ci.speakerPresent = false;
  TurnFit fit(ci, inputWithResults(), nullptr);
  TEST_ASSERT_TRUE(fit.shrink(1000000, FitScope::NextRung) > 0);
  TEST_ASSERT_TRUE(ci.recalled.empty());
  TEST_ASSERT_FALSE(ci.foodPlaybook.empty());
  TEST_ASSERT_TRUE(fit.shrink(1000000, FitScope::NextRung) > 0);
  TEST_ASSERT_TRUE(ci.foodPlaybook.empty());
  TEST_ASSERT_TRUE(hasQ(ci, 5));
  TEST_ASSERT_TRUE(fit.shrink(1000000, FitScope::NextRung) > 0);
  TEST_ASSERT_TRUE(ci.history.empty());
  TEST_ASSERT_TRUE(fit.inputs().find("cut to fit") == std::string::npos);
}

static void test_the_ladder_order_to_the_end() {
  ComposeInputs ci = baseInputs();
  ci.speakerPresent = true;
  agent::HeadTools tools;
  tools.specs.push_back({"memory_search", std::string(40000, 'd'), "{}"});
  const std::string huge = "OWNERHEAD " + std::string(40000, 'o') + " OWNERTAIL";
  TurnFit fit(ci, huge, &tools);
  // A small ask: the memories go, and history stays.
  TEST_ASSERT_TRUE(fit.shrink(1000, FitScope::Ladder) >= 1000);
  TEST_ASSERT_TRUE(ci.recalled.empty());
  TEST_ASSERT_FALSE(ci.foodPlaybook.empty());
  TEST_ASSERT_TRUE(hasQ(ci, 5));
  // More: the playbook, then the past turns (this input has no results block).
  TEST_ASSERT_TRUE(fit.shrink(3000, FitScope::Ladder) >= 3000);
  TEST_ASSERT_TRUE(ci.foodPlaybook.empty());
  TEST_ASSERT_FALSE(tools.specs.empty());
  // Past turns go whole: the history still opens on a user message and alternates.
  TEST_ASSERT_TRUE(ci.history.size() % 2 == 0);
  for (size_t i = 0; i < ci.history.size(); i++) TEST_ASSERT_EQUAL(i % 2 == 0, ci.history[i].user);
  // Everything: the input keeps both ends, the tools go, the owner's rules go last.
  TEST_ASSERT_TRUE(fit.shrink(1000000, FitScope::Ladder) > 0);
  TEST_ASSERT_TRUE(ci.history.empty());
  TEST_ASSERT_TRUE(tools.specs.empty());
  TEST_ASSERT_TRUE(ci.standingPrefs.empty());
  const std::string in = fit.inputs();
  TEST_ASSERT_TRUE(in.find("OWNERHEAD") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("OWNERTAIL") != std::string::npos);
  TEST_ASSERT_TRUE(in.size() < 8000);
  TEST_ASSERT_TRUE(in.find("Your tools did not fit") != std::string::npos);
  TEST_ASSERT_TRUE(fit.ownerLine().find("its tools") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)fit.shrink(1000, FitScope::Ladder));   // the ladder ends; it never spins
}

static void test_clips_stay_valid_utf8() {
  ComposeInputs ci = baseInputs();
  ci.recalled.clear();
  ci.foodPlaybook.clear();
  ci.speakerPresent = true;
  std::string input = "[FRESH RESULTS]\n";
  for (int i = 0; i < 3000; i++) input += "\xE2\x82\xAC";
  TurnFit fit(ci, input, nullptr);
  TEST_ASSERT_TRUE(fit.shrink(4000, FitScope::Ladder) > 0);
  const std::string in = fit.inputs();
  for (size_t i = 0; i < in.size();) {
    const unsigned char c = (unsigned char)in[i];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    TEST_ASSERT_TRUE(n > 0 && i + n <= in.size());
    for (size_t k = 1; k < n; k++) TEST_ASSERT_TRUE(((unsigned char)in[i + k] & 0xC0) == 0x80);
    i += n;
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_older_history_goes_first_and_alone);
  RUN_TEST(test_older_history_rung_runs_out);
  RUN_TEST(test_memories_and_playbook_go_before_results);
  RUN_TEST(test_owner_turn_clips_results_before_the_newest_turn);
  RUN_TEST(test_synthesis_keeps_results_over_the_newest_turn);
  RUN_TEST(test_next_rung_stops_at_the_first_that_gives);
  RUN_TEST(test_the_ladder_order_to_the_end);
  RUN_TEST(test_clips_stay_valid_utf8);
  return UNITY_END();
}
