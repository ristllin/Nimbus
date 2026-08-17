#pragma once
#include <map>
#include <set>
#include <string>

#include "nimbus/harness/config.h"

// FakeConfig - an in-memory HarnessConfig. Defaults mirror a healthy provisioned
// device: all three providers keyed, anthropic-first priority (the F20
// workaround the firmware ships), tool loop on with the store.h default caps.
namespace harness_test {

struct FakeConfig {
  std::set<std::string> keyed{"openai", "anthropic", "mistral"};
  std::map<std::string, std::string> keys{
      {"openai", "sk-fake-oai"}, {"anthropic", "sk-fake-ant"}, {"mistral", "sk-fake-mis"}};
  std::string orchHost;                                  // "" = top of priority
  std::string priority = "anthropic,openai,mistral";
  std::string subPriority = "anthropic,openai,mistral";
  std::map<std::string, std::string> orchModels, subModels, choices;
  std::string convId;                                    // "host|convId"
  std::string customBase, customKey, customConv = "openai", customModel;

  bool toolLoop = true;
  // cfg.loop.midTurnFailover. Default FALSE in the rig (device default is true):
  // every pre-fabric turn test was written against the legacy scripted-provider
  // path, and a rig that also wires hosts.fabric would silently reroute them.
  // Fabric-gate tests opt in with cfg.midFail = true.
  bool midFail = false;
  int rounds = 12, deadlineS = 600, resultCap = 4096, totalCap = 24576;

  std::set<std::string> overBudgetHosts;
  struct Recorded { std::string host; uint32_t in, out; std::string tag; };
  std::vector<Recorded> recorded;

  bool tts = false;
  std::string devName = "Nimbus";

  agent::HarnessConfig contract() {
    agent::HarnessConfig c;
    auto& p = c.provider;
    p.hasKey = [this](const std::string& h) { return keyed.count(h) > 0; };
    p.key = [this](const std::string& h) {
      auto it = keys.find(h);
      return it == keys.end() ? std::string() : it->second;
    };
    p.orchHost = [this] { return orchHost; };
    p.providerPriority = [this] { return priority; };
    p.subPriority = [this] { return subPriority; };
    p.orchModel = [this](const std::string& h) {
      auto it = orchModels.find(h);
      return it == orchModels.end() ? std::string("model-" + h) : it->second;
    };
    p.subModel = [this](const std::string& h) {
      auto it = subModels.find(h);
      return it == subModels.end() ? std::string("sub-" + h) : it->second;
    };
    p.modelChoices = [this](const std::string& h) {
      auto it = choices.find(h);
      return it == choices.end() ? std::string() : it->second;
    };
    p.convId = [this] { return convId; };
    p.setConvId = [this](const std::string& v) { convId = v; };
    p.customBase = [this] { return customBase; };
    p.customKey = [this] { return customKey; };
    p.customConv = [this] { return customConv; };
    p.customModel = [this] { return customModel; };

    c.loop.toolLoopOn = [this] { return toolLoop; };
    c.loop.midTurnFailover = [this] { return midFail; };
    c.loop.rounds = [this] { return rounds; };
    c.loop.deadlineS = [this] { return deadlineS; };
    c.loop.resultCap = [this] { return resultCap; };
    c.loop.totalCap = [this] { return totalCap; };

    c.budget.overBudget = [this](const std::string& h) {
      return overBudgetHosts.count(h) > 0;
    };
    c.budget.recordTokens = [this](const std::string& h, uint32_t in, uint32_t out,
                                   uint32_t, uint32_t, const std::string& tag) {
      recorded.push_back({h, in, out, tag});
    };

    c.ttsEnabled = [this] { return tts; };
    c.deviceName = [this] { return devName; };
    return c;
  }
};

}  // namespace harness_test
