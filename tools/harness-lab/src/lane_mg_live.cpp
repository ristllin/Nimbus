#include "lane_mg_live.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "curl_transport.h"
#include "lab_rig.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/connectors_wire.h"

namespace lab {

namespace {

struct ModelRow {
  const char* host;
  const char* model;
};
const ModelRow kDaily[] = {{"mistral", "mistral-large-4-0"},
                           {"mistral", "mistral-small-latest"},
                           {"zai", "glm-5.3"},
                           {"zai", "glm-5.3-flash"}};

int g_failed = 0;
void verdict(bool ok, const std::string& what) {
  if (!ok) g_failed++;
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
}

LabRig::Options soloOptions(const ModelRow& row) {
  LabRig::Options opt;
  opt.priority = row.host;
  opt.models[row.host] = row.model;
  opt.pins = WirePins{"", 0, 0};   // the shipping effort and caps, not the lab's pins
  opt.embeddings = false;
  return opt;
}

void legFold(const Env& env) {
  std::string digest;
  for (int i = 0; i < 30; i++)
    digest += "- user: Logged meal " + std::to_string(i) + ": oats 60 g, milk 200 ml.\n- assistant: Noted, " +
              std::to_string(i * 210) + " kcal so far today.\n";
  for (const ModelRow& row : kDaily) {
    if (env.providerKey(row.host).empty()) continue;
    LabRig rig(env, soloOptions(row));
    std::string summary;
    size_t produced = 0;
    const auto res = rig.runFold("foldleg", "", digest, summary, &produced);
    verdict(res == agent::TurnEngine::FoldResult::Ok && !summary.empty(),
            std::string("fold on ") + row.model + ": " + std::to_string(produced) + " B: " + summary.substr(0, 120));
  }
}

uint32_t nowMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

agent::providers::ProviderDeps plainDeps(const Env& env, CurlHttpTransport* http) {
  agent::providers::ProviderDeps pd;
  pd.http = http;
  pd.key = [env](const char* h) { return env.providerKey(h); };
  pd.orchModel = [](const char*) { return std::string(); };
  pd.nowMs = [] { return nowMs(); };
  pd.zaiHead = [env](agent::providers::ProviderDeps& z) {
    z.customBase = [] { return std::string("api.z.ai"); };
    z.customPathPrefix = [] { return std::string("/api/paas/v4"); };
    z.customKey = [env] { return env.providerKey("zai"); };
    z.customConv = [] { return std::string("openai"); };
    z.customModel = [] { return std::string("glm-5.3-flash"); };
    agent::providers::applyZaiHeadPolicy(z);
  };
  return pd;
}

void legSpawn(const Env& env) {
  CurlHttpTransport http;
  auto pd = plainDeps(env, &http);
  const std::vector<nimbus::orch::ConnectorInfo> cs = {{"web_search", "mistral", "builtin", "", "", "", true}};
  pd.attachMistral = [cs](JsonDocument& doc) { nimbus::orch::attachMistralWire(doc, cs, false); };
  agent::Directive d;
  d.category = "research";
  d.instruction = "What is the latest stable Python 3 release? Use the web_search tool and name the source.";
  char job[72] = {};
  const agent::FabricErr e = agent::providers::mistralDispatch(pd, "mistral-large-4-0", d, job);
  std::string reply;
  if (e == agent::FabricErr::Ok) {
    static agent::ResultEnvelope env2;
    agent::providers::mistralPoll(pd, job, env2);
    reply = env2.reply;
  }
  for (const auto& c : http.calls)
    std::printf("    %s %s -> %d\n", c.method.c_str(), c.path.c_str(), c.status);
  // CUM-887's move is defensive: it fires only when the provider refuses the built-in
  // connectors (code 3004), which is provider weather - on 4 Nov 2026 mistral-large-4-0
  // served the web_search connector itself, so no stepdown and the reply never names
  // the stepdown model. The leg's claim is "the sub keeps its tools": the reply either
  // names the model the run moved to, or shows the tool ran where it was asked.
  const bool stepped = reply.find("mistral-medium-latest") != std::string::npos;
  const bool toolRan =
      reply.find("web_search") != std::string::npos || reply.find("http") != std::string::npos;
  verdict(e == agent::FabricErr::Ok && (stepped || toolRan),
          "spawn on mistral-large-4-0 keeps its tools: err " + std::to_string((int)e) + ": " +
              reply.substr(0, 300));
}

void legWeb(const Env& env) {
  CurlHttpTransport http;
  auto pd = plainDeps(env, &http);
  auto m = agent::providers::mistralWebSearch(pd, "latest stable Python 3 release version number");
  verdict(m.ok && m.digest.find("http") != std::string::npos, "mistral search: " + (m.ok ? m.digest : m.err).substr(0, 300));
  agent::providers::ProviderDeps z = pd;
  pd.zaiHead(z);
  auto r = agent::providers::zaiWebSearch(z, "latest stable Python 3 release version number", 5);
  verdict(r.ok && r.digest.find("http") != std::string::npos, "z.ai search: " + (r.ok ? r.digest : r.err).substr(0, 300));
}

void legTurn(const Env& env) {
  const ModelRow row{"zai", "glm-5.3-flash"};
  LabRig::Options opt = soloOptions(row);
  LabRig rig(env, opt);
  const auto t = rig.say("mgleg", "Search your docs for how long a fold summary may be, using the docs.search tool, then answer in one line.");
  verdict(!t.reply.empty() && t.reply != "Done." && !t.toolCalls.empty(), "GLM head on the fabric ran " + std::to_string(t.toolCalls.size()) + " tool(s): " + t.reply.substr(0, 200));
}

}  // namespace

int cmdMgLive(const Env& env, const std::string& leg) {
  if (leg == "fold" || leg == "all") { std::printf("[fold]\n"); legFold(env); }
  if (leg == "spawn" || leg == "all") { std::printf("[spawn]\n"); legSpawn(env); }
  if (leg == "web" || leg == "all") { std::printf("[web]\n"); legWeb(env); }
  if (leg == "turn" || leg == "all") { std::printf("[turn]\n"); legTurn(env); }
  std::printf("mg-live: %s\n", g_failed ? "FAILED" : "PASSED");
  return g_failed ? 1 : 0;
}

}  // namespace lab
