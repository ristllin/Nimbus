#include "anthropic_adapter.h"

#include "../agent_config.h"
#include "../transport_tls.h"
#include "nimbus/harness/providers.h"

// Anthropic adapter - THIN device wrapper since Stage H of the harness
// extraction. The entire wire (Managed-Agents sub-sessions, the Messages-API
// forced-tool head turn, the stateless messages[] tool loop, the ADVISORY
// schema + description-strip, the echo-assistant-only-with-tool_use rule) lives
// in lib/harness/src/providers/anthropic.cpp, host-tested by
// test_harness_wire_anthropic. This file only binds the device deps
// (store/connectors/PSRAM/TLS via deviceProviderDeps) and converts
// String<->std::string at the public surface (unchanged).

namespace agent {

Capabilities AnthropicAdapter::capabilities() const {
  Capabilities c;
  c.cancelable        = true;
  c.humanInLoop       = true;
  c.typicalLatencySec = 120;
  return c;
}

FabricErr AnthropicAdapter::dispatch(const Directive& d, char outJobId[72]) {
  return providers::antDispatch(deviceProviderDeps(), ANT_MODEL, d, outJobId);
}

FabricErr AnthropicAdapter::poll(const char* jobId, ResultEnvelope& env) {
  return providers::antPoll(deviceProviderDeps(), jobId, env);
}

FabricErr AnthropicAdapter::answer(const char* jobId, const char* userText) {
  return providers::antAnswer(deviceProviderDeps(), jobId, userText);
}

FabricErr AnthropicAdapter::cancel(const char* jobId) {
  return providers::antCancel(deviceProviderDeps(), jobId);
}

bool orchTurnAnthropic(String& convId, const String& instructions, const String& inputs,
                       String& outJson, String& err, const HeadTools* tools,
                       nimbus::orch::TokenUsage* usage) {
  std::string cv(convId.c_str()), out, e;
  const bool ok = providers::orchTurnAnthropic(deviceProviderDeps(), cv,
                                               std::string(instructions.c_str()),
                                               std::string(inputs.c_str()),
                                               out, e, tools, usage);
  convId  = cv.c_str();
  outJson = out.c_str();
  err     = e.c_str();
  return ok;
}

}  // namespace agent
