#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for lane MONLY: a client with ONLY a Mistral key gets a complete
// product. Each runs the shipping engine, fabric and adapters against the real
// Mistral API, with the run's environment keying exactly one provider:
//
//   monly-stepdown  a cut round on the mistral-large-4-0 head (a reasoning model
//                   that cannot turn reasoning off) steps down same-host onto
//                   mistral-medium-latest before any other provider is asked:
//                   same provider, so no owner notice, and the turn answers.
//
// Pin the model with NIMBUS_LAB_MODEL=mistral/mistral-large-4-0 and run with a
// dotenv that holds only MISTRAL_API_KEY, so the fabric's host list is
// mistral-only and "no other provider asked" is a real assertion, not an
// artifact of the priority order.
namespace lab {
namespace monly {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void cutStepsDownSameHost(LabRig& rig, const Report& r);

}  // namespace monly
}  // namespace lab
