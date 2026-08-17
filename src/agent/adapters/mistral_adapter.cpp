#include "mistral_adapter.h"

#include "../store.h"
#include "../transport_tls.h"
#include "nimbus/harness/providers.h"

// Mistral adapter - THIN device wrapper since Stage H of the harness
// extraction. The entire wire (the Conversations-API head turn with strict
// response_format json_schema, the fresh-conversation tool loop with
// tool_choice "required" - NOT "any" - and no Studio built-ins on loop turns,
// the synchronous chat/completions sub-session + result cache) lives in
// lib/harness/src/providers/mistral.cpp, host-tested by
// test_harness_wire_mistral. This file only binds the device deps and converts
// String<->std::string at the public surface (unchanged).

namespace agent {

bool orchTurnMistral(String& convId, const String& instructions, const String& inputs,
                     String& outJson, String& err, const HeadTools* tools,
                     nimbus::orch::TokenUsage* usage) {
  std::string cv(convId.c_str()), out, e;
  const bool ok = providers::orchTurnMistral(deviceProviderDeps(), cv,
                                             std::string(instructions.c_str()),
                                             std::string(inputs.c_str()),
                                             out, e, tools, usage);
  convId  = cv.c_str();
  outJson = out.c_str();
  err     = e.c_str();
  return ok;
}

FabricErr MistralAdapter::dispatch(const Directive& d, char outJobId[72]) {
  return providers::mistralDispatch(deviceProviderDeps(),
                                    std::string(store::subModel("mistral").c_str()),
                                    d, outJobId);
}

FabricErr MistralAdapter::poll(const char* jobId, ResultEnvelope& env) {
  return providers::mistralPoll(deviceProviderDeps(), jobId, env);
}

FabricErr MistralAdapter::cancel(const char* jobId) {
  return providers::mistralCancel(deviceProviderDeps(), jobId);
}

}  // namespace agent
