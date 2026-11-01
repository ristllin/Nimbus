#pragma once
// The chat-completions tool loop's wire, shared by the heads that speak that dialect
// (Mistral and Z.ai): messages rendered from the canonical transcript, the tools array,
// and the reading of one round. What differs per host is how a call id and a tool name
// travel. Private to lib/harness/src/providers.
#include <ArduinoJson.h>

#include <string>

#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/transcript.h"

namespace agent {
namespace providers {

struct ChatDialect {
  std::string (*callId)(const std::string& id);          // transcript id -> wire id
  std::string (*wireName)(const std::string& name);      // registry name -> wire name
  std::string (*registryName)(const std::string& name);  // wire name -> registry name
};

// system (per-round instructions), the history, the user seed, then per tool-carrying
// round an assistant message answered by role:"tool" messages.
void renderChatMessages(const nimbus::orch::Transcript& tr, ArduinoJson::JsonArray msgs,
                        const std::string& sysText, bool showingImages, const layout::PromptParts* parts,
                        bool sees, const ChatDialect& dialect);

// The tools array: orch_turn first (shapes only, the field docs ride the prompt), then
// every registry tool under its wire name. `onlyFinal` leaves just orch_turn: a forced
// final round whose model might ignore a named tool_choice has nothing else to call.
bool putChatTools(const ProviderDeps& pd, ArduinoJson::JsonArray tools, const HeadTools* ht,
                  const ChatDialect& dialect, bool onlyFinal, std::string& err);

// The per-turn block and the photos join the seeded user message.
void seedChatUserMessage(ArduinoJson::JsonArray msgs, const layout::PromptParts& p,
                         nimbus::orch::TurnImages* imgs);

// Read one reply into `out`. False when it is cut (finish_reason "length" or
// "model_length", or arguments that do not parse): a cut argument string must never be
// dispatched or become the turn. `cutWhy` names it.
bool readChatRound(const JsonDocument& resp, nimbus::orch::HeadStep& out, std::string& cutWhy,
                   const ChatDialect& dialect);

// The response filter of one round: finish_reason, content, tool calls, usage, model.
void chatRoundFilter(JsonDocument& filter);

}  // namespace providers
}  // namespace agent
