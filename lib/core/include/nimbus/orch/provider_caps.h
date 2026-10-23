#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/spend_ceiling.h"

// provider_caps - each provider's monthly spend cap as a hard freeze, and the owner's
// daily spend warning (lane DEF2, owner request 22 Oct 2026). Portable (NO Arduino),
// host-tested in test/test_provider_caps.
//
// The cap is the owner's existing per-provider budget field (cents per period, the
// "budget=prov:tok:call:resetDay:cents" setting kept by the usage ledger). This module
// counts every metered call of each provider over that budget's period (the calendar
// month for reset day 1, in the device's time zone), and at the cap the provider is
// frozen: every paid seam of THAT provider refuses (the store hands the freeze to the
// per-provider park's pause switch with its own cause), while the others keep working.
// One owner notice per channel per freeze episode names the provider and the cap; an
// early warning goes out at 80 percent; raising the cap lifts the freeze at once; the
// next period starts a new count quietly. A provider with no cap is never frozen here.
//
// The owner's daily warning: once today's spend across every provider (device date)
// reaches the owner's threshold, one notice per channel that day. It replaces the
// default per-provider daily rule while it is set.
namespace nimbus {
namespace orch {

// A local calendar date (the device's time zone); year 0 = the clock is not set.
struct LocalDate {
  int year = 0, month = 0, day = 0;
  bool valid() const { return year > 0 && month >= 1 && month <= 12 && day >= 1; }
};
LocalDate localDateOf(time_t now);   // the process time zone; invalid when unset

// One owner cap: the provider, its cap (micro-USD, 0 = none) and the budget's reset day.
struct CapRule {
  std::string provider;
  uint64_t capMicro = 0;
  uint8_t resetDay = 1;
};

struct ProviderCaps {
  static constexpr size_t kMaxRows = 8;
  static constexpr uint64_t kWarnPercent = 80;

  struct Row {
    char     provider[16] = {};
    uint32_t periodKey = 0;     // the budget period the count belongs to (0 = clock unset)
    uint64_t spentMicro = 0;    // this period, every metered call of the provider
    bool     frozen = false;    // a freeze episode is open
    uint8_t  noticesSent = 0;   // bit per NoticeChannel: this episode's notices
    bool     warned = false;    // the 80 percent warning went out this period
    // From the cap table (not stored: the usage ledger's budgets own it).
    uint64_t capMicro = 0;
    uint8_t  resetDay = 1;
  };
  Row     rows[kMaxRows];
  uint8_t n = 0;

  int find(const char* provider) const;   // -1 when absent
  bool anyCap() const;                    // some provider has a cap
  // The owner's cap table. A capped provider gets a row; a cap removed leaves the row
  // (its count) uncapped. Returns whether the stored state changed.
  bool setRules(const std::vector<CapRule>& rules, const LocalDate& today);
  // Committed spend of `provider` (rolls its period first; makes the row).
  bool add(const char* provider, uint64_t micro, const LocalDate& today);
  // Every row's period rollover (a later period: a new count, no notice).
  bool roll(const LocalDate& today);
  // The provider's count starts from `micro` (the first load: the ledger's estimate).
  bool seed(const char* provider, uint64_t micro, const LocalDate& today);
  // The freeze decision for the stored count plus `pendingMicro` (its turn in flight).
  bool frozenWith(const char* provider, uint64_t pendingMicro) const;

  // The provider notice owed on `ch`: a freeze episode's (Telegram and the web chat),
  // then an 80 percent warning (once per period, on the first channel that takes it).
  struct Owed {
    enum Kind : uint8_t { None = 0, Freeze, Warn } kind = None;
    int row = -1;
  };
  Owed nextOwed(NoticeChannel ch) const;
  bool markDelivered(NoticeChannel ch, const Owed& o);

 private:
  int  ensure(const char* provider);
  // A full table gives a limited provider the row of one no rule limits (and not paused).
  int  ensureRuled(const char* provider, const std::vector<CapRule>& rules);
  bool settle(Row& r);   // frozen follows the cap; an edge resets the notices
  static uint32_t periodOf(const Row& r, const LocalDate& today);
};

// The owner's daily spend warning: today's spend across every provider.
struct DaySpend {
  uint64_t thresholdMicro = 0;   // the owner's daily warning (0 = the default rule)
  uint32_t dayKey = 0;           // the local day the count belongs to (civilDayKey; 0 = unset)
  uint64_t spentMicro = 0;       // today's spend, every provider, every metered call
  uint8_t  told = 0;             // bit per NoticeChannel: today's warning went out

  bool add(uint64_t micro, uint32_t today);   // rolls to `today` first
  bool roll(uint32_t today);
  bool owed(NoticeChannel ch) const;          // Telegram and the web chat
  bool mark(NoticeChannel ch);
  bool setThreshold(uint64_t micro);          // 0 = back to the default rule
};

// The one file holds the global ceiling, the day and the provider counts:
// SpendCeiling's line, then "|<daily>|<dayKey>|<daySpent>|<dayTold>" (append-only main
// fields, so an earlier build still reads its own nine), then one
// ";p|<provider>|<period>|<spent>|<frozen>|<notices>|<warned>" group per provider.
std::string serializeCeilingFile(const SpendCeiling& c, const DaySpend& d, const ProviderCaps& p);
// False when the main line is malformed (the provider and day parts never fail it: a
// bad group is skipped). `earlier` (optional): the file is from a release before these
// parts (its main line has no daily fields).
bool deserializeCeilingFile(const std::string& text, SpendCeiling& c, DaySpend& d,
                            ProviderCaps& p, bool* earlier = nullptr);

// ---- owner-facing copy ---------------------------------------------------------------

// The provider's display name ("Anthropic"; an unknown slug as it is).
std::string providerName(const std::string& slug);
// "1 November" for the period after `periodKey` with `resetDay` ("" for 0).
std::string capResetText(uint32_t periodKey, uint8_t resetDay);

struct CapFacts {
  std::string provider;   // slug
  uint64_t capMicro = 0;
  uint64_t spentMicro = 0;
  uint32_t periodKey = 0;
  uint8_t  resetDay = 1;
};
std::string providerFreezeText(const CapFacts& f);
std::string providerWarnText(const CapFacts& f);
std::string dailyWarnText(uint64_t spentMicro, uint64_t thresholdMicro);
// The first-boot line on a device with provider caps: which caps apply, that every
// other provider has none, and whether the overall ceiling applies.
std::string capsMigrationText(const std::vector<CapRule>& rules, bool overallActive,
                              uint64_t overallLimitMicro);

}  // namespace orch
}  // namespace nimbus
