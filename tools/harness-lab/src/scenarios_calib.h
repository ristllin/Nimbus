#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for the request budget's calibration: the live prefill and decode rates and the bytes per
// token each provider leg measures, which the request budget (request_budget.h) is fitted to
// (lane B of the history fix). A STUB from lane P0, registered so lane B fills
// this file without touching scenarios.cpp: it sends nothing and reports itself skipped,
// which the suite counts as not a pass.
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
