#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Head qualification (PRD section 9): the ONE table of which head (host, model
// family, wire) is measured for which release line, read by
//   - manifest generation: tools/make_manifest.py --hold-line renders holdHeads
//     from the rows of lib/core/src/orch_head_qual.cpp (it parses the HQ_ROW
//     lines; renderHoldHeads() below is the C++ twin, golden-locked with
//     tools/test_make_manifest.py);
//   - the device when a head is chosen (one factual line), nimbusd's per-turn
//     engine choice, the hosted tools, and the engine-2 fallback and router
//     ladder (lookup()).
// Pure: no Arduino, host-tested (test/test_head_qual).
//
// A head absent from the table is UNQUALIFIED on every line: held, the lean
// profile, the 85,000 B body cap, receipt-only truth checking. Every reader fails
// closed to that row.

namespace nimbus {
namespace orch {
namespace headqual {

// A release line. E1 is the old engine from v5.2.0 on (the guard deletions, the
// ledger tool, the prompt cut); E2 is the plain engine (v5.9.0 and v6).
enum class Line : uint8_t { E1 = 0, E2 = 1 };
const char* lineSlug(Line l);                       // "e1" / "e2"
bool lineFromSlug(const char* s, Line& out);        // false on anything else

// qualified: a parity run on this line passed. assumed: expected to work, not
// measured (it is held until a run lifts it). held: known not ready.
enum class State : uint8_t { Qualified = 0, Assumed = 1, Held = 2 };
const char* stateSlug(State s);                     // "qualified" / "assumed" / "held"
inline bool holds(State s) { return s != State::Qualified; }

// The wire a head speaks. On the device the host fixes it; nimbusd and engine 2
// may speak more than one per host, so it is part of the key.
enum class Wire : uint8_t {
  Any = 0,             // a lookup that does not care (the device's host -> wire)
  AnthropicMessages,
  OpenAiResponses,
  MistralChat,
  ChatCompat,          // Z.ai and the custom endpoint
  Router,              // the Cumulo router
};
const char* wireSlug(Wire w);
Wire wireForHost(const char* host);                 // the device's one wire per host

// The context profile cap (PRD section 2): the largest profile a head may run.
enum class Profile : uint8_t { Tight = 0, Lean = 1, Standard = 2, Wide = 3 };
const char* profileSlug(Profile p);

// The truth check mode a head STARTS in on a line (PRD section 8 item 8, ruling 9).
enum class Truth : uint8_t { ReceiptOnly = 0, Enforce = 1, Shadow = 2 };
const char* truthSlug(Truth t);

// The body cap an unqualified head is held at (PRD section 2, the lean profile).
inline constexpr uint32_t kUnqualifiedBodyCap = 85000;
// No cap from this table: main's requestBudgetFor decides alone.
inline constexpr uint32_t kNoTableCap = 0xFFFFFFFFu;

struct Row {
  const char* host;        // provider slug ("anthropic", "openai", "mistral", "zai", "cumulo", "custom")
  const char* family;      // model pattern: an exact id, a prefix ending in '*', or "*"
  Wire        wire;
  State       e1;
  State       e2;
  uint32_t    bodyCap;     // B when qualified (kNoTableCap = defer to requestBudgetFor)
  Profile     profileCap;  // the profile cap when qualified
  Truth       truth;       // the start mode of the truth check
};

// The rows, in match order: within a host the first row whose family matches
// wins, so specific families come before the host's "*" row.
const Row* rows(size_t& count);

// The verdict for one head on one line. Never null: an unknown head gets the
// built-in unqualified row (host "*", family "*", held on every line).
struct Verdict {
  const Row* row;
  State      state;
  uint32_t   bodyCap;      // kUnqualifiedBodyCap unless qualified on an E2 line
  Profile    profileCap;   // Lean unless qualified
  Truth      truth;
};
Verdict lookup(const char* host, const char* model, Line line, Wire wire = Wire::Any);

// The holdHeads array for a manifest of `line`, as compact JSON
// (["*/*","!anthropic/claude-sonnet-5-5",...]): every head is held, then each
// qualified row is excepted, later entries winning (nimbus::ota::headHeld reads
// it that way). Entries that cannot change a verdict are left out.
std::string renderHoldHeads(Line line);

// The one model-pattern rule both this table and the manifest's holdHeads use:
// "*" matches anything, "abc*" matches every id starting "abc", anything else
// matches only itself. An empty or null pattern or subject matches nothing.
inline bool patternMatch(const char* pat, const char* s) {
  if (!pat || !s || !*pat || !*s) return false;
  for (;; ++pat, ++s) {
    if (*pat == '*' && pat[1] == '\0') return true;
    if (*pat != *s) return false;
    if (!*pat) return true;
  }
}

}  // namespace headqual
}  // namespace orch
}  // namespace nimbus
