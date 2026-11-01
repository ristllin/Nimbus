#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for "a turn never dies of its size, and never ends in silence" (lane OVF).
// Each runs the shipping engine, fabric and adapters against the real API:
//
//   ovf-overflow     an old history row swells the request past the model's real window,
//                    so the provider really refuses; the trim drops it and the turn answers
//   ovf-round0       a synthesis turn whose first request is over the wire budget
//   ovf-cut          a round cut twice by real output limits, then the fabric's recovery
//   ovf-placeholder  the forced round's answer is swapped for filler on its way back; the
//                    correction pair must be a request the real provider accepts
//   ovf-promise      the same with Lumi's "next turn" sentence
//   ovf-album        six real photos in one turn, needs NIMBUS_LAB_PHOTOS
//
// Pin the model with --model or NIMBUS_LAB_MODEL; run with --record=<dir> so the spend is
// priced from provider-reported usage. Refused requests are not billed.
namespace lab {
namespace ovf {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void overflowRefusal(LabRig& rig, const Report& r);
void synthesisOverBudget(LabRig& rig, const Report& r);
void cutContinues(LabRig& rig, const Report& r);
void forcedPlaceholder(LabRig& rig, const Report& r);
void promiseCorrected(LabRig& rig, const Report& r);
void albumOfSix(LabRig& rig, const Report& r);

}  // namespace ovf
}  // namespace lab
