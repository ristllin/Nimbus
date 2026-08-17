#pragma once
#include "../adapter.h"
#include "../head_tools.h"  // agent::HeadTools - the multi-turn tool-use loop glue
#include "nimbus/orch/token_usage.h"  // TokenUsage - summed across the turn's rounds

// Mistral adapter - host turn + sub-session, ported/modernized from
// Nuage-Solide src/mistral.cpp (P5 debt paid, 2026-07).
//
//   1. orchTurnMistral() - one Head-Orchestrator turn on the CONVERSATIONS API
//      (POST /v1/conversations, then /v1/conversations/{id} to continue):
//      instructions + inputs, with completion_args.response_format carrying the
//      canonical orch_turn json_schema (nimbus/orch/orch_schema.h) - structured
//      output enforced server-side, per the repo's agent-I/O convention.
//      convId = the server conversation id (stateful across turns).
//
//   2. MistralAdapter - sub-session via ONE synchronous chat/completions call
//      (Nuage's proven pattern): dispatch() runs the task to completion and
//      caches the reply; poll() returns it as Done and frees the slot. Mistral
//      turns are seconds-fast, so the sync window is acceptable; a reboot
//      between dispatch and poll loses the cache and polls NotFound (the
//      journal's expired-job path handles that cleanly).
//
// HTTP transport mirrors anthropic_adapter's antRequest: HTTP/1.0 +
// Connection: close (no chunked encoding to parse), filter-based ArduinoJson
// reads, TLS via the work arbiter.
namespace agent {

// One stateful head-orchestrator turn on Mistral. Single-shot: strict
// response_format json_schema. When `tools` is non-null AND store::orchToolLoop()
// is on, runs the bounded multi-turn tool-use loop instead - function tools pin on
// a FRESH conversation (Conversations tools pin at creation), rounds resubmit
// function.result entries on that conversation_id, STOP on the orch_turn call.
// Returns true with outJson = the orch_turn object; false with err set.
bool orchTurnMistral(String& convId, const String& instructions, const String& inputs,
                     String& outJson, String& err, const HeadTools* tools = nullptr,
                     nimbus::orch::TokenUsage* usage = nullptr);

class MistralAdapter : public ManagedAgentAdapter {
 public:
  const char*  backendId()    const override { return "mistral"; }
  Capabilities capabilities() const override {
    Capabilities c;
    c.cancelable        = false;   // sync: by poll time it is already done
    c.typicalLatencySec = 20;
    return c;
  }

  FabricErr dispatch(const Directive& d, char outJobId[72]) override;
  FabricErr poll(const char* jobId, ResultEnvelope& env) override;
  FabricErr cancel(const char* jobId) override;
};

}  // namespace agent
