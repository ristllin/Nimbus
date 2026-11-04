#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Lane DISPO rate legs: how often a real head makes each choice, over repeated
// real runs (bench rows F03, H09, H20 on the v5.0.0 candidate 28813029 each
// failed once on a model choice; the disposition needs the rate, not one
// sample). NIMBUS_DISPO_REPS sets the rep count (default 5). Each rep runs on
// a fresh chat with freshly seeded files, exactly the bench shape:
//
//   dispo-f03  the F03 shape: a staged plan (food/plan.md, "Daily target:
//              2,400 kcal and 155 g protein"), then "how much protein is left
//              today?" and "and calories?". Counts, per rep: which targets
//              landed in food/food-targets.csv with a source, and whether
//              either reply carries a "(Correction:" line (the claim check's
//              honest bounce when a cited target did not land).
//   dispo-h09  the H09 turn-1 shape: a seeded qw3-bench/food-log.csv (Saturday
//              breakfast + lunch), then the dinner turn naming that file.
//              Counts, per rep: a keyed append to the requested file vs a
//              redirect to the one food log (food/food-log.csv).
//   dispo-h20  the H20 shape: "What is the latest Python 3 release?" on the
//              pinned head. Counts, per rep: whether web.search ran (the
//              model's choice; the tool is offered) and whether the reply
//              names a source.
namespace lab {
namespace dispolegs {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void f03Targets(LabRig& rig, const Report& r);
void h09Dinner(LabRig& rig, const Report& r);
void h20Search(LabRig& rig, const Report& r);

}  // namespace dispolegs
}  // namespace lab
