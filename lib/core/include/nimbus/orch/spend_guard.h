#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/spend_ceiling.h"

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

}  // namespace orch
}  // namespace nimbus
