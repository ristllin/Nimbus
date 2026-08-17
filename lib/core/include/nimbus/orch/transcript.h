#pragma once
// Transcript - the CANONICAL, provider-neutral record of one turn's tool loop
// (Context Fabric Stage 2). Today each provider adapter keeps its own private
// continuity (Anthropic replays a local messages[]; OpenAI chains
// previous_response_id; Mistral pins a conversation_id), which means:
//   - a mid-turn provider outage loses the turn (state lives on their server), and
//   - retry/failover must be SKIPPED once a tool ran, because re-running a turn
//     would replay its side effects.
//
// With the device owning the transcript, a failed round can be re-run against a
// DIFFERENT provider: the tools already ran and their results ride the transcript
// as DATA, so nothing replays. Each provider keeps a thin renderer that turns
// this into its own wire shape.
//
// Storage: entries in the PSRAM working-set allocator; the strings themselves
// spill to PSRAM on device (>=128 B) via the extmem threshold. Lifetime is ONE
// turn - constructed by the caller, seeded with the user entry, handed to
// runHeadLoop by reference, and still readable after the loop for debug capture
// and spawn-with-history.
//
// Arduino-free, host-tested (test_orch_transcript).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/gradient.h"     // TranscriptItem - the shared entry type
#include "nimbus/orch/head_loop.h"    // HeadToolCall / HeadToolResult
#include "nimbus/orch/psram_alloc.h"  // WorkingAllocator

namespace nimbus {
namespace orch {

class Transcript {
 public:
  void addUser(std::string text);
  void addAssistantText(std::string text, int round, const char* provider);
  void addToolCall(const HeadToolCall& c, int round, const char* provider);
  void addToolResult(const HeadToolResult& r, int round);
  // Attach an opaque provider payload to `round` (stored on the round's FIRST
  // item; no-op if the round has no items). Carrier for OpenAI reasoning items
  // with encrypted_content - replayed only by the provider that wrote it.
  void attachMeta(int round, std::string meta);

  // Cumulative ToolResult bytes - the loop's byte-budget input.
  size_t toolBytes() const { return toolBytes_; }
  size_t size() const { return e_.size(); }
  bool empty() const { return e_.empty(); }
  const std::vector<TranscriptItem, WorkingAllocator<TranscriptItem>>& entries() const {
    return e_;
  }

  // Replace the OLDEST ToolResult payloads with a "[trimmed N B]" stub until the
  // cumulative tool bytes fit. ⚠ Entries are never REMOVED - the ToolCall <->
  // ToolResult pairing is an invariant (an unanswered tool_use 400s on
  // Anthropic). Returns the bytes reclaimed.
  size_t trimToolOutputs(size_t maxTotalToolBytes);

  // Provider-neutral compact render (spawn-with-history, /api/lastturn debug).
  // Byte-bounded, oldest-first, with an explicit omission marker.
  std::string renderBrief(size_t maxBytes) const;

 private:
  std::vector<TranscriptItem, WorkingAllocator<TranscriptItem>> e_;
  size_t toolBytes_ = 0;
};

}  // namespace orch
}  // namespace nimbus
