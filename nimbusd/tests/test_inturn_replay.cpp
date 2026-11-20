// test_inturn_replay - lane RB-BGb part 2 (ruling 93 item 3) on the engine and the Anthropic wire
// nimbusd links: a turn that reads a 40 KB file in parts on claude-sonnet-5-5 with a half-full
// history keeps every part in the request until the answer; the same turn on a floor head
// (cumulo/anthropic-sonnet at 85,000 B) folds, names the folded rounds and how to read them back
// (results.get). The case is shared with the native suite (test/support/inturn_replay_case.h).
// Offline: scripted answers, no keys.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../test/support/inturn_replay_case.h"
#include "test_util.h"

using namespace inturn_replay;

int main() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
  ndtest::Ctx t;
  orch::installBudgetLink(orch::BudgetLink::Device);   // the floor head is a device's routed head
  {
    const size_t half = orch::headContextBudget("anthropic", "claude-sonnet-5-5", true, {}).historyMaxBytes / 2;
    Rig rig({"anthropic", "claude-sonnet-5-5"}, half);
    const Result r = rig.run();
    t.eqi(r.dispatched, kParts, "room: every part was read");
    for (size_t b = 1; b < r.bodies.size(); b++)
      t.eqi((long)partsIn(r.bodies[b]).size(), (long)std::min<size_t>(b, kParts),
            "room: request " + std::to_string(b) + " carries every part read so far");
  }
  {
    orch::setRoutedBudgetLiftForTest(0);
    const size_t half =
        orch::headContextBudget("cumulo", "anthropic/claude-sonnet-5-5", true, {}).historyMaxBytes / 2;
    Rig rig({"cumulo", "anthropic/claude-sonnet-5-5"}, half);
    const Result r = rig.run();
    t.eqi(r.dispatched, kParts, "floor: every part was read");
    const std::string& last = r.bodies.back();
    t.ok(last.size() <= orch::kLegacyWireBodyBytes, "floor: the answer's request fits 85,000 B");
    t.ok((int)partsIn(last).size() < kParts, "floor: older parts fold");
    t.ok(last.find("results.get") != std::string::npos, "floor: a folded result is named with results.get");
    orch::setRoutedBudgetLiftForTest(-1);
  }
  std::printf("\n%s: %d checks, %d failures\n", "test_inturn_replay", t.checks, t.failures);
  return t.failures ? 1 : 0;
}
