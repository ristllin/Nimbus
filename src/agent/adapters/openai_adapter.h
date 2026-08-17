#pragma once
#include "../adapter.h"
#include "../head_tools.h"  // agent::HeadTools - the multi-turn tool-use loop glue
#include "nimbus/orch/token_usage.h"  // TokenUsage - summed across the turn's rounds

// OpenAI Responses API adapter - background mode (async, server-side agent loop).
// Ported from Nuage-Solide src/agent/adapters/openai_adapter.h.
namespace agent {

// Reusable OpenAI-Responses HTTP, parameterized by host/key/model so the
// custom/proxy adapter can target any OpenAI-compatible endpoint.
namespace openai_responses {
FabricErr dispatch(const char* host, const String& key, const char* model,
                   const char* backend, const Directive& d, char outJobId[72]);
FabricErr poll(const char* host, const String& key, const char* backend,
               const char* jobId, ResultEnvelope& env);
FabricErr cancel(const char* host, const String& key, const char* jobId);

// One Head-Orchestrator turn on the Responses API. Single-shot: strict
// text.format json_schema (server-stored, unchanged). When `tools` is non-null
// AND store::orchToolLoop() is on, runs the bounded multi-turn tool-use loop -
// STATELESS since Stage 2 phase 3: every round replays the full input[] rendered
// from the device-owned canonical transcript (store:false, no
// previous_response_id - the old F20 chain-poisoning class is gone); reasoning
// models carry their encrypted reasoning items across rounds. STOPS when the
// model calls orch_turn (its arguments ARE the turn). convId gets a stateless
// marker. Returns the turn JSON in outJson; false+err on failure.
bool orchTurn(String& convId, const String& instructions, const String& inputs,
              String& outJson, String& err, const HeadTools* tools = nullptr,
              nimbus::orch::TokenUsage* usage = nullptr);
}  // namespace openai_responses

class OpenAIAdapter : public ManagedAgentAdapter {
 public:
  const char*  backendId()    const override { return "openai"; }
  Capabilities capabilities() const override;

  FabricErr dispatch(const Directive& d, char outJobId[72]) override;
  FabricErr poll(const char* jobId, ResultEnvelope& env) override;
  FabricErr cancel(const char* jobId) override;
};

}  // namespace agent
