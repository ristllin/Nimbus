#include "nimbus/harness/turn_progress.h"

namespace nimbus {
namespace harness {
namespace {
// About when the ONE progress line goes out: past this the turn no longer
// looks alive on its own (the real case ran 5 min 12 s with no signal).
constexpr uint32_t kLineAfterMs = 30000;
// ... or at the third round, whichever comes first (the real case's rounds
// each took ~35 s, so the round rule fires first there).
constexpr int kLineAtRound = 2;

// The plain-words activity for the tools the previous round dispatched, in the
// assistant's own voice in chat (the persona's first person in conversation,
// AGENTS.md copy guide): "Still looking through my memory for that". The names
// on the wire are the sanitized registry names (compose.h loopToolName:
// '.' -> '_'), so match on those; anything unmapped falls to the honest
// default and a machine name never reaches the owner.
struct ToolPhrase {
  const char* name;
  const char* phrase;
};
const ToolPhrase kToolPhrases[] = {
    {"web_search", "Still searching the web for that"},
    {"memory_search", "Still looking through my memory for that"},
    {"memory_episodic", "Still looking through my memory for that"},
    {"docs_search", "Still looking through the documents for that"},
    {"docs_read", "Still reading a document for that"},
    {"files_search", "Still looking through the files for that"},
    {"files_list", "Still looking through the files for that"},
    {"files_read", "Still reading a file for that"},
    {"results_get", "Still reading back what I found"},
    {"results_list", "Still reading back what I found"},
};
}  // namespace

std::string progressActivity(const std::vector<std::string>& tools) {
  for (const auto& t : tools)
    for (const auto& p : kToolPhrases)
      if (t == p.name) return p.phrase;
  return "Still working on that";
}

// Only a turn a person started may signal the chat: the owner or an approved
// member in chat (TurnSource::Owner), or the serial console (TurnSource::Serial).
// A dream, routine, loop, sub-agent/synthesis or other system turn never does
// (Lumi v5.0.0, 11 Nov 03:31Z: a dream turn's progress line reached the owner
// with no message of theirs to relate it to).
bool progressAllowedForSource(agent::TurnSource source) {
  return source == agent::TurnSource::Owner || source == agent::TurnSource::Serial;
}

std::string progressWait(uint32_t elapsedMs, int round, int maxRounds) {
  // The pace the turn has actually run at, projected over the rounds the cap
  // still allows: an honest rough wait, not a fixed guess.
  if (round <= 0 || maxRounds <= 0 || elapsedMs == 0) return "a moment more";
  const uint32_t pace = elapsedMs / (uint32_t)round;   // per completed round
  const int left = maxRounds > round ? maxRounds - round : 1;
  const uint32_t estimate = pace * (uint32_t)left;
  if (estimate <= 45000) return "about half a minute more";
  if (estimate <= 100000) return "about a minute more";
  return "a few minutes more";
}

TurnProgressPlan planTurnProgress(const orch::HeadProgress& hp, int maxRounds, bool lineAlreadySent,
                                  agent::TurnSource source) {
  TurnProgressPlan plan;
  if (hp.lastRound) return plan;   // the answer is imminent: stay quiet (no queue ack either)
  // A turn the owner never started (dream, routine, loop, synthesis) never
  // signals the chat: no typing, no line. The queue check still runs - a
  // message that arrived mid-turn is the owner's own and still deserves its ack.
  const bool personTurn = progressAllowedForSource(source);
  plan.keepAlive = personTurn;
  plan.checkQueue = true;
  if (personTurn && !lineAlreadySent && (hp.elapsedMs >= kLineAfterMs || hp.round >= kLineAtRound)) {
    plan.sayLine = true;
    plan.line = progressActivity(hp.tools) + ", " + progressWait(hp.elapsedMs, hp.round, maxRounds) + ".";
  }
  return plan;
}

}  // namespace harness
}  // namespace nimbus
