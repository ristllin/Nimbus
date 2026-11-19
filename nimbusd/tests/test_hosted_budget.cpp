// test_hosted_budget - the history profile and its governor on a Virtual Nimbus (lane RB-BG):
// the same file and rules as the device (lib/core budget_governor.h), the instance's spend
// ceiling as its facts. Offline:
//   * no file and a corrupt file read as the default profile (standard) and say so;
//   * a pinned profile survives a restart and is installed for every head's context;
//   * a month of overspend steps the instance down once three nights are observed, the
//     profile every head derives under follows, and the step's line is returned for the web
//     chat; the same night twice does nothing; a pinned profile is never moved;
//   * the instance's 20 USD default ceiling is read from SpendCeilingVn's state, never written.
#include <cstdio>
#include <string>

#include "hosted_budget.h"
#include "spend_ceiling_vn.h"
#include "test_util.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/loops.h"

using namespace nimbusd;
namespace orch = nimbus::orch;

namespace {

// The spend ceiling's state in the device's shape (SpendCeilingVn::stateJson).
std::string stateJson(uint64_t spentCents, uint32_t month, bool active = true, uint64_t limitCents = 2000) {
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "{\"limitCents\":%llu,\"spentCents\":%llu,\"frozen\":false,\"ownerSet\":false,\"active\":%s,"
                "\"month\":%lu,\"resetsOn\":\"1 December\",\"saved\":true,\"loaded\":true}",
                (unsigned long long)limitCents, (unsigned long long)spentCents, active ? "true" : "false",
                (unsigned long)month);
  return buf;
}

uint64_t epochOf(int y, unsigned m, unsigned d) { return (uint64_t)orch::daysFromCivil(y, m, d) * 86400u + 5 * 3600u; }

}  // namespace

int main() {
  ndtest::Ctx t;
  const std::string dir = ndtest::scratchDir("hosted-budget");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  const std::string path = dir + "/budget.json";

  {
    HostedBudget b(path);
    t.ok(b.readState() == orch::BudgetFileRead::Missing, "no file: missing");
    t.ok(b.effective() == orch::HistoryProfile::Standard, "no file: the default profile");
    t.ok(orch::installedHistoryProfile() == orch::HistoryProfile::Standard, "the default is installed");
    t.ok(b.status().find("the default") != std::string::npos, "the status says it is the default");
  }
  fsutil::writeFileAtomic(path, "{not json");
  {
    HostedBudget b(path);
    t.ok(b.readState() == orch::BudgetFileRead::Corrupt, "a corrupt file reads as corrupt");
    t.ok(b.effective() == orch::HistoryProfile::Standard, "a corrupt file reads as the default");
    t.ok(b.status().find("could not be read") != std::string::npos, "and says so");
    t.ok(b.pin(orch::HistoryProfile::Wide, 20776), "a pin stores");
    t.ok(orch::installedHistoryProfile() == orch::HistoryProfile::Wide, "and is installed");
  }
  {
    HostedBudget b(path);   // a restart
    t.ok(b.readState() == orch::BudgetFileRead::Ok && b.effective() == orch::HistoryProfile::Wide,
         "the pinned profile survives a restart");
    t.ok(orch::installedHistoryProfile() == orch::HistoryProfile::Wide, "and is installed at start");
    // A month of overspend: a pinned profile never moves.
    for (unsigned d = 1; d <= 20; d++) b.night(epochOf(2026, 11, d), 0, 240, stateJson(d * 150, 202611));
    t.ok(b.effective() == orch::HistoryProfile::Wide, "a pinned profile is never moved");
    t.ok(b.unpin(20777), "auto stores");
    t.ok(b.effective() == orch::HistoryProfile::Standard, "auto hands it back at standard");
  }
  {
    ndtest::rmTree(dir);
    fsutil::mkdirs(dir);
    HostedBudget b(path);
    // 0.70 USD a day at standard against the instance's 20 USD default ceiling.
    std::string line;
    orch::GovernorStep st;
    unsigned downNight = 0;
    for (unsigned d = 1; d <= 10 && line.empty(); d++) {
      line = b.night(epochOf(2026, 11, d), 0, 240, stateJson((d - 1) * 70, 202611), &st);
      if (!line.empty()) downNight = d;
    }
    t.ok(!line.empty() && st.changed(), "a month of overspend steps down");
    t.eqi(downNight, 4, "once three nights are observed");
    t.ok(st.to == orch::HistoryProfile::Lean, "to the largest profile that fits");
    t.ok(!st.record.empty(), "with a record line");
    t.ok(orch::installedHistoryProfile() == orch::HistoryProfile::Lean, "every head derives under it now");
    const orch::ContextBudget c = orch::headContextBudget("anthropic", "claude-sonnet-5-5", true, {});
    t.eqi((long)c.historySoftBytes, 40000, "the direct head's fold is due at lean's 40,000 B");
    t.ok(b.night(epochOf(2026, 11, downNight), 0, 240, stateJson(400, 202611)).empty(), "the same night twice: nothing");
    HostedBudget again(path);
    t.ok(again.effective() == orch::HistoryProfile::Lean, "the governor's step survives a restart");
    t.ok(again.status().find("set by the device") != std::string::npos, "the status names the device's step");
    // No ceiling (only provider limits): quiet nights, back up one step after seven.
    for (unsigned d = 1; d <= 9; d++) again.night(epochOf(2026, 12, d), 0, 240, stateJson(d * 500, 202612, false));
    t.ok(again.effective() == orch::HistoryProfile::Standard, "no ceiling: back to standard after seven quiet nights");
  }
  orch::installHistoryProfile(orch::kDefaultHistoryProfile);
  ndtest::rmTree(dir);
  std::printf("\n%s: %d checks, %d failures\n", "test_hosted_budget", t.checks, t.failures);
  return t.failures ? 1 : 0;
}
