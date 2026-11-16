#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// fold_ladder - the compaction fold's ladder of rungs. A rung is one host (and the
// model it runs) a fold may ask; a fold cycle tries the rungs in order until one
// answers with a summary. Declared by lane P0 of the history fix (16 Nov 2026) with
// today's behavior: the rungs are TurnEngine::foldHostCandidates() on each host's own
// model, and runFold walks them in one call. Lane F paces a cycle one rung per pump
// pass (Pending) and adds the head's fold model, the router fallback and the Sub step-down.
namespace agent {

// What a fold came to. Ok: a summary to store. Deferred: a gate (heap, a turn in
// flight, the spend ceiling, no usable host) said "not now"; the caller retries later
// and never counts it as a failure. Failed: every attempt failed. Pending: a paced
// cycle still has rungs to try (no failure, no notice); nothing returns it today.
enum class FoldResult : uint8_t { Ok = 0, Deferred, Failed, Pending };

// Where a rung sits in the ladder (the fold protocol's rung order).
enum class FoldRungKind : uint8_t { HeadHost = 0, OtherHost, RouterFallback, SubModel };

struct FoldRung {
  std::string host;
  std::string model;   // the model this rung asks for; "" = the host's own (its orchModel)
  FoldRungKind kind = FoldRungKind::HeadHost;
};

// What one rung came to (TurnEngine::runFoldRung).
struct FoldRungReport {
  FoldResult result = FoldResult::Deferred;
  std::string host;
  std::string model;
  size_t producedBytes = 0;   // the raw summary's size before capSummary (0 = none)
};

}  // namespace agent
