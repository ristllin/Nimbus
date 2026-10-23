#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Lane J live legs (run with real providers; every check reads the recorded
// trace and the store, deterministic grading):
//   prefs-e2e     a standing rule stated once in chat is saved (prefs.edit or a
//                 standing mem_write), rides [STANDING RULES] on the next plain
//                 turn, on a routine fire, and on the NEXT DAY; a reply then
//                 carries no em dash even when the model is asked for a long
//                 formatted answer
//   prefs-nextday the 19 Oct drink case: a standing default stated on day N
//                 ("my usual is a flat white unless I say otherwise") is
//                 applied on day N+1 without a reminder
namespace lab {
namespace prefslegs {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void e2e(LabRig& rig, const Report& r);
void nextDay(LabRig& rig, const Report& r);

}  // namespace prefslegs
}  // namespace lab
