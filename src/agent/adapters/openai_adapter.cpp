#include "openai_adapter.h"

#include <cstring>

#include "../agent_config.h"
#include "../store.h"
#include "../transport_tls.h"
#include "nimbus/harness/providers.h"

// OpenAI Responses API adapter - THIN device wrapper since Stage H of the
// harness extraction. The entire wire (background dispatch/poll/cancel, the
// strict-json_schema single-shot head turn, the STATELESS full-replay tool loop
// rendered from the canonical transcript) lives in lib/harness/src/providers/
// openai.cpp, host-tested by test_harness_wire_openai. This file only binds the
// device deps and keeps the historical openai_responses:: public surface +
// String<->std::string conversion. Model-default policy (OPENAI_MODEL + the
// deep_research pick) stays here - agent_config.h remains authoritative.

namespace agent {
namespace openai_responses {

FabricErr dispatch(const char* host, const String& key, const char* model,
                   const char* backend, const Directive& d, char outJobId[72]) {
  return providers::oaiDispatch(deviceProviderDeps(), host, std::string(key.c_str()),
                                model, backend, d, outJobId);
}

FabricErr poll(const char* host, const String& key, const char* backend,
               const char* jobId, ResultEnvelope& env) {
  return providers::oaiPoll(deviceProviderDeps(), host, std::string(key.c_str()),
                            backend, jobId, env);
}

FabricErr cancel(const char* host, const String& key, const char* jobId) {
  return providers::oaiCancel(deviceProviderDeps(), host, std::string(key.c_str()), jobId);
}

bool orchTurn(String& convId, const String& instructions, const String& inputs,
              String& outJson, String& err, const HeadTools* tools,
              nimbus::orch::TokenUsage* usage) {
  std::string cv(convId.c_str()), out, e;
  const bool ok = providers::orchTurnOpenAI(deviceProviderDeps(), cv,
                                            std::string(instructions.c_str()),
                                            std::string(inputs.c_str()),
                                            out, e, tools, usage);
  convId  = cv.c_str();
  outJson = out.c_str();
  err     = e.c_str();
  return ok;
}

}  // namespace openai_responses

// ---- OpenAIAdapter: thin wrapper binding the live OpenAI host/key/model -----

Capabilities OpenAIAdapter::capabilities() const {
  Capabilities c;
  c.cancelable        = true;
  c.typicalLatencySec = 120;
  return c;
}

FabricErr OpenAIAdapter::dispatch(const Directive& d, char outJobId[72]) {
  const char* model = (d.model && d.model[0]) ? d.model : OPENAI_MODEL;
  // deep_research skill picks a research model when the caller didn't pin one.
  if ((!d.model || !d.model[0]) && d.skill && (strcmp(d.skill, "deep_research") == 0 ||
                                            strcmp(d.skill, "deep-research") == 0))
    model = "o4-mini-deep-research";
  return openai_responses::dispatch(OPENAI_HOST, store::openaiKey(), model, "openai", d, outJobId);
}

FabricErr OpenAIAdapter::poll(const char* jobId, ResultEnvelope& env) {
  return openai_responses::poll(OPENAI_HOST, store::openaiKey(), "openai", jobId, env);
}

FabricErr OpenAIAdapter::cancel(const char* jobId) {
  return openai_responses::cancel(OPENAI_HOST, store::openaiKey(), jobId);
}

}  // namespace agent
