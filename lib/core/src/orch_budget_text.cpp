#include <string>
#include <vector>

#include "nimbus/orch/budget_command.h"
#include "nimbus/orch/provider_caps.h"   // providerName
#include "nimbus/orch/provider_park.h"   // PauseCause, ParkKind

// The words of the owner's spending commands and /model (budget_command.h): plain,
// ASCII, one fact per line so a reply reads well aloud. Pinned by
// test/golden/budget_command_copy.txt.
namespace nimbus {
namespace orch {

namespace {

// A provider inside a sentence ("Anthropic", "your custom endpoint").
std::string nameOf(const std::string& slug) { return providerName(slug); }
// A provider starting a sentence or a line ("Your custom endpoint").
std::string titleName(const std::string& slug) { return capitalFirst(providerName(slug)); }
// A provider as a noun before "limit" or "spend" ("custom endpoint", never "your ...").
std::string nounOf(const std::string& slug) {
  const std::string n = providerName(slug);
  return n.rfind("your ", 0) == 0 ? n.substr(5) : n;
}

std::string minutesText() {
  const uint32_t m = BudgetBounds::kConfirmWindowMs / 60000;
  return std::to_string(m) + (m == 1 ? " minute" : " minutes");
}

std::string joinList(const std::vector<std::string>& items) {
  std::string s;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) s += (i + 1 == items.size()) ? " and " : ", ";
    s += items[i];
  }
  return s;
}

// Why `provider` is paused, besides the overall ceiling ("" when it is not); without
// its own dollar limit when `withLimit` is false.
std::string pauseReason(const LimitsView& v, const std::string& provider, bool withLimit = true) {
  std::vector<std::string> why;
  const LimitRow* r = v.row(provider);
  const uint8_t bits = v.pausedBy(provider);
  if (withLimit && ((r && r->atLimit()) || (bits & kPauseProviderLimit))) why.push_back("monthly limit reached");
  if (v.usageLimited(provider)) why.push_back("token or call limit reached");
  if (bits & kPauseOwner) why.push_back("paused by you");
  const uint8_t kind = v.parkKindOf(provider);
  if (kind == (uint8_t)ParkKind::Billing) why.push_back("out of credit");
  if (kind == (uint8_t)ParkKind::Auth) why.push_back("its key was refused");
  return joinList(why);
}

// Every provider with a pause of its own, in the order first seen: the limit rows,
// then the other records.
std::vector<std::string> pausedProviders(const LimitsView& v) {
  std::vector<std::string> out;
  auto add = [&](const std::string& p) {
    if (p.empty() || p == "*") return;
    for (const std::string& q : out)
      if (q == p) return;
    if (!pauseReason(v, p).empty()) out.push_back(p);
  };
  for (const LimitRow& r : v.rows) add(r.provider);
  for (const PauseFact& f : v.pauses) add(f.provider);
  return out;
}

// "Anthropic (monthly limit reached)" for every provider paused on its own.
std::vector<std::string> pausedItems(const LimitsView& v) {
  std::vector<std::string> items;
  for (const std::string& p : pausedProviders(v)) items.push_back(nameOf(p) + " (" + pauseReason(v, p) + ")");
  return items;
}

std::string pausedLine(const LimitsView& v) {
  const std::vector<std::string> items = pausedItems(v);
  if (v.frozen)
    return "Paused now: every provider, because the overall ceiling is reached." +
           (items.empty() ? std::string() : " Also paused on their own: " + joinList(items) + ".");
  return items.empty() ? std::string("Nothing is paused.") : "Paused now: " + joinList(items) + ".";
}

std::string overallLine(const LimitsView& v) {
  if (v.active) return "Overall ceiling: " + formatUsd(v.limitMicro) + (v.frozen ? ", reached." : ".");
  return v.ownerSet ? std::string("Overall ceiling: off.")
                    : std::string("Overall ceiling: off while you have provider limits.");
}

std::string rowLine(const LimitsView& v, const LimitRow& r) {
  std::string s = titleName(r.provider) + ": " + formatUsd(r.spentMicro);
  s += r.capMicro ? " of its " + formatUsd(r.capMicro) + " limit" : std::string(", no limit");
  if (r.capMicro && !r.resetsOn.empty() && r.resetsOn != v.resetsOn) s += " (starts again " + r.resetsOn + ")";
  return s + ".";
}

std::string todayLine(const LimitsView& v) {
  return "Today: " + formatUsd(v.todayMicro) +
         (v.dailyWarnMicro ? ", daily warning at " + formatUsd(v.dailyWarnMicro) + "."
                           : std::string(", with the built-in daily warning."));
}

std::string hintLine(const LimitsView& v) {
  if (v.frozen)
    return "Send " + liftCommand(kBudgetAll, v.limitMicro, v.spentMicro) + " and then /budget confirm to raise the "
           "ceiling.";
  for (const LimitRow& r : v.rows)
    if (r.atLimit())
      return "Send " + liftCommand(r.provider, r.capMicro, r.spentMicro) + " and then /budget confirm to raise the " +
             nounOf(r.provider) + " limit.";
  return "Change a limit with /budget <provider> <USD>, or the ceiling with /budget all <USD>.";
}

// The target provider's own pause before and after its limit changes ("" when neither).
std::string targetEffect(const BudgetPlan& p, const LimitsView& v, const std::string& when) {
  const std::string name = titleName(p.req.target);
  if (p.targetFrozenBefore && !p.targetFrozenAfter) {
    const std::string other = pauseReason(v, p.req.target, /*withLimit=*/false);
    if (p.overallFrozenAfter) return name + " stays paused: the overall ceiling is reached.";
    if (!other.empty()) return name + " stays paused for another reason: " + other + ".";
    return "Paid calls to " + nameOf(p.req.target) + " resume" + when;
  }
  if (!p.targetFrozenBefore && p.targetFrozenAfter)
    return "Paid calls to " + nameOf(p.req.target) + " pause" + when +
           " This month's spend is already at or over the new limit.";
  if (p.targetFrozenAfter) return name + " stays paused: this month's spend is already over the new limit.";
  return std::string();
}

// What the overall ceiling does around a provider change ("" when nothing to say).
std::string overallAroundProvider(const BudgetPlan& p, const LimitsView& v, const std::string& when) {
  if (p.overallActiveBefore && !p.overallActiveAfter)
    return "The overall ceiling of " + formatUsd(v.limitMicro) + " turns off while you have provider limits, so " +
           "providers without a limit of their own have no monthly stop" +
           (p.overallFrozenBefore ? " and its pause ends" + when : std::string("."));
  if (!p.overallActiveBefore && p.overallActiveAfter)
    return "With no provider limits left, the overall ceiling of " + formatUsd(v.limitMicro) + " applies again" +
           (p.overallFrozenAfter ? ", and this month's spend is over it, so paid calls to every provider pause" + when
                                 : std::string("."));
  // The overall ceiling holds before and after: say so (unless the target's line did).
  const bool toldOverall = p.targetFrozenBefore && !p.targetFrozenAfter;
  if (p.overallFrozenBefore && p.overallFrozenAfter && !toldOverall)
    return "Every provider stays paused: the overall ceiling is reached. Send " +
           liftCommand(kBudgetAll, v.limitMicro, v.spentMicro) + " and then /budget confirm to raise it.";
  return std::string();
}

std::vector<std::string> providerEffects(const BudgetPlan& p, const LimitsView& v, bool now) {
  const std::string when = now ? " now." : " once it is applied.";
  std::vector<std::string> out;
  if (p.req.verb == BudgetVerb::Off)
    out.push_back(titleName(p.req.target) + (now ? " has" : " will have") + " no monthly limit" +
                  (p.overallActiveAfter ? std::string(".") : ", and nothing else limits it: the overall ceiling is off."));
  for (const std::string& line : {targetEffect(p, v, when), overallAroundProvider(p, v, when)})
    if (!line.empty()) out.push_back(line);
  return out;
}

std::vector<std::string> overallEffects(const BudgetPlan& p, const LimitsView& v, bool now) {
  std::vector<std::string> out;
  const std::string when = now ? " now." : " once it is applied.";
  if (p.overallFrozenBefore && !p.overallFrozenAfter) {
    std::vector<std::string> still;
    for (const std::string& q : pausedProviders(v)) still.push_back(nameOf(q));
    out.push_back("Paid calls resume" + when);
    if (!still.empty()) out.push_back("Still paused for their own reasons: " + joinList(still) + ".");
  } else if (!p.overallFrozenBefore && p.overallFrozenAfter) {
    out.push_back("This month's spend is already at or over it, so paid calls to every provider pause" + when);
  } else if (p.overallFrozenAfter) {
    out.push_back("Paid calls stay paused: this month's spend is already over the new ceiling.");
  }
  if (!p.overallActiveBefore && !v.ownerSet && v.anyLimit()) out.push_back("Your provider limits keep applying as well.");
  return out;
}

// Every effect line of a change, each on its own line.
std::string effects(const BudgetPlan& p, const LimitsView& v, bool now) {
  const std::vector<std::string> lines = p.overall ? overallEffects(p, v, now) : providerEffects(p, v, now);
  std::string s;
  for (const std::string& l : lines) s += "\n" + l;
  return s;
}

std::string questionLine(const BudgetPlan& p) {
  const std::string noun = nounOf(p.req.target);
  if (p.overall)
    return p.overallActiveBefore ? "Change the overall monthly ceiling from " + formatUsd(p.fromMicro) + " to " +
                                       formatUsd(p.toMicro) + "?"
                                 : "Turn on an overall monthly ceiling of " + formatUsd(p.toMicro) + "?";
  if (p.req.verb == BudgetVerb::Off) return "Remove the " + noun + " monthly limit of " + formatUsd(p.fromMicro) + "?";
  return p.fromMicro ? "Change the " + noun + " monthly limit from " + formatUsd(p.fromMicro) + " to " +
                           formatUsd(p.toMicro) + "?"
                     : "Set a monthly limit of " + formatUsd(p.toMicro) + " for " + nameOf(p.req.target) + "?";
}

std::string doneLine(const BudgetPlan& p) {
  const std::string noun = nounOf(p.req.target);
  if (p.overall)
    return "Done. The overall monthly ceiling is now " + formatUsd(p.toMicro) +
           (p.overallActiveBefore ? " (it was " + formatUsd(p.fromMicro) + ")." : std::string(" (it was off)."));
  if (p.req.verb == BudgetVerb::Off)
    return "Done. The " + noun + " monthly limit of " + formatUsd(p.fromMicro) + " is removed.";
  return "Done. The " + noun + " monthly limit is now " + formatUsd(p.toMicro) +
         (p.fromMicro ? " (it was " + formatUsd(p.fromMicro) + ")." : std::string(" (it had none)."));
}

std::string agoText(uint32_t s) {
  if (s < 60) return "just now";
  if (s < 3600) return std::to_string(s / 60) + " min ago";
  if (s < 86400) return std::to_string(s / 3600) + " h ago";
  return std::to_string(s / 86400) + (s < 2 * 86400 ? " day ago" : " days ago");
}

std::string choiceLine(const ModelFacts& f) {
  if (f.runsOnHost.empty()) return "No provider has a key yet, so the assistant cannot answer.";
  const std::string runs = nameOf(f.runsOnHost) + (f.runsOnModel.empty() ? "" : ", " + f.runsOnModel);
  if (f.chosenHost.empty()) return "The assistant runs on " + runs + " (the first provider in your order).";
  if (f.chosenHost == f.runsOnHost) return "The assistant runs on " + runs + " (your choice).";
  return "Your choice is " + nameOf(f.chosenHost) + ", which has no key, so the assistant runs on " + runs + ".";
}

std::string lastLine(const ModelFacts& f) {
  const LastTurnFacts& t = f.last;
  if (!t.known) return "No reply has run since the device started.";
  std::string s = "Last reply (" + agoText(t.agoS) + "): " + (t.ok ? std::string() : std::string("failed on "));
  s += nameOf(t.host) + (t.model.empty() ? "" : ", " + t.model);
  if (!t.concreteModel.empty() && t.concreteModel != t.model) s += " (the provider answered as " + t.concreteModel + ")";
  s += ".";
  if (!t.requestedHost.empty() && t.requestedHost != t.host)
    s += " It started on " + nameOf(t.requestedHost) + " and moved to " + nameOf(t.host) + ".";
  return s;
}

std::string modelPausedLine(const ModelFacts& f) {
  const LimitsView& v = f.limits;
  const std::string line = pausedLine(v);
  const bool headPaused = v.frozen || !pauseReason(v, f.runsOnHost).empty();
  if (!headPaused || f.runsOnHost.empty() || v.frozen) return line;
  return line + " Replies use the next provider in your order until " + nameOf(f.runsOnHost) +
         " is available again; the choice above stays.";
}

}  // namespace

std::string usageText(const LimitsView& v) {
  if (!v.loaded)
    return "Spending figures are not ready yet: the device is still starting, or its clock is not set. "
           "Try again in a minute.";
  std::string s = "Spending this month (estimated): " + formatUsd(v.spentMicro) + " across all providers.";
  s += "\n" + overallLine(v);
  for (const LimitRow& r : v.rows)
    if (r.capMicro || r.spentMicro) s += "\n" + rowLine(v, r);
  s += "\n" + todayLine(v) + "\n" + pausedLine(v);
  if (!v.resetsOn.empty()) s += "\nMonthly counts start again on " + v.resetsOn + ".";
  return s + "\n" + hintLine(v);
}

std::string proposalText(const BudgetPlan& p, const LimitsView& v) {
  if (p.verdict != BudgetVerdict::Ok) return p.why;
  const std::string spent = p.overall ? "Spend so far this month: " + formatUsd(p.spentMicro) + "."
                                      : capitalFirst(nounOf(p.req.target)) + " spend so far: " + formatUsd(p.spentMicro) +
                                            ".";
  return questionLine(p) + "\n" + spent + effects(p, v, false) + "\nSend /budget confirm within " + minutesText() +
         " to apply it. Any other message cancels it.";
}

std::string appliedText(const BudgetPlan& p, const LimitsView& v) { return doneLine(p) + effects(p, v, true); }

std::string replanRefusedText(const BudgetPlan& p) {
  if (p.verdict == BudgetVerdict::Refused && p.why.rfind("The spending limits are not ready", 0) == 0) return p.why;
  return "The limits changed since that was proposed, so nothing changed. " + p.why;
}

std::string storeFailedText(const BudgetRequest& req) {
  return "The change could not be stored, so nothing changed. To try again, send " + budgetCommandText(req) + ".";
}

std::string confirmMissText(BudgetConfirm::Ended why, const BudgetRequest& last) {
  using E = BudgetConfirm::Ended;
  const std::string again = last.target.empty() ? std::string() : " To try again, send " + budgetCommandText(last) + ".";
  switch (why) {
    case E::Expired:
      return "That change waited more than " + minutesText() + ", so nothing changed." + again;
    case E::Cancelled:
      return "That change was cancelled, so nothing changed.";
    case E::OtherMessage:
      return "That change was cancelled by your next message, so nothing changed." + again;
    case E::OtherChat:
      return "No change is waiting for this chat, so nothing changed. A change waiting in another chat is cancelled.";
    case E::ConfirmedElsewhere:
      return "That change was cancelled because a confirmation came from another chat, so nothing changed." + again;
    case E::Failed:
      return "The last change was not applied." + again;
    case E::Confirmed:
      return "Nothing is waiting: the last change is already applied.";
    case E::None:
      break;
  }
  return "No change is waiting. Send /budget <provider> <USD> first, for example /budget anthropic 70.";
}

std::string cancelText(bool cancelled) {
  return cancelled ? std::string("Cancelled. Nothing changed.") : std::string("No change is waiting.");
}

std::string budgetHelpText() {
  std::string providers;
  for (const std::string& p : budgetProviders()) providers += (providers.empty() ? "" : ", ") + p;
  return "Spending commands:\n"
         "/usage - this month's spending, limits and what is paused\n"
         "/budget <provider> <USD> - set a provider's monthly limit\n"
         "/budget all <USD> - set the overall monthly ceiling\n"
         "/budget <provider> off - remove a provider's limit\n"
         "/budget confirm - apply the change just proposed (within " + minutesText() + "); /budget cancel drops it\n"
         "Providers: " + providers + ". A raise above " + formatUsd(BudgetBounds::kNoForceMaxMicro) + ", or to more than " +
         std::to_string(BudgetBounds::kMaxRaiseFactor) + " times the limit, needs the word force after the amount; "
         "the reply says when a change needs it for another reason.";
}

std::string modelText(const ModelFacts& f) {
  std::string s = choiceLine(f) + "\n" + lastLine(f) + "\n" + modelPausedLine(f);
  if (f.last.known && !f.last.model.empty() && f.last.concreteModel.empty())
    s += "\nThe model named is the one the device asked for; the provider's exact version is not recorded yet.";
  return s;
}

std::string changeSummary(const BudgetPlan& p) {
  if (p.overall)
    return "Overall ceiling changed from " + (p.overallActiveBefore ? formatUsd(p.fromMicro) : std::string("off")) +
           " to " + formatUsd(p.toMicro);
  const std::string name = capitalFirst(nounOf(p.req.target));
  if (p.req.verb == BudgetVerb::Off) return name + " monthly limit of " + formatUsd(p.fromMicro) + " removed";
  return name + " monthly limit changed from " + (p.fromMicro ? formatUsd(p.fromMicro) : std::string("none")) +
         " to " + formatUsd(p.toMicro);
}

}  // namespace orch
}  // namespace nimbus
