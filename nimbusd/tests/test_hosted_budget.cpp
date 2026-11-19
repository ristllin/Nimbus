// test_hosted_budget - the owner's history profile and the nightly spending advice on a Virtual
// Nimbus (lane RB-BG, owner ruling 81): the same file and rules as the device (lib/core
// budget_governor.h), the instance's spend ceiling as its facts. Offline:
//   * no file and a corrupt file read as full (the default) and say so; a corrupt file is never
//     overwritten by the advice, and a profile its own field still names is kept;
//   * the owner's profile survives a restart and is installed for every head's context;
//   * a month of overspend never changes the profile: the owner is told once (web chat and
//     Telegram, each once), and the same night twice does nothing;
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
    t.ok(b.effective() == orch::HistoryProfile::Full, "no file: full, the default");
    t.ok(orch::installedHistoryProfile() == orch::HistoryProfile::Full, "the default is installed");
    t.ok(b.status().find("the default") != std::string::npos, "the status says it is the default");
  }
  fsutil::writeFileAtomic(path, "{not json");
  {
    HostedBudget b(path);
    for (unsigned d = 1; d <= 10; d++) b.night(epochOf(2026, 11, d), 0, 240, stateJson(d * 150, 202611));
    std::string text;
    t.ok(fsutil::readFile(path, text) && text == "{not json", "a corrupt file is not overwritten by the advice");
    t.ok(b.status().find("could not be read") != std::string::npos, "and the status says so");
  }
  fsutil::writeFileAtomic(path, "{\"v\":1,\"profile\":\"small\",\"night\":-4}");
  {
    HostedBudget b(path);
    t.ok(b.readState() == orch::BudgetFileRead::Corrupt && b.effective() == orch::HistoryProfile::Small,
         "the owner's profile a damaged file still names is kept");
    t.ok(b.choose(orch::HistoryProfile::Medium, 20776), "a choice stores");
    t.ok(orch::installedHistoryProfile() == orch::HistoryProfile::Medium, "and is installed");
  }
  {
    HostedBudget b(path);   // a restart
    t.ok(b.readState() == orch::BudgetFileRead::Ok && b.effective() == orch::HistoryProfile::Medium,
         "the owner's profile survives a restart");
    t.ok(orch::installedHistoryProfile() == orch::HistoryProfile::Medium, "and is installed at start");
    t.ok(b.choose(orch::HistoryProfile::Full, 20777), "back to the default");
  }
  {
    ndtest::rmTree(dir);
    fsutil::mkdirs(dir);
    HostedBudget b(path);
    // 0.72 USD a day on full against the instance's 20 USD default ceiling.
    orch::BudgetAdvice told;
    unsigned toldNight = 0, advices = 0;
    for (unsigned d = 1; d <= 30; d++) {
      const orch::BudgetAdvice a = b.night(epochOf(2026, 11, d), 0, 240, stateJson((d - 1) * 72, 202611));
      if (a.kind != orch::BudgetAdvice::Kind::Advice) continue;
      advices++;
      if (!toldNight) {
        toldNight = d;
        told = a;
      }
    }
    t.eqi(advices, 1, "a month of overspend: the owner is told once");
    t.eqi(toldNight, 4, "once three nights are observed");
    t.ok(b.effective() == orch::HistoryProfile::Full, "the profile never moves");
    t.ok(orch::installedHistoryProfile() == orch::HistoryProfile::Full, "every head keeps the default");
    const orch::ContextBudget c = orch::headContextBudget("anthropic", "claude-sonnet-5-5", true, {});
    t.eqi((long)c.historySoftBytes, 94464, "the direct head keeps v5.1.2's fold mark");
    t.ok(told.fits && told.line.find("/budget profile") != std::string::npos, "the line names the profile that fits");
    std::string web, tg, again2;
    t.ok(b.take(orch::NoticeChannel::Web, web) && web == told.line, "the web chat is owed the line");
    t.ok(b.take(orch::NoticeChannel::Telegram, tg) && tg == told.line, "and Telegram's owner chat");
    t.ok(!b.take(orch::NoticeChannel::Web, again2), "each once");
    t.ok(b.night(epochOf(2026, 11, 30), 0, 240, stateJson(2200, 202611)).kind == orch::BudgetAdvice::Kind::None,
         "the same night twice: nothing");
    // No ceiling (only provider limits): quiet.
    unsigned quiet = 0;
    for (unsigned d = 1; d <= 20; d++)
      quiet += b.night(epochOf(2026, 12, d), 0, 240, stateJson(d * 500, 202612, false)).kind ==
               orch::BudgetAdvice::Kind::Advice;
    t.eqi(quiet, 0, "no ceiling: nothing said");
  }
  orch::installHistoryProfile(orch::kDefaultHistoryProfile);
  ndtest::rmTree(dir);
  std::printf("\n%s: %d checks, %d failures\n", "test_hosted_budget", t.checks, t.failures);
  return t.failures ? 1 : 0;
}
