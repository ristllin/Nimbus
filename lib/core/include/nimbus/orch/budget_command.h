#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/command.h"
#include "nimbus/orch/spend_ceiling.h"

// budget_command - the owner's spending commands over Telegram and the web chat:
// /usage (and a bare /budget), /budget <provider|all> <USD> [force], /budget <provider>
// off [force], /budget confirm and /budget cancel, and /model. Portable (NO Arduino),
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
//   - defensive bounds: a limit is at least kMinMicro and at most kMaxMicro, even with
//     "force". The word "force" after the amount is needed for a raise of an existing
//     limit above kNoForceMaxMicro or to more than kMaxRaiseFactor times it, for a first
//     provider limit that turns the default overall ceiling off (every provider without
//     a limit of its own then has no monthly stop), and for "off" when nothing else would
//     limit that provider afterwards, or when the overall ceiling that then limits it is
//     such a raise over the limit removed. A new limit or a lower one needs nothing;
//   - no change while the device's clock is not set, and none from a message sent more
//     than the confirm window before it arrived; a confirm counts only when its message
//     was sent after the proposal's and within the window (a page Telegram serves again
//     after a power-on can never re-apply an old change); a voice note's transcript may
//     read but not change (speech can be misheard);
//   - the confirm applies exactly the effect the proposal stated: it is re-planned on the
//     state then and refused when anything it said would differ;
//   - the overall ceiling is turned off only on the web page;
//   - the effect a reply states is the spend rules re-run on the changed limits
//     (SpendCeiling for the overall ceiling, "at or over the limit" for a provider, as
//     ProviderCaps decides), so a change that turns the ceiling off or back on, or that
//     is still under this month's spend, says so before the owner confirms it.
// Who may run them (owner chats only, never a group or a guest) is decided by
// budgetAccessFor after the device's own owner gate.
namespace nimbus {
namespace orch {

struct BudgetBounds {
  static constexpr uint64_t kMinMicro = SpendCeiling::kMinLimitMicro;   // 1 USD
  static constexpr uint64_t kMaxMicro = SpendCeiling::kMaxLimitMicro;   // 100,000 USD, even with force
  static constexpr uint64_t kNoForceMaxMicro = 250000000ULL;            // a raise above 250 USD needs force
  static constexpr uint64_t kMaxRaiseFactor = 10;                       // more than 10x needs force
  static constexpr uint32_t kConfirmWindowMs = 120000;                  // 2 minutes
  static_assert(kConfirmWindowMs % 60000 == 0, "the copy states the window in whole minutes");
};

// The target word for the overall ceiling.
inline constexpr const char* kBudgetAll = "all";

// Every provider a monthly limit can be set for (the slots of provider_slots.h, then the
// custom endpoint and Tavily), in that order; the copy and the docs name the same list.
std::vector<std::string> budgetProviders();
// The provider slug a typed word names ("anthropic", "z.ai" -> "zai"), "all" for the
// overall ceiling, "" for anything else. Case-insensitive.
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
// A Telegram voice note's transcript (`voiceNote`) is read-only like hold-to-talk.
enum class BudgetAccess : uint8_t { Full, ReadOnly, NotOwner, Group };
BudgetAccess budgetAccessFor(const std::string& chatId, bool owner, bool voiceNote = false);

// What the device knows about the message a command came in: its Telegram send time
// (message.date, server seconds; 0 for the web chat, serial and hold-to-talk) and
// whether it is a voice note's transcript.
struct BudgetMsg {
  int64_t date = 0;
  bool    voiceNote = false;
};

// The refusal for a change (a proposal or a confirm) arriving at wall clock `nowEpoch`
// ("" = it may go on): the clock not set yet (the receive path's late-message guard and
// the message dates both need it), or a message sent more than the confirm window
// before it arrived.
std::string budgetTimingRefusal(int64_t nowEpoch, int64_t msgDate);
// The refusal for NotOwner / Group, the read-only note for ReadOnly ("" for Full).
std::string budgetAccessText(BudgetAccess a);

// Whether a slash command still runs in the web chat while the overall ceiling pauses
// paid calls: the spending commands, /model and /help (they never call a model). Any
// other text gets the web chat's "paid calls are paused" answer at once.
bool runsWhilePaused(const std::string& text);

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
// <amount> [usd] [force]" -> Set; "<target> off [force]" -> Off; anything else -> Error.
BudgetRequest parseBudgetArgs(const std::string& args);

// The command that asks for `req` again ("/budget anthropic 70 force").
std::string budgetCommandText(const BudgetRequest& req);

// Whether a message keeps a change that waits for confirmation (from the same chat):
// only "/budget confirm", "/budget cancel" (they decide it) and a new /budget change (it
// replaces it; a refused one cancels it, BudgetConfirm::offer). Anything else cancels.
bool budgetKeepsWaiting(const Command& cmd);

// ---- what the device knows -------------------------------------------------------

// Why paid calls to a provider are held off besides its own dollar limit.
struct PauseFact {
  std::string provider;           // slug; "*" holds the causes that pause every provider
  uint8_t     paused = 0;         // the park's PauseCause bits (provider_park.h)
  uint8_t     parkKind = 0;       // the park's ParkKind: 1 out of credit, 2 key refused
  bool        usageLimit = false; // its token or call budget is reached (the usage ledger)
};

// One provider's count against its monthly limit (the spend store's row).
struct LimitRow {
  std::string provider;
  uint64_t    capMicro = 0;     // 0 = no limit
  uint64_t    spentMicro = 0;   // this period
  bool        frozen = false;
  std::string resetsOn;         // "1 November" ("" when the clock is unset)
  // At or over its limit (the stored episode, or the count against the cap).
  bool atLimit() const { return capMicro && (frozen || spentMicro >= capMicro); }
};

// The spend store's state (agent::spend::stateJson) plus the other pauses.
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
  bool anyLimit() const;   // some provider has a dollar limit
  // The pause causes of `provider` (its own plus every-provider ones) and its park kind.
  uint8_t pausedBy(const std::string& provider) const;
  uint8_t parkKindOf(const std::string& provider) const;
  bool    usageLimited(const std::string& provider) const;
};

// A provider's token or call budget is used up for its period (the usage ledger's own
// rule: a limit of 0 is none). The device reads the counters from the ledger's budget
// list; until the provider's next paid call after its reset day they still hold the old
// period's figures.
inline bool usageBudgetReached(uint64_t tokens, uint64_t tokenLimit, uint64_t calls, uint64_t callLimit) {
  return (tokenLimit && tokens >= tokenLimit) || (callLimit && calls >= callLimit);
}

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
  uint64_t overallLimitAfter = 0;   // the overall ceiling after the change (when active)
  bool     targetFrozenBefore = false, targetFrozenAfter = false;   // a provider's own limit
  std::string why;               // NeedsForce / Refused / Unchanged: plain words
};

// The plan for a Set or Off request on the state `v` (re-run at confirm time on the
// state then, so a change made meanwhile on the web page is never overwritten blind).
BudgetPlan planBudgetChange(const BudgetRequest& req, const LimitsView& v);
// Whether two Ok plans state the same change and the same effects (the limit before and
// after, the overall ceiling and both pauses before and after): a confirm applies only
// when the plan at confirm time is the one the owner was shown.
bool samePlannedEffect(const BudgetPlan& shown, const BudgetPlan& now);

// Whether this build answers the spending commands: the device installs them at
// start-up (src/agent/budget_command.cpp); the hosted instance has no such command, so
// its pause notices are worded without one. Unset = not installed. (A process-wide
// switch rather than a field of CeilingFacts/CapFacts: the stores that fill those facts
// belong to the spend-ceiling seams and the hosted instance, which this lane leaves be.)
void installBudgetCommands(bool on = true);
bool budgetCommandsInstalled();

// A suggested new limit that lifts a pause at `capMicro` with `spentMicro` counted:
// about half again over the larger of the two, in whole dollars (tens from 100 USD,
// hundreds from 1,000 USD), at least a dollar over the cap, at most kMaxMicro.
uint64_t suggestedLimitMicro(uint64_t capMicro, uint64_t spentMicro);
// Whether a raise of an existing limit from `fromMicro` (0 = none) to `toMicro` needs
// "force" by the bounds alone (the plan adds the overall-ceiling and "off" cases).
bool budgetNeedsForce(uint64_t fromMicro, uint64_t toMicro);
// "/budget anthropic 75" (with " force" when the suggestion needs it) for `target` (a
// provider slug or kBudgetAll) paused at `capMicro` with `spentMicro` counted.
std::string liftCommand(const std::string& target, uint64_t capMicro, uint64_t spentMicro);
// The sentence a pause notice ends with: "Reply <liftCommand> and then /budget confirm
// to raise <what> to $X." ("" on a build without the commands).
std::string liftSentence(const std::string& target, const std::string& what, uint64_t capMicro,
                         uint64_t spentMicro);

// ---- the confirm window ----------------------------------------------------------

// One change waits for confirmation at a time, device-wide (RAM only: a restart drops
// it). nowMs is a monotonic 64-bit millisecond clock.
class BudgetConfirm {
 public:
  enum class Ended : uint8_t {
    None,                // nothing ever waited for that chat
    Confirmed,           // it was applied
    Expired,             // the confirm came after the window
    Cancelled,           // /budget cancel
    OtherMessage,        // another message from that chat
    OtherChat,           // (to the confirming chat) nothing waited here; a change waiting elsewhere is dropped
    ConfirmedElsewhere,  // (to the proposing chat) a confirm from another chat dropped it
    Failed,              // confirmed, but it could not be applied
    OutOfOrder,          // the confirm's message was sent before the proposal's
  };

  // `chat` proposed the change `plan` states, in a message sent at `msgDate` (0 = no
  // Telegram date): it waits when the plan is Ok (replacing any waiting change); a
  // refused proposal cancels that chat's waiting change instead.
  void offer(const std::string& chat, const BudgetPlan& plan, uint64_t nowMs, int64_t msgDate = 0);
  // "/budget confirm" from `chat` in a message sent at `msgDate`: true and the plan the
  // owner was shown when one waits for that chat within the window, by the device's
  // clock and, when both messages carry a Telegram date, by theirs (the confirm sent
  // after the proposal, within the window). Otherwise false, and `why` says what
  // happened to the last one (`out` = its plan, for "send it again"). A confirm from
  // another chat drops the waiting change (fail closed).
  bool take(const std::string& chat, uint64_t nowMs, int64_t msgDate, BudgetPlan& out, Ended& why);
  // The confirmed change of `chat` could not be applied (the next confirm says so).
  void failed(const std::string& chat);
  // "/budget cancel" from `chat`: true when it cancelled a change of that chat.
  bool cancel(const std::string& chat);
  // Any other message from `chat`: cancels a change that chat proposed; true when it
  // did. Messages from other chats change nothing.
  bool noteOther(const std::string& chat);
  bool waiting() const { return waiting_; }

 private:
  void end(Ended why, const std::string& chat);

  bool        waiting_ = false;
  std::string chat_;
  BudgetPlan  plan_;
  uint64_t    atMs_ = 0;
  int64_t     msgDate_ = 0;
  Ended       last_ = Ended::None;
  std::string lastChat_;
  BudgetPlan  lastPlan_;
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
// The reply to a confirm whose re-plan (on the state at confirm time) is not Ok, or
// states another effect than the proposal did.
std::string replanRefusedText(const BudgetPlan& p);
// The reply when a confirmed change could not be stored (nothing changed).
std::string storeFailedText(const BudgetRequest& req);
// The reply to a confirm with nothing to apply (`why`, `last` = the ended request).
std::string confirmMissText(BudgetConfirm::Ended why, const BudgetRequest& last);
// The reply to /budget cancel (`cancelled`: a change was waiting).
std::string cancelText(bool cancelled);
// The /budget usage lines.
std::string budgetHelpText();

// ---- /model ----------------------------------------------------------------------

struct LastTurnFacts {
  bool        known = false;     // a reply ran since the device started
  bool        ok = false;
  std::string host;              // the provider that answered (failover-final)
  std::string model;             // the model key it billed under ("" = none billed)
  std::string concreteModel;     // the exact id the provider reported ("" = not recorded:
                                 // the turn result does not carry it yet; lane HFM adds it)
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

// One line of the change record ("Anthropic monthly limit changed from $50.00 to
// $70.00"), from the applied plan.
std::string changeSummary(const BudgetPlan& p);

}  // namespace orch
}  // namespace nimbus
