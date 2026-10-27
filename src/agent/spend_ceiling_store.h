#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/orch/provider_caps.h"
#include "nimbus/orch/spend_ceiling.h"

// spend_ceiling_store - the device side of the monthly spend limits (the pure rules live
// in lib/core nimbus/orch/spend_ceiling.h and provider_caps.h).
//
// One flash file, /data/spend_ceiling.txt, written atomically (fs_atomic), never NVS:
// the overall ceiling the owner set (or off), this month's count, each provider's count
// against the monthly limit the owner set for it (the usage ledger's budget cents),
// today's count against the owner's daily warning, the open freeze episodes and the
// notices they already sent. A full settings store changes nothing here; a failed write
// keeps the RAM state (the freezes still hold) and retries once a minute.
//
// Loaded lazily once the file area is mounted (Orchestrator mode). The first load on a
// device with no file is the first boot on this release: a device that already spent
// (its usage ledger has history) starts this month's counts from the ledger's estimates
// and owes the owner one notice (naming its provider limits when it has any).
//
// Thread-safe: the turn task commits spend, the web task edits the limits and reads the
// state, the Telegram poll task (orchTick) rolls the periods and pumps the notices, and
// the main loop only reads the lock-free screen line.

namespace agent {
namespace spend {

// Installs the paid-call gate, the pending sink, the seam meter's sink and the park
// listener. Once at boot, before any turn can run.
void install();

// The overall gate's answer (installed with nimbus::orch::installSpendFreeze): the
// stored count plus the turn in flight at or over the overall ceiling (never while it
// is off). Every report also hands each provider's freeze to the park. RAM only, cheap.
bool frozen();

// One provider's share of a committed spend (micro-USD).
struct Part {
  const char* provider;
  uint64_t micro;
};
// A turn's (or a fold's, or a paid call's) whole spend by provider. `turn`: it ends the
// turn in flight, so its pending part is replaced by this committed total.
void commit(const Part* parts, size_t n, bool turn);
// A billed round of the turn in flight on `host` (nimbus::orch::notePendingUsage's sink).
void addPending(const char* host, uint64_t micro);

// The owner's overall ceiling (0 = off). setLimitMicro answers false when the file could
// not be written (nothing changed: the route answers 507).
uint64_t limitMicro();
bool     setLimitMicro(uint64_t micro);
// The owner's daily warning (0 = the default per-provider rule); false when not stored.
bool     setDailyWarnMicro(uint64_t micro);
// The daily warning, lock-free (the default rule's check reads it on every turn).
uint64_t dailyWarnMicro();
// The owner changed a provider's budget: re-read the limits (a raised one lifts at once).
void capsChanged();
// Whether `provider` is at its monthly dollar limit, lock-free (the park's provider-limit
// cause). `known` is false until the limits file has loaded: the caller then falls back to
// the usage ledger's own estimate (fail-closed).
bool providerAtLimit(const char* provider, bool& known);

// Once a minute from the Telegram poll task (never the watched main loop: it writes the
// file): the period and day rollovers, the limits re-read, and a pending write's retry.
void tick();

// The notice pump (the Telegram poll task). nextNotice fills `text` with the first notice
// owed on `ch` and returns what it is; delivered() marks it sent once the channel took it.
struct Owed {
  nimbus::orch::SpendCeiling::Notice global = nimbus::orch::SpendCeiling::Notice::None;
  nimbus::orch::ProviderCaps::Owed provider;
  bool daily = false;
  bool any() const {
    return global != nimbus::orch::SpendCeiling::Notice::None ||
           provider.kind != nimbus::orch::ProviderCaps::Owed::None || daily;
  }
};
Owed nextNotice(nimbus::orch::NoticeChannel ch, std::string& text);
void delivered(nimbus::orch::NoticeChannel ch, const Owed& owed);

// The device screen's line while the overall ceiling holds: the title ("" when not
// frozen) and its detail. Lock-free (the watched main loop calls it every frame).
std::string screenLine(std::string& detail);

// The state for /api/state and /api/orch: {"limitCents":..,"spentCents":..,"frozen":..,
// "ownerSet":..,"active":..,"month":..,"resetsOn":"..","saved":..,"loaded":..,
// "dailyWarnCents":..,"todayCents":..,"providers":[{"p":..,"capCents":..,"spentCents":..,
// "frozen":..,"resetsOn":".."}]}. Empty on the web task while a commit holds the state
// across its flash write (nimbus/sys/web_guard.h): the caller serves its last answer.
std::string stateJson();
// The setup wizard line (defaults and the ceiling, plain words); empty like stateJson.
std::string setupText();

// Test builds: seed the counts (the bench SPENDSET verb). Not in production images.
#if defined(NIMBUS_TEST)
void testSetSpent(uint64_t micro);
void testSetProviderSpent(const char* provider, uint64_t micro);
void testSetDaySpent(uint64_t micro);
#endif

}  // namespace spend
}  // namespace agent
