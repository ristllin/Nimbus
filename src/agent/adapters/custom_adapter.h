#pragma once
#include "../adapter.h"

// Custom / proxy adapter - ported from Nuage-Solide
// src/agent/adapters/custom_adapter.h. Routes a category to any OpenAI-, Mistral-,
// or Anthropic-compatible endpoint (Azure OpenAI, Bedrock/Foundry proxies,
// Fireworks, vLLM, LiteLLM, self-hosted, ...). Configured via NVS: base host, API
// key, wire convention, model. Active (registered as backend "custom") when
// store::customBase() is non-empty.
//
// A generic proxy exposes a synchronous chat-completion, not an async agent
// harness, so this adapter runs the completion inline in dispatch() and caches the
// answer; poll() returns it immediately (one RAM slot - proxy jobs are short-lived).
namespace agent {

class CustomAdapter : public ManagedAgentAdapter {
 public:
  const char*  backendId()    const override { return "custom"; }
  Capabilities capabilities() const override;

  FabricErr dispatch(const Directive& d, char outJobId[72]) override;
  FabricErr poll(const char* jobId, ResultEnvelope& env) override;
  FabricErr cancel(const char* jobId) override;
};

}  // namespace agent
