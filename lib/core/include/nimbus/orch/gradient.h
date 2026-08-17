#pragma once
// Gradient context trimming - the in-turn arm of the compaction research
// (plans/compaction-plan.md: "summary + verbatim tail" - keep the LATEST
// verbose, fold older content into one-line summaries, never dumb-trim).
//
// This module is MECHANICAL and deterministic: zero model calls, safe to run
// mid-loop on every round. The LLM-fold (anchored CONVERSATION SUMMARY) stays
// where it is - cross-turn compaction via buildCompactInputs/runFold.
//
// TranscriptItem is the CANONICAL transcript entry shared with the
// canonical-transcript refactor (Stage 2 adopts this exact type): a trimmer is
// a pure function over the vector, not over any provider's JSON.
//
// Arduino-free, host-tested (test_orch_gradient).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nimbus {
namespace orch {

struct TranscriptItem {
  enum class Kind : uint8_t { User, AssistantText, ToolUse, ToolResult };
  Kind        kind = Kind::User;
  std::string id;       // provider pairing id (tool_use.id) - ToolUse/ToolResult
  std::string name;     // tool name - ToolUse/ToolResult
  std::string text;     // prose / args JSON / result text
  std::string meta;     // opaque provider blob (e.g. OpenAI encrypted reasoning);
                        // replayed ONLY by the provider that wrote it
  char        provider[10] = {};  // origin host (mid-turn failover attribution)
  int8_t      round = -1;         // loop round that produced it
  bool        isError = false;    // ToolResult only
  bool        pinned = false;     // never folded (the seeded [USER] message)
};

struct GradientPolicy {
  int    keepRounds = 2;      // newest N rounds stay verbatim
  size_t lineMax = 160;       // one-line fold cap (UTF-8-safe)
  size_t triggerBytes = 0;    // fold only when the summed text exceeds this (0 = always)
};

// One-line fold of a labeled blob - shared by gradientTrim, [FRESH RESULTS]
// overflow stubs, and the assembler's omitted-section trailer. UTF-8-safe.
std::string foldLine(const std::string& label, const std::string& text, size_t lineMax);

// Total bytes of item text (the trigger input).
size_t transcriptBytes(const std::vector<TranscriptItem>& items);

// Fold each COMPLETE ToolUse+ToolResult pair older than the newest keepRounds
// rounds into ONE User-kind line ("[earlier round k] name(...) -> gist…").
// Invariants, by construction:
//   - no orphan ToolUse survives (an unanswered tool_use 400s on Anthropic) -
//     a pair folds together or not at all;
//   - pinned items and AssistantText prose are preserved verbatim;
//   - below triggerBytes the input is returned unchanged (byte-identical).
std::vector<TranscriptItem> gradientTrim(const std::vector<TranscriptItem>& in,
                                         const GradientPolicy& pol);

}  // namespace orch
}  // namespace nimbus
