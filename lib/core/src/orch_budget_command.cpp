#include "nimbus/orch/budget_command.h"

#include <ArduinoJson.h>

#include <atomic>
#include <cctype>
#include <cstdio>

#include "nimbus/orch/provider_caps.h"   // providerName
#include "nimbus/orch/provider_slots.h"

// The parse, the bounds, the plan and the confirm window of the owner's spending
// commands (budget_command.h). The words live in orch_budget_text.cpp.
namespace nimbus {
namespace orch {

namespace {

std::string lower(std::string s) {
  for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

std::vector<std::string> wordsOf(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    size_t j = i;
    while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

bool endsWith(const std::string& s, const char* tail) {
  const std::string t(tail);
  return s.size() > t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// A word as it may be quoted back: printable ASCII, at most 20 characters.
std::string quoted(const std::string& w) {
  std::string out;
  for (char ch : w) {
    if (out.size() >= 20) break;
    out += (ch >= 32 && ch < 127) ? ch : '?';
  }
  return "\"" + out + "\"";
}

// "anthropic, openai, ..., tavily, or all" (the providers a limit can be set for).
std::string targetsText() {
  std::string s;
  for (const std::string& p : budgetProviders()) s += p + ", ";
  return s + "or all";
}

BudgetRequest errorOf(const std::string& why) {
  BudgetRequest r;
  r.verb = BudgetVerb::Error;
  r.error = why;
  return r;
}

uint64_t ceilDollarsMicro(uint64_t micro) { return (micro + 999999ULL) / 1000000ULL * 1000000ULL; }

// Rounds a whole-dollar amount up to a readable step: tens from 100 USD, hundreds from
// 1,000 USD.
uint64_t roundUpReadable(uint64_t micro) {
  const uint64_t step = micro >= 1000000000ULL ? 100000000ULL : micro >= 100000000ULL ? 10000000ULL : 1000000ULL;
  return (micro + step - 1) / step * step;
}

SpendCeiling ceilingOf(const LimitsView& v) {
  SpendCeiling c;
  c.limitMicro = v.limitMicro;
  c.ownerSet = v.ownerSet;
  c.monthKey = v.monthKey;
  c.spentMicro = v.spentMicro;
  c.providerCaps = v.anyLimit();
  c.frozen = c.active() && c.spentMicro >= c.limitMicro;
  return c;
}

// The dollar split of a typed amount ("70.5" -> 70, 50, 1 decimal); false when it is not
// digits with at most one '.' and two decimals.
bool splitDollarsCents(const std::string& s, uint64_t& dollars, uint64_t& cents, int& decimals) {
  int digits = 0;
  for (char ch : s) {
    if (ch == '.') {
      if (decimals >= 0) return false;
      decimals = 0;
      continue;
    }
    if (ch < '0' || ch > '9' || decimals == 2) return false;
    ++digits;
    if (decimals >= 0) {
      cents = cents * 10 + (uint64_t)(ch - '0');
      ++decimals;
    } else if ((dollars = dollars * 10 + (uint64_t)(ch - '0')) > 1000000000ULL) {
      dollars = 1000000000ULL;   // saturate: refused by the plan as too large
    }
  }
  return digits > 0 && decimals != 0;   // not "", "$" or "70."
}

// ---- the parse ---------------------------------------------------------------------

// A lone keyword ("confirm", "cancel", "help"); false for anything else.
bool keywordOf(const std::vector<std::string>& w, BudgetRequest& r) {
  if (w.size() != 1) return false;
  const std::string k = lower(w[0]);
  if (k == "confirm") r.verb = BudgetVerb::Confirm;
  else if (k == "cancel") r.verb = BudgetVerb::Cancel;
  else if (k == "help") r.verb = BudgetVerb::Help;
  else return false;
  return true;
}

constexpr const char* kAllOff = "The overall ceiling can be turned off only on the device's web page (Assistant > "
                                "Usage). To raise it, send /budget all <USD>.";

// "<target> off [force]": a provider's limit removed (the overall ceiling never by command).
BudgetRequest offOf(const std::vector<std::string>& w, BudgetRequest r) {
  r.force = w.size() == 3 && lower(w[2]) == "force";
  if (w.size() != 2 && !r.force)
    return errorOf("To remove a limit, send /budget " + r.target + " off (or /budget " + r.target + " off force).");
  if (r.target == kBudgetAll) return errorOf(kAllOff);
  r.verb = BudgetVerb::Off;
  return r;
}

// "<target> <amount> [usd] [force]".
BudgetRequest setOf(const std::vector<std::string>& w, BudgetRequest r) {
  if (w[1].find(',') != std::string::npos)
    return errorOf("Write the amount without commas, for example /budget " + r.target + " 1000.");
  if (!parseUsdAmount(w[1], r.micro))
    return errorOf(quoted(w[1]) + " is not an amount. Write dollars, for example /budget " + r.target + " 70 or 70.50.");
  size_t i = 2;
  if (i < w.size() && (lower(w[i]) == "usd" || lower(w[i]) == "dollars")) ++i;
  if (i < w.size() && lower(w[i]) == "force") {
    r.force = true;
    ++i;
  }
  if (i != w.size())
    return errorOf("Only the word force may follow the amount, for example /budget " + r.target + " 500 force.");
  r.verb = BudgetVerb::Set;
  return r;
}

// ---- the plan ----------------------------------------------------------------------

std::string limitWhat(const BudgetPlan& p) {
  if (p.overall) return "the overall ceiling";
  const std::string n = providerName(p.req.target);
  return (n.rfind("your ", 0) == 0 ? "the " + n.substr(5) : "the " + n) + " limit";
}

std::string forceAgain(const BudgetPlan& p) {
  BudgetRequest again = p.req;
  again.force = true;
  return " If you mean it, send " + budgetCommandText(again) + ".";
}

void planProvider(BudgetPlan& p, const LimitsView& v) {
  const LimitRow* r = v.row(p.req.target);
  p.fromMicro = r ? r->capMicro : 0;
  p.spentMicro = r ? r->spentMicro : 0;
  p.toMicro = p.req.verb == BudgetVerb::Off ? 0 : p.req.micro;
  p.targetFrozenBefore = r && r->atLimit();
  p.targetFrozenAfter = p.toMicro && p.spentMicro >= p.toMicro;
  bool anyAfter = p.toMicro != 0;
  for (const LimitRow& row : v.rows)
    if (row.provider != p.req.target && row.capMicro) anyAfter = true;
  SpendCeiling c = ceilingOf(v);
  c.setProviderCaps(anyAfter);
  p.overallActiveAfter = c.active();
  p.overallFrozenAfter = c.frozenWith(0);
  p.overallLimitAfter = c.limitMicro;
}

void planOverall(BudgetPlan& p, const LimitsView& v) {
  p.fromMicro = v.active ? v.limitMicro : 0;
  p.spentMicro = v.spentMicro;
  p.toMicro = p.req.micro;
  SpendCeiling c = ceilingOf(v);
  c.setLimit(p.req.micro);
  p.overallActiveAfter = c.active();
  p.overallFrozenAfter = c.frozenWith(0);
  p.overallLimitAfter = c.limitMicro;
}

// The "force" a change needs ("" when none): for a provider, a first limit that turns the
// default overall ceiling off and "off" when nothing would limit it afterwards; for every
// target, a raise beyond the bounds (budgetNeedsForce).
// "off" is a raise to whatever stops the provider afterwards: nothing (the overall ceiling
// off), or the overall ceiling, held to the same force rule as a typed raise.
std::string offForceNeeded(const BudgetPlan& p) {
  const std::string name = capitalFirst(providerName(p.req.target));
  if (!p.overallActiveAfter)
    return name + " would have no monthly stop at all: the overall ceiling is off." + forceAgain(p);
  if (!budgetNeedsForce(p.fromMicro, p.overallLimitAfter)) return std::string();
  return "Removing it lets " + providerName(p.req.target) + " spend up to the overall ceiling of " +
         formatUsd(p.overallLimitAfter) + ", " +
         (p.overallLimitAfter > BudgetBounds::kNoForceMaxMicro
              ? "above the " + formatUsd(BudgetBounds::kNoForceMaxMicro) + " a month that /budget sets without a second word."
              : "more than " + std::to_string(BudgetBounds::kMaxRaiseFactor) + " times its limit of " +
                    formatUsd(p.fromMicro) + ".") +
         forceAgain(p);
}

std::string providerForceNeeded(const BudgetPlan& p) {
  if (p.req.verb == BudgetVerb::Off) return offForceNeeded(p);
  if (p.overallActiveBefore && !p.overallActiveAfter)
    return "A first provider limit turns the overall ceiling off, so every provider without a limit of its "
           "own would have no monthly stop." + forceAgain(p) +
           " To keep a ceiling as well, send /budget all <USD> afterwards.";
  return std::string();
}

std::string forceNeeded(const BudgetPlan& p) {
  if (p.req.force) return std::string();
  if (!p.overall) {
    const std::string why = providerForceNeeded(p);
    if (!why.empty()) return why;
  }
  if (p.req.verb == BudgetVerb::Set && budgetNeedsForce(p.fromMicro, p.toMicro))
    return p.toMicro > BudgetBounds::kNoForceMaxMicro
               ? formatUsd(p.toMicro) + " is above the " + formatUsd(BudgetBounds::kNoForceMaxMicro) +
                     " a month that /budget sets without a second word." + forceAgain(p)
               : "That raises " + limitWhat(p) + " more than " + std::to_string(BudgetBounds::kMaxRaiseFactor) +
                     " times, from " + formatUsd(p.fromMicro) + " to " + formatUsd(p.toMicro) + "." + forceAgain(p);
  return std::string();
}

// Why a change is refused outright ("" when it is not): the overall ceiling turned off,
// or an amount outside the bounds (even with force).
std::string refusedWhy(const BudgetPlan& p) {
  if (p.overall && p.req.verb == BudgetVerb::Off) return kAllOff;
  if (p.req.verb != BudgetVerb::Set) return std::string();
  const bool low = p.req.micro < BudgetBounds::kMinMicro;
  if (!low && p.req.micro <= BudgetBounds::kMaxMicro) return std::string();
  return std::string(low ? "The smallest" : "The largest") + (p.overall ? " overall ceiling is " : " monthly limit is ") +
         formatUsd(low ? BudgetBounds::kMinMicro : BudgetBounds::kMaxMicro) + "." +
         (low && !p.overall ? " To remove a provider's limit, send /budget <provider> off." : "");
}

// Why a change changes nothing ("" when it does).
std::string unchangedWhy(const BudgetPlan& p, const LimitsView& v) {
  if (p.req.verb == BudgetVerb::Off)
    return p.fromMicro ? std::string() : capitalFirst(providerName(p.req.target)) + " has no monthly limit. Nothing to change.";
  const bool same = p.overall ? (v.active && v.ownerSet && v.limitMicro == p.toMicro) : p.fromMicro == p.toMicro;
  return same ? capitalFirst(limitWhat(p)) + " is already " + formatUsd(p.toMicro) + ". Nothing to change." : std::string();
}

// The verdict of a planned change; `why` filled for every verdict but Ok.
BudgetVerdict verdictOf(BudgetPlan& p, const LimitsView& v) {
  if (!(p.why = refusedWhy(p)).empty()) return BudgetVerdict::Refused;
  if (!(p.why = unchangedWhy(p, v)).empty()) return BudgetVerdict::Unchanged;
  p.why = forceNeeded(p);
  return p.why.empty() ? BudgetVerdict::Ok : BudgetVerdict::NeedsForce;
}

}  // namespace

std::string capitalFirst(std::string s) {
  if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
  return s;
}

std::vector<std::string> budgetProviders() {
  std::vector<std::string> out;
  for (const ProviderSlot& s : kProviderSlots) out.push_back(s.slug);
  out.push_back("custom");
  out.push_back("tavily");
  return out;
}

std::string budgetTargetSlug(const std::string& word) {
  const std::string w = lower(word);
  if (w == kBudgetAll) return kBudgetAll;
  if (w == "z.ai") return "zai";
  for (const std::string& p : budgetProviders())
    if (w == p) return w;
  return std::string();
}

bool parseUsdAmount(const std::string& text, uint64_t& micro) {
  std::string s = lower(text);
  if (!s.empty() && s[0] == '$') s.erase(0, 1);
  if (endsWith(s, "usd")) s.resize(s.size() - 3);
  uint64_t dollars = 0, cents = 0;
  int decimals = -1;
  if (!splitDollarsCents(s, dollars, cents, decimals)) return false;
  if (decimals == 1) cents *= 10;
  micro = dollars * 1000000ULL + cents * 10000ULL;
  return true;
}

// ---- who may run them --------------------------------------------------------------

BudgetAccess budgetAccessFor(const std::string& chatId, bool owner, bool voiceNote) {
  if (!chatId.empty() && chatId[0] == '-') return BudgetAccess::Group;
  if (!owner) return BudgetAccess::NotOwner;
  if (chatId == "voice" || voiceNote) return BudgetAccess::ReadOnly;
  return BudgetAccess::Full;
}

std::string budgetTimingRefusal(int64_t nowEpoch, int64_t msgDate) {
  if (nowEpoch < (int64_t)kCeilingClockValid)
    return "The spending limits are not ready yet: the device is still starting, or its clock is not set. "
           "Try again in a minute.";
  if (msgDate > 0 && nowEpoch - msgDate > (int64_t)(BudgetBounds::kConfirmWindowMs / 1000))
    return "That message was sent more than " + std::to_string(BudgetBounds::kConfirmWindowMs / 60000) +
           " minutes before it arrived, so it changed nothing. Send it again if you still want the change.";
  return std::string();
}

std::string budgetAccessText(BudgetAccess a) {
  switch (a) {
    case BudgetAccess::Group:
      return "Spending commands work only in the owner's own chat with the device, never in a group.";
    case BudgetAccess::NotOwner:
      return "Only the device's owner can see or change spending limits.";
    case BudgetAccess::ReadOnly:
      return "Spending limits change only from a typed message: send /budget from Telegram or the web chat.";
    case BudgetAccess::Full:
      break;
  }
  return std::string();
}

bool runsWhilePaused(const std::string& text) {
  const Command c = parseCommand(text);
  if (!c.isCommand) return false;
  for (const char* v : {"usage", "budget", "model", "help", "start"})
    if (c.verb == v) return true;
  return false;
}

// ---- the parse ---------------------------------------------------------------------

BudgetRequest parseBudgetArgs(const std::string& args) {
  const std::vector<std::string> w = wordsOf(args);
  BudgetRequest r;
  if (w.empty()) {
    r.verb = BudgetVerb::Show;
    return r;
  }
  if (keywordOf(w, r)) return r;
  r.target = budgetTargetSlug(w[0]);
  if (r.target.empty()) return errorOf("There is no provider called " + quoted(w[0]) + ". Use " + targetsText() + ".");
  if (w.size() == 1)
    return errorOf("Add the new monthly limit in dollars, for example /budget " + r.target + " 70.");
  return lower(w[1]) == "off" ? offOf(w, r) : setOf(w, r);
}

std::string budgetCommandText(const BudgetRequest& req) {
  std::string s = "/budget " + req.target;
  if (req.verb == BudgetVerb::Off) return s + (req.force ? " off force" : " off");
  const uint64_t cents = req.micro / 10000ULL;
  char amt[32];
  if (cents % 100)
    snprintf(amt, sizeof(amt), " %llu.%02llu", (unsigned long long)(cents / 100), (unsigned long long)(cents % 100));
  else
    snprintf(amt, sizeof(amt), " %llu", (unsigned long long)(cents / 100));
  return s + amt + (req.force ? " force" : "");
}

bool budgetKeepsWaiting(const Command& cmd) {
  if (!cmd.isCommand || cmd.verb != "budget") return false;
  const BudgetVerb v = parseBudgetArgs(cmd.args).verb;
  return v == BudgetVerb::Confirm || v == BudgetVerb::Cancel || v == BudgetVerb::Set || v == BudgetVerb::Off;
}

// ---- the view ----------------------------------------------------------------------

const LimitRow* LimitsView::row(const std::string& provider) const {
  for (const LimitRow& r : rows)
    if (r.provider == provider) return &r;
  return nullptr;
}

bool LimitsView::anyLimit() const {
  for (const LimitRow& r : rows)
    if (r.capMicro) return true;
  return false;
}

uint8_t LimitsView::pausedBy(const std::string& provider) const {
  uint8_t bits = 0;
  for (const PauseFact& f : pauses)
    if (f.provider == provider || f.provider == "*") bits |= f.paused;
  return bits;
}

uint8_t LimitsView::parkKindOf(const std::string& provider) const {
  for (const PauseFact& f : pauses)
    if (f.provider == provider && f.parkKind) return f.parkKind;
  return 0;
}

bool LimitsView::usageLimited(const std::string& provider) const {
  for (const PauseFact& f : pauses)
    if (f.provider == provider && f.usageLimit) return true;
  return false;
}

bool limitsViewFromJson(const std::string& stateJson, LimitsView& out) {
  JsonDocument d;
  if (deserializeJson(d, stateJson) != DeserializationError::Ok || !d.is<JsonObject>()) return false;
  const uint64_t kCent = 10000ULL;
  out.loaded = d["loaded"] | false;
  out.limitMicro = (d["limitCents"] | (uint64_t)0) * kCent;
  out.spentMicro = (d["spentCents"] | (uint64_t)0) * kCent;
  out.ownerSet = d["ownerSet"] | false;
  out.active = d["active"] | true;
  out.frozen = d["frozen"] | false;
  out.monthKey = d["month"] | (uint32_t)0;
  out.resetsOn = (const char*)(d["resetsOn"] | "");
  out.dailyWarnMicro = (d["dailyWarnCents"] | (uint64_t)0) * kCent;
  out.todayMicro = (d["todayCents"] | (uint64_t)0) * kCent;
  out.rows.clear();
  for (JsonObjectConst p : d["providers"].as<JsonArrayConst>()) {
    LimitRow r;
    r.provider = (const char*)(p["p"] | "");
    if (r.provider.empty()) continue;
    r.capMicro = (p["capCents"] | (uint64_t)0) * kCent;
    r.spentMicro = (p["spentCents"] | (uint64_t)0) * kCent;
    r.frozen = p["frozen"] | false;
    r.resetsOn = (const char*)(p["resetsOn"] | "");
    out.rows.push_back(r);
  }
  return true;
}

// ---- the plan ----------------------------------------------------------------------

bool budgetNeedsForce(uint64_t fromMicro, uint64_t toMicro) {
  if (fromMicro == 0 || toMicro <= fromMicro) return false;   // a new limit or a lower one
  return toMicro > BudgetBounds::kNoForceMaxMicro || toMicro > fromMicro * BudgetBounds::kMaxRaiseFactor;
}

BudgetPlan planBudgetChange(const BudgetRequest& req, const LimitsView& v) {
  BudgetPlan p;
  p.req = req;
  p.overall = req.target == kBudgetAll;
  if (!v.loaded) {
    p.why = "The spending limits are not ready yet: the device is still starting, or its clock is not set. "
            "Try again in a minute.";
    return p;
  }
  if ((req.verb != BudgetVerb::Set && req.verb != BudgetVerb::Off) || req.target.empty()) {
    p.why = budgetHelpText();
    return p;
  }
  const SpendCeiling before = ceilingOf(v);
  p.overallActiveBefore = before.active();
  p.overallFrozenBefore = v.frozen || before.frozenWith(0);
  if (p.overall)
    planOverall(p, v);
  else
    planProvider(p, v);
  p.verdict = verdictOf(p, v);
  return p;
}

bool samePlannedEffect(const BudgetPlan& a, const BudgetPlan& b) {
  return a.verdict == BudgetVerdict::Ok && b.verdict == BudgetVerdict::Ok && a.overall == b.overall &&
         a.req.target == b.req.target && a.req.verb == b.req.verb && a.fromMicro == b.fromMicro &&
         a.toMicro == b.toMicro && a.overallActiveBefore == b.overallActiveBefore &&
         a.overallActiveAfter == b.overallActiveAfter && a.overallFrozenBefore == b.overallFrozenBefore &&
         a.overallFrozenAfter == b.overallFrozenAfter && a.overallLimitAfter == b.overallLimitAfter &&
         a.targetFrozenBefore == b.targetFrozenBefore && a.targetFrozenAfter == b.targetFrozenAfter;
}

namespace {
std::atomic<bool> g_commands{false};   // constant-initialized: set before main() by the device
}  // namespace

void installBudgetCommands(bool on) { g_commands.store(on); }
bool budgetCommandsInstalled() { return g_commands.load(); }

uint64_t suggestedLimitMicro(uint64_t capMicro, uint64_t spentMicro) {
  const uint64_t base = capMicro > spentMicro ? capMicro : spentMicro;
  uint64_t s = roundUpReadable(ceilDollarsMicro(base + base / 2));
  const uint64_t atLeast = ceilDollarsMicro(capMicro + 1000000ULL);
  if (s < atLeast) s = roundUpReadable(atLeast);
  return s > BudgetBounds::kMaxMicro ? BudgetBounds::kMaxMicro : s;
}

std::string liftCommand(const std::string& target, uint64_t capMicro, uint64_t spentMicro) {
  BudgetRequest r;
  r.verb = BudgetVerb::Set;
  r.target = target;
  r.micro = suggestedLimitMicro(capMicro, spentMicro);
  r.force = budgetNeedsForce(capMicro, r.micro);
  return budgetCommandText(r);
}

std::string liftSentence(const std::string& target, const std::string& what, uint64_t capMicro,
                         uint64_t spentMicro) {
  if (!budgetCommandsInstalled()) return std::string();
  return " Reply " + liftCommand(target, capMicro, spentMicro) + " and then /budget confirm to raise " + what +
         " to " + formatUsd(suggestedLimitMicro(capMicro, spentMicro)) + ".";
}

// ---- the confirm window ------------------------------------------------------------

void BudgetConfirm::end(Ended why, const std::string& chat) {
  waiting_ = false;
  last_ = why;
  lastChat_ = chat;
  lastPlan_ = plan_;
}

void BudgetConfirm::offer(const std::string& chat, const BudgetPlan& plan, uint64_t nowMs, int64_t msgDate) {
  if (plan.verdict != BudgetVerdict::Ok) {
    noteOther(chat);
    return;
  }
  waiting_ = true;
  chat_ = chat;
  plan_ = plan;
  atMs_ = nowMs;
  msgDate_ = msgDate;
}

bool BudgetConfirm::take(const std::string& chat, uint64_t nowMs, int64_t msgDate, BudgetPlan& out, Ended& why) {
  if (waiting_ && chat_ != chat) {   // a confirm from another chat: the waiting change is dropped
    end(Ended::ConfirmedElsewhere, chat_);
    out = lastPlan_;
    why = Ended::OtherChat;
    return false;
  }
  if (waiting_) {
    const int64_t windowS = (int64_t)(BudgetBounds::kConfirmWindowMs / 1000);
    const bool dated = msgDate_ > 0 && msgDate > 0;
    const bool inTime = nowMs >= atMs_ && nowMs - atMs_ <= BudgetBounds::kConfirmWindowMs &&
                        (!dated || msgDate - msgDate_ <= windowS);
    end(dated && msgDate < msgDate_ ? Ended::OutOfOrder : inTime ? Ended::Confirmed : Ended::Expired, chat);
    out = lastPlan_;
    why = last_;
    return last_ == Ended::Confirmed;
  }
  why = lastChat_ == chat ? last_ : Ended::None;
  out = lastPlan_;
  return false;
}

void BudgetConfirm::failed(const std::string& chat) {
  if (!waiting_ && lastChat_ == chat && last_ == Ended::Confirmed) last_ = Ended::Failed;
}

bool BudgetConfirm::cancel(const std::string& chat) {
  if (!waiting_ || chat_ != chat) return false;
  end(Ended::Cancelled, chat);
  return true;
}

bool BudgetConfirm::noteOther(const std::string& chat) {
  if (!waiting_ || chat_ != chat) return false;
  end(Ended::OtherMessage, chat);
  return true;
}

}  // namespace orch
}  // namespace nimbus
