#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/spend_ceiling.h"

// spend_ceiling_store - the device side of the monthly spend ceiling (the pure rules
// live in lib/core nimbus/orch/spend_ceiling.h).
//
// One flash file, /data/spend_ceiling.txt, written atomically (fs_atomic), never NVS:
// the ceiling the owner set, this month's count, the open freeze episode and the
// notices it already sent. A full settings store changes nothing here; a failed
// write keeps the RAM state (the freeze still holds) and retries once a minute.
//
// Loaded lazily once the file area is mounted (Orchestrator mode). The first load on
// a device with no file is the first boot on this release: a device that already
// spent (its usage ledger has history) starts this month's count from the ledger's
// month-to-date estimate and owes the owner one notice about the ceiling.
//
// Thread-safe: the turn task commits spend, the web task edits the ceiling and reads
// the state, the Telegram poll task (orchTick) rolls the month and pumps the notices,
// and the main loop only reads the lock-free screen line.

namespace agent {
namespace spend {

// Installs the paid-call gate (nimbus::orch::installSpendFreeze) and the pending sink
// (installPendingSpend, priced by store::priceSpendMicro). Once at boot, before any
// turn can run.
void install();

// The gate's answer (installed with nimbus::orch::installSpendFreeze): the stored
// count plus the turn in flight at or over the ceiling. RAM only, cheap.
bool frozen();

// A turn's (or a fold's) whole spend, priced (micro-USD). `turn`: it ends the turn in
// flight, so its pending part is replaced by this committed total.
void commit(uint64_t micro, bool turn);
// A billed round of the turn in flight (nimbus::orch::notePendingUsage's sink).
void addPending(uint64_t micro);

// The owner's ceiling. setLimitMicro answers false when the file could not be
// written (nothing changed: the route answers 507).
uint64_t limitMicro();
bool     setLimitMicro(uint64_t micro);

// Once a minute from the Telegram poll task (never the watched main loop: it writes
// the file): the month rollover and a pending write's retry.
void tick();

// The notice pump (the Telegram poll task). nextNotice fills `text` with the first
// notice owed on `ch` (a freeze episode's, the 80 percent warning, or the first-boot
// ceiling notice) and returns its kind; delivered() marks it sent once the channel took it.
using NoticeKind = nimbus::orch::SpendCeiling::Notice;
NoticeKind nextNotice(nimbus::orch::NoticeChannel ch, std::string& text);
void delivered(nimbus::orch::NoticeChannel ch, NoticeKind kind);

// The device screen's line while frozen: the title ("" when not frozen) and its detail.
// Lock-free (the watched main loop calls it every frame): the view the last state
// change published.
std::string screenLine(std::string& detail);

// The state for /api/state and /api/orch: {"limitCents":..,"spentCents":..,
// "frozen":..,"ownerSet":..,"month":..,"resetsOn":"..","saved":..}.
std::string stateJson();
// The setup wizard line (defaults and the ceiling, plain words).
std::string setupText();

// Test builds: seed the count (the bench SPENDSET verb). Not in production images.
#if defined(NIMBUS_TEST)
void testSetSpent(uint64_t micro);
#endif

}  // namespace spend
}  // namespace agent
