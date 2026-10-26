#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/spend_ceiling.h"

// budget_command - the owner's spending commands over Telegram and the web chat:
// /usage (and a bare /budget), /budget <provider|all> <USD> [force], /budget <provider>
// off, /budget confirm and /budget cancel, and /model. Portable (NO Arduino),
// host-tested in test/test_budget_command.
//
// Why: with every provider paused by a limit the device answers no message until the
// 1st, and the only way to raise a limit was the web page, out of reach away from home
// without the cloud link. Slash commands never call a model, so they keep working while
// everything is paused (owner request, 26 Oct 2026).
//
// The rules (money, so fail closed):
//   - every change is proposed first: the reply states the exact change and what it
//     pauses or resumes, and it applies only on "/budget confirm" from the same chat
//     within kConfirmWindowMs; any other message from that chat cancels it;
//   - defensive bounds: a limit is at least kMinMicro and at most kMaxMicro; a single
//     change above kNoForceMaxMicro, or more than kMaxRaiseFactor times the current
//     limit, needs the word "force" after the amount;
//   - "/budget <provider> off" removes a provider's limit (an explicit word, confirmed
//     like every change); the overall ceiling is turned off only on the web page;
//   - the effect a reply states is the spend rules re-run on the changed limits
//     (SpendCeiling for the overall ceiling, "at or over the limit" for a provider), so
//     a change that turns the default ceiling back on, or that is still under this
//     month's spend, says so before the owner confirms it.
// Who may run them (owner chats only, never a group or a guest) is the device's check;
// this module only parses, plans and words.
namespace nimbus {
namespace orch {

struct BudgetBounds {
  static constexpr uint64_t kMinMicro = SpendCeiling::kMinLimitMicro;   // 1 USD
  static constexpr uint64_t kMaxMicro = SpendCeiling::kMaxLimitMicro;   // 100,000 USD, even with force
  static constexpr uint64_t kNoForceMaxMicro = 250000000ULL;            // 250 USD without "force"
  static constexpr uint64_t kMaxRaiseFactor = 10;                       // more than 10x needs "force"
  static constexpr uint32_t kConfirmWindowMs = 120000;                  // 2 minutes
};

// The target word for the overall ceiling.
inline constexpr const char* kBudgetAll = "all";

// The provider slug a typed word names ("anthropic", "z.ai" -> "zai"), "all" for the
// overall ceiling, "" for anything else. Case-insensitive. The providers are the ones a
// monthly limit can be set for: every provider slot, the custom endpoint and Tavily.
std::string budgetTargetSlug(const std::string& word);

// A typed dollar amount ("70", "70.5", "$70.25", "70usd") in micro-USD. False for
// anything else: signs, letters, commas, more than two decimals. A huge amount saturates
// above kMaxMicro (the plan refuses it with the largest limit named).
bool parseUsdAmount(const std::string& text, uint64_t& micro);

// ---- who may run them ------------------------------------------------------------

// The owner check of every spending command and /model, after the device's own owner
// gate: a Telegram group or channel (its chat id starts with '-') is refused even when
// it is on the owner list (everyone in it would see the figures and could confirm a
// change); a member is refused; the hold-to-talk channel ("voice") may read but not
// change a limit (a misheard amount must never reach a confirm). The owner's own
// Telegram chat, the web chat and the serial console have full access.
enum class BudgetAccess : uint8_t { Full, ReadOnly, NotOwner, Group };
BudgetAccess budgetAccessFor(const std::string& chatId, bool owner);
// The refusal for NotOwner / Group, the read-only note for ReadOnly ("" for Full).
std::string budgetAccessText(BudgetAccess a);

// ---- the parse -------------------------------------------------------------------

enum class BudgetVerb : uint8_t { Show, Set, Off, Confirm, Cancel, Help, Error };

struct BudgetRequest {
  BudgetVerb  verb = BudgetVerb::Error;
  std::string target;      // Set / Off: a provider slug or kBudgetAll
  uint64_t    micro = 0;   // Set: the new limit
  bool        force = false;
  std::string error;       // Error: what was wrong, in plain words
};

// The arguments after "/budget": "" -> Show; "confirm", "cancel", "help"; "<target>
// <amount> [usd] [force]" -> Set; "<target> off" -> Off; anything else -> Error.
BudgetRequest parseBudgetArgs(const std::string& args);

// The command that asks for `req` again ("/budget anthropic 70 force").
std::string budgetCommandText(const BudgetRequest& req);

// ---- what the device knows -------------------------------------------------------

// Why paid calls to a provider are held off besides its own limit: the park's pause
// causes (nimbus/orch/provider_park.h PauseCause bits) and refusal kind.
struct PauseFact {
  std::string provider;   // slug; "*" holds the causes that pause every provider
  uint8_t     paused = 0; // PauseCause bits
  uint8_t     parkKind = 0;   // ParkKind: 1 out of credit, 2 key refused
};

// One provider's count against its monthly limit (the spend store's row).
struct LimitRow {
  std::string provider;
  uint64_t    capMicro = 0;     // 0 = no limit
  uint64_t    spentMicro = 0;   // this period
  bool        frozen = false;
  std::string resetsOn;         // "1 November" ("" when the clock is unset)
};

// The spend store's state (agent::spend::stateJson) plus the park's pauses.
struct LimitsView {
  bool        loaded = false;
  uint64_t    limitMicro = SpendCeiling::kDefaultLimitMicro;
  uint64_t    spentMicro = 0;
  bool        ownerSet = false;
  bool        active = true;
  bool        frozen = false;
  uint32_t    monthKey = 0;
  std::string resetsOn;
  uint64_t    dailyWarnMicro = 0;   // 0 = the built-in warning
  uint64_t    todayMicro = 0;
  std::vector<LimitRow>  rows;
  std::vector<PauseFact> pauses;

  const LimitRow* row(const std::string& provider) const;
  // The pause causes of `provider` (its own plus every-provider ones) and its park kind.
  uint8_t pausedBy(const std::string& provider) const;
  uint8_t parkKindOf(const std::string& provider) const;
};

// The store's state JSON into `out` (rows and the overall figures; pauses untouched).
// False for a body that does not parse (out keeps loaded=false).
bool limitsViewFromJson(const std::string& stateJson, LimitsView& out);

// ---- the plan --------------------------------------------------------------------

enum class BudgetVerdict : uint8_t { Ok, Unchanged, NeedsForce, Refused };

struct BudgetPlan {
  BudgetVerdict verdict = BudgetVerdict::Refused;
  BudgetRequest req;
  bool     overall = false;      // the target is the overall ceiling
  uint64_t fromMicro = 0;        // the limit now (0 = none, or the ceiling off)
  uint64_t toMicro = 0;          // the limit after (0 = none)
  uint64_t spentMicro = 0;       // the target's count this period
  // The spend rules before and after the change.
  bool     overallActiveBefore = false, overallActiveAfter = false;
  bool     overallFrozenBefore = false, overallFrozenAfter = false;
  uint64_t overallLimitAfter = 0;
  bool     targetFrozenBefore = false, targetFrozenAfter = false;   // a provider's own limit
  std::vector<std::string> resumes;   // providers whose own-limit pause ends
  std::vector<std::string> pauses;    // providers whose own-limit pause starts
  std::string why;                    // NeedsForce / Refused / Unchanged: plain words
};

// The plan for a Set or Off request on the state `v` (re-run at confirm time on the
// state then, so a change made meanwhile on the web page is never overwritten blind).
BudgetPlan planBudgetChange(const BudgetRequest& req, const LimitsView& v);

// Whether this build answers the spending commands: the device installs them at
// start-up (src/agent/budget_command.cpp); the hosted instance has no such command, so
// its pause notices are worded without one. Unset = not installed.
void installBudgetCommands(bool on = true);
bool budgetCommandsInstalled();

// A suggested new limit that lifts a pause at `capMicro` with `spentMicro` counted:
// about half again over the larger of the two, in whole dollars, at least a dollar over
// the cap, at most kMaxMicro.
uint64_t suggestedLimitMicro(uint64_t capMicro, uint64_t spentMicro);
// Whether moving a limit from `fromMicro` (0 = none) to `toMicro` needs "force".
bool budgetNeedsForce(uint64_t fromMicro, uint64_t toMicro);
// "/budget anthropic 75" (with " force" when the suggestion needs it) for `target` (a
// provider slug or kBudgetAll) paused at `capMicro` with `spentMicro` counted.
std::string liftCommand(const std::string& target, uint64_t capMicro, uint64_t spentMicro);

// ---- the confirm window ----------------------------------------------------------

// One change waits for confirmation at a time, device-wide (RAM only: a restart drops
// it). nowMs is a monotonic millisecond clock (wraps safely).
class BudgetConfirm {
 public:
  enum class Ended : uint8_t { None, Confirmed, Expired, Cancelled, Replaced, OtherMessage, OtherChat };

  // `chat` proposes `req`; a change already waiting (from any chat) is replaced.
  void propose(const std::string& chat, const BudgetRequest& req, uint32_t nowMs);
  // "/budget confirm" from `chat`: true and the request when one waits for that chat
  // within the window. Otherwise false, and `why` says what happened to the last one:
  // Expired, Cancelled / OtherMessage / Replaced / OtherChat, or None (nothing ever
  // waited). A confirm from another chat cancels the waiting change (fail closed).
  bool take(const std::string& chat, uint32_t nowMs, BudgetRequest& out, Ended& why);
  // "/budget cancel" from `chat`: true when it cancelled a change of that chat.
  bool cancel(const std::string& chat);
  // Any other message from `chat` (a chat message or another command): cancels a change
  // that chat proposed; true when it did. Messages from other chats change nothing.
  bool noteOther(const std::string& chat);
  bool waiting() const { return waiting_; }
  // The request that waits (valid while waiting()).
  const BudgetRequest& request() const { return req_; }

 private:
  bool          waiting_ = false;
  std::string   chat_;
  BudgetRequest req_;
  uint32_t      atMs_ = 0;
  Ended         last_ = Ended::None;
  std::string   lastChat_;
  BudgetRequest lastReq_;
};

// ---- owner-facing copy (plain words, ASCII, reads well aloud) ----------------------

// "anthropic" -> "Anthropic", "your custom endpoint" -> "Your custom endpoint".
std::string capitalFirst(std::string s);

// The /usage reply. Not loaded: says the figures are not ready yet.
std::string usageText(const LimitsView& v);
// The proposal ("Change ... ? Send /budget confirm within 2 minutes ...") for an Ok
// plan, the refusal for NeedsForce / Refused, the "already" line for Unchanged. `v` is
// the state the plan ran on (its other pauses are named).
std::string proposalText(const BudgetPlan& p, const LimitsView& v);
// The reply once the change was stored (`p` re-planned at confirm time on `v`).
std::string appliedText(const BudgetPlan& p, const LimitsView& v);
// The reply to a confirm with nothing to apply (`why`, `last` = the ended request).
std::string confirmMissText(BudgetConfirm::Ended why, const BudgetRequest& last);
// The /budget usage lines.
std::string budgetHelpText();

// ---- /model ----------------------------------------------------------------------

struct LastTurnFacts {
  bool        known = false;     // a reply ran since the device started
  bool        ok = false;
  std::string host;              // the provider that answered (failover-final)
  std::string model;             // the model key it billed under ("" = none billed)
  std::string concreteModel;     // the exact id the provider reported ("" = not recorded)
  std::string requestedHost;     // the head the turn started on
  uint32_t    agoS = 0;
};

struct ModelFacts {
  std::string chosenHost;        // the owner's pick ("" = the first in the order)
  std::string runsOnHost;        // the head a turn starts on now (a keyless pick is skipped)
  std::string runsOnModel;       // its model (the owner's pick, else the default)
  LastTurnFacts last;
  LimitsView    limits;          // rows + pauses: what is paused and why
};

// The /model reply: the owner's choice, the provider and model the last reply ran on,
// and what is paused. Deterministic, no model call.
std::string modelText(const ModelFacts& f);

// ---- the Usage page's record ------------------------------------------------------

// One line of the change record ("Anthropic monthly limit $50.00 -> $70.00"), from the
// applied plan.
std::string changeSummary(const BudgetPlan& p);

}  // namespace orch
}  // namespace nimbus
