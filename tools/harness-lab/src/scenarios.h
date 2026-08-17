#pragma once
#include <string>
#include <vector>

#include "lab_env.h"
#include "lab_rig.h"

// The scenario suite: real, multi-step agent work against real providers, with
// pass/fail assertions on what actually came back. These are not smoke tests -
// "the API returned 200" is exactly the bar that let a permanently broken
// web.search ship, so each scenario checks an OUTCOME.
namespace lab {

void listScenarios();
int  runScenarios(const Env& env, const LabRig::Options& opt,
                  const std::vector<std::string>& names);
int  cmdProviders(const Env& env, const LabRig::Options& opt);
int  cmdSearch(const Env& env, const LabRig::Options& opt, const std::string& query);

}  // namespace lab
