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

}  // namespace orch
}  // namespace nimbus
