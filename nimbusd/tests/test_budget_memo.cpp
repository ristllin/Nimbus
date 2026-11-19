// test_budget_memo - lane RB-BGb (ruling 83 item 9 (b)) on a Virtual Nimbus: one dense
// bytes-per-token reading (the wire's noteObservedRatio after a 2xx answer) tightens only the
// window bound. On v5.1.2 one reading at the memo's 1.5 B floor on claude-haiku-5-5 tightened its
// price-tier bound for the rest of the process: 180,000 B to 135,000 B and the history to the
// v5.1.1 window. Offline, no provider.
#include <cstdio>
#include <string>

#include "test_util.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/request_budget.h"

namespace orch = nimbus::orch;

int main() {
  ndtest::Ctx t;
  orch::installBudgetLink(orch::BudgetLink::Server);
  orch::clearObservedRatios();
  const orch::ContextBudget before = orch::headContextBudget("anthropic", "claude-haiku-5-5", true, {});
  t.eqi((long)before.historyMaxBytes, 62080, "Haiku's v5.1.2 history max on a server");
  orch::noteObservedRatio("anthropic", "claude-haiku-5-5", 150000, 100000);   // 1.5 B a token
  t.ok(orch::observedRatioX100("anthropic", "claude-haiku-5-5") == 150, "the reading is kept");
  const orch::RequestBudget rb = orch::requestBudgetFor("anthropic", "claude-haiku-5-5", false);
  t.eqi((long)rb.wireBodyBytes, 180000, "the body keeps its price tier at the safe ratio");
  const orch::ContextBudget after = orch::headContextBudget("anthropic", "claude-haiku-5-5", true, {});
  t.eqi((long)after.historyMaxBytes, 62080, "the history max stays v5.1.2's");
  t.eqi((long)after.historyKeepBytes, (long)before.historyKeepBytes, "and the keep");
  t.ok(!after.floorBranch, "never the v5.1.1 floor branch");
  // Counter-case: a head whose window is the limit still tightens (a larger request would not fit).
  orch::clearObservedRatios();
  const uint32_t wide = orch::requestBudgetFor("openai", "gpt-4o", false).wireBodyBytes;
  orch::noteObservedRatio("openai", "gpt-4o", 150000, 100000);
  const orch::RequestBudget small = orch::requestBudgetFor("openai", "gpt-4o", false);
  t.ok(small.wireBodyBytes < wide && small.bound == orch::BudgetBound::Window, "a window-bound head still tightens");
  orch::clearObservedRatios();
  std::printf("\n%s: %d checks, %d failures\n", "test_budget_memo", t.checks, t.failures);
  return t.failures ? 1 : 0;
}
