#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/rbac.h"

// Moderation gates - the PURE, host-tested decision core (no Arduino, no network).
// The device seam runs the actual classifier HTTPS call (Cumulo moderation endpoint
// on a Cumulo key, else Mistral /v1/moderations on the user's key) and feeds the
// verdict here; this module owns every policy decision so the fail-open/fail-closed
// contract is testable and cannot silently regress:
//
//   Gate            role gate         classifier fail   flagged content
//   InboundText     non-admin only    fail-CLOSED        block the turn
//   OutboundReply   non-admin only    fail-OPEN          block the reply
//   WorldContent    always (content)  fail-OPEN + mark   mark untrusted (never block)
//
// Admin traffic is NEVER classified (the owner is trusted and pays per call). The
// world-content gate is about fetched/summarized data, not a chat role, so it runs
// regardless of who is talking; it MARKS untrusted rather than blocking, both on a
// flag and on a classifier error (fail-open with marking).

namespace nimbus {
namespace orch {

// Which gate a moderation check guards.
enum class ModGate : uint8_t {
  InboundText = 0,   // guest/member message, screened PRE-turn
  OutboundReply,     // a reply about to be delivered to a guest/member
  WorldContent,      // fetched/summarized world content entering the turn
};
const char* modGateName(ModGate g);

// What the classifier said (or why it did not run).
enum class ClassifierVerdict : uint8_t {
  Unchecked = 0,  // gate off, admin-exempt, or not run for this text
  Allow,          // classifier says clean
  Flag,           // classifier says disallowed / injection
  Error,          // classifier could not run (no key / network / quota / parse)
};

// The action the caller takes.
enum class ModAction : uint8_t {
  Allow = 0,      // proceed normally
  Block,          // refuse: inbound = do not run the turn; outbound = do not send
  MarkUntrusted,  // proceed, but tag the content as untrusted (world content only)
};
const char* modActionName(ModAction a);

// Fail behaviour when the classifier errors.
enum class FailMode : uint8_t { Open = 0, Closed };

// Owner switches - each gate is opt-in (off by default; every one costs a paid
// classifier call per screened item, surfaced as a cost note in the UI).
struct ModConfig {
  bool inbound   = false;
  bool outbound  = false;
  bool injection = false;
};

// Does this gate run for this role, given the switches? Admin is NEVER classified.
// (Unknown is already denied every turn upstream; it is treated as non-admin here
// so a screened gate still fail-closes rather than leak, but the caller usually
// never reaches an unknown-role turn.)
bool gateApplies(ModGate g, Role role, const ModConfig& cfg);

// The frozen per-gate fail mode (see the table above).
FailMode failModeFor(ModGate g);

// The decision, from the gate and the classifier verdict. Pure lookup - the whole
// point is that the fail-open/fail-closed contract lives in one testable place.
ModAction decide(ModGate g, ClassifierVerdict v);

// May an outbound reply skip the OutboundReply screen? ONLY genuine device-authored
// system copy is exempt, and provenance is signalled OUT-OF-BAND by the emitting
// code path (`systemProvenance`), NEVER by anything in the reply text. A guest or a
// model steers the text but not this flag, so no message content - not even one that
// reproduces the device name or the full self-tag - can buy the exemption. The reply
// text is deliberately NOT a parameter here: content cannot influence the decision by
// construction. (CUM-275: the old text.startsWith(deviceName) exemption was satisfied
// by model free-text, letting a guest prompt-inject past the screen.)
bool outboundExempt(bool systemProvenance);

// --- provider selection -----------------------------------------------------
enum class ModProvider : uint8_t { None = 0, Cumulo, Mistral };
const char* modProviderName(ModProvider p);
// Cumulo endpoint (on a Cumulo key) is preferred; else Mistral (user's key); else
// none (the gate then cannot run and its fail mode applies as a classifier Error).
ModProvider pickProvider(bool hasCumuloKey, bool hasMistralKey);

// --- injection heuristics ---------------------------------------------------
// A cheap, portable pre-filter for the world-content gate: does the text contain a
// prompt-injection pattern (role spoofing, "ignore previous instructions", tool/
// system directives aimed at the model)? Case-insensitive substring scan. This is
// deliberately conservative - a hit MARKS untrusted, it never blocks - so a false
// positive only adds a "treat as data" note. Returns true when suspicious.
bool looksLikeInjection(const std::string& text);

}  // namespace orch
}  // namespace nimbus
