#pragma once
// recorded_models - every model the recorded provider lists name, and their stored-list
// windows, for the request budget's class tests (test_model_window, test_request_budget,
// test_output_caps). Lane B of the history fix (16 Nov 2026).
//
// The models are the ones test/support/fixtures/budgets_v5_1_1.json was written over
// (models_2026-10 for anthropic, openai, mistral, zai and models_2026-11 for mistral,
// Mistral's aliases included), in the same stable order. The listed lookup reads the
// stored lists the device would hold after a model-list refresh: the 2026-11 Mistral list
// (falling back to 2026-10 for an id only it names) and the 2026-10 Anthropic list.
#include <unity.h>

#include <ArduinoJson.h>

#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/model_catalog.h"

namespace recorded {

inline std::string readFixture(const std::string& path) {
  std::ifstream f(path);
  TEST_ASSERT_TRUE_MESSAGE(f.good(), path.c_str());
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Model {
  std::string provider;
  std::string id;
};

inline void addIds(std::vector<Model>& out, std::map<std::string, bool>& seen, const char* provider,
                   const std::string& path) {
  JsonDocument doc;
  TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, readFixture(path)), path.c_str());
  for (JsonObjectConst m : doc["data"].as<JsonArrayConst>()) {
    std::vector<std::string> ids{m["id"] | ""};
    for (JsonVariantConst a : m["aliases"].as<JsonArrayConst>()) ids.push_back(a | "");
    for (const std::string& id : ids) {
      const std::string key = std::string(provider) + "\t" + id;
      if (id.empty() || seen[key]) continue;
      seen[key] = true;
      out.push_back({provider, id});
    }
  }
}

inline const std::vector<Model>& models() {
  static const std::vector<Model> all = [] {
    std::vector<Model> out;
    std::map<std::string, bool> seen;
    const std::string f = "test/support/fixtures/";
    for (const char* p : {"anthropic", "openai", "mistral", "zai"}) addIds(out, seen, p, f + "models_2026-10/" + p + ".json");
    addIds(out, seen, "mistral", f + "models_2026-11/mistral.json");
    return out;
  }();
  return all;
}

// A provider's stored list as the device writes it after a refresh.
inline std::string storeTextOf(const char* provider, const std::string& fixture) {
  nimbus::orch::ModelHarvest h;
  h.provider = provider;
  h.now = 1794000000u;   // 2026-11-07, before any recorded retire date that matters here
  const std::string body = readFixture(fixture);
  nimbus::orch::harvestBody(h, body.data(), body.size());
  nimbus::orch::finishHarvest(h);
  return nimbus::orch::serializeModelStore(nimbus::orch::ModelStoreMeta(), h.rows);
}

struct Stores {
  std::string mistralNov, mistralOct, anthropic;
};
inline const Stores& stores() {
  static const Stores s = [] {
    const std::string f = "test/support/fixtures/";
    Stores x;
    x.mistralNov = storeTextOf("mistral", f + "models_2026-11/mistral.json");
    x.mistralOct = storeTextOf("mistral", f + "models_2026-10/mistral.json");
    x.anthropic = storeTextOf("anthropic", f + "models_2026-10/anthropic.json");
    return x;
  }();
  return s;
}

// What each recorded list says the window is (0 = it does not say): Mistral's November
// list first, then October's; Anthropic's October list. OpenAI and Z.ai lists never say.
inline uint32_t listedIn(const std::string& host, const std::string& model, bool october) {
  const Stores& s = stores();
  if (host == "mistral") return nimbus::orch::listedCtxTokens(host, october ? s.mistralOct : s.mistralNov, model);
  if (host == "anthropic") return nimbus::orch::listedCtxTokens(host, s.anthropic, model);
  return 0;
}

// The device's listed lookup over the recorded lists (installListedCtxLookup).
inline uint32_t listedLookup(const char* host, const char* model) {
  const uint32_t nov = listedIn(host, model, false);
  return nov ? nov : listedIn(host, model, true);
}

}  // namespace recorded
