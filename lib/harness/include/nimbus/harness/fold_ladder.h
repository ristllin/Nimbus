#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// fold_ladder - the compaction fold's ladder of rungs. A rung is one host and the
// model it runs; a fold cycle tries the rungs in order until one answers with a
// summary. Declared by lane P0 of the history fix (16 Nov 2026); lane F builds the
// whole ladder (owner ruling 2: a failure walks every keyed provider) and paces a
// cycle one rung per pump pass (fold_driver.h):
//   1. the head host on its fold model: the head model, or the host's non-reasoning
//      model when the head is a forced reasoner (Large 4 folds on Mistral Small);
//   2. every other keyed, unparked, in-budget host, in the owner's priority order
//      (then any keyed provider the list leaves out), each on its own model;
//   3. the router fallback (Cumulo, then Z.ai) when no provider key is set;
//   4. the head host on its Sub model (a direct host only).
// No (host, model) appears twice. A cycle asks each rung once.
namespace agent {

// What a fold came to. Ok: a summary to store. Deferred: a gate (heap, a turn in
// flight, the spend ceiling, no usable host) said "not now"; the caller retries later
// and never counts it as a failure. Failed: every attempt failed. Pending: a paced
// cycle still has rungs to try (no failure, no notice).
enum class FoldResult : uint8_t { Ok = 0, Deferred, Failed, Pending };

// Where a rung sits in the ladder (the fold protocol's rung order).
enum class FoldRungKind : uint8_t { HeadHost = 0, OtherHost, RouterFallback, SubModel };

struct FoldRung {
  std::string host;
  std::string model;   // the model this rung asks for; "" = the host's own (its orchModel)
  FoldRungKind kind = FoldRungKind::HeadHost;
};

// Why a rung did not answer with a summary: the rung line's <kind>.
enum class FoldFailKind : uint8_t {
  None = 0,
  Transport,   // no answer: connect, a cut connection, low memory, nothing sent
  Timeout,     // sent, and no answer in the read window
  Http4xx,     // the request was refused (400, 401, 403, 413, 422, 429)
  Park,        // the account is out of credit or refused (402 and billing refusals): parked
  Http5xx,     // the provider failed (5xx, 529, an unreadable answer)
  Cut,         // cut at the output limit twice (the wire's own retry included)
  Empty,       // a 2xx with no summary in it
  Thin,        // a 2xx too thin to be the summary of a big digest (foldAnswerThin)
};
const char* foldFailKindName(FoldFailKind k);

// What one rung came to (TurnEngine::runFoldRung).
struct FoldRungReport {
  FoldResult result = FoldResult::Deferred;
  std::string host;
  std::string model;           // the model the request ran on
  size_t producedBytes = 0;    // the raw summary's size before capSummary (0 = none)
  FoldFailKind fail = FoldFailKind::None;
  std::string detail;          // the provider's error line, one line, bounded
  // Requests the provider bills: every 2xx answer and a timeout after the upload
  // (the device-wide daily cap counts these; a refused request costs nothing).
  uint32_t paidRequests = 0;
  // A 2xx that carried no usable summary (empty or thin): this (host, model) goes
  // last in the next cycle.
  bool degraded = false;
  uint32_t promptTokens = 0, completionTokens = 0;
  uint32_t elapsedMs = 0;
  // A Thin answer of at least kFoldThinFloor bytes that only named too few sections (a
  // summary written in another language, or with renamed headings), capped: the cycle
  // keeps the longest and stores it when every rung failed, so such an owner still gets
  // a summary instead of an endless backoff.
  std::string thinSummary;
};

// The model the head host's rung folds on ("" = the head's own model). A forced
// reasoner (it refuses effort "none", so a fold's output budget goes to thinking: Lumi,
// 2 Nov, 4,194 tokens of thought for a 1 KB summary) folds on its host's non-reasoning
// model. Mistral Large 4 -> mistral-small-latest at effort "none" (orchestrator ruling
// of 16 Nov: Medium is deprecated upstream; the ladder covers Small's failure).
std::string foldModelForHead(const std::string& host, const std::string& model);

// The quality gate, for digests of kFoldQualityDigestMin or more: a 2xx answer is thin
// when it is shorter than min(800 B, 30% of the previous summary) (no size floor for a
// first summary) or names fewer than 2 of the prompt's 5 sections (matched leniently:
// case-insensitive keywords, any numbering or markup).
constexpr size_t kFoldQualityDigestMin = 8u * 1024u;
constexpr size_t kFoldThinFloor = 800;
bool foldAnswerThin(const std::string& summary, size_t digestBytes, size_t prevSummaryBytes);
// How many of the 5 sections the summary names (the lenient match).
int foldSectionsNamed(const std::string& summary);

// The rung line every failed rung logs: "fold(<chat>): rung i/n <host>/<model> failed ->
// <kind> (<detail>)". It keeps the token "failed" the persona rig's regex reads.
std::string foldRungFailLine(const std::string& chatId, size_t index, size_t count, const FoldRungReport& r);

}  // namespace agent
