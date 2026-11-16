#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for the fold ladder: a fold cycle walks its rungs one pump pass at a time, a thin
// answer goes last next cycle, and Summarize now answers the owner
// (lane F of the history fix). A STUB from lane P0, registered so lane F fills
// this file without touching scenarios.cpp: it sends nothing and reports itself skipped,
// which the suite counts as not a pass.
namespace lab {
namespace foldladder {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
  std::function<void(const std::string& why)> skip;
};

void run(LabRig& rig, const Report& r);

}  // namespace foldladder
}  // namespace lab
