#include "nimbus/orch/servedby.h"

#include "nimbus/orch/model_catalog.h"

namespace nimbus {
namespace orch {

ServedBy servedByDisclosure(const std::string& reqHost, const std::string& reqModel,
                            const std::string& servedHost, const std::string& servedModel) {
  ServedBy s;
  const bool hostDiff = !servedHost.empty() && servedHost != reqHost;
  // A same-provider model difference is a real substitution ONLY across model
  // FAMILIES. A requested model is often an alias (gpt-5.5, mistral-large-latest)
  // and the provider echoes the resolved dated snapshot (gpt-5.5-2026-01,
  // mistral-large-2411) - the SAME model, not a swap. Comparing raw ids flagged
  // "fallback" on every ordinary turn; compare the coarse family instead, and only
  // when both families are known (never flag on uncertainty).
  bool modelDiff = false;
  if (!hostDiff && !reqModel.empty() && !servedModel.empty() && servedModel != reqModel) {
    const std::string rf = modelFamily(reqHost, reqModel);
    const std::string sf = modelFamily(reqHost, servedModel);
    modelDiff = !rf.empty() && !sf.empty() && rf != sf;
  }
  s.fallback = hostDiff || modelDiff;
  if (s.fallback) {
    const std::string p = servedHost.empty() ? reqHost : servedHost;
    s.text = "served by " + p + (servedModel.empty() ? std::string() : (" " + servedModel));
  }
  return s;
}

TurnChipDisclosure turnChipDisclosure(const std::string& reqHost, const std::string& reqModel,
                                      const std::string& servedHost, const std::string& servedModel,
                                      const std::string& configuredModel) {
  TurnChipDisclosure d;
  d.fallback = servedByDisclosure(reqHost, reqModel, servedHost, servedModel).fallback;
  // Show what actually answered on a substitution; otherwise the head's own label.
  d.model = (d.fallback && !servedModel.empty()) ? servedModel : configuredModel;
  return d;
}

}  // namespace orch
}  // namespace nimbus
