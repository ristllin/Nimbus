#include "nimbus/orch/request_budget.h"

#include "nimbus/orch/budget.h"         // kBudgetBytesPerToken - today's bytes per token
#include "nimbus/orch/model_window.h"   // modelCtxTokens - the window table

namespace nimbus {
namespace orch {

namespace {
// The head's round deadline today (providers.h: kMistralRoundMs, kZaiHeadTimeoutMs,
// kHeadRoundCeilingMs; test_engine_seams pins them equal).
uint32_t legacyRoundMs(const std::string& host) {
  if (host == "mistral") return 65000;
  if (host == "zai") return 50000;
  return 120000;
}
}  // namespace

RequestBudget requestBudgetFor(const std::string& host, const std::string& model, bool routed) {
  RequestBudget b;
  b.windowTokens = modelCtxTokens(host, model);
  b.rNomX100 = (uint16_t)(kBudgetBytesPerToken * 100);
  b.rSafeX100 = b.rNomX100;
  b.wireBodyBytes = kLegacyWireBodyBytes;
  b.targetTokens = b.wireBodyBytes / kBudgetBytesPerToken;
  b.roundMs = legacyRoundMs(host);
  b.routed = routed;
  b.flat = host == "custom";
  return b;
}

}  // namespace orch
}  // namespace nimbus
