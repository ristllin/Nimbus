#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/board_identity.h"
#include "nimbus/harness/config.h"
#include "nimbus/harness/conversation.h"       // HistMsg - the history message form
#include "nimbus/harness/prompt_layout.h"      // CacheMeta, Msg - the cache settings
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
  // CUM-514: the board this build runs on (identity line + the capability
  // line's screen/ring facts). nullptr = buildBoardIdentity(), the build's own.
  const BoardIdentity* board = nullptr;
  std::string directive;             // owner directive (OrchMemory::directive)
  std::string standingPrefs;         // the owner's standing rules block (owner_prefs.h;
                                     // "" hides [STANDING RULES] - the fresh device)
  std::string runningMemory;         // model working notes (OrchMemory::model)
  nimbus::orch::Hardware hw;         // live manifest flags (deviceName ignored -
                                     // set from devName for lifetime safety)
  std::vector<nimbus::orch::ToolInfo> tools;  // filtered + sanitized by caller
  bool loopOn = false;               // loopActiveNow() at the device
  // R3: the tool loop is ENABLED in the device config (not this turn's heap
  // gate - a per-turn flag would move the cached capability manifest with the
  // heap). True = the tools ride the request itself, so the manifest points at
  // the request's tool list instead of listing them again (orch_world.cpp).
  bool toolsOnWire = false;
  int loopRounds = kLoopRoundsDefault;  // the loop's round cap, as the prompt states it
  // CUM-523: false = this turn has no Telegram chat it can deliver to (the engine
  // sets it from its one HarnessConfig::telegramDeliverable sample). Default true: a caller that
  // does not say (nimbusd, host rigs) keeps the Telegram wording unchanged.
  bool telegram = true;
  // CUM-523: false = files.send cannot deliver on this turn (no allowlisted chat),
  // though reply.telegram may (a public-mode guest turn). Default true.
  bool fileSend = true;
  std::vector<nimbus::orch::SessionInfo> sessions;
  std::string recentConversation;    // pre-rendered "## RECENT CONVERSATION" block
                                     // ("" => none; device fills from the episodic
                                     // ring per chat - Release B1). histV1 only:
                                     // under histV2 the engine passes `history`
                                     // instead and this stays empty.
  std::string chatSummary;           // the chat's fold summary, RAW (no header -
                                     // the history pair frames it; "" => never
                                     // compacted). Rides the history as its
                                     // first message pair (prompt_layout.h).
  // The conversation history as real messages (G0's renderHistory - the message
  // form): one user and one assistant message per past turn, the summary pair
  // first when the chat was compacted. Providers send them as history items
  // (prompt_layout.h); a flat render shows them as the RECENT CONVERSATION
  // block. "" = this turn carries no history (sub-agents, the fold prompt).
  std::vector<HistMsg> history;
  // The skills shelf: the skills index, its own cached block after the static
  // one (it changes on a skill or owner edit, not per turn; "" = none). The
  // owner's standing preferences join it there (out of the static block, so an
  // owner edit re-caches only this block).
  std::string skillsShelf;
  // The turn's cache settings (prompt_layout.h): the TTL table's choice for
  // this chat and the per-chat prompt_cache_key. The engine fills both; the
  // providers read them from the split parts.
  layout::CacheMeta cache;
  const nimbus::orch::Scratchpad* scratchpad = nullptr;
  std::vector<std::string> recalled;
  // Why automatic recall was keyword-only this turn ("" = it was not): the prompt
  // says so even with no hits, so an outage never reads as "nothing relevant".
  std::string recallDegraded;
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
  // N11 (harness prompt audit): select the simplified v2 prompt (compressed
  // field docs + de-duplicated rails, wire schema unchanged). Default false =
  // byte-identical v1, so a fleet on defaults is untouched. The device sets it
  // from the NVS `promptV2` flag via HarnessConfig::promptV2; adoption is gated
  // by the eval suite. See docs/proposals/harness-prompt-audit.md.
  bool promptV2 = false;
  // The stable catalogs ([AVAILABLE PROVIDERS], [YOUR MODEL], [AVAILABLE MODELS],
  // the skills index, [PROVIDERS & CONNECTORS]) - the engine's
  // buildStableCatalogs(). They change only with the device config, so they ride
  // the end of the static block (prompt_layout.h); not counted in budgetBytes.
  std::string catalogs;
  // True when [AVAILABLE MODELS] in in.catalogs is the models.list pointer line
  // (config's loop toggle + registry + a looping head; NEVER this turn's heap
  // gate, so the cached static block cannot flip per turn). Only the dynamic
  // loop-off note reads it: on a heap-gated single-shot turn it says models.list
  // cannot run either (prism row 1).
  bool modelsPointer = false;
};

// Compose the World system prompt as the cacheable parts of prompt_layout.h,
// joined with layout::kPartSplit:
//   static  - ORCH_ROLE (rules + output contract), identity (the date, never the
//             minute), HOW YOU RUN, the capability manifest, the memory
//             explainer, then in.catalogs;
//   shelf   - the owner's standing preferences and the skills shelf (their own
//             cached block: they change on an owner or skill edit, not per turn);
//   history - the conversation history as real messages (the summary pair first
//             when the chat was compacted);
//   dynamic - the clock, the speaker line, [RUNNING MEMORY], the tool-loop note,
//             running sessions, the histV1 text window, the scratchpad and the
//             recalled memories;
//   cache   - the turn's cache settings (TTL, key), for the providers.
// The static block is budgeted alone (budgetBytes minus a per-turn reserve that
// depends on budgetBytes only, minus the shelf, history and catalog bytes), so
// two turns that differ only in per-turn content share it byte for byte.
// Providers split the string again (layout::split) or send it flat
// (layout::flat).
std::string composeInstructions(const ComposeInputs& in);

}  // namespace agent
