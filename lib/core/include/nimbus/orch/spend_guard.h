#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/token_usage.h"

// spend_guard - the rules that keep every paid call inside the monthly spend ceiling
// where the ceiling's own counter could be bypassed (lane QW4, owner requirement: max
// 20 USD a month before paid calls freeze, especially on accounts with auto-recharge).
// Portable (NO Arduino), host-tested in test/test_spend_guard.
//
//   - a custom endpoint on a paid host is never priced at 0 (model_price_defaults.h
//     holds the switch; customEndpointIsLocal decides it from the configured base URL);
//   - a message refused while paid calls are paused always gets a short reply saying so,
//     so the one freeze notice can never be the only way the owner learns of it;
//   - counts made while the clock is not set go into a provisional month and are
//     enforced at once; the first valid clock reconciles them with the usage ledger;
//   - a count read back from flash never reads lower than the usage ledger's own
//     estimate for the month (a write that failed before a restart is not forgiven).
namespace nimbus {
namespace orch {

// True when the custom endpoint's base URL names a host on the device's own network:
// loopback, a private or link-local IPv4 address (any spelling), the unspecified
// address, localhost, or an mDNS ".local" name. The base is read the way the custom
// adapter dials it ("http(s)://host[:port][/path]", "host", "host:port"); anything else
// - a public host, an IPv6 literal, userinfo, a query, an empty or unparsable base - is
// not local.
bool customEndpointIsLocal(const std::string& baseUrl);

// The custom endpoint counts as free (a local model) only when it is on the device's
// own network AND carries no key: a keyed endpoint, even on the LAN, is most likely a
// metered proxy (LiteLLM and friends in front of a paid provider), so it is priced as a
// paid service until the owner sets its price.
bool customEndpointIsFree(const std::string& baseUrl, bool hasKey);

// The short reply to a Telegram message the device did not answer because paid calls
// are paused (plain words, one paragraph): what happened and the one next step.
std::string frozenReplyText(const CeilingFacts& f);

// Whether a message that just finished on `telegramChat` gets that reply: the overall
// ceiling holds and nothing was sent to that chat while the message was handled (a
// command, or an honest failure reply, already answered it).
bool frozenReplyDue(bool telegramChat, bool frozen, bool answered);

// Round 3 (merge review 7): the paused reply, throttled. The owner's chat gets the full
// reply at most once per kFrozenReplyEveryMs, and again when a new freeze starts; any
// other chat (a member, a guest, a group) one neutral line per freeze
// (frozenReplyTextOther: no amount, no owner steps). `episode` names the freeze (its
// month and ceiling: raising the ceiling or a new month starts another). A bounded table:
// past its room the chat seen longest ago gives its place.
inline constexpr uint32_t kFrozenReplyEveryMs = 30u * 60u * 1000u;
class FrozenReplyThrottle {
 public:
  bool allow(const std::string& chat, bool owner, uint32_t episode, uint32_t nowMs);

 private:
  struct Slot {
    uint32_t chat = 0, episode = 0, atMs = 0;
    bool used = false;
  };
  static constexpr int kSlots = 16;
  Slot slots_[kSlots];
};
std::string frozenReplyTextOther();

// Round 3 (merge review major 5): a time base when the clock never sets (a network that
// blocks NTP; TLS works without a valid date on this hardware). The newest server date
// seen (an HTTPS answer's Date header, a Telegram update) plus the uptime since stands in
// for the clock, so the month still rolls; a set clock always wins, and an older date
// never moves it back. Thread-safe.
bool parseHttpDate(const char* s, int64_t& epoch);   // IMF-fixdate, "Tue, 27 Oct 2026 03:10:00 GMT"
class ServerClock {
 public:
  void note(int64_t epoch, uint32_t nowMs);
  // `systemNow` when it is a set clock, else the server date plus the uptime since, else
  // `systemNow` (unset).
  int64_t epoch(int64_t systemNow, uint32_t nowMs) const;

 private:
  int64_t date_ = 0;
  uint32_t atMs_ = 0;
};
ServerClock& serverClock();                              // the device's one
void noteServerDate(int64_t epoch, uint32_t nowMs);      // serverClock().note
// A response's Date header value (after "Date:"), noted when it parses.
void noteHttpDateHeader(const char* value, uint32_t nowMs);
// Round 3 review: whose dates are trusted: a provider's or Telegram's API host (over TLS,
// the caller checks), never a custom endpoint or any other host.
bool trustedDateHost(const std::string& host);
// No date past this is believed (2040-01-01), nor one more than kServerDateMaxJumpS ahead
// of the time already known.
inline constexpr int64_t kServerDateMax = 2208988800LL;
inline constexpr int64_t kServerDateMaxJumpS = 7LL * 86400;

// The first valid clock (`month` non-zero) after counts made without one: a provisional
// count (month 0) moves into `month` (SpendCeiling::roll keeps it), and when the store
// owed a seed (it started without the ledger's estimate of this month, which needs the
// date) `ledgerMonthToDateMicro` is added on top, even if a commit already moved the
// count into the month. A count still filed under an earlier month (a restart with no
// clock after the month changed) starts the new month, keeping `provisionalMicro`, the
// spend made while the clock was unset. Fail-closed: the provisional window may be
// counted twice, never missed. Returns whether the state changed.
bool reconcileProvisional(SpendCeiling& c, uint32_t month, bool seedOwed,
                          uint64_t ledgerMonthToDateMicro, uint64_t provisionalMicro = 0);

// What a start without a clock owes the counts once the clock is set (lane QW4): the
// usage ledger's estimate of this month (the seed: the first file of a device that spent
// before, a recount, or an earlier-release file needs the date), a floor at that
// estimate (a load made with no clock), and the spend committed meanwhile. The device
// keeps the seed and the provisional spend in a small marker file beside the ceiling
// file, so a restart before the clock is set keeps them.
struct ClockDebt {
  uint8_t  seed = 0;          // 0 none; 1 the ledger's month-to-date; 2 that and the first-boot notice
  bool     floor = false;     // floor at the ledger once the clock is set (RAM: each load decides)
  uint64_t provisional = 0;   // spend committed while the clock was unset

  bool owed() const { return seed || floor || provisional; }
  bool needsLedger() const { return seed || floor; }   // settle() reads the ledger's estimate
  // The marker's content: what must survive a restart ("" when nothing does).
  std::string serialize() const;
  // "<seed>|<provisional>", or an earlier marker's lone "1" / "2"; false (state kept at
  // zero, the seed owed) for anything else, since a marker exists only when something
  // was owed (fail-closed).
  bool deserialize(const std::string& text);
  // A commit of `micro` with the clock set or not.
  void noteCommit(uint64_t micro, bool clock);

  struct Settled {
    bool changed = false;     // the ceiling's state changed
    bool seeded = false;      // the ledger's estimates were added: seed the providers and today too
    bool migration = false;   // and the first-boot notice is owed
  };
  // The clock reads `month` (0: nothing happens): settle the debt into `c`, the ledger's
  // estimate of this month being `ledgerMonthToDateMicro` (read only when needsLedger()).
  Settled settle(SpendCeiling& c, uint32_t month, uint64_t ledgerMonthToDateMicro);
};

// A count read back from flash, for the month `month` (non-zero), never below the usage
// ledger's estimate of that month: a commit whose file write failed before a restart is
// still counted (the ledger writes its own file). Returns whether the count rose.
bool floorFromLedger(SpendCeiling& c, uint32_t month, uint64_t ledgerMonthToDateMicro);

// A provider verify's usability probe and the Tavily verify are paid requests (a
// fraction of a cent each). The device's own re-checks skip them while the ceiling
// pauses paid calls or the provider's park is closed; the owner's Verify always runs
// them (a topped-up account proves itself at once). Either way they are counted.
bool verifyProbeAllowed(bool ownerAsked, bool frozen, bool parkOpen);

// The body of a raw HTTP answer (everything after the blank line that ends the
// headers), or "" when the headers never ended. The probe's metered usage is read from
// it; a cut body prices at the high estimate instead.
std::string httpBodyOf(const std::string& raw);

// Lane QW4 (CUM-778, owner ruling): the one message a chat gets when the spend ceiling or
// a provider's monthly limit stops its running helpers (sent as the stops are asked). `labels` are the helpers' owner
// labels; `ceiling` says the overall ceiling paused them, else `providers` (display
// names) are the ones whose limits did. Plain words, ASCII.
std::string helpersStoppedText(const std::vector<std::string>& labels, bool ceiling,
                               const std::vector<std::string>& providers);

// Lane QW4 (CUM-776): a paid request that left the device and got no usable answer (no
// response, or an answer cut in its headers or body; a 4xx or 5xx refusal is not one).
// The provider may well have billed it, so it is
// counted at a conservative estimate: one input token per two characters sent (each
// photo the request carried counts kImageEstimateChars), no output tokens.
// `transportError` is the transport's error text; `status` the HTTP status it returned.
bool sentWithoutAnswer(int status, const std::string& transportError);
TokenUsage sentRequestEstimate(size_t bodyChars, size_t images);

// Round 3 (merge review major 6): the full estimate for such a request, by what is known
// of it. Input: the sent body (sentRequestEstimate), and at least the session estimate
// when the request starts a run on the provider's side (an OpenAI background job, a
// Mistral Conversations run). Output: what a model can have written while the request
// was held open (kEstimateOutputTokensPerSec a second), capped by the request's own
// output cap when it names one.
inline constexpr uint32_t kEstimateOutputTokensPerSec = 200;
struct UnansweredRequest {
  size_t bodyChars = 0;
  size_t images = 0;
  uint32_t heldMs = 0;      // from the send to the failure
  uint32_t outputCap = 0;   // 0 = the request names none
  bool startsRun = false;
};
TokenUsage unansweredEstimate(const UnansweredRequest& r);
// The output cap a request body names (max_tokens, max_output_tokens or
// max_completion_tokens; 0 when none), and whether it starts a server-side run.
uint32_t bodyOutputCap(const std::string& body);
bool bodyStartsRun(const std::string& path, const std::string& body);

// ---- breakers in front of the model (PRD section 7; lane RB-X3) ------------------------------
// Per chat, scaled to the head. They stop runaways (a routine or agent that fires itself in a loop,
// unattended spend out of bounds), never honest work, and never the owner (ruling 84 item 10):
// both count only turns that no person's message started, and a person's message is always
// admitted, also while they are paused. Commands never pass through them (the binding answers a
// command before any model turn), so /resume and every other command still run.
//   - turn rate: more than kTurnRateMaxTurns such turns started in kTurnRateWindowMs pauses;
//   - spend rate: such turns' spend in the rolling kSpendRateWindowMs above
//     spendRateCapMicro(monthly ceiling, the head's blended cost per turn) pauses.
// A pause refuses those turns with one line (lineDue once per pause) until /resume (resume()),
// which also clears both windows. No breaker changes a profile or the window: the blended cost is
// read at the profile in force (full by default; on engine 1 RB-BG's installedHistoryProfile()),
// and a pause only holds turns back.
inline constexpr uint32_t kTurnRateWindowMs = 10u * 60u * 1000u;
inline constexpr uint8_t kTurnRateMaxTurns = 20;
inline constexpr uint32_t kSpendRateWindowMs = 60u * 60u * 1000u;
inline constexpr uint64_t kSpendRateFloorMicro = 1000000;      // 1.00 USD
inline constexpr uint32_t kSpendRateCeilingPermille = 100;     // 10% of the monthly ceiling
inline constexpr uint32_t kSpendRateTurns = 15;                // 15 x the head's blended cost per turn
// The measured full arm (RB-T4, claude-sonnet-5-5 at full: 1.72 requests a turn, 56.55% of turns
// open cold): a turn is one first request, cold at that share and warm otherwise, plus 0.72 warm.
inline constexpr uint32_t kRequestsPerTurnMilli = 1720;
inline constexpr uint32_t kColdTurnPermille = 566;

// The head's blended cost per turn from one warm and one cold request at the profile in force
// (plain::HeadCost's warm and cold request prices on engine 2).
uint64_t blendedTurnMicro(uint64_t warmRequestMicro, uint64_t coldRequestMicro);
// max(1.00 USD, 10% of the monthly ceiling (0: none set), 15 x the blended cost per turn).
uint64_t spendRateCapMicro(uint64_t monthlyCeilingMicro, uint64_t blendedTurnMicro);

enum class BreakerTrip : uint8_t { None = 0, TurnRate = 1, SpendRate = 2 };
const char* breakerTripSlug(BreakerTrip t);   // "none", "turn-rate", "spend-rate": frozen

struct BreakerVerdict {
  bool run = true;          // the model turn may start
  bool lineDue = false;     // send breakerPausedText now (the pause's one line)
  BreakerTrip why = BreakerTrip::None;
};

class ChatBreaker {
 public:
  void setSpendCap(uint64_t capMicro) { cap_ = capMicro ? capMicro : kSpendRateFloorMicro; }
  uint64_t spendCap() const { return cap_; }
  // A model turn is about to start; `person`: a person's message started it.
  BreakerVerdict admit(bool person, uint32_t nowMs);
  // A turn's spend, as it is committed.
  void spent(bool person, uint64_t micro, uint32_t nowMs);
  // /resume: lifts a pause and clears both windows; true when a pause was lifted.
  bool resume();
  bool paused() const { return why_ != BreakerTrip::None; }
  BreakerTrip why() const { return why_; }
  uint64_t windowSpendMicro(uint32_t nowMs) const;   // unattended spend in the rolling hour
  uint8_t windowTurns(uint32_t nowMs) const;          // unattended turns started in the last 10 min

 private:
  struct Bucket {
    uint32_t atMs = 0;
    uint64_t micro = 0;
  };
  static constexpr int kBuckets = 60;   // one a minute
  uint32_t starts_[kTurnRateMaxTurns] = {};
  uint8_t nStarts_ = 0, next_ = 0;
  Bucket spend_[kBuckets];
  uint64_t cap_ = kSpendRateFloorMicro;
  BreakerTrip why_ = BreakerTrip::None;
  bool lineSent_ = false;
};

// The pause's one line (owner-facing, plain words): what happened and the one next step.
std::string breakerPausedText(BreakerTrip why, uint64_t capMicro);
// The answer to /resume.
std::string breakerResumedText(bool wasPaused);

}  // namespace orch
}  // namespace nimbus
