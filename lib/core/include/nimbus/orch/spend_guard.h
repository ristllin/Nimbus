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

}  // namespace orch
}  // namespace nimbus
