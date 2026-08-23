#pragma once
#include "../adapter.h"

// Cumulo Nimbus router adapter - thin device wrapper over the portable
// OpenAI-compatible sub-session. Registered as backend "cumulo" when
// store::hasCumuloKey(). One key, per-role upstream: the selected model id is
// "<upstream>/<model>" (e.g. "anthropic/claude-sonnet-5"); the adapter routes to
// <base>/router/<upstream>/v1/chat/completions with the model after the slash.
namespace agent {

class CumuloAdapter : public ManagedAgentAdapter {
 public:
  const char*  backendId()    const override { return "cumulo"; }
  Capabilities capabilities() const override;

  FabricErr dispatch(const Directive& d, char outJobId[72]) override;
  FabricErr poll(const char* jobId, ResultEnvelope& env) override;
  FabricErr cancel(const char* jobId) override;
};

}  // namespace agent
