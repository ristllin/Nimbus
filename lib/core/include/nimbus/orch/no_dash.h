#pragma once
// no_dash - the device-enforced half of a "never use em dashes" standing rule.
//
// A rule in the prompt is followed most of the time; models still slip on a
// single-character style rule in long formatted replies (15 Oct: 14 of 36 replies
// after the rule). The owner asked for "never ever, anywhere", which only a
// deterministic filter on the delivery path guarantees. It runs on model-written
// text at the delivery seam (agent::orchestrator deliver(), the reply.telegram
// tool and the history row the web chat renders), only while an active standing
// rule switched it on (owner_prefs.h kPrefFilterNoDash).
//
// What it replaces, outside fenced code blocks (``` or ~~~ fences are kept byte
// for byte): the em dash U+2014 and its escaped forms (the named, decimal and hex
// HTML entities, and a literal JSON-style backslash-u escape), plus the two- and
// three-em dashes U+2E3A and U+2E3B.
//   "word - word" spacing:  "a <dash> b"   -> "a, b"
//   joined or one-sided:    "a<dash>b"     -> "a - b"
//   at the start of a line: "<dash> item"  -> "- item"
// En dashes (U+2013) and hyphens are left alone. The output is valid UTF-8 when
// the input was, mask 0 returns the input unchanged, and filtering twice gives the
// same text as filtering once.
//
// Portable, Arduino-free, host-tested (test_owner_prefs).

#include <cstdint>
#include <string>

namespace nimbus {
namespace orch {

// Every em dash form above, replaced (fences kept).
std::string removeEmDashes(const std::string& text);
// Does any em dash form remain outside fenced code? (tests, diagnostics)
bool hasEmDash(const std::string& text);
// The filters an active rule set switched on (owner_prefs.h PrefFilter bits).
std::string applyOutputFilters(const std::string& text, uint32_t mask);

}  // namespace orch
}  // namespace nimbus
