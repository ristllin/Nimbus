#pragma once
#include <functional>
#include <vector>

#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/tool_registry.h"

// head_tools - the device-side glue passed into the orchTurn* adapters to enable
// the head's multi-turn tool-use loop. The orchestrator builds one of these ONLY
// when store::orchToolLoop() is on (registry tool specs + a dispatch bound to the
// memory Lock + the loop bounds from agent_config.h) and passes it by pointer;
// a nullptr means the adapter runs its original single-shot structured turn, so
// the default path is byte-for-byte unchanged.
namespace agent {

// Byte cap for the per-turn transcript brief handed to HeadTools::onBrief.
// Bounded because it rides a PSRAM dossier that is itself capped.
static constexpr size_t kHeadBriefMax = 8192;

struct HeadTools {
  // Provider-neutral tool advertisements (ToolRegistry::toolSpecs()). Each adapter
  // wraps these into its provider's function-tool shape alongside the terminal
  // orch_turn tool.
  std::vector<nimbus::orch::ToolRegistry::Spec> specs;

  // Execute one tool call (name + raw JSON args) -> result. The orchestrator wires
  // this to memory::registry().dispatch() under agent::memory::Lock. Never throws.
  std::function<nimbus::orch::HeadToolResult(const nimbus::orch::HeadToolCall&)> dispatch;

  // Loop bounds (rounds / deadline / per-round heap re-gate / accumulator caps).
  nimbus::orch::HeadLoopConfig cfg;

  // Optional round-prose observer (Glass Box A4): providers forward it into
  // HeadLoopHooks.onText so mid-turn "thinking" reaches the engine's hook spine.
  std::function<void(const std::string& text, int round)> onRoundText;

  // Optional recent-results spill (Context Fabric): forwarded into
  // HeadLoopHooks.spill - a clamped tool result's FULL text lands in the
  // results ring and the truncation marker carries the results.get handle.
  std::function<std::string(const nimbus::orch::HeadToolResult& full)> spill;

  // Optional canonical-transcript observer (Glass Box P3). Every loop builds a
  // Transcript (the richest per-turn artifact: user seed, per-round prose, tool
  // calls WITH args, results, provider attribution) and destroys it at loop
  // exit - renderBrief() had no caller anywhere. Each loop now hands the brief
  // here on the way out, so the engine can put the tool-loop's actual middle
  // into the turn dossier instead of only its entry and exit.
  std::function<void(const std::string& brief)> onBrief;
};

}  // namespace agent
