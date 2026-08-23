#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/harness/fabric.h"      // Directive / ResultEnvelope / FabricErr
#include "nimbus/harness/head_tools.h"  // HeadTools - the multi-turn tool-use loop glue
#include "nimbus/harness/http.h"        // HttpTransport - the device/host seam
#include "nimbus/orch/token_usage.h"

// providers - the PORTABLE provider wire layer (Stage H of the harness
// extraction). Everything that used to live inside the device adapters except
// the socket: request URL/header/body construction, structured-output config,
// the head tool-use loop steps (via nimbus::orch::runHeadLoop), response
// parsing, usage extraction, and error mapping - all in std::string space over
// the agent::HttpTransport contract, so the full provider wire is host-tested
// with FakeHttpTransport (test/test_harness_wire_*).
//
// The DEVICE keeps only: the TLS transport (src/agent/transport_tls.cpp -
// WiFiClientSecure + tls_arbiter + the one-buffer-single-write rule) and thin
// String<->std::string wrappers in src/agent/adapters/*.cpp (public surfaces
// unchanged).
//
// Every AGENTS.md wire lesson moved verbatim - the comments ride with the code:
// Anthropic ADVISORY schema + echo-assistant-only-with-tool_use, OpenAI strict
// json_schema + previous_response_id + the F20 chain-poison guard, Mistral
// tool_choice "required" (NOT "any") + no built-in connectors on loop turns,
// custom keyless-on-http. Bodies are built as ONE contiguous std::string and
// handed to the transport whole (on the device, malloc >=128 B spills to PSRAM
// via heap_caps_malloc_extmem_enable(128) - see main.cpp - so large bodies
// never sit on internal SRAM; the transport then does a single client.write).
namespace agent {
namespace providers {

// The dependency bundle every portable provider function takes. All closures
// are nullable unless noted; a missing closure reads as "" / false / no-op.
// The device builds ONE of these (src/agent/transport_tls.cpp
// deviceProviderDeps()) over store:: + connectors:: + PsramJsonAllocator;
// tests build it over fakes.
struct ProviderDeps {
  HttpTransport* http = nullptr;   // REQUIRED

  // Credentials + models (host = "openai" | "anthropic" | "mistral").
  std::function<std::string(const char* host)> key;
  std::function<std::string(const char* host)> orchModel;

  // The head tool-loop master switch (store::orchToolLoop). The engine also
  // gates by passing tools=nullptr; both must be on for a loop turn (the
  // pre-split adapters checked store::orchToolLoop() themselves - preserved).
  std::function<bool()> toolLoopOn;

  // Anthropic managed-agents NVS caches (sub-sessions only): the shared cloud
  // environment id + the per-model agent map "model=agentid;...".
  std::function<std::string()> antEnvId;
  std::function<void(const std::string&)> setAntEnvId;
  std::function<std::string()> antAgentMap;
  std::function<void(const std::string&)> setAntAgentMap;

  // Custom/proxy endpoint (backend "custom"): base URL ("http://host:port[/..]"
  // = plain HTTP + keyless; "https://"/bare host = TLS), key, wire convention
  // ("openai"|"mistral"|"anthropic"), model.
  std::function<std::string()> customBase, customKey, customConv, customModel;

  // Provider-side connector/MCP attach (device: connectors::attach* append tool
  // entries to the request doc). Nullable = none. attachOpenAI rides the OpenAI
  // Responses head + sub-agent dispatch; attachMistral the Conversations
  // single-shot head; attachAnthropic the managed-agent CREATION body
  // (mcp_servers[], sub-agents only).
  std::function<void(JsonDocument&)> attachOpenAI;
  std::function<void(JsonDocument&)> attachMistral;
  std::function<void(JsonDocument&)> attachAnthropic;

  // Platform (loop deadline/heap re-gate). nowMs required for loop turns.
  std::function<uint32_t()> nowMs;
  std::function<uint32_t()> freeHeap;
  // Optional: largest contiguous free INTERNAL block (fragmentation trace for the
  // per-round headloop log - free-vs-largest divergence IS fragmentation).
  std::function<uint32_t()> largestBlock;

  // JsonDocument allocator for the big loop docs (device: PsramJsonAllocator).
  // nullptr = ArduinoJson's default (host tests; also fine on device since
  // malloc spills to PSRAM at >=128 B, but the explicit allocator keeps the
  // node pools off internal SRAM deterministically).
  ArduinoJson::Allocator* alloc = nullptr;
};

// ---- head-orchestrator turns (std::string space) ----------------------------
// Same contract as the engine's ProviderTurnFn: send system(instructions) +
// user(inputs), get the strict orch_turn JSON in outJson. convId is in-out
// provider continuity; tools non-null (AND toolLoopOn) => the bounded
// multi-turn tool-use loop.
bool orchTurnAnthropic(const ProviderDeps& pd, std::string& convId,
                       const std::string& instructions, const std::string& inputs,
                       std::string& outJson, std::string& err,
                       const HeadTools* tools, nimbus::orch::TokenUsage* usage);
bool orchTurnOpenAI(const ProviderDeps& pd, std::string& convId,
                    const std::string& instructions, const std::string& inputs,
                    std::string& outJson, std::string& err,
                    const HeadTools* tools, nimbus::orch::TokenUsage* usage);
bool orchTurnMistral(const ProviderDeps& pd, std::string& convId,
                     const std::string& instructions, const std::string& inputs,
                     std::string& outJson, std::string& err,
                     const HeadTools* tools, nimbus::orch::TokenUsage* usage);
// Mistral reserved-connector-name guard (web_search collides with our Tavily
// tool → 422). safeName renames on the wire; unsafeName inverts before dispatch.
std::string mistralSafeName(const std::string& n);
std::string mistralUnsafeName(const std::string& n);

// ---- per-provider STEP FACTORIES (Stage 2 phase 5) --------------------------
// Each returns a HeadStepFn performing ONE model turn on that provider, rendered
// from (and paired with) the SHARED canonical transcript - the building block of
// the engine's mid-turn failover: a failed round on host A re-runs on host B
// against the same transcript (results are data; nothing re-dispatches).
// ⚠ Lifetime: `instructions`/`ht`/`usage`/`tr` are captured BY REFERENCE - the
// caller's frame must outlive every call of the returned step. `pd` is copied.
nimbus::orch::HeadStepFn antLoopStep(const ProviderDeps& pd, const std::string& instructions,
                                     const HeadTools& ht, nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr);
nimbus::orch::HeadStepFn oaiLoopStep(const ProviderDeps& pd, const std::string& instructions,
                                     const HeadTools& ht, nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr);
nimbus::orch::HeadStepFn misLoopStep(const ProviderDeps& pd, const std::string& instructions,
                                     const HeadTools& ht, nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr);

// True iff the fabric loop can drive this host (anthropic/openai/mistral - the
// custom LAN backend is single-shot only). The engine consults this before
// routing a turn to the fabric; keep in sync with loop_common's makeStep.
bool fabricSupports(const std::string& host);

// The engine-owned multi-provider fabric loop (impl: providers/loop_common.cpp).
// One canonical Transcript; a failed round retries same-host once, then switches
// to the next host in `hostList` (bounded ≤2 switches), re-running the SAME
// round - executed results ride the transcript, nothing re-dispatches. `notify`
// fires once per switch (fromHost, toHost).
bool runFabricLoop(const ProviderDeps& pd, const std::vector<std::string>& hostList,
                   const std::string& instructions, const std::string& inputs,
                   std::string& outJson, std::string& err, const HeadTools& ht,
                   nimbus::orch::TokenUsage* usage,
                   const std::function<void(const std::string& fromHost,
                                            const std::string& toHost)>& notify);
// Normalize a tool_call id to chat-completions' required 9-alphanumeric shape.
// Mistral's own ids pass through; a FOREIGN id (an Anthropic/OpenAI id in a
// transcript carried over by mid-turn failover) maps to a deterministic 9-char
// digest - applied consistently to a call and its paired tool message.
std::string mistralCallId(const std::string& id);
// NEW (Stage H): the head on a custom/LAN endpoint - ONE single-shot structured
// turn over the OpenAI chat-completions dialect (response_format json_schema
// when the backend accepts it; on a 400 it retries once schema-less with a
// JSON-only nudge, leaning on the engine's tolerant parseTurn + salvage).
// v1 has NO tool loop (tools ignored); customConv "anthropic" is refused (the
// head speaks chat-completions only). Registered in orchestrator.cpp ONLY when
// store::hasCustom().
bool orchTurnCustom(const ProviderDeps& pd, std::string& convId,
                    const std::string& instructions, const std::string& inputs,
                    std::string& outJson, std::string& err,
                    const HeadTools* tools, nimbus::orch::TokenUsage* usage);

// ---- sub-session dispatch/poll/cancel/answer --------------------------------
// Anthropic Managed Agents (cloud sandbox sessions; env/agent cached via the
// ant* closures). defaultModel = the device's ANT_MODEL fallback.
FabricErr antDispatch(const ProviderDeps& pd, const char* defaultModel,
                      const Directive& d, char outJobId[72]);
FabricErr antPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env);
FabricErr antCancel(const ProviderDeps& pd, const char* jobId);
FabricErr antAnswer(const ProviderDeps& pd, const char* jobId, const char* userText);

// OpenAI Responses background mode, host/key/model-parameterized (kept generic
// exactly like the pre-split openai_responses:: so a proxy could reuse it).
FabricErr oaiDispatch(const ProviderDeps& pd, const char* host, const std::string& key,
                      const char* model, const char* backend, const Directive& d,
                      char outJobId[72]);
FabricErr oaiPoll(const ProviderDeps& pd, const char* host, const std::string& key,
                  const char* backend, const char* jobId, ResultEnvelope& env);
FabricErr oaiCancel(const ProviderDeps& pd, const char* host, const std::string& key,
                    const char* jobId);

// Mistral sub-session: ONE synchronous chat/completions call, result cached in
// a RAM slot; poll serves it as Done. model = the resolved sub model.
FabricErr mistralDispatch(const ProviderDeps& pd, const std::string& model,
                          const Directive& d, char outJobId[72]);
FabricErr mistralPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env);
FabricErr mistralCancel(const ProviderDeps& pd, const char* jobId);

// Custom/proxy sub-session: synchronous chat-completion per the configured wire
// convention (openai/mistral -> /v1/chat/completions; anthropic -> /v1/messages).
FabricErr customDispatch(const ProviderDeps& pd, const Directive& d, char outJobId[72]);
FabricErr customPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env);
FabricErr customCancel(const ProviderDeps& pd, const char* jobId);

// Generic OpenAI-compatible sub-session (Z.ai GLM, Cumulo router, or any other
// chat-completions endpoint). Synchronous: dispatch runs the call inline against
// host+basePath (basePath+"/chat/completions", Bearer key) and caches the answer;
// poll returns it. jobId = "<backendTag>:<id>"; the SAME backendTag routes poll/
// cancel back here via the device adapter. A small multi-slot RAM cache lets Z.ai
// and Cumulo coexist. The endpoint is passed explicitly (not via the pd.custom*
// closures) so several such providers run at once.
// The upstream's native wire. A router (Cumulo) proxies each upstream on ITS OWN
// wire under /router/<upstream>/v1/: OpenAI/Mistral/Z.ai speak chat-completions,
// Anthropic speaks Messages. Auth is Bearer either way (the router key).
enum class CompatWire { OpenAIChat, AnthropicMessages };

struct CompatEndpoint {
  const char* host = nullptr;
  uint16_t    port = 443;
  bool        tls = true;
  std::string basePath;     // e.g. "/api/paas/v4" or "/router/anthropic/v1"
  std::string key;          // Bearer token ("" = no auth header)
  std::string model;
  const char* backendTag = "compat";  // jobId prefix + adapter route key
  CompatWire  wire = CompatWire::OpenAIChat;
};
FabricErr openaiCompatDispatch(const ProviderDeps& pd, const CompatEndpoint& ep,
                               const Directive& d, char outJobId[72]);
FabricErr openaiCompatPoll(const ProviderDeps& pd, const char* backendTag, const char* jobId,
                           ResultEnvelope& env);
FabricErr openaiCompatCancel(const ProviderDeps& pd, const char* jobId);

}  // namespace providers
}  // namespace agent
