#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/turn.h"

// turn_contract - the engine's half of the turn-contract fixes (lane L):
//   - pseudo-tool salvage (subagents F2): a head that calls the session_ops
//     FIELD as a tool has its spawn items read with the turn normalizer and
//     carried into the turn, deduped against its final orch_turn;
//   - the turn-shape final check: one correction round for an orch_turn whose
//     spawn list the device cannot read, or whose reply says sub-agents started
//     while no spawn item is present (the promise-without-spawn class);
//   - the leak-repair gate: fields recovered from leaked tool markup carry
//     actions, so they apply only on an attended owner turn that read no
//     external content.
// Portable, host-tested (test_harness_turn_contract); the engine wires them.
namespace agent {

// Spawn items the head sent through spawn-field tool calls this turn.
struct PseudoSpawns {
  std::vector<nimbus::orch::SessionOp> items;   // accepted spawn items, in call order
  std::vector<nimbus::orch::SpawnDrop> drops;   // items those calls carried that cannot start
  int calls = 0;                                // spawn-field tool calls seen
};

// The dedupe key of a spawn: lowercase provider, '|', then the task's first
// 200 bytes lowercased with whitespace runs collapsed - so a final orch_turn
// that repeats a salvaged item (re-quoted, re-spaced) does not start it twice.
std::string spawnDedupeKey(const std::string& provider, const std::string& task);

// One spawn-field tool call: its items join `sink`. Returns the answer the
// model gets - accepted items are confirmed (start when the turn ends; do not
// repeat them) - or an empty output when the call held no usable item (the
// caller then gives the registry's "not a tool" recovery text).
nimbus::orch::HeadToolResult takePseudoSpawns(const nimbus::orch::HeadToolCall& call,
                                              PseudoSpawns& sink);

// Carry the salvaged items into the parsed final turn: an item whose key the
// turn already holds is skipped, as is one past the per-turn cap (reported in
// t.spawnDrops). The calls' unusable items join t.spawnDrops. Returns the count
// added.
int mergePseudoSpawns(nimbus::orch::Turn& t, const PseudoSpawns& p);

// The turn-shape final check: "" when the final orch_turn is fine, else the
// model-facing correction text (it names each problem and shows the array
// form). `pseudo` (nullable) counts spawn items already salvaged this turn.
std::string turnShapeCorrection(const std::string& finalTurn, const PseudoSpawns* pseudo);

// The leak-repair gate: clear every field the parser recovered from leaked tool
// markup (device actions, memory writes and queries, spawns, the scratchpad, and
// a recovered memory note or ask). Only the cut reply stays. Every dropped spawn
// is recorded in Turn::spawnDrops, so apply reports it to the owner and the
// head. Returns true when anything was cleared.
bool dropRecoveredContainers(nimbus::orch::Turn& t);

// What the owner gets when the head's answer cannot be applied (parsing P3):
// the salvaged reply (and a complete ask), markup cleaned, plus the honest cut
// marker exactly when the answer was cut - the reply string ends before its
// closing quote, the parser saw a truncated object, or the provider reported a
// cut (`reportedCut`). With nothing salvageable, one plain cause line instead
// of the old "formatting error" apology. `cut` reports which case it was, for
// the turn-end row's err ("cut: ..." or "parse: ...").
// Nothing in such an answer runs, so salvaged text that says sub-agents started
// also gets the spawn correction line (`claimsSpawn`), cut or not: a claim is
// never delivered without it.
struct SalvageOutcome {
  std::string text;        // what to deliver (never empty)
  bool salvaged = false;   // text came from the answer itself
  bool cut = false;
  bool claimsSpawn = false;   // the salvaged text claims sub-agents (none started)
};
SalvageOutcome salvageForOwner(const std::string& raw, const std::string& parseDetail,
                               bool reportedCut);
// The plain lines for an answer with nothing salvageable.
extern const char kCutNothingSalvaged[];
extern const char kUnreadableNothingSalvaged[];

}  // namespace agent
