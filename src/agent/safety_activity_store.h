#pragma once
#include <string>

#include "nimbus/orch/safety_activity.h"

// safety_activity_store - the DEVICE seam behind the portable Safety ACTIVITY
// core (lib/core/orch_safety_activity, CUM-215). It owns the RAM ring + scoped
// allowlist, persists both to the SAME card/flash tier the durable error log uses
// (nimbus::errlog::activeFs(); NEVER NVS), and is fed a verdict by the scanner
// choke points (the orchestrator moderation gate + the fetched-content injection
// scan). The web routes (src/net/safety_routes) and the SAFETY? console command
// read + mutate through here.
//
// Concurrency: all state is guarded by agent::memory::Lock, the recursive mutex
// that already serializes card I/O across every task (main, poll, AsyncTCP web).
// Reads (listJson) and writes (record/dismiss/approve) all take it, so the ring
// is consistent across the poll task that records and the web task that renders.
// No handle is held across a network send.

namespace agent {
namespace safety {

// Load the ring + allowlist from the durable tier. Idempotent; safe before the FS
// is mounted (it simply loads nothing and a later record() persists once ready).
void begin();

// Record one scanner verdict. `rawExcerpt` is redacted here (the same secret set
// the durable log masks) and clamped to the contract cap before storage. Returns
// false without recording when the allowlist already allows this item (a scoped
// unblock the owner set) - the caller uses allowed() to also skip blocking. On a
// real record it assigns an id, prepends newest-first, evicts the oldest past the
// device cap, and rewrites the durable file.
bool record(nimbus::orch::SafetyVerdict verdict, const std::string& rule,
            const std::string& channel, const std::string& sender,
            const std::string& source, const std::string& rawExcerpt);

// Gate consult: would a stored scoped allow-rule already permit this item? Pure
// RAM lookup (no FS), safe to call on the poll/turn task inside a gate. The
// excerpt is redacted the same way record() does so a Pattern rule matches the
// stored form. Used by the scanner choke points to turn an owner "approve" into a
// real, SCOPED unblock (never a global off switch).
bool allowed(const std::string& rule, const std::string& sender, const std::string& rawExcerpt);

// ---- web-route + console surface (all take the Lock) ----

// {entries:[...newest-first...], allow:[...], report:{available,copy,pending,last:{...}}}
// for GET /api/safety. `hasCumuloKey` gates the report availability flag + copy; the
// `pending`/`last` fields let the tab poll a deferred report's outcome.
std::string listJson(bool hasCumuloKey);

// Mark an entry dismissed. false when the id is unknown. Persists.
bool dismiss(const std::string& id);

// Approve an entry: add ONE scoped allow-rule derived from the entry (scope =
// sender|content-class|pattern; the route validates the choice) and mark the entry
// approved. Persists both files. false when the id is unknown or the derived value
// is empty (which would be a catch-all and is refused by the core).
bool approve(const std::string& id, nimbus::orch::AllowScope scope, std::string& msgOut);

// Revoke an allow-rule by id. false when unknown. Persists.
bool revokeAllow(const std::string& id);

// Outcome of a report REQUEST from the web task (the actual POST runs later on a
// worker task, never on the AsyncTCP task - a TLS acquire+handshake there could
// stall every web request and trip the loop watchdog, the cloud_mint precedent).
enum class ReportRequest : uint8_t {
  Started = 0,     // queued on the worker; poll listJson for report.last
  NoEntitlement,   // no Cumulo key: refused here, no TLS, no task
  NotFound,        // unknown entry id
  Busy,            // a report is already in flight (one at a time)
};

// Queue a subscription-gated report of `id` to Cumulo. Returns immediately: it
// validates the id + the local Cumulo-key gate, then spawns a short-lived worker
// task that builds the frozen-contract payload and POSTs it via the TLS work-slot
// arbiter, recording the outcome for the tab to poll. `msgOut` carries owner copy.
ReportRequest requestReport(const std::string& id, std::string& msgOut);

// One-line-per-metric summary for the SAFETY? console command.
std::string consoleSummary();

}  // namespace safety
}  // namespace agent
