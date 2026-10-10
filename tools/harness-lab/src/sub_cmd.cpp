// nimbus-lab sub - drive one Anthropic sub-agent through the real wire code.
//
// The scenario suite only exercises head turns; a sub-agent spawn is a separate
// multi-call chain (environment -> agent -> session -> user.message -> event
// polls) that had no host harness at all, so CUM-481 ("create session HTTP 400")
// could only be chased on a board. This command runs that chain with the shipping
// providers::antDispatch / antPoll over libcurl. --nvs-full makes every cache
// write a no-op, which is what a full NVS partition does to the device's
// antEnvId / antAgentMap keys (CUM-479), so the bench failure reproduces here.
#include "sub_cmd.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include "curl_transport.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/rate_limit.h"

namespace lab {

namespace {

const char* kDeviceDefaultModel = "claude-sonnet-5";   // src/agent/agent_config.h ANT_MODEL

uint32_t nowMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The device's two NVS cache keys. `full` drops every write and reports
// nothing back, exactly like store::setAntEnvId on a full partition.
struct NvsSim {
  std::string env, agents;
  bool full = false;
};

int countCreates(const CurlHttpTransport& http, size_t from) {
  int n = 0;
  for (size_t i = from; i < http.calls.size(); i++) {
    const std::string& p = http.calls[i].path;
    if (p == "/v1/environments" || p == "/v1/agents") n++;
  }
  return n;
}

void printFailedCalls(const CurlHttpTransport& http, size_t from) {
  for (size_t i = from; i < http.calls.size(); i++) {
    const auto& c = http.calls[i];
    if (c.status != 0 && c.status < 300) continue;
    std::printf("    %s %s -> %d %s\n", c.method.c_str(), c.path.c_str(), c.status,
                c.status ? c.respBody.substr(0, 240).c_str() : c.err.c_str());
  }
}

// Write each exchange's response body to <dir>/NN-METHOD-path-STATUS.json, the
// raw material for recorded host fixtures. Bodies only: request headers (the
// key) are never written.
void recordBodies(const CurlHttpTransport& http, const std::string& dir) {
  for (size_t i = 0; i < http.calls.size(); i++) {
    const auto& c = http.calls[i];
    std::string slug = c.path.substr(0, c.path.find('?'));
    for (char& ch : slug)
      if (!std::isalnum((unsigned char)ch)) ch = '_';
    char name[64];
    std::snprintf(name, sizeof(name), "%02u-%s", (unsigned)(i + 1), c.method.c_str());
    const std::string path = dir + "/" + name + slug + "-" + std::to_string(c.status) + ".json";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(c.respBody.data(), 1, c.respBody.size(), f);
      std::fclose(f);
    }
  }
  std::printf("recorded %zu response bodies under %s\n", http.calls.size(), dir.c_str());
}

// Poll until the job is terminal or the deadline passes. 0 = Done.
int pollToEnd(const agent::providers::ProviderDeps& pd, const char* jobId,
              const SubOptions& opt) {
  std::unique_ptr<agent::ResultEnvelope> env(new agent::ResultEnvelope());
  const uint32_t deadline = nowMs() + (uint32_t)opt.timeoutS * 1000u;
  int last = -1;
  while (nowMs() < deadline) {
    std::this_thread::sleep_for(std::chrono::seconds(opt.pollS));
    *env = agent::ResultEnvelope();
    agent::FabricErr e = agent::providers::antPoll(pd, jobId, *env);
    if (e != agent::FabricErr::Ok) {
      std::printf("  poll err=%d\n", (int)e);
      continue;
    }
    if ((int)env->state != last) {
      std::printf("  state=%d\n", (int)env->state);
      last = (int)env->state;
    }
    if (!agent::isTerminal(env->state)) continue;
    if (env->state == agent::JobState::Done) {
      std::printf("  reply: %s\n", std::string(env->reply).substr(0, 400).c_str());
      return 0;
    }
    std::printf("  error: %s\n", env->error);
    return 1;
  }
  std::printf("  poll deadline (%d s) passed with no terminal state\n", opt.timeoutS);
  return 1;
}

}  // namespace

int cmdSub(const Env& env, const SubOptions& opt) {
  if (env.providerKey("anthropic").empty()) {
    std::fprintf(stderr, "no ANTHROPIC_API_KEY configured - skipped\n");
    return 2;
  }
  CurlHttpTransport http;
  http.verbose = opt.verbose;
  NvsSim nvs;
  nvs.full = opt.nvsFull;

  agent::providers::ProviderDeps pd;
  pd.http = &http;
  pd.key = [&env](const char* h) { return env.providerKey(h); };
  pd.antEnvId = [&nvs] { return nvs.env; };
  pd.setAntEnvId = [&nvs](const std::string& v) { if (!nvs.full) nvs.env = v; };
  pd.antAgentMap = [&nvs] { return nvs.agents; };
  pd.setAntAgentMap = [&nvs](const std::string& v) { if (!nvs.full) nvs.agents = v; };
  pd.nowMs = nowMs;
  agent::hlog::setSink(+[](const char* l) { std::fprintf(stderr, "[harness] %s\n", l); });

  std::printf("nimbus-lab sub - anthropic model=%s nvs=%s runs=%d\n",
              opt.model.empty() ? kDeviceDefaultModel : opt.model.c_str(),
              opt.nvsFull ? "full (writes dropped)" : "ok", opt.runs);
  int failures = 0;
  for (int run = 1; run <= opt.runs; run++) {
    const size_t mark = http.calls.size();
    agent::RateLimit window = agent::RateLimit::Unknown;
    agent::Directive d;
    d.category = "research";
    d.instruction = opt.task.c_str();
    d.model = opt.model.empty() ? nullptr : opt.model.c_str();
    d.rateLimit = &window;
    char jobId[72] = {};
    agent::FabricErr e = agent::providers::antDispatch(pd, kDeviceDefaultModel, d, jobId);
    std::printf("run %d: dispatch err=%d job=%s creates=%d\n", run, (int)e,
                jobId[0] ? jobId : "-", countCreates(http, mark));
    if (e != agent::FabricErr::Ok) {
      if (e == agent::FabricErr::RateLimited)
        std::printf("  rate limited: %s\n", agent::rateLimitSlug(window));
      printFailedCalls(http, mark);
      failures++;
      continue;
    }
    if (pollToEnd(pd, jobId, opt) != 0) failures++;
  }
  agent::hlog::setSink(nullptr);
  if (!opt.recordDir.empty()) recordBodies(http, opt.recordDir);
  std::printf("\n%d HTTP calls, %s\n", (int)http.calls.size(),
              failures ? "SUB-AGENT RUN FAILED" : "sub-agent run(s) reached Done");
  return failures ? 1 : 0;
}

}  // namespace lab
