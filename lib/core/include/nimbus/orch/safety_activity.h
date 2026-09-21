#pragma once
#include <cstdint>
#include <string>
#include <vector>

// safety_activity - the PURE, host-tested core of the Safety ACTIVITY surface
// (CUM-215). No Arduino, no network, no filesystem: this module owns every policy
// decision so the privacy-first contract is testable and cannot silently regress.
//
//   * SafetyActivityLog - a bounded, newest-first ring of scanner verdicts (what
//     the scanner blocked or suspected, when, on which channel, by which rule).
//     The device seam feeds it a verdict per screened item and persists it to the
//     card/flash tier the errlog uses (never NVS); host tests inject entries and
//     assert the bound + ordering.
//   * SafetyAllowlist - the SCOPED approve/unblock list. An owner approves a
//     specific pattern, sender, or content class; the scope can NEVER widen to a
//     global off switch, and that is enforced by construction (there is no global
//     scope) and by test. Inspectable + revocable from the same surface.
//   * report payload builder + gate - the exact wire-contract fields for a
//     Cumulo safety report and nothing else, plus the subscription gate (a report
//     is possible only when the device holds a Cumulo key; entitlement itself is
//     the cloud's 401/403 to decide, mapped back to owner copy here).
//
// Portable + Arduino-free: the JSONL codec and the report JSON are built with
// std:: types + ArduinoJson (the same host-available lib the rest of lib/core
// uses), so the wire/persistence formats are byte-tested under `pio test -e native`.

namespace nimbus {
namespace orch {

// ---- verdict + entry status --------------------------------------------------

// What the scanner decided about an item. Maps 1:1 to the wire contract's
// verdict field: a text gate that blocked -> Blocked; world content marked
// untrusted / an injection heuristic hit -> Suspected.
enum class SafetyVerdict : uint8_t { Blocked = 0, Suspected = 1 };
const char* safetyVerdictName(SafetyVerdict v);            // "blocked" | "suspected"
bool        safetyVerdictFromName(const std::string& s, SafetyVerdict& out);

// Local owner state of an entry. Approved means the owner allow-listed the
// matching scope from this entry (the entry stays visible as a record).
enum class SafetyStatus : uint8_t { Active = 0, Dismissed = 1, Approved = 2 };
const char* safetyStatusName(SafetyStatus s);
bool        safetyStatusFromName(const std::string& s, SafetyStatus& out);

// ---- ring sizing -------------------------------------------------------------
//
// The activity log is a bounded ring, newest-first. The bound exists because the
// log rides the SAME small durable tier the errlog uses (src/sys/errlog: an SD
// card when present, else a deliberately tiny LittleFS fallback), and it is
// rewritten whole on each mutation (dismiss/approve edit an existing entry, which
// an append-only log cannot do), so the file must stay small enough that one
// rewrite is cheap and can never crowd out config or the degraded /data store.
//
// Sizing: excerpts are hard-capped at 512 bytes (the wire contract's own cap), so
// one serialized entry is bounded to well under ~700 bytes. 64 entries is at most
// ~45 KB of history, which fits the SD tier comfortably and is dwarfed by a card;
// on the flash-only fallback (48 KB of errlog) the ring is the same file-shaped
// cost, still bounded. 64 recent verdicts is far more than an owner reviews in one
// sitting yet cheap to hold in RAM (the device keeps the ring resident so the tab
// answers with zero card reads on the common path).
inline constexpr int    kSafetyRingCap    = 64;
inline constexpr size_t kSafetyExcerptMax = 512;   // frozen: the wire contract's excerpt cap
inline constexpr int    kSafetyAllowMax   = 64;    // bound the allowlist too (same rationale)

// One recorded scanner verdict.
struct SafetyEntry {
  std::string   id;                              // stable per entry ("a" + hex suffix on device)
  uint32_t      tsEpoch = 0;                      // unix seconds the verdict was recorded
  SafetyVerdict verdict = SafetyVerdict::Blocked;
  std::string   rule;                            // the rule/category that fired (the content class)
  std::string   channel;                         // delivery channel: telegram | web | voice | download
  std::string   sender;                          // principal that produced it ("" when none, e.g. web/world)
  std::string   source;                          // the scanner/gate: inbound | outbound | world
  std::string   excerpt;                         // redacted snippet, clamped to kSafetyExcerptMax
  SafetyStatus  status = SafetyStatus::Active;

  bool operator==(const SafetyEntry& o) const;
};

// Clamp an excerpt to the contract cap (UTF-8-agnostic byte clamp; the device
// already redacts secrets upstream, this only bounds length). Exposed for the
// device seam so it clamps identically to the store.
std::string clampExcerpt(const std::string& in);

// ---- the activity log --------------------------------------------------------
//
// A bounded ring of entries kept newest-first (index 0 == most recent). Adding
// past the cap drops the OLDEST. Pure in-memory; the device seam owns persistence
// and calls loadLine()/each()/serialize() to bridge the durable file.
class SafetyActivityLog {
 public:
  explicit SafetyActivityLog(int cap = kSafetyRingCap) : cap_(cap > 0 ? cap : kSafetyRingCap) {}

  // Record a verdict at the FRONT (newest). The excerpt is clamped. Returns the
  // stored entry (with its excerpt clamped). Evicts the oldest past the cap.
  const SafetyEntry& record(SafetyEntry e);

  int    size() const { return (int)entries_.size(); }
  int    cap()  const { return cap_; }
  bool   empty() const { return entries_.empty(); }

  // Newest-first view.
  const std::vector<SafetyEntry>& entries() const { return entries_; }

  // Find by id; nullptr when absent.
  const SafetyEntry* find(const std::string& id) const;

  // Mutate one entry's status. false when the id is unknown.
  bool setStatus(const std::string& id, SafetyStatus s);
  bool dismiss(const std::string& id) { return setStatus(id, SafetyStatus::Dismissed); }
  bool approve(const std::string& id) { return setStatus(id, SafetyStatus::Approved); }

  // Drop all entries (owner "clear"). Persistence rewrites empty.
  void clear() { entries_.clear(); }

  // ---- persistence bridge (JSONL, one entry per line) ----
  // Serialize the whole ring newest-first, one JSON object per line (with a
  // trailing '\n' per line). This is what the device seam writes to the durable
  // file on every mutation.
  std::string serialize() const;

  // Replace the ring from a serialized blob (as produced by serialize()).
  // Tolerant: a torn/garbage line is skipped, and the result is re-bounded +
  // re-sorted newest-first so a hand-edited or truncated file can never exceed
  // the cap or scramble ordering. Returns entries loaded.
  int loadAll(const std::string& blob);

 private:
  void rebound();   // keep newest-first and <= cap_

  int cap_;
  std::vector<SafetyEntry> entries_;   // newest-first (index 0 = most recent)
};

// One-entry JSONL codec (shared with the device seam + tests). encodeLine emits a
// single line WITHOUT a trailing newline; decodeLine is tolerant (false on a
// torn/garbage/incomplete line).
std::string encodeSafetyLine(const SafetyEntry& e);
bool        decodeSafetyLine(const std::string& line, SafetyEntry& out);

// ---- the scoped allowlist ----------------------------------------------------
//
// The approve/unblock surface. Approving an entry adds ONE scoped rule; the scope
// is one of exactly these three and there is deliberately NO "global"/"off"
// value, so an allow can never be a blanket disable. Each rule carries a concrete,
// non-empty value; a rule with an empty value is refused, which is the closest
// thing to a catch-all and is exactly what must be impossible.
enum class AllowScope : uint8_t {
  Sender = 0,        // trust a specific principal (entry.sender == value)
  ContentClass = 1,  // trust a specific rule/category (entry.rule == value)
  Pattern = 2,       // trust a specific content pattern (value is a substring of entry.excerpt)
};
const char* allowScopeName(AllowScope s);                  // "sender"|"content-class"|"pattern"
bool        allowScopeFromName(const std::string& s, AllowScope& out);

struct AllowRule {
  std::string id;         // stable per rule
  AllowScope  scope = AllowScope::Sender;
  std::string value;      // concrete, non-empty target for the scope
  uint32_t    tsEpoch = 0;

  bool operator==(const AllowRule& o) const;
};

// True when `value` is a legal allow target: non-empty after trimming. This is
// the guard that makes a global/catch-all rule unrepresentable.
bool isConcreteAllowValue(const std::string& value);

// Does a single scoped rule match a given entry? Pure, and scope-specific: each
// scope compares ONE concrete field, so a rule can only ever match entries that
// share that field value. No scope matches unconditionally.
bool ruleMatches(const AllowRule& r, const SafetyEntry& e);

class SafetyAllowlist {
 public:
  explicit SafetyAllowlist(int cap = kSafetyAllowMax) : cap_(cap > 0 ? cap : kSafetyAllowMax) {}

  // Add a scoped rule. Refuses an empty/whitespace value (the anti-global guard)
  // and a duplicate (same scope+value). On success returns the rule id via `outId`
  // and true. `id` may be supplied (device id scheme / reload); "" auto-assigns.
  bool add(AllowScope scope, const std::string& value, uint32_t tsEpoch,
           std::string& outId, const std::string& id = "");

  // Revoke by id. false when unknown. Revocation is always allowed (the list is
  // inspectable + revocable from the same tab).
  bool revoke(const std::string& id);

  int  size() const { return (int)rules_.size(); }
  bool empty() const { return rules_.empty(); }
  const std::vector<AllowRule>& rules() const { return rules_; }
  const AllowRule* find(const std::string& id) const;

  // Would this entry be pre-approved by SOME rule already on the list? Used by the
  // device seam to suppress re-recording an item the owner already allow-listed.
  bool allows(const SafetyEntry& e) const;

  // ---- persistence bridge ----
  std::string serialize() const;         // JSONL, one rule per line
  int         loadAll(const std::string& blob);

 private:
  int cap_;
  std::vector<AllowRule> rules_;
  uint32_t nextSfx_ = 1;
};

std::string encodeAllowLine(const AllowRule& r);
bool        decodeAllowLine(const std::string& line, AllowRule& out);

// ---- report payload builder + subscription gate ------------------------------
//
// The frozen wire contract (lane C3 builds the server):
//   POST {cumuloBase}/devices/safety-report
//   Authorization: Bearer <cumulo_sk_ key>
//   { deviceId, reportedAt (ISO), verdict, rule, channel,
//     excerpt (<=512, already redacted), meta: { fw, source } }
// Responses: 202 {id}; 401/403 no entitlement; 413 oversize; 429.
//
// The builder emits EXACTLY those fields and nothing else, so a report can never
// carry more than the contract says. The excerpt is clamped again here as
// defense-in-depth (the entry is already clamped on record).
struct SafetyReportInput {
  std::string deviceId;
  std::string reportedAtIso;   // the device formats this from its clock (kept out of pure code)
  SafetyVerdict verdict = SafetyVerdict::Blocked;
  std::string rule;
  std::string channel;
  std::string excerpt;
  std::string fw;              // firmware version -> meta.fw
  std::string source;         // scanner/gate -> meta.source
};

std::string buildSafetyReportJson(const SafetyReportInput& in);

// Build the report input from a stored entry + device identity, so the route
// cannot accidentally add a field the contract does not name.
SafetyReportInput reportInputFromEntry(const SafetyEntry& e, const std::string& deviceId,
                                       const std::string& reportedAtIso, const std::string& fw);

// ---- the subscription gate ----
// Device-local availability: a report can be ATTEMPTED only when the device holds
// a Cumulo key. Entitlement (the account actually being subscribed) is the cloud's
// call, returned as 401/403 and mapped below to the same owner copy.
bool reportAvailable(bool hasCumuloKey);

// Owner copy for the "cannot report" case, so the device and the tab never
// disagree. Frozen wording per the wire contract.
const char* reportUnavailableCopy();   // "Reporting needs a Cumulo subscription."

// Outcome of a report attempt, derived from the cloud HTTP status.
enum class ReportOutcome : uint8_t {
  Sent = 0,          // 202 accepted
  NoEntitlement,     // 401 / 403 -> reportUnavailableCopy()
  TooLarge,          // 413 (excerpt oversize; should not happen given the clamp)
  RateLimited,       // 429
  Failed,            // anything else / transport error
};
const char* reportOutcomeName(ReportOutcome o);
ReportOutcome reportOutcomeFromHttp(int httpStatus);
// Short owner copy for an outcome (sentence case, next-step where useful).
const char* reportOutcomeCopy(ReportOutcome o);

}  // namespace orch
}  // namespace nimbus
