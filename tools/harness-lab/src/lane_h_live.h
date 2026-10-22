#pragma once
#include <string>

#include "lab_env.h"
#include "lane_l_live.h"   // LaneLOptions: --head-host --head-model --effort --reps --out

// The models-on-demand live suite (owner ruling R5, real provider calls, cheapest
// head first):
//
//   nimbus-lab models-live [lists|switch|router-id|all]
//     lists      GET each keyed provider's model list (free) and parse it with the
//                device's own parser; these lists back the legs below.
//     switch     one owner turn "Switch to Sonnet 5.5." on the head (claude-haiku-5-5
//                by default): pass = an orch_model to anthropic/claude-sonnet-5-5
//                applied, with the id taken from the live Anthropic list.
//     router-id  one owner turn that asks for a router id ("<upstream>/<model>") on a
//                direct provider through device.control: pass = the device refused
//                it with "not in <provider> list" and the head then switched to an
//                id that provider lists (L19-39). --head-host=mistral|zai runs the
//                rep on mistral-small-latest or glm-5.3-flash.
//
// The device's TurnEngine, apply rails, models.list tool and refresh queue run as
// shipped; the model lists come from the providers live. Keys are read by name and
// never printed; provider-reported usage is summed and priced per model, and the
// totals print at the end (and go to --out=<file> as JSON).
namespace lab {

int cmdModelsLive(const Env& env, LaneLOptions opt, const std::string& leg);

}  // namespace lab
