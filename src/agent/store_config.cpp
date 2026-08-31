#include "store_config.h"

#include "store.h"
#include "../sys/config_nvs.h"   // nimbus::sys::deviceName - prompt identity
#include "nimbus/orch/provider_slots.h"   // CUM-246: canonical registry + anySlotWhere fanout

// HarnessConfig over agent::store:: (NVS). String<->std::string conversion
// happens exactly here - the harness side is std::string throughout.
//
// SECURITY RAIL (structural, matches nimbus/harness/config.h): this table
// exposes NO setter for provider keys, providerPriority, or orchHost. Those
// writes exist only on human-driven surfaces (web UI / provisioning console).
// The single provider-side write is setConvId (per-host conversation state).

namespace agent {

static std::string s(const String& v) { return std::string(v.c_str()); }

// The single slug->stored-key router. Every "is this provider keyed?" answer below
// derives from this one switch (CUM-246), so the keyed predicates can never drift
// from the key that actually runs a turn. The per-provider store accessors live in
// store.cpp (this lane does not own it); a generic store::key(slug) would let even
// this switch fall away - see the DECISION on CUM-246.
static String keyFor(const std::string& host) {
  if (host == "openai")    return store::openaiKey();
  if (host == "anthropic") return store::anthropicKey();
  if (host == "mistral")   return store::mistralKey();
  if (host == "custom")    return store::customKey();
  if (host == "cumulo")    return store::cumuloKey();
  if (host == "zai")       return store::zaiKey();
  return String();
}

HarnessConfig harnessConfigFromStore() {
  HarnessConfig c;
  auto& p = c.provider;
  // Keyed-ness derives from the ONE slug->store router (keyFor), so it can never drift
  // from key(): for a registry slot, keyed == a stored key (each has*Key() is exactly
  // its keyField().length() > 0). CUM-242: the router heads (cumulo/zai) are first-class
  // now, so head resolution + failover see their key - the registry carries them, so no
  // per-head line here. "custom" is the free-form endpoint (not a registry slot) and its
  // presence is the base URL, not the key, so it keeps its own accessor.
  p.hasKey = [](const std::string& h) {
    if (h == "custom") return store::hasCustom();
    return keyFor(h).length() > 0;
  };
  p.key = [](const std::string& h) { return s(keyFor(h)); };
  // CUM-242 / CUM-201: with no BYOK head keyed, the verified router key is the
  // fallback SOURCE that runs the whole assistant. Cumulo first (the flagship
  // "one key, one balance" path), then Z.ai. Consulted by the engine's head
  // resolution only after every BYOK head misses.
  p.routerFallbackHost = [] {
    if (store::hasCumuloKey()) return std::string("cumulo");
    if (store::hasZaiKey())    return std::string("zai");
    return std::string();
  };
  // CUM-211 / CUM-246: device-truth "is any provider configured", enumerated over the
  // canonical registry (anySlotWhere) so a new slot counts automatically - the b2f4930
  // miss (cumulo/zai absent from a hand-listed OR) cannot recur. Drives only the honest
  // "no provider set up" reply, so a Cumulo-only device is never wrongly told it has no
  // provider. Plus the free-form custom endpoint, which is not a registry slot.
  p.anyKeyed = [] {
    return nimbus::orch::anySlotWhere(
               [](const char* slug) { return keyFor(slug).length() > 0; }) ||
           store::hasCustom();
  };
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
