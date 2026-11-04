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

// The spawn leg's verdict, extracted so the offline selftest can pin the
// class (independent-review finding on the H18 leg): "the sub keeps its
// tools" is proven only by the provider's own run record - the tool.execution
// entries mistralPoll copies into ResultEnvelope.webRuns - never by the reply
// text, which a from-memory answer can fill with a URL or the word
// "web_search" with no tool run at all. `reply` stays a parameter so the
// selftest can pin that no reply text rescues a run with webRuns == 0.
bool spawnKeepsTools(bool dispatchOk, const std::string& reply, unsigned webRuns);

}  // namespace lab
