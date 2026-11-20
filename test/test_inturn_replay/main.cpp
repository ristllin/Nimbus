// test_inturn_replay - lane RB-BGb part 2 (ruling 93 item 3): the in-turn replay budget is what
// the request has free, never under v5.1.2's figure for the head, and older rounds fold only when
// the request would otherwise not fit; a fold is named and reachable (results.get).
#include <unity.h>

#include <string>

#include "../support/inturn_replay_case.h"

using namespace inturn_replay;
using harness_test::LogCapture;

void setUp() { LogCapture::install(); }
void tearDown() { nimbus::orch::setRoutedBudgetLiftForTest(-1); }

static std::string partsText(const std::vector<int>& in) {
  std::string s;
  for (int i : in) s += std::to_string(i) + " ";
  return s;
}

// Sonnet direct (250,000 B body) with a history about half its max: five 7,800 B parts (about
// 15,600 tokens in all) fit with room to spare, so every part rides every later request, the
// answer's included.
static void test_a_head_with_room_keeps_every_part_until_the_answer() {
  const size_t half = nimbus::orch::headContextBudget("anthropic", "claude-sonnet-5-5", true, {}).historyMaxBytes / 2;
  Rig rig({"anthropic", "claude-sonnet-5-5"}, half);
  const Result r = rig.run();
  TEST_ASSERT_EQUAL_MESSAGE(kParts, r.dispatched, "every part was read");
  TEST_ASSERT_TRUE_MESSAGE(r.bodies.size() >= (size_t)kParts + 1, "a request after the last part");
  for (size_t b = 1; b < r.bodies.size(); b++) {
    const std::vector<int> in = partsIn(r.bodies[b]);
    const int want = (int)std::min<size_t>(b, kParts);
    TEST_ASSERT_EQUAL_MESSAGE(want, (int)in.size(),
                              ("request " + std::to_string(b) + " carries parts " + partsText(in)).c_str());
  }
  TEST_ASSERT_FALSE_MESSAGE(LogCapture::contains("folded"), "nothing was folded");
}

// The same turn on a floor head (cumulo/anthropic-sonnet, 85,000 B): the parts do not all fit, so
// older rounds fold, and the request that goes says so and names how to read each folded result
// back.
static void test_a_floor_head_folds_and_says_so() {
  nimbus::orch::setRoutedBudgetLiftForTest(0);
  const size_t half =
      nimbus::orch::headContextBudget("cumulo", "anthropic/claude-sonnet-5-5", true, {}).historyMaxBytes / 2;
  Rig rig({"cumulo", "anthropic/claude-sonnet-5-5"}, half);
  const Result r = rig.run();
  TEST_ASSERT_EQUAL_MESSAGE(kParts, r.dispatched, "every part was read");
  const std::string& last = r.bodies.back();
  TEST_ASSERT_TRUE_MESSAGE(last.size() <= nimbus::orch::kLegacyWireBodyBytes, std::to_string(last.size()).c_str());
  const std::vector<int> in = partsIn(last);
  TEST_ASSERT_TRUE_MESSAGE((int)in.size() < kParts, partsText(in).c_str());
  TEST_ASSERT_TRUE_MESSAGE(last.find("results.get") != std::string::npos, "a folded result is named with results.get");
  for (int i = 0; i < kParts; i++) {
    bool carried = false;
    for (int j : in) carried = carried || j == i;
    if (carried) continue;
    // Each folded part's round is named in the request (its call, offset and size).
    const std::string offset = "offset\\\":" + std::to_string(i * kPartBytes);
    TEST_ASSERT_TRUE_MESSAGE(last.find(offset) != std::string::npos, ("part " + std::to_string(i) + " is named").c_str());
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_head_with_room_keeps_every_part_until_the_answer);
  RUN_TEST(test_a_floor_head_folds_and_says_so);
  return UNITY_END();
}
