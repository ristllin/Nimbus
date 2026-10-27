#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/budget_command.h"
#include "nimbus/orch/command.h"
#include "nimbus/orch/provider_caps.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/spend_ceiling.h"

// The owner's spending commands (lane BUD, owner request 26 Oct 2026: with every
// provider paused the device was mute until the 1st unless the web page was in reach).
// The parse, the bounds (a raise of more than 10x or above 250 USD needs "force"), the
// confirm window (2 minutes, the same chat, anything else cancels), who may run them
// (owner chats, never a group, no change by voice), the plan's stated effects (the
// spend rules re-run on the changed limits), and the class rule behind every pause
// notice: the command it ends with, sent as written, is accepted and lifts that pause.

using namespace nimbus::orch;

void setUp() {
  setenv("TZ", "UTC0", 1);
  tzset();
  installBudgetCommands(true);   // the device build (src/agent/budget_command.cpp)
}
void tearDown() {}

namespace {
constexpr uint64_t kUsd = 1000000ULL;
constexpr uint64_t kCent = 10000ULL;

// Lumi's shape on 26 Oct: provider limits set, the overall ceiling off, Anthropic and
// OpenAI at their limits, Mistral uncapped.
LimitsView lumiView() {
  LimitsView v;
  v.loaded = true;
  v.limitMicro = 20 * kUsd;
  v.ownerSet = false;
  v.active = false;
  v.frozen = false;
  v.monthKey = 202610;
  v.resetsOn = "1 November";
  v.spentMicro = 37700 * kCent;
  v.dailyWarnMicro = 5 * kUsd;
  v.todayMicro = 120 * kCent;
  v.rows = {{"anthropic", 250 * kUsd, 29682 * kCent, true, "1 November"},
            {"openai", 75 * kUsd, 7705 * kCent, true, "1 November"},
            {"mistral", 0, 313 * kCent, false, "1 November"}};
  v.pauses = {{"anthropic", kPauseProviderLimit, 0}, {"openai", kPauseProviderLimit, 0}};
  return v;
}

// A new device: the default 20 USD ceiling, no provider limits.
LimitsView defaultView(uint64_t spentMicro) {
  LimitsView v;
  v.loaded = true;
  v.limitMicro = 20 * kUsd;
  v.active = true;
  v.monthKey = 202610;
  v.resetsOn = "1 November";
  v.spentMicro = spentMicro;
  v.frozen = spentMicro >= v.limitMicro;
  if (v.frozen) v.pauses = {{"*", kPauseSpendCeiling, 0}};
  return v;
}

BudgetRequest set(const char* target, uint64_t micro, bool force = false) {
  BudgetRequest r;
  r.verb = BudgetVerb::Set;
  r.target = target;
  r.micro = micro;
  r.force = force;
  return r;
}

bool printable(const std::string& s) {
  for (char ch : s)
    if (!(ch == '\n' || (ch >= 0x20 && ch < 0x7f))) return false;
  return true;
}

std::string slurp(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
}  // namespace

// ---- the parse ---------------------------------------------------------------------

static void test_the_parse_reads_every_form() {
  TEST_ASSERT_TRUE(parseBudgetArgs("").verb == BudgetVerb::Show);
  TEST_ASSERT_TRUE(parseBudgetArgs("   ").verb == BudgetVerb::Show);
  TEST_ASSERT_TRUE(parseBudgetArgs("confirm").verb == BudgetVerb::Confirm);
  TEST_ASSERT_TRUE(parseBudgetArgs("CONFIRM").verb == BudgetVerb::Confirm);
  TEST_ASSERT_TRUE(parseBudgetArgs("cancel").verb == BudgetVerb::Cancel);
  TEST_ASSERT_TRUE(parseBudgetArgs("help").verb == BudgetVerb::Help);
  BudgetRequest r = parseBudgetArgs("anthropic 70");
  TEST_ASSERT_TRUE(r.verb == BudgetVerb::Set);
  TEST_ASSERT_EQUAL_STRING("anthropic", r.target.c_str());
  TEST_ASSERT_EQUAL_UINT64(70 * kUsd, r.micro);
  TEST_ASSERT_FALSE(r.force);
  r = parseBudgetArgs("  Anthropic   $70.5  usd  force ");
  TEST_ASSERT_TRUE(r.verb == BudgetVerb::Set);
  TEST_ASSERT_EQUAL_UINT64(70500000ULL, r.micro);
  TEST_ASSERT_TRUE(r.force);
  TEST_ASSERT_EQUAL_UINT64(1234500000ULL, parseBudgetArgs("all 1234.50usd").micro);
  TEST_ASSERT_EQUAL_STRING("zai", parseBudgetArgs("Z.ai 10").target.c_str());
  TEST_ASSERT_EQUAL_STRING("all", parseBudgetArgs("ALL 30").target.c_str());
  r = parseBudgetArgs("openai off");
  TEST_ASSERT_TRUE(r.verb == BudgetVerb::Off);
  TEST_ASSERT_EQUAL_STRING("openai", r.target.c_str());
  for (const char* t : {"cumulo", "openai", "anthropic", "mistral", "zai", "custom", "tavily"})
    TEST_ASSERT_EQUAL_STRING(t, budgetTargetSlug(t).c_str());
}

static void test_the_parse_refuses_with_plain_words() {
  struct Case { const char* args; const char* words; };
  const Case cases[] = {
      {"claude 70", "no provider called \"claude\""},
      {"anthropic", "Add the new monthly limit"},
      {"anthropic 1,000", "without commas"},
      {"anthropic 70.123", "is not an amount"},
      {"anthropic -5", "is not an amount"},
      {"anthropic 70.", "is not an amount"},
      {"anthropic seventy", "is not an amount"},
      {"anthropic 70 please", "Only the word force"},
      {"anthropic 70 force now", "Only the word force"},
      {"anthropic off now", "To remove a limit, send /budget anthropic off"},
      {"all off", "only on the device's web page"},
      {"confirm now", "no provider called"},
  };
  for (const Case& c : cases) {
    const BudgetRequest r = parseBudgetArgs(c.args);
    TEST_ASSERT_TRUE_MESSAGE(r.verb == BudgetVerb::Error, c.args);
    TEST_ASSERT_TRUE_MESSAGE(r.error.find(c.words) != std::string::npos, (std::string(c.args) + " -> " + r.error).c_str());
    TEST_ASSERT_TRUE(printable(r.error));
  }
  // A quoted word is cut and made printable: a control byte never reaches the reply.
  const BudgetRequest r = parseBudgetArgs("ab\x01" "cdefghijklmnopqrstuvwxyz 5");
  TEST_ASSERT_TRUE(printable(r.error));
  TEST_ASSERT_TRUE(r.error.find("klmnopqrstuvwxyz") == std::string::npos);
}

// Every amount in cents, written back the way a reply writes it, parses to itself.
static void test_every_amount_round_trips_through_the_command_text() {
  for (uint64_t cents = 1; cents <= 30000; cents += (cents < 300 ? 1 : 37)) {
    const BudgetRequest r = set("mistral", cents * kCent, (cents % 2) == 0);
    const std::string cmd = budgetCommandText(r);
    const Command c = parseCommand(cmd);
    TEST_ASSERT_EQUAL_STRING("budget", c.verb.c_str());
    const BudgetRequest back = parseBudgetArgs(c.args);
    TEST_ASSERT_TRUE_MESSAGE(back.verb == BudgetVerb::Set, cmd.c_str());
    TEST_ASSERT_EQUAL_UINT64(r.micro, back.micro);
    TEST_ASSERT_EQUAL(r.force, back.force);
  }
  uint64_t m = 0;
  TEST_ASSERT_TRUE(parseUsdAmount("99999999999999", m));   // saturates, refused by the plan
  TEST_ASSERT_TRUE(m > BudgetBounds::kMaxMicro);
}

// ---- the bounds ---------------------------------------------------------------------

// The rule over a grid: a raise needs "force" exactly when it is more than 10x the
// current limit or above 250 USD; a new limit, a lower one and an equal one never do.
static void test_force_is_needed_exactly_for_a_big_raise() {
  const uint64_t grid[] = {0, 1 * kUsd, 5 * kUsd, 20 * kUsd, 24999 * kCent, 25 * kUsd, 200 * kUsd,
                           250 * kUsd, 25001 * kCent, 251 * kUsd, 500 * kUsd, 2500 * kUsd, 100000 * kUsd};
  for (uint64_t from : grid) {
    for (uint64_t to : grid) {
      if (to == 0) continue;
      const bool expect = from > 0 && to > from && (to > 10 * from || to > 250 * kUsd);
      TEST_ASSERT_EQUAL_MESSAGE(expect, budgetNeedsForce(from, to), (std::to_string(from) + "->" + std::to_string(to)).c_str());
      LimitsView v = defaultView(0);
      v.ownerSet = true;   // an owner-set ceiling stays on: the bounds alone decide here
      v.rows = {{"anthropic", from, 0, false, "1 November"}};
      const BudgetPlan plain = planBudgetChange(set("anthropic", to), v);
      if (from == to) {
        TEST_ASSERT_TRUE(plain.verdict == BudgetVerdict::Unchanged);
        continue;
      }
      TEST_ASSERT_TRUE(plain.verdict == (expect ? BudgetVerdict::NeedsForce : BudgetVerdict::Ok));
      TEST_ASSERT_TRUE(planBudgetChange(set("anthropic", to, true), v).verdict == BudgetVerdict::Ok);
    }
  }
  // Exactly 10x is not "more than 10x".
  TEST_ASSERT_FALSE(budgetNeedsForce(7 * kUsd, 70 * kUsd));
  TEST_ASSERT_TRUE(budgetNeedsForce(7 * kUsd, 7001 * kCent));
}

static void test_the_bounds_hold_even_with_force() {
  LimitsView v = defaultView(0);
  BudgetPlan p = planBudgetChange(set("anthropic", 99 * kCent, true), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Refused);
  TEST_ASSERT_TRUE(p.why.find("smallest monthly limit is $1.00") != std::string::npos);
  p = planBudgetChange(set("all", 100001 * kUsd, true), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Refused);
  TEST_ASSERT_TRUE(p.why.find("largest overall ceiling is $100000.00") != std::string::npos);
  p = planBudgetChange(set("all", 100000 * kUsd, true), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);
  // The force refusal names the exact command that would pass.
  v.rows = {{"anthropic", 50 * kUsd, 0, false, ""}};
  p = planBudgetChange(set("anthropic", 501 * kUsd), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::NeedsForce);
  TEST_ASSERT_TRUE(p.why.find("send /budget anthropic 501 force.") != std::string::npos);
  // A device that is not loaded refuses every change.
  LimitsView cold;
  p = planBudgetChange(set("anthropic", 70 * kUsd), cold);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Refused);
  TEST_ASSERT_TRUE(p.why.find("not ready yet") != std::string::npos);
}

// ---- the plan's stated effects -------------------------------------------------------

static void test_raising_a_paused_provider_resumes_it() {
  const LimitsView v = lumiView();
  BudgetPlan p = planBudgetChange(set("anthropic", 400 * kUsd, true), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);
  TEST_ASSERT_TRUE(p.targetFrozenBefore);
  TEST_ASSERT_FALSE(p.targetFrozenAfter);
  TEST_ASSERT_FALSE(p.overallActiveAfter);   // other limits remain: the ceiling stays off
  const std::string text = proposalText(p, v);
  TEST_ASSERT_TRUE(text.find("Change the Anthropic monthly limit from $250.00 to $400.00?") == 0);
  TEST_ASSERT_TRUE(text.find("Anthropic spend so far: $296.82.") != std::string::npos);
  TEST_ASSERT_TRUE(text.find("Paid calls to Anthropic resume once it is applied.") != std::string::npos);
  TEST_ASSERT_TRUE(text.find("Send /budget confirm within 2 minutes") != std::string::npos);
  // Still under this month's spend: it says the pause stays.
  p = planBudgetChange(set("anthropic", 290 * kUsd, true), v);
  TEST_ASSERT_TRUE(p.targetFrozenAfter);
  TEST_ASSERT_TRUE(proposalText(p, v).find("stays paused: this month's spend is already over the new limit") !=
                   std::string::npos);
  // Out of credit as well: the limit is lifted but the other pause is named.
  LimitsView credit = v;
  credit.pauses[0].parkKind = (uint8_t)ParkKind::Billing;
  p = planBudgetChange(set("anthropic", 400 * kUsd, true), credit);
  TEST_ASSERT_TRUE(proposalText(p, credit).find("stays paused for another reason: out of credit") != std::string::npos);
}

static void test_lowering_a_limit_under_the_spend_pauses_it() {
  const LimitsView v = lumiView();
  const BudgetPlan p = planBudgetChange(set("mistral", 2 * kUsd), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);
  TEST_ASSERT_FALSE(p.targetFrozenBefore);
  TEST_ASSERT_TRUE(p.targetFrozenAfter);
  TEST_ASSERT_TRUE(proposalText(p, v).find("Set a monthly limit of $2.00 for Mistral?") == 0);
  TEST_ASSERT_TRUE(proposalText(p, v).find("Paid calls to Mistral pause once it is applied.") != std::string::npos);
  TEST_ASSERT_TRUE(appliedText(p, v).find("Paid calls to Mistral pause now.") != std::string::npos);
}

// Removing the last provider limit turns the default ceiling back on: with this month's
// spend over it, every provider pauses, and the proposal says so before the confirm.
static void test_removing_the_last_limit_says_the_ceiling_returns() {
  LimitsView v = lumiView();
  v.rows[1].capMicro = 0;
  v.rows[1].frozen = false;
  v.pauses.pop_back();
  const BudgetPlan p = planBudgetChange([] { BudgetRequest r; r.verb = BudgetVerb::Off; r.target = "anthropic"; return r; }(), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);
  TEST_ASSERT_FALSE(p.overallActiveBefore);
  TEST_ASSERT_TRUE(p.overallActiveAfter);
  TEST_ASSERT_TRUE(p.overallFrozenAfter);
  const std::string text = proposalText(p, v);
  TEST_ASSERT_TRUE(text.find("Remove the Anthropic monthly limit of $250.00?") == 0);
  TEST_ASSERT_TRUE(text.find("Anthropic stays paused: the overall ceiling is reached.") != std::string::npos);
  TEST_ASSERT_TRUE(text.find("the overall ceiling of $20.00 applies again, and this month's spend is over it, so "
                             "paid calls to every provider pause once it is applied.") != std::string::npos);
  // Off with no limit: nothing to change.
  BudgetRequest off;
  off.verb = BudgetVerb::Off;
  off.target = "mistral";
  TEST_ASSERT_TRUE(planBudgetChange(off, v).verdict == BudgetVerdict::Unchanged);
}

static void test_the_overall_ceiling_plans() {
  // Raised over the spend: paid calls resume.
  LimitsView v = defaultView(2013 * kCent);
  BudgetPlan p = planBudgetChange(set("all", 31 * kUsd), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);
  TEST_ASSERT_TRUE(p.overallFrozenBefore);
  TEST_ASSERT_FALSE(p.overallFrozenAfter);
  TEST_ASSERT_TRUE(proposalText(p, v).find("Change the overall monthly ceiling from $20.00 to $31.00?") == 0);
  TEST_ASSERT_TRUE(appliedText(p, v).find("Done. The overall monthly ceiling is now $31.00 (it was $20.00).\n"
                                          "Paid calls resume now.") == 0);
  // Turned on while provider limits keep it off: over the spend, every provider pauses.
  const LimitsView lumi = lumiView();
  p = planBudgetChange(set("all", 30 * kUsd), lumi);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);   // turning a ceiling on never needs force
  TEST_ASSERT_TRUE(p.overallFrozenAfter);
  const std::string text = proposalText(p, lumi);
  TEST_ASSERT_TRUE(text.find("Turn on an overall monthly ceiling of $30.00?") == 0);
  TEST_ASSERT_TRUE(text.find("so paid calls to every provider pause once it is applied") != std::string::npos);
  TEST_ASSERT_TRUE(text.find("Your provider limits keep applying as well.") != std::string::npos);
  // A provider raise while the overall ceiling holds says every provider stays paused.
  LimitsView both = defaultView(2013 * kCent);
  both.ownerSet = true;   // with provider limits set, only an owner-set ceiling stays on
  both.rows = {{"mistral", 5 * kUsd, 1 * kUsd, false, "1 November"}};
  p = planBudgetChange(set("mistral", 8 * kUsd), both);
  TEST_ASSERT_TRUE(proposalText(p, both).find("Every provider stays paused: the overall ceiling is reached. Send "
                                              "/budget all 31 and then /budget confirm to raise it.") !=
                   std::string::npos);
  // Same value: unchanged.
  LimitsView owner = defaultView(0);
  owner.ownerSet = true;
  TEST_ASSERT_TRUE(planBudgetChange(set("all", 20 * kUsd), owner).verdict == BudgetVerdict::Unchanged);
}

// ---- the class rule behind every pause notice ---------------------------------------

// For every pause shape (a cap and a count at or over it), the command the notice ends
// with parses to a Set of an amount over the count, passes the bounds without a second
// word unless the command carries it, and the plan on that state says the pause lifts.
static void test_every_pause_notice_ends_with_a_command_that_lifts_it() {
  const uint64_t caps[] = {1 * kUsd, 2 * kUsd, 10 * kUsd, 20 * kUsd, 50 * kUsd, 99 * kUsd, 100 * kUsd,
                           249 * kUsd, 250 * kUsd, 1000 * kUsd, 99999 * kUsd};
  const uint64_t overs[] = {0, 1 * kCent, 13 * kCent, 1 * kUsd, 7 * kUsd, 46 * kUsd, 300 * kUsd, 5000 * kUsd};
  for (uint64_t cap : caps) {
    for (uint64_t over : overs) {
      const uint64_t spent = cap + over;
      CapFacts f;
      f.provider = "anthropic";
      f.capMicro = cap;
      f.spentMicro = spent;
      f.periodKey = 2026 * 12 + 9;
      const std::string notice = providerFreezeText(f);
      std::smatch m;
      TEST_ASSERT_TRUE_MESSAGE(std::regex_search(notice, m, std::regex(R"re(Reply (/budget [^ ]+ [0-9.]+( force)?) and then /budget confirm to raise its monthly limit to \$[0-9.]+\.$)re")),
                               notice.c_str());
      const Command c = parseCommand(m[1].str());
      const BudgetRequest r = parseBudgetArgs(c.args);
      TEST_ASSERT_TRUE(r.verb == BudgetVerb::Set);
      LimitsView v = defaultView(0);
      v.rows = {{"anthropic", cap, spent, true, "1 November"}};
      const BudgetPlan p = planBudgetChange(r, v);
      if (spent >= BudgetBounds::kMaxMicro) {
        TEST_ASSERT_TRUE(p.targetFrozenAfter);   // nothing within the bounds lifts it
        continue;
      }
      TEST_ASSERT_TRUE_MESSAGE(p.verdict == BudgetVerdict::Ok, notice.c_str());
      TEST_ASSERT_FALSE_MESSAGE(p.targetFrozenAfter, notice.c_str());
      // The overall ceiling's notice, the same rule.
      CeilingFacts cf;
      cf.limitMicro = cap;
      cf.spentMicro = spent;
      cf.monthKey = 202610;
      const std::string all = freezeNoticeText(cf);
      TEST_ASSERT_TRUE_MESSAGE(std::regex_search(all, m, std::regex(R"re(Reply (/budget all [0-9.]+( force)?) and then /budget confirm to raise the ceiling to \$[0-9.]+\.$)re")),
                               all.c_str());
      LimitsView g = defaultView(0);
      g.ownerSet = true;
      g.limitMicro = cap;
      g.spentMicro = spent;
      g.frozen = true;
      const BudgetPlan pa = planBudgetChange(parseBudgetArgs(parseCommand(m[1].str()).args), g);
      TEST_ASSERT_TRUE_MESSAGE(pa.verdict == BudgetVerdict::Ok, all.c_str());
      TEST_ASSERT_FALSE_MESSAGE(pa.overallFrozenAfter, all.c_str());
    }
  }
  // A build that does not answer the commands (the hosted instance) names none.
  installBudgetCommands(false);
  CapFacts hf;
  hf.provider = "anthropic";
  hf.capMicro = 50 * kUsd;
  hf.spentMicro = 51 * kUsd;
  CeilingFacts hc;
  hc.limitMicro = 20 * kUsd;
  hc.spentMicro = 21 * kUsd;
  TEST_ASSERT_TRUE(providerFreezeText(hf).find("/budget") == std::string::npos);
  TEST_ASSERT_TRUE(freezeNoticeText(hc).find("/budget") == std::string::npos);
  TEST_ASSERT_TRUE(noticeText(SpendCeiling::Notice::Migration, hc).find("/budget") == std::string::npos);
  installBudgetCommands(true);
  // The first-boot notice names the command only when paid calls are paused by it.
  CeilingFacts mig;
  mig.limitMicro = 20 * kUsd;
  mig.spentMicro = 6320 * kCent;
  mig.monthKey = 202610;
  TEST_ASSERT_TRUE(noticeText(SpendCeiling::Notice::Migration, mig).find("Reply /budget all 95 and then /budget confirm to raise the ceiling") !=
                   std::string::npos);
  mig.spentMicro = 660 * kCent;
  TEST_ASSERT_TRUE(noticeText(SpendCeiling::Notice::Migration, mig).find("/budget") == std::string::npos);
}


// ---- the force cases beyond the bounds -------------------------------------------------

// On a default (not owner-set) ceiling the first provider limit turns the ceiling off:
// every provider without a limit of its own then has no monthly stop. A slip there needs
// the second word, and the proposal says what happens.
static void test_a_first_limit_that_turns_the_ceiling_off_needs_force() {
  const LimitsView paused = defaultView(25 * kUsd);
  BudgetPlan p = planBudgetChange(set("tavily", 1 * kUsd), paused);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::NeedsForce);
  TEST_ASSERT_TRUE(p.why.find("A first provider limit turns the overall ceiling off") == 0);
  TEST_ASSERT_TRUE(p.why.find("If you mean it, send /budget tavily 1 force.") != std::string::npos);
  p = planBudgetChange(set("tavily", 1 * kUsd, true), paused);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);
  TEST_ASSERT_TRUE(proposalText(p, paused).find("The overall ceiling of $20.00 turns off while you have provider "
                                                "limits, so providers without a limit of their own have no monthly "
                                                "stop and its pause ends once it is applied.") != std::string::npos);
  // An owner-set ceiling stays on: no force, nothing about the ceiling.
  LimitsView owner = defaultView(5 * kUsd);
  owner.ownerSet = true;
  p = planBudgetChange(set("tavily", 1 * kUsd), owner);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);
  TEST_ASSERT_TRUE(proposalText(p, owner).find("overall ceiling") == std::string::npos);
}

// The overall ceiling's bounds are the same rule: a raise of an owner-set ceiling beyond
// 10x or above 250 USD needs force; turning a ceiling on (from off) never does.
static void test_the_overall_ceiling_needs_force_for_a_big_raise() {
  const uint64_t grid[] = {0, 1 * kUsd, 20 * kUsd, 25 * kUsd, 200 * kUsd, 250 * kUsd, 251 * kUsd, 5000 * kUsd};
  for (uint64_t from : grid)
    for (uint64_t to : grid) {
      if (to == 0) continue;
      LimitsView v = defaultView(0);
      v.ownerSet = true;
      v.limitMicro = from;
      v.active = from > 0;
      const BudgetPlan p = planBudgetChange(set("all", to), v);
      if (from == to) {
        TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Unchanged);
        continue;
      }
      const bool expect = from > 0 && to > from && (to > 10 * from || to > 250 * kUsd);
      TEST_ASSERT_TRUE_MESSAGE(p.verdict == (expect ? BudgetVerdict::NeedsForce : BudgetVerdict::Ok),
                               (std::to_string(from) + "->" + std::to_string(to)).c_str());
      TEST_ASSERT_TRUE(planBudgetChange(set("all", to, true), v).verdict == BudgetVerdict::Ok);
    }
  // The default ceiling raised to 5,000 USD in one step: the second word.
  TEST_ASSERT_TRUE(planBudgetChange(set("all", 5000 * kUsd), defaultView(0)).why.find("send /budget all 5000 force.") !=
                   std::string::npos);
}

// Token and call budgets are their own pause: counted from the ledger's counters, named
// beside a dollar limit, never read as "resume" when a dollar raise leaves them in place.
static void test_token_and_call_budgets_are_named() {
  TEST_ASSERT_FALSE(usageBudgetReached(10, 0, 5, 0));        // no limits
  TEST_ASSERT_TRUE(usageBudgetReached(1000, 1000, 0, 0));    // tokens at the limit
  TEST_ASSERT_FALSE(usageBudgetReached(999, 1000, 0, 0));
  TEST_ASSERT_TRUE(usageBudgetReached(0, 0, 50, 50));        // calls at the limit
  LimitsView v = lumiView();
  v.pauses.push_back({"anthropic", 0, 0, true});
  TEST_ASSERT_TRUE(usageText(v).find("Anthropic (monthly limit reached and token or call limit reached)") !=
                   std::string::npos);
  const BudgetPlan p = planBudgetChange(set("anthropic", 400 * kUsd, true), v);
  TEST_ASSERT_TRUE(proposalText(p, v).find("Anthropic stays paused for another reason: token or call limit reached.") !=
                   std::string::npos);
}

// "off" with the overall ceiling off leaves the provider with no stop at all.
static void test_off_with_nothing_else_limiting_needs_force() {
  const LimitsView v = lumiView();   // ceiling off, Anthropic keeps its limit
  BudgetPlan p = planBudgetChange(parseBudgetArgs("openai off"), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::NeedsForce);
  TEST_ASSERT_EQUAL_STRING("OpenAI would have no monthly stop at all: the overall ceiling is off. If you mean it, "
                           "send /budget openai off force.",
                           p.why.c_str());
  p = planBudgetChange(parseBudgetArgs("openai off force"), v);
  TEST_ASSERT_TRUE(p.verdict == BudgetVerdict::Ok);
  TEST_ASSERT_TRUE(proposalText(p, v).find("OpenAI will have no monthly limit, and nothing else limits it: the "
                                           "overall ceiling is off.") != std::string::npos);
  // The overall ceiling is never turned off by command, whatever reaches the plan.
  BudgetRequest allOff;
  allOff.verb = BudgetVerb::Off;
  allOff.target = "all";
  allOff.force = true;
  TEST_ASSERT_TRUE(planBudgetChange(allOff, v).verdict == BudgetVerdict::Refused);
  TEST_ASSERT_EQUAL_STRING("The smallest overall ceiling is $1.00.",
                           planBudgetChange(set("all", 50 * kCent, true), v).why.c_str());
}

// ---- which messages keep a waiting change ----------------------------------------------

static void test_only_a_confirm_cancel_or_new_change_keeps_a_waiting_one() {
  struct Case { std::string text; bool keeps; };
  const Case cases[] = {
      {"/budget confirm", true},       {"/budget@NimbusBot confirm", true}, {"/budget cancel", true},
      {"/budget anthropic 70", true},  {"/budget anthropic 7000", true},    {"/budget openai off", true},
      {"/budget", false},              {"/budget help", false},             {"/budget bogus 5", false},
      {"/budget confirm now", false},  {"/usage", false},                   {"/model", false},
      {"/help", false},                {"/clear yes", false},               {"hello there", false},
      {forwardedText("/budget confirm"), false},
  };
  for (const Case& c : cases)
    TEST_ASSERT_EQUAL_MESSAGE(c.keeps, budgetKeepsWaiting(parseCommand(c.text)), c.text.c_str());
}

// The paused web chat lets through exactly the commands that never call a model.
static void test_the_paused_web_chat_runs_only_model_free_commands() {
  for (const char* t : {"/usage", "/budget anthropic 70", "/budget confirm", "/model", "/help", "/start", " /usage"})
    TEST_ASSERT_TRUE_MESSAGE(runsWhilePaused(t), t);
  for (const char* t : {"/compact", "/remind 5m tea", "/loops", "/zzz", "hello", "usage"})
    TEST_ASSERT_FALSE_MESSAGE(runsWhilePaused(t), t);
}

// ---- every provider, every line ----------------------------------------------------------

static void checkWording(const std::string& text, const std::string& where) {
  TEST_ASSERT_TRUE_MESSAGE(printable(text), where.c_str());
  for (const char* bad : {"the your", "The your", "s your custom", " Your ", "estimated your"})
    TEST_ASSERT_TRUE_MESSAGE(text.find(bad) == std::string::npos, (where + ": " + text).c_str());
}

// The provider list is the registry plus the custom endpoint and Tavily, and every one
// reads right in every line that names it (the custom endpoint's name starts "your").
static void test_every_provider_reads_right_in_every_line() {
  std::vector<std::string> want;
  for (const ProviderSlot& slot : kProviderSlots) want.push_back(slot.slug);
  want.push_back("custom");
  want.push_back("tavily");
  TEST_ASSERT_TRUE(budgetProviders() == want);
  const std::string doc = slurp("docs/spend-ceiling.md");
  const size_t from = doc.find("## Commands");
  const std::string commands = doc.substr(from, doc.find("\n## ", from + 3) - from);
  for (const std::string& t : want) {
    TEST_ASSERT_EQUAL_STRING(t.c_str(), budgetTargetSlug(t).c_str());
    TEST_ASSERT_TRUE_MESSAGE(budgetHelpText().find(t) != std::string::npos, t.c_str());
    TEST_ASSERT_TRUE_MESSAGE(commands.find("`" + t + "`") != std::string::npos, t.c_str());
    LimitsView v = lumiView();
    v.rows.push_back({t, 5 * kUsd, 6 * kUsd, true, "1 November"});
    v.pauses.push_back({t, kPauseProviderLimit, (uint8_t)ParkKind::Billing, true});
    checkWording(usageText(v), t + " usage");
    for (const char* args : {" 7", " 5", " off force", " 2"}) {
      const BudgetPlan p = planBudgetChange(parseBudgetArgs(t + args), v);
      checkWording(proposalText(p, v), t + args);
      if (p.verdict == BudgetVerdict::Ok) checkWording(appliedText(p, v) + "\n" + changeSummary(p), t + args);
    }
    LimitsView fresh = lumiView();
    const BudgetPlan first = planBudgetChange(set(t.c_str(), 4 * kUsd), fresh);
    checkWording(proposalText(first, fresh), t + " first");
    CapFacts cf;
    cf.provider = t;
    cf.capMicro = 5 * kUsd;
    cf.spentMicro = 6 * kUsd;
    cf.periodKey = 2026 * 12 + 9;
    const std::string notice = providerFreezeText(cf);
    checkWording(notice + "\n" + providerWarnText(cf), t + " notices");
    const size_t at = notice.find("Reply /budget ");
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, notice.c_str());
    const BudgetRequest r = parseBudgetArgs(parseCommand(notice.substr(at + 6)).args.substr(0, notice.substr(at + 6).find(" and then") - 8));
    TEST_ASSERT_TRUE_MESSAGE(r.verb == BudgetVerb::Set && r.target == t, notice.c_str());
    ModelFacts m;
    m.chosenHost = t;
    m.runsOnHost = t;
    m.runsOnModel = "some-model";
    m.limits = v;
    m.last.known = true;
    m.last.host = t;
    m.last.requestedHost = "anthropic";
    checkWording(modelText(m), t + " model");
  }
}

// ---- the plan against the real rules -------------------------------------------------------

namespace {
const LocalDate kToday{2026, 10, 22};

// The real counters for one grid point: Anthropic's limit `cap` (0 = none) and count
// `spent`, OpenAI limited too when `otherCap`, the ceiling owner-set at 20 USD or the
// default, and the month's count a dollar over Anthropic's.
struct Real {
  std::vector<CapRule> rules;
  ProviderCaps pc;
  SpendCeiling c;
};
Real realOf(bool ownerSet, bool otherCap, uint64_t cap, uint64_t spent) {
  Real r;
  if (cap) r.rules.push_back({"anthropic", cap, 1});
  if (otherCap) r.rules.push_back({"openai", 10 * kUsd, 1});
  r.pc.seed("anthropic", spent, kToday);
  r.pc.setRules(r.rules, kToday);
  if (ownerSet) r.c.setLimit(20 * kUsd);
  r.c.add(spent + 1 * kUsd, 202610);
  r.c.setProviderCaps(r.pc.anyCap());
  return r;
}

LimitsView viewOf(const Real& r) {
  LimitsView v;
  v.loaded = true;
  v.limitMicro = r.c.limitMicro;
  v.spentMicro = r.c.spentMicro;
  v.ownerSet = r.c.ownerSet;
  v.active = r.c.active();
  v.frozen = r.c.frozenWith(0);
  v.monthKey = 202610;
  for (int i = 0; i < (int)r.pc.n; ++i)
    v.rows.push_back({r.pc.rows[i].provider, r.pc.rows[i].capMicro, r.pc.rows[i].spentMicro, r.pc.rows[i].frozen, ""});
  return v;
}

// The same change on the real counters: Anthropic's limit becomes `to` (0 = removed).
void applyReal(Real& r, uint64_t to) {
  bool found = false;
  for (CapRule& rule : r.rules)
    if (rule.provider == "anthropic") { rule.capMicro = to; found = true; }
  if (to && !found) r.rules.push_back({"anthropic", to, 1});
  r.pc.setRules(r.rules, kToday);
  r.c.setProviderCaps(r.pc.anyCap());
}
}  // namespace

// The plan's simulation over a grid of states and changes agrees with the real counters:
// ProviderCaps (the rules, the seed, the freeze) and SpendCeiling (on, off, frozen).
static void test_the_plan_matches_the_real_counters() {
  const uint64_t caps[] = {0, 5 * kUsd, 50 * kUsd};
  const uint64_t spends[] = {0, 3 * kUsd, 50 * kUsd, 60 * kUsd};
  const uint64_t news[] = {0, 2 * kUsd, 50 * kUsd, 100 * kUsd};   // 0 = off
  int checked = 0;
  for (int k = 0; k < 2 * 2 * 3 * 4 * 4; ++k) {
    const bool ownerSet = k & 1, otherCap = (k >> 1) & 1;
    const uint64_t cap = caps[(k >> 2) % 3], spent = spends[(k / 12) % 4], to = news[(k / 48) % 4];
    Real r = realOf(ownerSet, otherCap, cap, spent);
    const BudgetPlan p =
        planBudgetChange(to ? set("anthropic", to, true) : parseBudgetArgs("anthropic off force"), viewOf(r));
    if (p.verdict != BudgetVerdict::Ok) continue;
    applyReal(r, to);
    TEST_ASSERT_EQUAL(r.pc.frozenWith("anthropic", 0), p.targetFrozenAfter);
    TEST_ASSERT_EQUAL(r.c.active(), p.overallActiveAfter);
    TEST_ASSERT_EQUAL(r.c.frozenWith(0), p.overallFrozenAfter);
    ++checked;
  }
  TEST_ASSERT_TRUE(checked > 40);
}

// ---- the confirm window ---------------------------------------------------------------

static void test_the_confirm_window_is_two_minutes_and_one_chat() {
  BudgetConfirm w;
  BudgetRequest out;
  BudgetConfirm::Ended why;
  const BudgetRequest r = set("anthropic", 70 * kUsd);
  TEST_ASSERT_FALSE(w.take("111", 0, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::None);
  // On the last millisecond of the window: applied, once.
  w.offer("111", r, true, 5000);
  TEST_ASSERT_TRUE(w.take("111", 5000 + BudgetBounds::kConfirmWindowMs, out, why));
  TEST_ASSERT_EQUAL_UINT64(70 * kUsd, out.micro);
  TEST_ASSERT_FALSE(w.take("111", 5001 + BudgetBounds::kConfirmWindowMs, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::Confirmed);
  // One millisecond late: expired, and the reply offers the command again.
  w.offer("111", r, true, 5000);
  TEST_ASSERT_FALSE(w.take("111", 5001 + BudgetBounds::kConfirmWindowMs, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::Expired);
  TEST_ASSERT_TRUE(confirmMissText(why, out).find("To try again, send /budget anthropic 70.") != std::string::npos);
  // The clock is 64-bit: 50 days later (a 32-bit millisecond clock would have wrapped
  // back into the window) the change is long expired.
  w.offer("111", r, true, 1000);
  TEST_ASSERT_FALSE(w.take("111", 1000 + 50ULL * 86400000ULL, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::Expired);
  // A confirm from another chat applies nothing and drops the waiting change; the
  // proposer then reads why.
  w.offer("111", r, true, 0);
  TEST_ASSERT_FALSE(w.take("222", 10, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::OtherChat);
  TEST_ASSERT_FALSE(w.waiting());
  TEST_ASSERT_FALSE(w.take("111", 20, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::ConfirmedElsewhere);
  TEST_ASSERT_TRUE(confirmMissText(why, out).find("a confirmation came from another chat") != std::string::npos);
  // Any other message from the same chat cancels; from another chat it changes nothing.
  w.offer("111", r, true, 0);
  TEST_ASSERT_FALSE(w.noteOther("222"));
  TEST_ASSERT_TRUE(w.waiting());
  TEST_ASSERT_TRUE(w.noteOther("111"));
  TEST_ASSERT_FALSE(w.take("111", 10, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::OtherMessage);
  TEST_ASSERT_TRUE(confirmMissText(why, out).find("cancelled by your next message") != std::string::npos);
  // /budget cancel, and a newer proposal replaces an older one.
  w.offer("111", r, true, 0);
  TEST_ASSERT_FALSE(w.cancel("222"));
  TEST_ASSERT_TRUE(w.cancel("111"));
  TEST_ASSERT_FALSE(w.take("111", 10, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::Cancelled);
  w.offer("111", r, true, 0);
  w.offer("111", set("anthropic", 80 * kUsd), true, 10);
  TEST_ASSERT_TRUE(w.take("111", 20, out, why));
  TEST_ASSERT_EQUAL_UINT64(80 * kUsd, out.micro);
  w.offer("111", r, true, 0);
  w.offer("web", set("openai", 9 * kUsd), true, 10);
  TEST_ASSERT_FALSE(w.take("111", 20, out, why));   // drops the web chat's change too (fail closed)
  TEST_ASSERT_FALSE(w.waiting());
  // A refused proposal cancels the chat's waiting change ("anything else cancels").
  w.offer("111", r, true, 0);
  w.offer("111", set("anthropic", 900 * kUsd), false, 10);
  TEST_ASSERT_FALSE(w.waiting());
  TEST_ASSERT_FALSE(w.take("111", 20, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::OtherMessage);
  // A confirmed change that could not be applied says so on the next confirm, never
  // "already applied".
  w.offer("111", r, true, 0);
  TEST_ASSERT_TRUE(w.take("111", 10, out, why));
  w.failed("111");
  TEST_ASSERT_FALSE(w.take("111", 20, out, why));
  TEST_ASSERT_TRUE(why == BudgetConfirm::Ended::Failed);
  TEST_ASSERT_TRUE(confirmMissText(why, out).find("The last change was not applied. To try again, send /budget "
                                                  "anthropic 70.") == 0);
  for (int e = 0; e <= (int)BudgetConfirm::Ended::Failed; ++e)
    TEST_ASSERT_TRUE(printable(confirmMissText((BudgetConfirm::Ended)e, r)));
}

// ---- who may run them -------------------------------------------------------------------

static void test_owner_chats_only_never_a_group() {
  TEST_ASSERT_TRUE(budgetAccessFor("123456789", true) == BudgetAccess::Full);
  TEST_ASSERT_TRUE(budgetAccessFor("web", true) == BudgetAccess::Full);
  TEST_ASSERT_TRUE(budgetAccessFor("serial", true) == BudgetAccess::Full);
  TEST_ASSERT_TRUE(budgetAccessFor("voice", true) == BudgetAccess::ReadOnly);
  TEST_ASSERT_TRUE(budgetAccessFor("123456789", false) == BudgetAccess::NotOwner);
  // A group or a channel is refused even when it is on the owner list.
  TEST_ASSERT_TRUE(budgetAccessFor("-1001234567890", true) == BudgetAccess::Group);
  TEST_ASSERT_TRUE(budgetAccessFor("-42", false) == BudgetAccess::Group);
  for (BudgetAccess a : {BudgetAccess::ReadOnly, BudgetAccess::NotOwner, BudgetAccess::Group})
    TEST_ASSERT_FALSE(budgetAccessText(a).empty());
  TEST_ASSERT_TRUE(budgetAccessText(BudgetAccess::Full).empty());
}

// The device's dispatcher runs the access rule before every spending verb, and the
// verbs are owner-only in the one table.
static void test_the_dispatcher_wires_the_verbs() {
  for (const char* v : {"usage", "budget", "model"}) TEST_ASSERT_TRUE(isOwnerVerb(v));
  const std::string src = slurp("src/agent/orchestrator.cpp");
  const size_t help = src.find("if (v == \"help\" || v == \"start\")");
  TEST_ASSERT_TRUE(help != std::string::npos);
  const std::string helpBlock = src.substr(help, src.find("return;", help) - help);
  for (const char* line : {"/usage - ", "/budget <provider|all> <USD>", "/model - "})
    TEST_ASSERT_TRUE_MESSAGE(helpBlock.find(line) != std::string::npos, line);
  const std::string dev = slurp("src/agent/budget_command.cpp");
  TEST_ASSERT_TRUE(dev.find("budgetAccessFor(") != std::string::npos);
}

// ---- the state JSON and the copy --------------------------------------------------------

static void test_the_state_json_reads_into_the_view() {
  LimitsView v;
  TEST_ASSERT_TRUE(limitsViewFromJson(
      "{\"limitCents\":2000,\"spentCents\":37700,\"frozen\":false,\"ownerSet\":false,\"active\":false,"
      "\"month\":202610,\"resetsOn\":\"1 November\",\"saved\":true,\"loaded\":true,\"dailyWarnCents\":500,"
      "\"todayCents\":120,\"providers\":[{\"p\":\"anthropic\",\"capCents\":25000,\"spentCents\":29682,"
      "\"frozen\":true,\"resetsOn\":\"1 November\"},{\"p\":\"\",\"capCents\":1}]}",
      v));
  TEST_ASSERT_TRUE(v.loaded);
  TEST_ASSERT_FALSE(v.active);
  TEST_ASSERT_EQUAL_UINT64(377 * kUsd, v.spentMicro);
  TEST_ASSERT_EQUAL(1, (int)v.rows.size());   // a row with no provider is skipped
  TEST_ASSERT_EQUAL_UINT64(250 * kUsd, v.rows[0].capMicro);
  TEST_ASSERT_TRUE(v.rows[0].frozen);
  LimitsView bad;
  TEST_ASSERT_FALSE(limitsViewFromJson("not json", bad));
  TEST_ASSERT_FALSE(bad.loaded);
}

static void test_copy_goldens() {
  std::string out;
  const LimitsView lumi = lumiView();
  out += "[usage, provider limits, two paused]\n" + usageText(lumi) + "\n";
  LimitsView fresh = defaultView(0);
  out += "[usage, new device]\n" + usageText(fresh) + "\n";
  LimitsView frozen = defaultView(2013 * kCent);
  out += "[usage, overall ceiling reached]\n" + usageText(frozen) + "\n";
  LimitsView credit = lumi;
  credit.pauses.push_back({"zai", 0, (uint8_t)ParkKind::Billing});
  credit.rows.push_back({"zai", 15 * kUsd, 2 * kUsd, false, "15 November"});
  out += "[usage, out of credit, a reset day]\n" + usageText(credit) + "\n";
  out += "[usage, not loaded]\n" + usageText(LimitsView()) + "\n";
  LimitsView bothPaused = frozen;
  bothPaused.ownerSet = true;
  bothPaused.rows = {{"anthropic", 10 * kUsd, 12 * kUsd, true, "1 November"}};
  out += "[usage, overall and a provider limit]\n" + usageText(bothPaused) + "\n";
  BudgetPlan p = planBudgetChange(set("anthropic", 400 * kUsd, true), lumi);
  out += "[proposal, provider raise]\n" + proposalText(p, lumi) + "\n";
  out += "[applied, provider raise]\n" + appliedText(p, lumi) + "\n";
  out += "[record, provider raise]\n" + changeSummary(p) + "\n";
  out += "[refusal, force]\n" + proposalText(planBudgetChange(set("anthropic", 400 * kUsd), lumi), lumi) + "\n";
  p = planBudgetChange(set("all", 31 * kUsd), frozen);
  out += "[proposal, overall raise]\n" + proposalText(p, frozen) + "\n";
  out += "[applied, overall raise]\n" + appliedText(p, frozen) + "\n";
  out += "[record, overall raise]\n" + changeSummary(p) + "\n";
  p = planBudgetChange(parseBudgetArgs("openai off"), lumi);
  out += "[refusal, off with nothing else limiting]\n" + proposalText(p, lumi) + "\n";
  p = planBudgetChange(parseBudgetArgs("openai off force"), lumi);
  out += "[proposal, provider off force]\n" + proposalText(p, lumi) + "\n";
  out += "[record, provider off]\n" + changeSummary(p) + "\n";
  const LimitsView dflt = defaultView(25 * kUsd);
  out += "[refusal, a first limit turns the ceiling off]\n" +
         proposalText(planBudgetChange(set("anthropic", 10 * kUsd), dflt), dflt) + "\n";
  p = planBudgetChange(set("anthropic", 10 * kUsd, true), dflt);
  out += "[proposal, a first limit with force]\n" + proposalText(p, dflt) + "\n";
  out += "[replies, confirm paths]\n" + storeFailedText(set("anthropic", 70 * kUsd)) + "\n" +
         replanRefusedText(planBudgetChange(set("anthropic", 900 * kUsd), lumi)) + "\n" + cancelText(true) +
         "\n" + cancelText(false) + "\n";
  out += "[help]\n" + budgetHelpText() + "\n";
  CapFacts cf;
  cf.provider = "anthropic";
  cf.capMicro = 250 * kUsd;
  cf.spentMicro = 29682 * kCent;
  cf.periodKey = 2026 * 12 + 9;
  out += "[notice, provider limit, a raise that needs force]\n" + providerFreezeText(cf) + "\n";
  cf.capMicro = 50 * kUsd;
  cf.spentMicro = 5012 * kCent;
  out += "[notice, provider limit]\n" + providerFreezeText(cf) + "\n";
  CeilingFacts gf;
  gf.limitMicro = 20 * kUsd;
  gf.spentMicro = 2013 * kCent;
  gf.monthKey = 202610;
  out += "[notice, overall ceiling]\n" + freezeNoticeText(gf) + "\n";
  ModelFacts m;
  m.chosenHost = "anthropic";
  m.runsOnHost = "anthropic";
  m.runsOnModel = "claude-opus-5";
  m.limits = lumi;
  m.last.known = true;
  m.last.ok = true;
  m.last.host = "mistral";
  m.last.model = "mistral-medium-latest";
  m.last.requestedHost = "anthropic";
  m.last.agoS = 180;
  out += "[model, head paused, failover]\n" + modelText(m) + "\n";
  m.limits = defaultView(0);
  m.last.host = "anthropic";
  m.last.model = "claude-opus-5";
  m.last.concreteModel = "claude-opus-5-20261001";
  m.last.requestedHost = "anthropic";
  out += "[model, nothing paused, exact id recorded]\n" + modelText(m) + "\n";
  ModelFacts none;
  none.runsOnHost = "openai";
  none.runsOnModel = "gpt-5.6-luna";
  out += "[model, first boot]\n" + modelText(none) + "\n";
  ModelFacts keyless;
  keyless.chosenHost = "openai";
  keyless.runsOnHost = "anthropic";
  keyless.runsOnModel = "claude-sonnet-5";
  out += "[model, chosen provider has no key]\n" + modelText(keyless) + "\n";
  TEST_ASSERT_TRUE_MESSAGE(printable(out), "printable ASCII");
  TEST_ASSERT_TRUE(out.find("\xE2\x80\x94") == std::string::npos);
  const char* path = "test/golden/budget_command_copy.txt";
  if (getenv("GOLDEN_UPDATE")) {
    std::ofstream(path, std::ios::binary) << out;
    TEST_PASS_MESSAGE("golden written");
    return;
  }
  TEST_ASSERT_EQUAL_STRING(slurp(path).c_str(), out.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_parse_reads_every_form);
  RUN_TEST(test_the_parse_refuses_with_plain_words);
  RUN_TEST(test_every_amount_round_trips_through_the_command_text);
  RUN_TEST(test_force_is_needed_exactly_for_a_big_raise);
  RUN_TEST(test_the_bounds_hold_even_with_force);
  RUN_TEST(test_raising_a_paused_provider_resumes_it);
  RUN_TEST(test_lowering_a_limit_under_the_spend_pauses_it);
  RUN_TEST(test_removing_the_last_limit_says_the_ceiling_returns);
  RUN_TEST(test_the_overall_ceiling_plans);
  RUN_TEST(test_every_pause_notice_ends_with_a_command_that_lifts_it);
  RUN_TEST(test_a_first_limit_that_turns_the_ceiling_off_needs_force);
  RUN_TEST(test_off_with_nothing_else_limiting_needs_force);
  RUN_TEST(test_the_overall_ceiling_needs_force_for_a_big_raise);
  RUN_TEST(test_token_and_call_budgets_are_named);
  RUN_TEST(test_only_a_confirm_cancel_or_new_change_keeps_a_waiting_one);
  RUN_TEST(test_the_paused_web_chat_runs_only_model_free_commands);
  RUN_TEST(test_every_provider_reads_right_in_every_line);
  RUN_TEST(test_the_plan_matches_the_real_counters);
  RUN_TEST(test_the_confirm_window_is_two_minutes_and_one_chat);
  RUN_TEST(test_owner_chats_only_never_a_group);
  RUN_TEST(test_the_dispatcher_wires_the_verbs);
  RUN_TEST(test_the_state_json_reads_into_the_view);
  RUN_TEST(test_copy_goldens);
  return UNITY_END();
}
