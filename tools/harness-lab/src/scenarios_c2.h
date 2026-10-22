#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live leg for lane C2 (L19-35): finished work is not run again after a restart.
//
//   finished-jobs   The owner asked for two research jobs; both finished and were
//                   saved before the device restarted, but the head's own working
//                   note still says it is waiting for them, and after the restart
//                   nothing is running. An unrelated owner message follows.
//                   Leg A (the journal's finished ring, as on the device): the turn
//                   context lists both finished jobs and the head starts nothing.
//                   Leg B (the counter-leg, the ring removed): the same turn with no
//                   record of them, the 19 Oct state; any re-spawn is counted.
//                   Leg A must hold on every rep; leg B is reported (it shows the
//                   class the ring closes, a model may or may not trip it).
//
// Run on claude-haiku-5-5; see tools/harness-lab/README.md.
namespace lab {
namespace c2 {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void finishedJobs(LabRig& rig, const Report& r);

}  // namespace c2
}  // namespace lab
