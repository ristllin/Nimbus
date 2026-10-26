#include "nimbus/orch/budget_command.h"

#include <ArduinoJson.h>

#include <atomic>
#include <cctype>

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

constexpr const char* kTargets = "anthropic, openai, mistral, zai, cumulo, custom, tavily, or all";

BudgetRequest errorOf(const std::string& why) {
  BudgetRequest r;
  r.verb = BudgetVerb::Error;
  r.error = why;
  return r;
}

uint64_t ceilDollarsMicro(uint64_t micro) {
  const uint64_t d = (micro + 999999ULL) / 1000000ULL;
  return d * 1000000ULL;
}

// Rounds a whole-dollar amount up to a readable step: tens from 100 USD, hundreds from
// 1,000 USD.
uint64_t roundUpReadable(uint64_t micro) {
  const uint64_t step = micro >= 1000000000ULL ? 100000000ULL : micro >= 100000000ULL ? 10000000ULL : 1000000ULL;
  return (micro + step - 1) / step * step;
}

bool anyCap(const std::vector<LimitRow>& rows) {
  for (const LimitRow& r : rows)
    if (r.capMicro) return true;
  return false;
}

SpendCeiling ceilingOf(const LimitsView& v) {
  SpendCeiling c;
  c.limitMicro = v.limitMicro;
  c.ownerSet = v.ownerSet;
  c.monthKey = v.monthKey;
  c.spentMicro = v.spentMicro;
  c.providerCaps = anyCap(v.rows);
  c.frozen = c.active() && c.spentMicro >= c.limitMicro;
  return c;
}

bool atOrOver(uint64_t capMicro, uint64_t spentMicro) { return capMicro && spentMicro >= capMicro; }

// The bounds every Set passes before anything else.
bool boundsRefusal(const BudgetRequest& req, std::string& why) {
  if (req.micro < BudgetBounds::kMinMicro) {
    why = "The smallest monthly limit is " + formatUsd(BudgetBounds::kMinMicro) + ". To remove a provider's "
          "limit, send /budget <provider> off.";
    return true;
  }
  if (req.micro > BudgetBounds::kMaxMicro) {
    why = "The largest monthly limit is " + formatUsd(BudgetBounds::kMaxMicro) + ".";
    return true;
  }
  return false;
}

std::string forceWhy(const BudgetPlan& p, const std::string& name) {
  BudgetRequest again = p.req;
  again.force = true;
  std::string s;
  if (p.toMicro > BudgetBounds::kNoForceMaxMicro)
    s = formatUsd(p.toMicro) + " is above the " + formatUsd(BudgetBounds::kNoForceMaxMicro) +
        " a month that /budget sets without a second word.";
  else
    s = "That raises " + name + " more than " + std::to_string(BudgetBounds::kMaxRaiseFactor) + " times, from " +
        formatUsd(p.fromMicro) + " to " + formatUsd(p.toMicro) + ".";
  return s + " If you mean it, send " + budgetCommandText(again) + ".";
}

void planProvider(BudgetPlan& p, const LimitsView& v) {
  const std::string& slug = p.req.target;
  const LimitRow* r = v.row(slug);
  p.fromMicro = r ? r->capMicro : 0;
  p.spentMicro = r ? r->spentMicro : 0;
  p.toMicro = p.req.verb == BudgetVerb::Off ? 0 : p.req.micro;
  p.targetFrozenBefore = r ? (r->frozen || atOrOver(r->capMicro, r->spentMicro)) : false;
  p.targetFrozenAfter = atOrOver(p.toMicro, p.spentMicro);
  std::vector<LimitRow> after = v.rows;
  bool found = false;
  for (LimitRow& row : after)
    if (row.provider == slug) { row.capMicro = p.toMicro; found = true; }
  if (!found) {
    LimitRow row;
    row.provider = slug;
    row.capMicro = p.toMicro;
    after.push_back(row);
  }
  SpendCeiling c = ceilingOf(v);
  c.setProviderCaps(anyCap(after));
  p.overallActiveAfter = c.active();
  p.overallFrozenAfter = c.frozenWith(0);
  p.overallLimitAfter = c.limitMicro;
  if (p.targetFrozenBefore && !p.targetFrozenAfter) p.resumes.push_back(slug);
  if (!p.targetFrozenBefore && p.targetFrozenAfter) p.pauses.push_back(slug);
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

// The Unchanged / NeedsForce / Refused verdicts of a planned change ("" when Ok).
BudgetVerdict verdictOf(BudgetPlan& p, const LimitsView& v) {
  const std::string name = p.overall ? std::string("the overall ceiling") : providerName(p.req.target);
  if (p.req.verb == BudgetVerb::Off) {
    if (p.fromMicro) return BudgetVerdict::Ok;
    p.why = capitalFirst(name) + " has no monthly limit. Nothing to change.";
    return BudgetVerdict::Unchanged;
  }
  if (boundsRefusal(p.req, p.why)) return BudgetVerdict::Refused;
  const bool same = p.overall ? (v.active && v.ownerSet && v.limitMicro == p.toMicro) : p.fromMicro == p.toMicro;
  if (same) {
    p.why = (p.overall ? std::string("The overall monthly ceiling") : "The " + name + " monthly limit") +
            " is already " + formatUsd(p.toMicro) + ". Nothing to change.";
    return BudgetVerdict::Unchanged;
  }
  if (!p.req.force && budgetNeedsForce(p.fromMicro, p.toMicro)) {
    p.why = forceWhy(p, name);
    return BudgetVerdict::NeedsForce;
  }
  return BudgetVerdict::Ok;
}

}  // namespace

std::string capitalFirst(std::string s) {
  if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
  return s;
}

std::string budgetTargetSlug(const std::string& word) {
  const std::string w = lower(word);
  if (w == kBudgetAll) return kBudgetAll;
  if (w == "z.ai") return "zai";
  if (isProviderSlug(w.c_str()) || w == "custom" || w == "tavily") return w;
  return std::string();
}

bool parseUsdAmount(const std::string& text, uint64_t& micro) {
  std::string s = lower(text);
  if (!s.empty() && s[0] == '$') s.erase(0, 1);
  if (endsWith(s, "usd")) s.resize(s.size() - 3);
  uint64_t dollars = 0, cents = 0;
  int decimals = -1, digits = 0;
  for (char ch : s) {
    if (ch == '.') {
      if (decimals >= 0) return false;
      decimals = 0;
      continue;
    }
    if (ch < '0' || ch > '9') return false;
    ++digits;
    if (decimals < 0) {
      dollars = dollars * 10 + (uint64_t)(ch - '0');
      if (dollars > 1000000000ULL) dollars = 1000000000ULL;   // saturate: refused as too large
    } else {
      if (decimals == 2) return false;
      cents = cents * 10 + (uint64_t)(ch - '0');
      ++decimals;
    }
  }
  if (digits == 0 || decimals == 0) return false;   // "", "$", "70."
  if (decimals == 1) cents *= 10;
  micro = dollars * 1000000ULL + cents * 10000ULL;
  return true;
}

namespace {

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

// "<target> off": a provider's limit removed (the overall ceiling never by command).
BudgetRequest offOf(const std::vector<std::string>& w, BudgetRequest r) {
  if (w.size() != 2) return errorOf("To remove a limit, send /budget " + r.target + " off on its own.");
  if (r.target == kBudgetAll)
    return errorOf("The overall ceiling can be turned off only on the device's web page (Assistant > Usage). "
                   "To raise it, send /budget all <USD>.");
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

}  // namespace

BudgetRequest parseBudgetArgs(const std::string& args) {
  const std::vector<std::string> w = wordsOf(args);
  BudgetRequest r;
  if (w.empty()) {
    r.verb = BudgetVerb::Show;
    return r;
  }
  if (keywordOf(w, r)) return r;
  r.target = budgetTargetSlug(w[0]);
  if (r.target.empty())
    return errorOf("There is no provider called " + quoted(w[0]) + ". Use " + kTargets + ".");
  if (w.size() == 1)
    return errorOf("Add the new monthly limit in dollars, for example /budget " + r.target + " 70.");
  return lower(w[1]) == "off" ? offOf(w, r) : setOf(w, r);
}

std::string budgetCommandText(const BudgetRequest& req) {
  std::string s = "/budget " + req.target;
  if (req.verb == BudgetVerb::Off) return s + " off";
  const uint64_t cents = req.micro / 10000ULL;
  char amt[32];
  if (cents % 100)
    snprintf(amt, sizeof(amt), " %llu.%02llu", (unsigned long long)(cents / 100), (unsigned long long)(cents % 100));
  else
    snprintf(amt, sizeof(amt), " %llu", (unsigned long long)(cents / 100));
  s += amt;
  if (req.force) s += " force";
  return s;
}

// ---- who may run them --------------------------------------------------------------

BudgetAccess budgetAccessFor(const std::string& chatId, bool owner) {
  if (!chatId.empty() && chatId[0] == '-') return BudgetAccess::Group;
  if (!owner) return BudgetAccess::NotOwner;
  if (chatId == "voice") return BudgetAccess::ReadOnly;
  return BudgetAccess::Full;
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

// ---- the view ----------------------------------------------------------------------

const LimitRow* LimitsView::row(const std::string& provider) const {
  for (const LimitRow& r : rows)
    if (r.provider == provider) return &r;
  return nullptr;
}

uint8_t LimitsView::pausedBy(const std::string& provider) const {
  uint8_t bits = 0;
  for (const PauseFact& f : pauses)
    if (f.provider == provider || f.provider == "*") bits |= f.paused;
  return bits;
}

uint8_t LimitsView::parkKindOf(const std::string& provider) const {
  for (const PauseFact& f : pauses)
    if (f.provider == provider) return f.parkKind;
  return 0;
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

namespace {
std::atomic<bool> g_commands{false};   // constant-initialized: set before main() by the device
}  // namespace

void installBudgetCommands(bool on) { g_commands.store(on); }
bool budgetCommandsInstalled() { return g_commands.load(); }

uint64_t suggestedLimitMicro(uint64_t capMicro, uint64_t spentMicro) {
  const uint64_t base = capMicro > spentMicro ? capMicro : spentMicro;
  uint64_t s = roundUpReadable(ceilDollarsMicro(base + base / 2));
  const uint64_t floor = ceilDollarsMicro(capMicro + 1000000ULL);
  if (s < floor) s = roundUpReadable(floor);
  if (s <= spentMicro) s = roundUpReadable(ceilDollarsMicro(spentMicro + 1000000ULL));
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

// ---- the confirm window ------------------------------------------------------------

void BudgetConfirm::propose(const std::string& chat, const BudgetRequest& req, uint32_t nowMs) {
  if (waiting_ && chat_ != chat) {
    last_ = Ended::Replaced;
    lastChat_ = chat_;
    lastReq_ = req_;
  }
  waiting_ = true;
  chat_ = chat;
  req_ = req;
  atMs_ = nowMs;
}

bool BudgetConfirm::take(const std::string& chat, uint32_t nowMs, BudgetRequest& out, Ended& why) {
  if (waiting_ && chat_ == chat) {
    waiting_ = false;
    lastChat_ = chat;
    lastReq_ = req_;
    if ((uint32_t)(nowMs - atMs_) <= BudgetBounds::kConfirmWindowMs) {
      last_ = Ended::Confirmed;
      out = req_;
      why = Ended::Confirmed;
      return true;
    }
    last_ = Ended::Expired;
    why = Ended::Expired;
    out = lastReq_;
    return false;
  }
  if (waiting_) {   // a confirm from another chat: the waiting change is dropped
    waiting_ = false;
    last_ = Ended::OtherChat;
    lastChat_ = chat_;
    lastReq_ = req_;
    why = Ended::OtherChat;
    return false;
  }
  why = lastChat_ == chat ? last_ : Ended::None;
  out = lastReq_;
  return false;
}

bool BudgetConfirm::cancel(const std::string& chat) {
  if (!waiting_ || chat_ != chat) return false;
  waiting_ = false;
  last_ = Ended::Cancelled;
  lastChat_ = chat;
  lastReq_ = req_;
  return true;
}

bool BudgetConfirm::noteOther(const std::string& chat) {
  if (!waiting_ || chat_ != chat) return false;
  waiting_ = false;
  last_ = Ended::OtherMessage;
  lastChat_ = chat;
  lastReq_ = req_;
  return true;
}

}  // namespace orch
}  // namespace nimbus
