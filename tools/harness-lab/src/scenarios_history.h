#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for the history window (lane H of the history fix): a 12-turn food day whose
// every earlier exchange rides each request (or is named in its LEFT OUT note), and the
// 15 Nov complaint replayed with fixture rows (the 15:55 estimate and the 15:57 comparison
// in the 16:00 request). One head per run (--host, --model); see scenarios_history.cpp.
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
