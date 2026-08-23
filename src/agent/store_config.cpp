#include "store_config.h"

#include "store.h"
#include "../sys/config_nvs.h"   // nimbus::sys::deviceName - prompt identity

// HarnessConfig over agent::store:: (NVS). String<->std::string conversion
// happens exactly here - the harness side is std::string throughout.
//
// SECURITY RAIL (structural, matches nimbus/harness/config.h): this table
// exposes NO setter for provider keys, providerPriority, or orchHost. Those
// writes exist only on human-driven surfaces (web UI / provisioning console).
// The single provider-side write is setConvId (per-host conversation state).

namespace agent {

static std::string s(const String& v) { return std::string(v.c_str()); }

static String keyFor(const std::string& host) {
  if (host == "openai")    return store::openaiKey();
  if (host == "anthropic") return store::anthropicKey();
  if (host == "mistral")   return store::mistralKey();
  if (host == "custom")    return store::customKey();
  return String();
}

HarnessConfig harnessConfigFromStore() {
  HarnessConfig c;
  auto& p = c.provider;
  p.hasKey = [](const std::string& h) {
    if (h == "openai")    return store::hasOpenaiKey();
    if (h == "anthropic") return store::hasAnthropicKey();
    if (h == "mistral")   return store::hasMistralKey();
    if (h == "custom")    return store::hasCustom();
    return false;
  };
  p.key = [](const std::string& h) { return s(keyFor(h)); };
  p.orchHost = [] { return s(store::orchHost()); };
  p.providerPriority = [] { return s(store::providerPriority()); };
  p.subPriority = [] { return s(store::subPriority()); };
  p.orchModel = [](const std::string& h) { return s(store::orchModel(h.c_str())); };
  p.subModel = [](const std::string& h) { return s(store::subModel(h.c_str())); };
  p.modelChoices = [](const std::string& h) { return s(store::modelChoices(h.c_str())); };
  p.fallbackRules = [] { return s(store::fallbackRulesJson()); };
  p.convId = [] { return s(store::orchConvId()); };
  p.setConvId = [](const std::string& v) { store::setOrchConvId(v.c_str()); };
  p.customBase = [] { return s(store::customBase()); };
  p.customKey = [] { return s(store::customKey()); };
  p.customConv = [] { return s(store::customConv()); };
  p.customModel = [] { return s(store::customModel()); };

  c.loop.toolLoopOn = [] { return store::orchToolLoop(); };
  c.loop.midTurnFailover = [] { return store::midTurnFailover(); };
  c.promptV2 = [] { return store::orchPromptV2(); };
  c.loop.rounds = [] { return store::orchLoopRounds(); };
  c.loop.deadlineS = [] { return store::orchLoopDeadlineS(); };
  c.loop.resultCap = [] { return store::orchLoopResultCap(); };
  c.loop.totalCap = [] { return store::orchLoopTotalCap(); };

  c.budget.overBudget = [](const std::string& h) {
    return store::providerOverBudget(h.c_str());
  };
  c.budget.recordTokens = [](const std::string& h, uint32_t in, uint32_t out,
                             uint32_t cacheR, uint32_t cacheW, const std::string& tag) {
    store::recordProviderTokens(h.c_str(), in, out, cacheR, cacheW, tag.c_str());
  };

  c.ttsEnabled = [] { return store::ttsEnabled(); };
  c.deviceName = [] {
    std::string n = s(nimbus::sys::deviceName());
    return n.empty() ? std::string("Nimbus") : n;
  };
  return c;
}

}  // namespace agent
