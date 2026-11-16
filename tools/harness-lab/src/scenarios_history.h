#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for the history window: every live row after the fold's anchor is in the request,
// covered by the summary, or named in its LEFT OUT note, across a long conversation
// (lane H of the history fix). A STUB from lane P0, registered so lane H fills
// this file without touching scenarios.cpp: it sends nothing and reports itself skipped,
// which the suite counts as not a pass.
namespace lab {
namespace histfix {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
  std::function<void(const std::string& why)> skip;
};

void run(LabRig& rig, const Report& r);

}  // namespace histfix
}  // namespace lab
