#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/scratchpad.h"

// world - the orchestrator's self-awareness + per-turn context assembly.
// It owns system-prompt section ordering, memory injection + budget, and the
// running-sessions digest. Three portable pieces, all Arduino-free and
// host-tested:
//
//   1. CapabilityManifest - the "## CAPABILITIES" block: the hardware the agent
//      inhabits + the tools it can call. Generated from a Hardware descriptor +
//      a tool list (the tool list comes from the ToolRegistry so it never drifts
//      from what actually dispatches).
//   2. SessionsDigest - the "## RUNNING SESSIONS" block: a summary of live
//      sub-agents so the orchestrator is always aware of its children.
//   3. assembleContext - builds the full system prompt in the §2 priority order
//      under a byte budget, truncating/dropping the lowest-priority sections
//      first.
namespace nimbus {
namespace orch {

// ---- capability / world manifest -------------------------------------------
struct Hardware {
  const char* deviceName = "Nimbus";
  const char* version    = "";
  // Display/input pair. A device has EITHER eink+encoder OR touch (whose panel
  // consumes the encoder's pins) - never both. Without `touch` the manifest
  // could not express a touchscreen at all, so the model told owners of a touch
  // board to look at an e-ink panel and listed no input device.
  bool ring = true, eink = true, touch = false, encoder = true, mic = false, speaker = false;
  bool battery = false, sd = false, wifi = false, ble = false, telegram = false;
  // E1 artifact store: SD /mem/files present (files.list / artifact.save /
  // files.send are genuinely callable).
  bool files = false;
  // Owner's "Voice replies" toggle (default ON for a speaker board, N12): when
  // false, spoken output (tts action / reply.speak / reply.telegram voice) is
  // disabled and the prompt tells the model to reply in text (never audio-in-
  // addition). The struct default here stays false (a conservative host/test
  // default); the device fills it from store::ttsEnabled(), whose default is ON.
  bool voiceReplies = false;
  // W10: the LIVE ring LED count from the driver - the manifest hardcoded
  // "45-LED ring" while the animator already took the count as a parameter.
  uint16_t ledCount = 45;
};

struct ToolInfo {
  std::string name;         // e.g. "memory.search"
  std::string description;  // one line
};

// Render the "## CAPABILITIES" block. Lists the present hardware and, if any,
// the available tools by name+description. Deterministic (stable ordering) so
// golden/host tests can pin it.
std::string renderCapabilities(const Hardware& hw, const std::vector<ToolInfo>& tools);

// ---- running-sessions digest -----------------------------------------------
struct SessionInfo {
  std::string id;        // journal tag, e.g. "job0003"
  std::string provider;  // "openai" | "anthropic" | ...
  std::string model;
  std::string title;     // the task, one line
  std::string state;     // "queued" | "running" | "done" | ...
  int  turns = 0;
  bool pendingReply = false;  // a reply is waiting to be polled
  uint8_t hue = 255;          // ring accent hue for this session (255 = white/unknown)
};

// Render the "## RUNNING SESSIONS" block, or "" if there are none (the assembler
// then skips the section) - the active-session summary.
std::string renderSessions(const std::vector<SessionInfo>& sessions);

// ---- per-turn context assembly (§2) ----------------------------------------
// The inputs the assembler weaves, in priority order. Every field is optional
// (empty = section skipped). `recalled` are the top-K associative memories
// already retrieved (Ph3 supplies them; Ph1 accepts them as strings).
struct ContextInputs {
  std::string immutableRules;   // 1. safety + device rails (always first)
  std::string identity;         // 2. "You are Nimbus vX on an ESP32-S3..."
  std::string directive;        // 3. user directive (verbatim, already capped)
  std::string capabilities;     // 4. renderCapabilities(...)
  std::string sessions;         // 5. renderSessions(...)  ("" => skipped)
  std::string chatSummary;         // 5a. per-chat "## CONVERSATION SUMMARY" (fold)
  std::string recentConversation;  // 5b. per-chat "## RECENT CONVERSATION" window
                                   //     (pre-rendered, byte-capped; "" => skipped)
  const Scratchpad* scratchpad = nullptr;  // 6. model working memory
  std::vector<std::string> recalled;       // 7. "## RELEVANT MEMORIES" bullets
  std::string memoryExplainer;  // 8. how-your-memory-works note ("" => skipped)
};

// Outcome of an assembly: the prompt plus which sections were truncated/dropped
// under the budget (so the device can surface it and tests can assert it).
struct AssembledContext {
  std::string prompt;
  int  bytes = 0;
  bool truncated = false;                 // any section dropped/cut
  std::vector<std::string> droppedSections;  // names, for diagnostics
};

// Assemble the system prompt in §2 order under `budgetBytes`. Higher-priority
// sections (rules/identity/directive) are never dropped; lower-priority sections
// (recall, scratchpad long-tail) truncate or drop first when the budget binds.
// Deterministic and allocation-bounded.
AssembledContext assembleContext(const ContextInputs& in, int budgetBytes = kContextBudgetMax);

}  // namespace orch
}  // namespace nimbus
