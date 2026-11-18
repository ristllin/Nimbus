#pragma once
#include <string>

// model_family - which concrete model a provider id means, where the device's behavior
// depends on it: Mistral Large 4 (its request budget row, its rates, its reasoning effort)
// and the models that are forced to reason (they refuse reasoning effort "none", so a cut
// round steps down and the fold never runs on one). One alias table, read by the request
// budget (orch_request_budget.cpp), the output caps (lib/harness output_caps.cpp) and the
// fold ladder (lib/harness engine_fold.cpp). Portable, host-tested (test_request_budget's
// class test over the recorded model lists, test_output_caps).
//
// Mistral moves its -latest aliases: on the 2026-10 list mistral-large-latest was an alias
// of mistral-large-2512; on 14 Nov 2026 GET /v1/models lists it (with mistral-large-4-0) as
// an alias of mistral-large-4. The table follows the newest recorded list
// (test/support/fixtures/models_2026-11), and the class test fails when a recorded Large 4
// alias is missing from it.
namespace nimbus {
namespace orch {

// Is `model` Mistral Large 4 under any id Mistral serves it as: the mistral-large-4 family
// (mistral-large-4, mistral-large-4-0, a later mistral-large-4-N) and its listed aliases
// (mistral-large-latest). A router selector's "mistral/" prefix is read through; case is
// ignored.
bool isMistralLarge4(const std::string& model);

// Is `model` forced to reason on Mistral's wire: it refuses reasoning effort "none" and
// takes "low" (recorded 1 Nov 2026): Large 4 under every id isMistralLarge4 accepts, and
// the GLM models Mistral hosts (glm-*, zai-glm-*). A "mistral/" prefix is read through.
bool mistralForcedReasoner(const std::string& model);

}  // namespace orch
}  // namespace nimbus
