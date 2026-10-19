#pragma once
#include <functional>
#include <string>

#include "lab_env.h"
#include "lab_rig.h"

// The nightly dream, live: the SHIPPING night pipeline (agent::dream::runNight)
// driven against real providers and real embeddings, with the lab's RAM stores in
// place of the device's. Each leg asserts an outcome, never just "it returned".
//   dream-distill  L-1: a synthetic day becomes durable memories, a changed fact is
//                  updated rather than duplicated, nothing is delivered;
//   dream-quiet    L-2: a quiet night skips the paid turn (zero provider calls);
//   dream-badkey   L-3: a refused key fails the night with cause "key", nothing
//                  delivered, no spend, no retry ($0: the requests are faulted);
//   dream-noembed  L-4: with embeddings down the night is degraded and the turn
//                  is told not to try memory calls.
// L-5 is dream-distill on each provider (--host / NIMBUS_LAB_MODEL).
namespace lab {

using Check = std::function<void(bool ok, const std::string& what)>;
using Note = std::function<void(const std::string& what)>;

void dreamDistill(LabRig& rig, const Check& check, const Note& note);
void dreamQuiet(LabRig& rig, const Check& check, const Note& note);
// These two build their own rig from `base`'s environment and options.
void dreamBadKey(LabRig& base, const Check& check, const Note& note);
void dreamNoEmbed(LabRig& base, const Check& check, const Note& note);

}  // namespace lab
