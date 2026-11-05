#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for "a long turn never looks frozen" (lane PROG). Each runs the
// shipping engine, fabric and adapters against the real API:
//
//   prog-long-turn   the real Waitrose case replayed: a multi-round lookup turn
//                    whose body crosses the 85 KB wire budget mid-turn, after
//                    tools ran. Asserts the round-boundary progress seam fired
//                    at every boundary (round 0 included), the portable plan
//                    (turn_progress.h) turns those boundaries into a keep-alive
//                    each and ONE plain progress line, and each budget crossing
//                    folded enough in ONE cycle (the real case needed two
//                    refusals per crossing). Notes the timeline before (the
//                    recorded 5 min 12 s silence) and after (the measured
//                    boundaries and the line the owner would see).
//
// Pin the model with --model or NIMBUS_LAB_MODEL. The budget refusals are
// local (nothing leaves the lab, nothing is billed); the refitted replays are
// real provider calls.
namespace lab {
namespace prog {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void longTurnReplay(LabRig& rig, const Report& r);

}  // namespace prog
}  // namespace lab
