#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/episodic_log.h"

// retention - the policy around the episodic retention pass (docs/orchestrator-storage.md
// §4): what it may never delete, when it may run, and the schedule that retries a
// pass a turn pushed back. Portable so every rule here is host-tested; the device
// (memory_subsystem.cpp, main.cpp) only wires it to the card, the clock and the
// turn flag.
namespace nimbus {
namespace orch {

// Root of the Telegram archive on the card: every inbound and outbound Telegram
// item, kept at full fidelity. Retention never touches it (no age rule, no blob
// sweep); the archive has its own storage-pressure policy.
inline constexpr const char* kTgArchiveRoot = "/mem/tg";

// True for the archive root and every path under it. Every delete a retention
// pass makes goes through this one check. The test is case-insensitive (the card
// is FAT) and ignores repeated slashes and "." segments; a path with a ".."
// segment is treated as exempt, so an odd path fails closed (kept, not deleted).
bool isRetentionExempt(const std::string& path);

enum class RetentionStatus : uint8_t {
  Ran,           // the pass ran; see the report
  DeferredTurn,  // a turn is in flight: nothing read or removed, retry on the next loop pass
  DeferredBusy,  // the card lock stayed busy (a web query or a memory tool): same retry
  NoStore,       // no SD append-log (or retention disabled): nothing to do
  ClockUnset,    // the clock has not advanced past the window: nothing to do
};
const char* retentionStatusName(RetentionStatus s);

// One line for a forced pass (the test console's EPIPRUNE): the status name and,
// for a pass that ran, what it removed, e.g. "status=ran dayfiles=3 blobs=5 kept=1200"
// ("blobs=deferred" while the index is incomplete) or "status=deferred-turn".
std::string retentionSummary(RetentionStatus st, const EpiPruneReport& rep);

struct RetentionResult {
  RetentionStatus status = RetentionStatus::NoStore;
  uint32_t        cutoffDay = 0;  // days strictly older than this were eligible (Ran only)
  EpiPruneReport  report;         // what the pass removed (Ran only)
};

// One episodic retention pass, behind the turn gate. While a turn is in flight
// the pass does nothing at all: its directory walks and deletes would compete
// with the turn for the card and for internal heap (a pass that overlapped a
// turn is the leading suspect in a field crash). The caller retries on its next
// loop pass (see RetentionSchedule), not a whole period later.
RetentionResult runEpisodicRetention(AppendLogEpisodicStore* store, uint32_t nowDay,
                                     int retentionDays, const std::string& blobDir,
                                     bool turnInFlight);

// When to attempt a pass: every `periodMs` of uptime, the first at `periodMs`. A
// deferred pass stays due, so the next loop pass retries it straight away. Safe
// across the 32-bit millisecond wrap.
class RetentionSchedule {
 public:
  explicit RetentionSchedule(uint32_t periodMs) : periodMs_(periodMs) {}
  bool due(uint32_t nowMs) const {
    return pending_ || (uint32_t)(nowMs - lastMs_) > periodMs_;
  }
  // Record an attempt made at `nowMs`. Returns true exactly once per deferred
  // pass (its first deferral, for either reason), so the caller logs "deferred"
  // once rather than on every loop pass while the turn runs.
  bool record(RetentionStatus st, uint32_t nowMs);
  bool pending() const { return pending_; }

 private:
  uint32_t periodMs_;
  uint32_t lastMs_ = 0;
  bool     pending_ = false;
};

}  // namespace orch
}  // namespace nimbus
