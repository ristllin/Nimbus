// test_inturn_replay - lane RB-BGb part 2 (ruling 93 item 3): the in-turn replay budget is what
// the request has free, never under v5.1.2's figure for the head, and older rounds fold only when
// the request would otherwise not fit; a fold is named and reachable (results.get).
#include <unity.h>

#include <cstdlib>
#include <fstream>
#include <map>
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

// The property (ruling 93 item 3): no request carries less of the turn's tool traffic than
// v5.1.2's replay figure for its head, or all of it when there is less, unless the request had no
// room for one more part. Every head the rig drives, histories from empty to full.
// v5.1.2's own carry on the same turns, recorded once from rb-main 8db879b4c's engine files
// (INTURN_RECORD=1 writes it; evidence/p2-v512-record.log): "<model> <q> <request> <parts>".
static const char kV512Carry[] = "test/golden/inturn_replay_v512.txt";

static std::map<std::string, int> v512Carry() {
  std::map<std::string, int> m;
  std::ifstream f(kV512Carry);
  std::string model;
  int q, b, parts;
  while (f >> model >> q >> b >> parts) m[model + " " + std::to_string(q) + " " + std::to_string(b)] = parts;
  return m;
}

static void test_no_request_carries_less_than_the_v512_replay() {
  const char* rec = std::getenv("INTURN_RECORD");
  const bool record = rec && std::string(rec) == "1";
  std::ofstream out;
  if (record) out.open(kV512Carry);
  const std::map<std::string, int> old = v512Carry();
  TEST_ASSERT_TRUE_MESSAGE(record || !old.empty(), kV512Carry);
  const Head heads[] = {{"anthropic", "claude-sonnet-5-5"}, {"anthropic", "claude-haiku-5-5"},
                        {"cumulo", "anthropic/claude-sonnet-5-5"}, {"cumulo", "anthropic/claude-haiku-5-5"}};
  int requests = 0, short_ = 0, partsNow = 0, partsThen = 0;
  nimbus::orch::setRoutedBudgetLiftForTest(0);
  for (const Head& h : heads)
    for (int q = 0; q <= 4; q++) {
      const nimbus::orch::ContextBudget cb = nimbus::orch::headContextBudget(h.host, h.model, true, {});
      const size_t v512 = cb.replayBytes;   // the wire's fixed replay before this lane
      const size_t budget = nimbus::orch::turnBudgetFor(h.host, h.model, true).wireBodyBytes;
      Rig rig(h, cb.historyMaxBytes * (size_t)q / 4);
      const Result r = rig.run();
      for (size_t b = 1; b < r.bodies.size(); b++) {
        requests++;
        const size_t produced = std::min<size_t>(b, kParts) * kPartBytes;
        const size_t carried = partsIn(r.bodies[b]).size() * kPartBytes;
        const std::string what = std::string(h.model) + " history " + std::to_string(q) + "/4 request " +
                                 std::to_string(b) + ": carried " + std::to_string(carried) + " of " +
                                 std::to_string(produced) + " (v5.1.2 replay " + std::to_string(v512) + ")";
        TEST_ASSERT_TRUE_MESSAGE(r.bodies[b].size() <= budget, what.c_str());
        const int parts = (int)partsIn(r.bodies[b]).size();
        const std::string key = std::string(h.model) + " " + std::to_string(q) + " " + std::to_string(b);
        if (record) {
          out << key << " " << parts << "\n";
          continue;
        }
        const auto was = old.find(key);
        TEST_ASSERT_TRUE_MESSAGE(was != old.end(), (key + " missing from the v5.1.2 record").c_str());
        partsNow += parts;
        partsThen += was->second;
        TEST_ASSERT_TRUE_MESSAGE(parts >= was->second, (what + ", v5.1.2 carried " + std::to_string(was->second) + " parts").c_str());
        if (carried >= std::min(produced, v512) - kPartBytes) continue;   // whole parts: one part's grain
        short_++;
        TEST_ASSERT_TRUE_MESSAGE(r.bodies[b].size() + kPartBytes > budget, what.c_str());
      }
    }
  char line[256];
  std::snprintf(line, sizeof line,
                "%d requests, %d under the v5.1.2 replay (each with no room for one more part); parts carried %d "
                "against v5.1.2's %d",
                requests, short_, partsNow, partsThen);
  TEST_MESSAGE(line);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_head_with_room_keeps_every_part_until_the_answer);
  RUN_TEST(test_a_floor_head_folds_and_says_so);
  RUN_TEST(test_no_request_carries_less_than_the_v512_replay);
  return UNITY_END();
}
