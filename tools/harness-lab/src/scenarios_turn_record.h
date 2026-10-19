#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for a turn's record: what it cost per provider, and why it failed.
//
//   metering-switch        a turn that switches provider mid-turn files each
//                          provider's spend under that provider and model, once
//   metering-fail          a failed turn still files what its billed rounds cost
//   diagnose-from-evidence the head reports the recorded first cause of a failed
//                          dream, and invents none
//   provider-error-reason  a request body with invalid UTF-8 is rejected before
//                          inference, with the provider's reason, and unbilled
//
// The metering legs print one "lab-ledger: {...}" line per metered row; with
// --record=<dir>, metering_check.py compares those rows with <dir>/usage.jsonl
// (the provider-reported usage). Faults make the switch happen; see
// tools/harness-lab/README.md for the exact commands.
namespace lab {
namespace turnrec {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void meteringSwitch(LabRig& rig, const Report& r);
void meteringFail(LabRig& rig, const Report& r);
void diagnoseFromEvidence(LabRig& rig, const Report& r);
void providerErrorReason(LabRig& rig, const Report& r);

}  // namespace turnrec
}  // namespace lab
