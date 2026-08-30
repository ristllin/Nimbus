#pragma once
#include <map>
#include <string>
#include <vector>

#include "fake_http.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/head_loop.h"

// FakeProviderDeps - the wire-suite rig: a providers::ProviderDeps over
// FakeHttpTransport + in-memory config. Defaults mirror a healthy keyed device
// with the tool loop ON (the firmware default); tests flip fields per case.
namespace harness_test {

struct FakeProviderDeps {
  FakeHttpTransport http;

  std::map<std::string, std::string> keys{{"openai", "sk-fake-oai"},
                                          {"anthropic", "sk-fake-ant"},
                                          {"mistral", "sk-fake-mis"}};
  bool toolLoop = true;

  std::string antEnv;                    // cached environment id ("" = none)
  std::string antAgents;                 // "model=agentid;..."
  int antEnvSets = 0, antAgentSets = 0;  // write counters (stale-cache clears)

  std::string custBase, custKey, custModel = "mock-model";
  std::string custConv = "openai";
  std::string custPathPrefix;   // "" = default /v1; router heads set their API base

  int openAiAttaches = 0, mistralAttaches = 0, anthropicAttaches = 0;
  uint32_t now = 1000;
  uint32_t heap = 100000;

  agent::providers::ProviderDeps contract() {
    agent::providers::ProviderDeps pd;
    pd.http = &http;
    pd.key = [this](const char* h) {
      auto it = keys.find(h);
      return it == keys.end() ? std::string() : it->second;
    };
    pd.orchModel = [](const char* h) { return "model-" + std::string(h); };
    pd.toolLoopOn = [this] { return toolLoop; };
    pd.antEnvId = [this] { return antEnv; };
    pd.setAntEnvId = [this](const std::string& v) { antEnv = v; antEnvSets++; };
    pd.antAgentMap = [this] { return antAgents; };
    pd.setAntAgentMap = [this](const std::string& v) { antAgents = v; antAgentSets++; };
    pd.customBase = [this] { return custBase; };
    pd.customKey = [this] { return custKey; };
    pd.customConv = [this] { return custConv; };
    pd.customModel = [this] { return custModel; };
    pd.customPathPrefix = [this] { return custPathPrefix; };
    pd.attachOpenAI = [this](JsonDocument& d) {
      openAiAttaches++;
      JsonObject t = d["tools"].add<JsonObject>();
      t["type"] = "mcp";
      t["server_label"] = "fake-connector";
    };
    pd.attachMistral = [this](JsonDocument& d) {
      mistralAttaches++;
      d["tools"].add<JsonObject>()["type"] = "web_search";
    };
    pd.attachAnthropic = [this](JsonDocument& d) {
      anthropicAttaches++;
      JsonObject s = d["mcp_servers"].add<JsonObject>();
      s["type"] = "url";
      s["name"] = "fake-mcp";
      s["url"] = "https://mcp.example.com";
    };
    pd.nowMs = [this] { now += 10; return now; };
    pd.freeHeap = [this] { return heap; };
    return pd;
  }

  // A HeadTools with one registry tool + a scripted dispatch recorder, using the
  // firmware-default loop caps.
  struct ToolRig {
    agent::HeadTools ht;
    std::vector<nimbus::orch::HeadToolCall> dispatched;
    std::string result = "tool-ok";
    bool resultIsError = false;
  };
  // NOTE: the returned rig holds self-referencing lambdas; keep it alive and
  // in place for the duration of the call.
  void fillTools(ToolRig& rig) {
    rig.ht.specs.push_back(nimbus::orch::ToolRegistry::Spec{
        "memory_search", "search the memory",
        "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}"});
    rig.ht.dispatch = [&rig](const nimbus::orch::HeadToolCall& c) {
      rig.dispatched.push_back(c);
      nimbus::orch::HeadToolResult r;
      r.id = c.id;
      r.name = c.name;
      r.output = rig.result;
      r.isError = rig.resultIsError;
      return r;
    };
    rig.ht.cfg.maxRounds = 12;
    rig.ht.cfg.deadlineMs = 600000;
    rig.ht.cfg.roundMinHeap = 28000;
    rig.ht.cfg.maxToolResultBytes = 4096;
    rig.ht.cfg.maxTotalToolBytes = 24576;
  }
};

// Body of request #i (0-based), "" when absent.
inline const std::string& reqBody(const FakeProviderDeps& d, size_t i) {
  static const std::string empty;
  return i < d.http.seen.size() ? d.http.seen[i].body : empty;
}

inline bool bodyHas(const FakeProviderDeps& d, size_t i, const char* needle) {
  return reqBody(d, i).find(needle) != std::string::npos;
}

inline std::string headerOf(const agent::HttpRequest& r, const std::string& name) {
  for (const auto& h : r.headers)
    if (h.first == name) return h.second;
  return "";
}

}  // namespace harness_test
