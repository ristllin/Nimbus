#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/status.h"

// plain/turn.h - the types every part of the plain engine (namespace agent::plain)
// shares: the turn's context, the wire dialect, tool calls and results. It includes
// nothing from the old engine, so engine-2 code never drags engine-1 headers in.
//
// Seam declared by lane RB-B0a.
namespace nimbus {
namespace orch {
struct Principal;   // tool_registry.h: who is calling (the per-person data boundary)
}  // namespace orch
}  // namespace nimbus

namespace agent {
namespace plain {

using Status = nimbus::orch::tx::Status;
using Err = nimbus::orch::tx::Err;

// What started a turn. Slugs are stored on the t item: frozen.
enum class Trigger : uint8_t {
  Owner = 0,        // a person's chat message
  Console = 1,      // the serial or test console
  Routine = 2,      // a routine fire (its own transcript routine:<id>)
  AgentResult = 3,  // a sub-agent report arrived
  Notice = 4,       // a device notice with no model call (update notice, reminder)
  Resume = 5,       // the one automatic resume after a crash
};
inline const char* triggerSlug(Trigger t) {
  switch (t) {
    case Trigger::Owner: return "owner";
    case Trigger::Console: return "console";
    case Trigger::Routine: return "routine";
    case Trigger::AgentResult: return "agent";
    case Trigger::Notice: return "notice";
    case Trigger::Resume: return "resume";
  }
  return "owner";
}
// A turn a person is waiting on (progress signals, the late-message join). The one
// automatic resume answers an owner message, so it counts.
inline bool personTrigger(Trigger t) { return t == Trigger::Owner || t == Trigger::Console || t == Trigger::Resume; }

// The request shape a head speaks (PRD section 1, "Request on each wire").
enum class Dialect : uint8_t {
  AnthropicMessages = 0,   // plain_wire_anthropic.cpp
  OpenAIResponses = 1,     // plain_wire_openai.cpp
  ChatCompletions = 2,     // plain_wire_chat.cpp: Mistral, Z.ai, OpenAI-compatible, routed
  Flat = 3,                // plain_wire_flat.cpp: custom endpoints, with tools or text mode
};
inline constexpr Dialect kAllDialects[] = {Dialect::AnthropicMessages, Dialect::OpenAIResponses,
                                           Dialect::ChatCompletions, Dialect::Flat};
inline const char* dialectSlug(Dialect d) {
  switch (d) {
    case Dialect::AnthropicMessages: return "anthropic";
    case Dialect::OpenAIResponses: return "responses";
    case Dialect::ChatCompletions: return "chat";
    case Dialect::Flat: return "flat";
  }
  return "chat";
}

// The complete list of harness-written messages (PRD section 8.9). Slugs are the
// exemption names of the delivered-bytes test: frozen. Append only.
enum class StatusLine : uint8_t {
  None = 0,
  UpdateNotice = 1,
  VoiceHeard = 2,
  LimitReached = 3,
  CutAnswer = 4,
  ProviderError = 5,
  CrashNotice = 6,
  PausedSpend = 7,
  FailedWrite = 8,
  SummarizerTrouble = 9,
  StorageState = 10,
  Stopped = 11,
  ModerationBlock = 12,
};
inline constexpr StatusLine kAllStatusLines[] = {
    StatusLine::UpdateNotice,  StatusLine::VoiceHeard,   StatusLine::LimitReached,      StatusLine::CutAnswer,
    StatusLine::ProviderError, StatusLine::CrashNotice,  StatusLine::PausedSpend,       StatusLine::FailedWrite,
    StatusLine::SummarizerTrouble, StatusLine::StorageState, StatusLine::Stopped,       StatusLine::ModerationBlock,
};
inline const char* statusLineSlug(StatusLine s) {
  switch (s) {
    case StatusLine::None: return "none";
    case StatusLine::UpdateNotice: return "update-notice";
    case StatusLine::VoiceHeard: return "voice-heard";
    case StatusLine::LimitReached: return "limit-reached";
    case StatusLine::CutAnswer: return "cut-answer";
    case StatusLine::ProviderError: return "provider-error";
    case StatusLine::CrashNotice: return "crash-notice";
    case StatusLine::PausedSpend: return "paused-spend";
    case StatusLine::FailedWrite: return "failed-write";
    case StatusLine::SummarizerTrouble: return "summarizer-trouble";
    case StatusLine::StorageState: return "storage-state";
    case StatusLine::Stopped: return "stopped";
    case StatusLine::ModerationBlock: return "moderation-block";
  }
  return "none";
}

// One tool call as the model made it, and what the model is shown back.
struct ToolCall {
  std::string id;        // the provider's call id
  std::string name;      // the tool name as advertised (read_file, edit_file, ...)
  std::string argsJson;  // the raw JSON arguments object ("{}" if none)
  bool complete = true;  // false: cut or not valid JSON; never dispatched
};
struct ToolOutput {
  std::string id;        // echoes ToolCall::id
  std::string name;
  std::string text;      // what the model sees
  bool isError = false;
  bool outside = false;  // outside content (web, fetch, connectors): taints the turn
};

// One advertised tool (name, description at most 240 B, JSON schema).
struct ToolSpec {
  std::string name;
  std::string description;
  std::string schemaJson;
};

// The turn every seam is told about. Borrowed by the hooks for the length of one call.
struct TurnCtx {
  std::string chatKey;          // the transcript key (/mem/tx/<chatKey>)
  std::string chatId;           // the routing chat
  uint32_t turn = 0;
  Trigger trigger = Trigger::Owner;
  std::string host;             // the head answering this turn
  std::string model;
  Dialect dialect = Dialect::AnthropicMessages;
  nimbus::orch::tx::Profile profile = nimbus::orch::tx::Profile::Standard;
  uint8_t attempt = 0;
  uint16_t roundNo = 0;         // the round about to run or just run
  uint64_t startedAt = 0;       // epoch seconds; 0 = clock not set
  uint32_t startedMs = 0;       // monotonic
  bool tainted = false;         // the request carries an outside result whole
  const nimbus::orch::Principal* who = nullptr;
};

}  // namespace plain
}  // namespace agent
