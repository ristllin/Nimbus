#pragma once
#include "../adapter.h"

// Z.ai (GLM) adapter - thin device wrapper over the portable OpenAI-compatible
// sub-session (lib/harness/src/providers/openai_compat.cpp). Registered as backend
// "zai" when store::hasZaiKey(). Host is the probed base (api.z.ai |
// open.bigmodel.cn), base path /api/paas/v4 (NOT /v1). A GLM sub-agent runs one
// synchronous chat-completion; poll returns the cached reply.
namespace agent {

class ZaiAdapter : public ManagedAgentAdapter {
 public:
  const char*  backendId()    const override { return "zai"; }
  Capabilities capabilities() const override;

  FabricErr dispatch(const Directive& d, char outJobId[72]) override;
  FabricErr poll(const char* jobId, ResultEnvelope& env) override;
  FabricErr cancel(const char* jobId) override;
};

}  // namespace agent
