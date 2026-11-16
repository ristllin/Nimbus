#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for each wire at its own request budget: a long history and a long answer on every
// provider leg, inside the body budget and the round's deadline
// (lane W of the history fix). A STUB from lane P0, registered so lane W fills
// this file without touching scenarios.cpp: it sends nothing and reports itself skipped,
// which the suite counts as not a pass.
namespace lab {
namespace budget {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
  std::function<void(const std::string& why)> skip;
};

void run(LabRig& rig, const Report& r);

}  // namespace budget
}  // namespace lab
