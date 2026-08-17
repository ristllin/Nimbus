#pragma once
#include "../adapter.h"
#include "../head_tools.h"  // agent::HeadTools - the multi-turn tool-use loop glue
#include "nimbus/orch/token_usage.h"  // TokenUsage - summed across the turn's rounds

// Anthropic adapter: Claude Managed Agents - ported from Nuage-Solide
// src/agent/adapters/anthropic_adapter.h. The agent harness runs server-side in a
// managed cloud sandbox with the built-in toolset (bash, web search/fetch, file
// ops); the device creates a session, sends a turn, and polls events.
//
//   one-time (cached in NVS): POST /v1/agents, POST /v1/environments
//   dispatch:  POST /v1/sessions  +  POST /v1/sessions/{id}/events (user.message)
//   poll:      GET  /v1/sessions/{id}/events  -> agent.message text + status_idle
//   cancel:    POST /v1/sessions/{id}/events (user.interrupt)
//   answer:    POST /v1/sessions/{id}/events (user.message) - resumes NeedsInput
// Beta header: managed-agents-2026-04-01.  jobId = "anthropic:<session_id>".
//
// LIVE-GATED: needs store::anthropicKey() + env/agent creation (web UI / NVS) +
// STA WiFi. VERIFY(2026-06): beta header + endpoints re-verify at implementation
// (platform.claude.com/docs/en/managed-agents).
namespace agent {

class AnthropicAdapter : public ManagedAgentAdapter {
 public:
  const char*  backendId()    const override { return "anthropic"; }
  Capabilities capabilities() const override;

  FabricErr dispatch(const Directive& d, char outJobId[72]) override;
  FabricErr poll(const char* jobId, ResultEnvelope& env) override;
  FabricErr cancel(const char* jobId) override;
  FabricErr answer(const char* jobId, const char* userText) override;
};

// One Head-Orchestrator turn hosted on Anthropic via the Messages API. Stateless
// by design (the turn's `memory` field carries cross-turn state); convId is a
// marker. When `tools` is non-null AND store::orchToolLoop() is on, runs the
// bounded multi-turn tool-use loop (the model calls registry tools mid-turn,
// terminating on the orch_turn tool); otherwise a single forced-orch_turn shot.
// false+err on failure. `tools` defaults to nullptr (single-shot).
bool orchTurnAnthropic(String& convId, const String& instructions, const String& inputs,
                       String& outJson, String& err, const HeadTools* tools = nullptr,
                       nimbus::orch::TokenUsage* usage = nullptr);

}  // namespace agent
