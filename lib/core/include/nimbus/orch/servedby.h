#pragma once
#include <string>

// Served-by disclosure (CUM-236) - the pure rule for "was this turn answered by a
// fallback provider or model, and if so, how do we say it honestly?" Kept portable
// (NO Arduino) so the engine, the daemon, and a host test share ONE class rule: a
// new fallback path that forgets to disclose fails the test, not ships silently.
//
// A provider FAILURE (429/5xx/network) makes the router substitute along the
// same-dialect fallback rules and bill for what actually served; the caller gets no
// explicit notice today beyond the model string inside the payload. This turns that
// implicit swap into an explicit line ("served by <provider> <model>").

namespace nimbus {
namespace orch {

struct ServedBy {
  bool        fallback = false;  // served differs from requested (provider or model)
  std::string text;              // "served by <provider> <model>" ("" when no fallback)
};

// fallback iff the served provider differs from the requested one, OR a model WAS
// requested AND a served model is known AND they differ. A provider merely echoing a
// more specific model id when nothing specific was requested is NOT a fallback (it
// matches the cloud router's own disclosure rule, CUM-236 / TF-C9).
ServedBy servedByDisclosure(const std::string& reqHost, const std::string& reqModel,
                            const std::string& servedHost, const std::string& servedModel);

// The device turn-chip disclosure (CUM-236 device leg). The device writes one
// per-turn `ev:turnend` row; its turn view (_turnChip) reads {model, fallback} to
// annotate a substituted turn "served by <host> <model> (fallback)". This decides
// those two fields from the turn's requested/served identity, sharing the ONE rule
// above so a new fallback path the rule flags discloses on the device too - it does
// not re-implement the comparison. `configuredModel` is the head's configured model
// (the normal-turn label); on a real substitution the served model that ACTUALLY
// answered is shown instead, so the chip never names a model that did not reply.
struct TurnChipDisclosure {
  std::string model;         // model to show in the chip (served model on a fallback)
  bool        fallback = false;
};
TurnChipDisclosure turnChipDisclosure(const std::string& reqHost, const std::string& reqModel,
                                      const std::string& servedHost, const std::string& servedModel,
                                      const std::string& configuredModel);

}  // namespace orch
}  // namespace nimbus
