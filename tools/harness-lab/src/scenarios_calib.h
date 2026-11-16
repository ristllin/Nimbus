#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for the request budget's calibration (lane B of the history fix): per PL provider
// leg, one cold request at the head's own request budget (request_budget.h) asking for a long
// final under the load-aware cap, checked for prompt tokens within -15%/+5% of the target, bytes
// per token at or above the dense ratio, and a round inside the host's read window; plus Mistral
// Medium's repeat and extension cache leg. Fixture content only; see scenarios_calib.cpp for the
// environment switches (NIMBUS_CALIB_LEGS, _DRY, _MISTRAL_PROXY, _OUT).
namespace lab {
namespace calib {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
  std::function<void(const std::string& why)> skip;
};

void run(LabRig& rig, const Report& r);

}  // namespace calib
}  // namespace lab
