#pragma once
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/guards.h"   // HeadRates
#include "nimbus/harness/plain/turn.h"
#include "nimbus/harness/plain/wire.h"     // HeadEndpoint
#include "nimbus/orch/turn_spend.h"         // TokenUsage

// plain/head_slot.h - one model on one provider slot of the released engine, as engine 2 asks it
// (lane E2-R8a, router design sections 2 C14, 3.3 and 10). The device and a test build the head,
// each summarizer rung and each routine's model here, so no binding names a provider itself:
//   - the slot: anthropic, openai, mistral, zai (each its table row's host and wire), custom (the
//     owner's base URL and convention) and cumulo (the router's host, the selector's upstream wire);
//   - the turn host the engine reads (a direct slot's API host, else the slot's own name);
//   - the price rows (a local custom endpoint is free) and the spend in the ledger's counts.
namespace agent {
namespace plain {

// The provider slots engine 2 runs on, in the released engine's order.
inline constexpr const char* kHeadSlots[] = {"anthropic", "openai", "mistral", "zai", "custom", "cumulo"};

struct HeadSlot {
  std::string provider;   // a kHeadSlots name: budget, price, spend, park
  std::string model;      // the model asked for (cumulo: the "<upstream>/<model>" selector)
  Dialect dialect = Dialect::AnthropicMessages;
  HeadEndpoint endpoint;  // host, port, tls, basePath, routed, key
  // TurnCtx::host: a direct slot's API host, else the slot name (the engine's slug lookup reads it).
  std::string turnHost() const { return provider == "custom" || provider == "cumulo" ? provider : endpoint.host; }
};

// What the device knows about its slots. Every function is read when a request is rendered.
struct SlotSettings {
  std::function<std::string(const std::string& provider)> key;   // the slot's key ("" = none)
  std::string customBase;    // "http[s]://host[:port][/prefix]" or a bare host
  std::string customConv;    // "anthropic": the Messages wire; anything else: chat completions
  // The custom endpoint's verify accepted a tool call and its window is known (design 3.3); false:
  // a chat-convention endpoint runs in text mode (Dialect::Flat).
  bool customTools = false;
  std::string zaiHost;       // the probed Z.ai host ("" = the table's)
  std::string routerHost;    // the Cumulo router's base URL or host
};

// The head `model` on `provider`. InvalidArgument for a provider no slot names, an empty model, a
// custom slot with no base, or a router selector whose upstream no slot speaks.
Status headSlotFor(const SlotSettings& s, const std::string& provider, const std::string& model, HeadSlot& out);

// The wire `model` speaks on `provider`: a router selector's upstream wire on cumulo (a routine's or
// a rung's small model may name another upstream than the head), else `head` (the slot's own).
Dialect slotDialect(const std::string& provider, const std::string& model, Dialect head);

// The rates a slot bills `model` at: the shared price rows (headRatesFor); a local custom endpoint
// (customEndpointPaid false) is free. False: nothing prices it (money fails closed).
bool slotRatesFor(const std::string& provider, const std::string& model, HeadRates& out);

// A request's usage in the counts the usage ledger reads for `provider` (usage_ledger.cpp
// ledgerDelta): engine 2's codecs report the uncached part of the prompt on every wire, so a
// provider whose input count includes its cache (OpenAI, Mistral, Z.ai, a custom endpoint, the
// router unless it bills an Anthropic upstream split) gets the cache tokens added back.
nimbus::orch::TokenUsage ledgerUsage(const std::string& provider, const std::string& model, nimbus::orch::TokenUsage u);

// The custom endpoint's verify step (design section 3.3; src/agent/provider_verify.cpp sends it).
// The window a server publishes for `model` on its /v1/models answer (the row whose id is `model`,
// or the only row): max_model_len (vLLM), context_length, context_window or max_context_length; 0
// when it publishes none. Which field each server fills is read from documentation, not verified.
uint32_t servedWindowOf(const std::string& modelsBody, const std::string& model);
// The one tool-call probe: a chat request that offers one tool and asks for it, and whether a chat
// answer called it.
std::string toolProbeBody(const std::string& model);
bool answerCalledTool(const std::string& chatBody);
// The smallest window tools are offered on: the fixed overhead (about 6,000 tokens) at most half full.
inline constexpr uint32_t kCustomToolsMinWindow = 12000;
// What a stored verify says: tools for `model` only when its probe was accepted and its window is
// known and holds kCustomToolsMinWindow (the custom model store text, model_catalog.h).
bool customToolsFrom(const std::string& storeText, const std::string& model);

}  // namespace plain
}  // namespace agent
