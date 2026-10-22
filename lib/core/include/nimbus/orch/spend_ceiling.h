#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/turn_spend.h"

// spend_ceiling - the monthly spend ceiling across ALL providers combined (owner
// requirement, 21 Oct 2026). Portable (NO Arduino), host-tested in test/test_spend_ceiling.
//
// What it is:
//   - one counter per calendar month in the device timezone (the process zone the
//     device applies with setenv/tzset; UTC when no zone is set), estimated from
//     provider-reported usage at the meter seam, priced with the usage ledger's one
//     formula (every model at its own price, fail-closed for an unknown id);
//   - a ceiling the owner can change (default 20 USD); at the ceiling every paid seam
//     refuses BEFORE any request (the freeze is on the device's side: a provider on
//     auto-recharge never stops it, so the device never relies on it);
//   - one owner notice per channel per freeze episode, raised once, survives reboots;
//     raising the ceiling clears the freeze at once, the month rollover clears it with
//     no notice;
//   - the counter, the freeze and the notices-sent state live in ONE flash file
//     (/data/spend_ceiling.txt on the device, never NVS), so a full settings store
//     changes nothing.
//
// The clock (fail-closed money): spend made while the clock is unset counts toward the
// month the clock first reads; a clock set back never clears spend (the counter moves
// to the earlier month and keeps its total); only a later month starts a new count.

namespace nimbus {
namespace orch {

// The device's own "clock is set" threshold (2020-09-13, as memory::clockSynced).
inline constexpr time_t kCeilingClockValid = 1600000000;

// a + b, held at UINT64_MAX instead of wrapping (every money sum here).
inline uint64_t addSatMicro(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }

// YYYYMM of `now` in the process timezone; 0 when the clock is not set.
uint32_t ceilingMonthKey(time_t now);
// The month after `monthKey` (YYYYMM), 0 for 0.
uint32_t nextMonthKey(uint32_t monthKey);

// The owner channels a notice goes to.
enum class NoticeChannel : uint8_t { Telegram = 0, Web = 1, Screen = 2 };
inline constexpr int kNoticeChannelCount = 3;
const char* noticeChannelSlug(NoticeChannel c);   // "telegram" | "web" | "screen"

struct SpendCeiling {
  static constexpr uint64_t kDefaultLimitMicro = 20000000ULL;      // 20 USD
  static constexpr uint64_t kMinLimitMicro = 1000000ULL;           // 1 USD
  static constexpr uint64_t kMaxLimitMicro = 100000000000ULL;      // 100,000 USD
  static constexpr uint64_t kWarnPercent = 80;                     // the early warning

  uint64_t limitMicro = kDefaultLimitMicro;
  bool     ownerSet = false;     // the owner chose the ceiling (else the default applies)
  uint32_t monthKey = 0;         // YYYYMM the counter belongs to (0 = clock never read)
  uint64_t spentMicro = 0;       // this month's estimate, micro-USD
  bool     frozen = false;       // a freeze episode is open
  uint32_t episodes = 0;         // freeze episodes so far (diagnostics)
  uint8_t  noticesSent = 0;      // bit per NoticeChannel: the open episode's notices
  bool     warned = false;       // the 80 percent warning went out this month
  uint8_t  migrationOwed = 0;    // bit per NoticeChannel: the first-boot ceiling notice

  // The freeze decision for the stored count plus `pendingMicro` (the turn in
  // flight, not yet committed): true at or over the ceiling.
  bool frozenWith(uint64_t pendingMicro) const;

  // Each transition returns whether the stored state changed (the caller persists).
  // The month rollover for `nowMonth` (0 = clock unset: nothing moves).
  bool roll(uint32_t nowMonth);
  // Committed spend (rolls first). A crossing opens a freeze episode.
  bool add(uint64_t micro, uint32_t nowMonth);
  // A new ceiling, clamped to [kMinLimitMicro, kMaxLimitMicro]; marks it owner-set.
  // Raising it above the count closes the episode at once.
  bool setLimit(uint64_t micro);
  // The notice for channel `c` is owed: an open episode that has not told it yet.
  bool noticeOwed(NoticeChannel c) const;
  bool markNotice(NoticeChannel c);
  // The 80 percent warning is owed (once per month, never during a freeze).
  bool warnOwed() const;
  bool markWarned();
  // The first-boot ceiling notice (an existing device's first boot on this release).
  bool migrationNoticeOwed(NoticeChannel c) const;
  bool markMigrationNotice(NoticeChannel c);

  // The state a store starts from when it has no file: this month (0 = clock unset),
  // and for a device that already spent on an earlier release (`spentBefore`) this
  // month's estimate so far plus one first-boot notice owed on the web chat and, when a
  // Telegram bot is set, on Telegram. A new device starts at zero and owes nothing.
  static SpendCeiling firstBoot(uint32_t month, bool spentBefore, uint64_t monthToDateMicro,
                                bool telegram);

  // The notice owed on `ch` now, in priority order: the first-boot notice, this pause's
  // notice, then the 80 percent warning (the warning goes out once, on the first
  // channel that takes it). The screen draws its own line and owes only the first.
  enum class Notice : uint8_t { None = 0, Freeze, Warn, Migration };
  Notice nextNotice(NoticeChannel ch) const;
  // That notice went out on `ch`; whether the stored state changed. The first-boot
  // notice already says when paid calls are paused, so it also counts as this pause's
  // notice on that channel (never a second message about the same pause).
  bool markDelivered(NoticeChannel ch, Notice kind);

  // One text line: "!v|1;<limit>|<ownerSet>|<month>|<spent>|<frozen>|<episodes>|
  // <notices>|<warned>|<migration>". Fields are append-only (a missing tail field
  // reads 0); a malformed body leaves the defaults and answers false.
  std::string serialize() const;
  bool deserialize(const std::string& text);

 private:
  void settle();   // frozen follows spent >= limit; an edge resets the notices
};

// The owner's ceiling field (whole cents, as the web page posts it) in micro-USD,
// clamped to [kMinLimitMicro, kMaxLimitMicro]; 0 for anything that is not a positive
// whole number (empty, signs, letters, a fraction).
uint64_t parseCeilingCents(const std::string& field);

// ---- owner-facing copy (plain words, ASCII; goldens in test/golden/) ----------------

struct CeilingFacts {
  uint64_t limitMicro = SpendCeiling::kDefaultLimitMicro;
  uint64_t spentMicro = 0;
  uint32_t monthKey = 0;      // YYYYMM (0 = clock unset: no reset date named)
  int      dayOfMonth = 0;    // today in the device timezone (0 = unknown: no pace line)
  std::string headModel;      // the assistant's configured model ("" = none)
  bool     premiumHead = false;   // the head runs in its provider's priciest tier
};

// "$20.00": micro-USD as dollars and cents (rounded down to the cent).
std::string formatUsd(uint64_t micro);
// "1 November" for the month after `monthKey`; "" for 0.
std::string resetDateText(uint32_t monthKey);

// The freeze notice (Telegram and the web chat).
std::string freezeNoticeText(const CeilingFacts& f);
// The device screen's line while paused (the hold-to-talk line's place): a short title
// and a detail that wraps to two lines of the mic bar. Printable ASCII.
std::string freezeScreenLine(const CeilingFacts& f);
std::string freezeScreenDetail(const CeilingFacts& f);
// The early warning at 80 percent of the ceiling.
std::string warnNoticeText(const CeilingFacts& f);
// The first-boot notice on an existing device: what the ceiling is, what it means at
// this month's pace (and for a head in the priciest tier), where to change it.
std::string migrationNoticeText(const CeilingFacts& f);
// The setup wizard line stating the defaults and the ceiling.
std::string setupDefaultsText(uint64_t limitMicro);
// The text of a notice kind ("" for None).
std::string noticeText(SpendCeiling::Notice kind, const CeilingFacts& f);

// ---- the paid-call gate (process-wide) ---------------------------------------------

// Every paid seam asks paidCallAllowed() before its first request. The table is the
// contract the seam test enumerates: a seam that never consults fails it.
enum class SpendSeam : uint8_t {
  Head = 0,        // a head turn (owner, synthesis), before recall's embed
  Wire,            // every harness provider request (head rounds, sub dispatch, fold)
  Sub,             // a sub-agent dispatch
  Embed,           // an embedding (recall, memory writes, the pending queue)
  VisionNative,    // images handed to the head
  VisionFallback,  // the fallback captioner
  Stt,             // speech to text
  Tts,             // text to speech
  ImageGen,        // image generation
  UrlFetch,        // the url_fetch safety scan
  Search,          // web search
  Dream,           // the nightly dream
  Routine,         // a scheduled routine
  Wakeup,          // a one-time wakeup or reminder
  Fold,            // the compaction fold
  Moderation,      // the message classifier (inbound and outbound gates)
};
inline constexpr int kSpendSeamCount = 16;
const char* spendSeamSlug(SpendSeam s);

// The device installs the freeze answer (the ceiling store); unset = never frozen.
using SpendFrozenFn = bool (*)();
void installSpendFreeze(SpendFrozenFn fn);
bool spendFrozen();

// The freeze's other consumer: lane K2's per-provider park, which answers "paused" for
// every provider while the ceiling holds (its pause switch, cause "spend ceiling").
// The ceiling store reports every change of the freeze (and the state at load) through
// noteFreeze; the listener hears only changes, never a repeat.
using FreezeListener = void (*)(bool frozen);
void installFreezeListener(FreezeListener fn);
void noteFreeze(bool frozen);

// True when a paid call on `seam` may go out; false while the ceiling freezes paid
// calls. Every call is counted per seam (the seam test reads the counts).
bool paidCallAllowed(SpendSeam seam);
uint32_t paidGateConsults(SpendSeam seam);
uint32_t paidGateRefusals(SpendSeam seam);
void resetPaidGateCounts();

// The refusal text a seam reports when the gate says no (a fixed marker the failure
// classifiers read: never retried, never failed over, never parks a provider).
inline constexpr const char* kSpendFrozenError = "spend ceiling: paid calls are paused";
bool isSpendFrozenError(const std::string& err);

// ---- the turn in flight ------------------------------------------------------------
// A turn files its spend once, at its end; a long tool loop could cross the ceiling
// before that. Each billed round adds its priced usage here (installed pricer), the
// gate counts it at once, and the turn's committed spend replaces it.
using SpendPricer = uint64_t (*)(const TurnSpend& spend);   // micro-USD
using PendingSink = void (*)(uint64_t micro);
void installPendingSpend(SpendPricer pricer, PendingSink sink);
// The usage `host` billed between `before` and `after` (the same host's running
// counter) priced and handed to the sink. Nothing billed: nothing happens.
void notePendingUsage(const char* host, const char* configuredModel, const TokenUsage& before,
                      const TokenUsage& after);

// ---- the meter: what a paid call outside a turn spent --------------------------------
// A turn's requests are metered by the turn's own spend record. Every other paid call
// (a sub-agent, an embedding, a photo caption, speech, the download scan, moderation,
// an image) reports what it cost once the provider answered: the usage the provider
// reported when the answer carries it (meterSeamUsage), else a conservative estimate
// (meterSeamMicro; seam_meter.h prices it). The device and the hosted instance install
// the sink (the ceiling's counter); unset = nothing is counted (the lab).
using SeamSpendSink = void (*)(SpendSeam seam, const char* provider, const TurnSpend& spend,
                               uint64_t estimateMicro);
void installSeamMeter(SeamSpendSink sink);
// `u` in the provider's native meaning (tokenUsageFromJson); nothing billed: nothing
// happens. `model` is the model the request named (a Cumulo key reads its upstream).
void meterSeamUsage(SpendSeam seam, const char* provider, const char* model, const TokenUsage& u);
void meterSeamMicro(SpendSeam seam, const char* provider, uint64_t micro);
// Per-seam reports (host and test builds; the seam test reads them).
uint32_t seamMeterReports(SpendSeam seam);
void resetSeamMeterCounts();

}  // namespace orch
}  // namespace nimbus
