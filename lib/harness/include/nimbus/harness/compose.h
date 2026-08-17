#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/config.h"
#include "nimbus/orch/world.h"

// compose - the World system-prompt composition, lifted verbatim from
// src/agent/orchestrator.cpp (2026-07 harness extraction, Stage D). The device
// glue gathers the live inputs (HAL health, fault mask, registry specs,
// sessions, scratchpad, recall) into a ComposeInputs; this module owns every
// string the model sees, so the prompt is host-testable and golden-pinned
// (test_harness_compose + test/golden/orch_prompt_*.txt).
namespace agent {

// ---- head tool-loop advertisement filters (shared prompt <-> dispatch) ------
// Tools whose device hooks are null on this fabric (fire-and-forget: tell/poll
// can't work; spawn rides the turn's spawn[]/session_ops[]) - never advertised
// to the loop, never dispatched from it.
bool loopToolHidden(const std::string& n);
// Provider function-tool names must match ^[a-zA-Z0-9_-]{1,64}$ (all three
// hosts); registry names are MCP-style dotted (memory.write) -> advertise '_'
// for '.'.
std::string loopToolName(std::string n);

// "provider / model" line for the system-prompt identity (owner R7).
std::string hostForPrompt(const ProviderConfig& p);

struct ComposeInputs {
  std::string devName;               // "" -> "Nimbus"
  std::string hostLabel;             // hostForPrompt(...)
  std::string now;                   // local "YYYY-MM-DD Day HH:MM TZ"; "" = clock unsynced
  std::string fw;                    // firmware version (+build) for the identity line
  std::string directive;             // owner directive (OrchMemory::directive)
  std::string runningMemory;         // model working notes (OrchMemory::model)
  nimbus::orch::Hardware hw;         // live manifest flags (deviceName ignored -
                                     // set from devName for lifetime safety)
  std::vector<nimbus::orch::ToolInfo> tools;  // filtered + sanitized by caller
  bool loopOn = false;               // loopActiveNow() at the device
  std::vector<nimbus::orch::SessionInfo> sessions;
  std::string recentConversation;    // pre-rendered "## RECENT CONVERSATION" block
                                     // ("" => none; device fills from the episodic
                                     // ring per chat - Release B1)
  std::string chatSummary;           // pre-rendered "## CONVERSATION SUMMARY" block
                                     // (v3.6.0 fold; "" => never compacted). Emitted
                                     // ABOVE the recent window: summary + verbatim tail.
  const nimbus::orch::Scratchpad* scratchpad = nullptr;
  std::vector<std::string> recalled;
  int budgetBytes = nimbus::orch::kContextBudgetMax;
  // W10: who THIS turn's message is from - the prompt used to claim every
  // conversation was "your owner", which is wrong for member/guest chats (the
  // device is multi-tenant since v3.7.0). "" leaves the speaker line out
  // (synthesis/scheduled turns with no human message).
  std::string speakerRole;    // "admin" | "user" | "guest" | "unknown" | ""
  std::string speakerLabel;   // display name from the channel ("Roy"); may be ""
  // True only when a HUMAN sent this turn's message. A synthesis turn (sub-agent
  // results) or a scheduled loop has a chat id but no person behind it, and the
  // untrusted text it carries must never be stamped with the owner's authority
  // ("This message is from Roy - role: admin" on an injection-carrying
  // [FRESH RESULTS] turn). The engine sets it from the turn source.
  bool speakerPresent = false;
};

// Compose the full World system prompt in the assembler's priority order under
// the byte budget: ORCH_ROLE (rules + output contract) + identity + directive
// (+ running-memory) + capability manifest (live tool list) + running-sessions
// digest + scratchpad + recalled memories + the memory explainer.
std::string composeInstructions(const ComposeInputs& in);

}  // namespace agent
