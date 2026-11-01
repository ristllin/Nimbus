#pragma once
#include <string>

#include "lab_env.h"

// The Mistral and GLM live legs (real provider calls, a few cents each):
//
//   nimbus-lab mg-live [fold|spawn|web|turn|all]
//     fold   the real engine fold, on each daily-driver model alone
//     spawn  a sub-agent with the built-in web search on mistral-large-4-0
//     web    web.search on each provider's own search
//     turn   one owner turn on a GLM head through the fabric, the device's path
//
// Exit 0 only when every leg passed. Keys are read by name and never printed.
namespace lab {

int cmdMgLive(const Env& env, const std::string& leg);

}  // namespace lab
