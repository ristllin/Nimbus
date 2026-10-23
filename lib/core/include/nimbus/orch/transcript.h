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
#include "nimbus/orch/turn_images.h"  // TurnImages - the photos of the seeded user message

namespace nimbus {
namespace orch {

class Transcript {
 public:
  void addUser(std::string text);
  // The photos the seeded user message carries (R1, turn_images.h): each renderer
  // puts them into that message, the newest user message of the request, as its
  // provider's image parts, never into a later one. Not owned; nullptr = none.
  void setImages(TurnImages* imgs) { images_ = imgs; }
  TurnImages* images() const { return images_; }
  void addAssistantText(std::string text, int round, const char* provider);
  void addToolCall(const HeadToolCall& c, int round, const char* provider);
  void addToolResult(const HeadToolResult& r, int round);
  // Attach an opaque provider payload to `round` (stored on the round's FIRST
  // item; no-op if the round has no items). Carrier for OpenAI reasoning items
  // with encrypted_content - replayed only by the provider that wrote it.
  void attachMeta(int round, std::string meta);

  // Every replayed byte (F6b): call arguments, prose and results - the loop's
  // byte-budget input. A 40 KB artifact_save argument rides every later round,
  // so it counts like a 40 KB result; only the seeded [USER] message (budgeted
  // with the prompt) is left out.
  size_t toolBytes() const { return replayBytes_; }
  size_t size() const { return e_.size(); }
  bool empty() const { return e_.empty(); }
  const std::vector<TranscriptItem, WorkingAllocator<TranscriptItem>>& entries() const {
    return e_;
  }

  // Fit the replay under the cap with ONE transcript-level fold and hysteresis
  // (H6), the same on every provider:
  //   * at or under the cap nothing changes, so the replayed request only grows
  //     at its end and each round reads the last one from the provider cache;
  //   * over the cap, the oldest whole rounds (every call with its result, never
  //     the newest round, which the model has not seen yet) fold into one line
  //     each, appended to the seeded message, until the replay is at or under
  //     HALF the cap - so the next fold is about cap/2 bytes of new rounds away
  //     instead of one round away;
  //   * a newest round that alone exceeds the room left is clipped with a
  //     visible "[cut: ...]" marker, or replaced by a "[not shown: ...]" note.
  // A call and its result fold together or not at all (an unanswered tool_use
  // 400s on Anthropic). Returns the replay bytes reclaimed.
  size_t trimToolOutputs(size_t maxTotalToolBytes);
  // The heading the folded lines sit under in the seeded message.
  static const char kFoldNote[];

  // Provider-neutral compact render (spawn-with-history, /api/lastturn debug).
  // Byte-bounded, oldest-first, with an explicit omission marker.
  std::string renderBrief(size_t maxBytes) const;

 private:
  std::vector<TranscriptItem, WorkingAllocator<TranscriptItem>> e_;
  size_t replayBytes_ = 0;
  TurnImages* images_ = nullptr;
  int newestRound() const;
  void foldOldRounds(int newest, size_t lowWater);
  void clipNewest(int newest, size_t cap);
};

}  // namespace orch
}  // namespace nimbus
