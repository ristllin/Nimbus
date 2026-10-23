#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Lane I live legs (skills used and editable). Every check reads only the
// recorded turn trace and the store state - deterministic grading, no judge.
//
//   skills-e2e  a read skill attaches across turns ([ACTIVE SKILLS]), edits
//               stage as SKILL.next.md with the live file untouched, the
//               owner's skill survives a delete request, and authoring passes
//               the save lint (S1-S5 of the skills study)
//   meal-eval   the L19-50 ledger discipline: an owner-stated figure and a
//               correction land as files.append rows on the one food log,
//               never a claimed update without the effect
namespace lab {
namespace skillslegs {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void e2e(LabRig& rig, const Report& r);
void mealEval(LabRig& rig, const Report& r);

}  // namespace skillslegs
}  // namespace lab
