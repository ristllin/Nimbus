// test_sub_cut - CUM-538: every sub dispatcher reads a transport failure of the
// request that starts its paid work by ONE rule, on the device and the hosted
// builds alike (both call these providers::*Dispatch functions):
//   - the request never fully left ("request body truncated", "head write
//     failed"): Network, "did not start" (a respawn duplicates nothing);
//   - it went out and no answer came back ("no response", the device's word and
//     the hosted transport's): Timeout, "may have started" (never spawned again);
//   - any other failure: Timeout only when it took about the whole deadline.
// Before, only the Z.ai, Cumulo and custom subs honoured "no response"; a Mistral,
// OpenAI or Anthropic sub cut after its POST read "did not start" and the head was
// told to spawn the paid work again. A new dispatcher joins the table below or it
// is not covered.
#include <unity.h>

#include <string>

#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"

using namespace harness_test;
namespace prov = agent::providers;
using agent::FabricErr;

void setUp() {}
void tearDown() {}

static agent::Directive directive() {
  agent::Directive d;
  d.category = "research";
  d.instruction = "summarize the release notes";
  d.task = "summarize the release notes";
  return d;
}

static FabricErr compat(FakeProviderDeps& rig, const char* host, const char* base, const char* tag,
                        prov::CompatWire wire) {
  const prov::ProviderDeps pd = rig.contract();
  prov::CompatEndpoint ep;
  ep.host = host;
  ep.basePath = base;
  ep.key = "k";
  ep.model = "glm-5.3";
  ep.backendTag = tag;
  ep.wire = wire;
  char job[72] = {};
  return prov::openaiCompatDispatch(pd, ep, directive(), job);
}
static FabricErr zaiSub(FakeProviderDeps& rig) {
  return compat(rig, "api.z.ai", "/api/paas/v4", "zai", prov::CompatWire::OpenAIChat);
}
static FabricErr cumuloSub(FakeProviderDeps& rig) {
  return compat(rig, "app.cumulo-nimbus.ai", "/router/openai/v1", "cumulo", prov::CompatWire::OpenAIChat);
}
static FabricErr cumuloMessagesSub(FakeProviderDeps& rig) {
  return compat(rig, "app.cumulo-nimbus.ai", "/router/anthropic/v1", "cumulo",
                prov::CompatWire::AnthropicMessages);
}
static FabricErr customSub(FakeProviderDeps& rig) {
  rig.custBase = "https://p.example";
  const prov::ProviderDeps pd = rig.contract();
  char job[72] = {};
  return prov::customDispatch(pd, directive(), job);
}
static FabricErr customMessagesSub(FakeProviderDeps& rig) {
  rig.custConv = "anthropic";
  return customSub(rig);
}
static FabricErr mistralSub(FakeProviderDeps& rig) {
  const prov::ProviderDeps pd = rig.contract();
  char job[72] = {};
  return prov::mistralDispatch(pd, "mistral-small-latest", directive(), job);
}
static FabricErr openaiSub(FakeProviderDeps& rig) {
  const prov::ProviderDeps pd = rig.contract();
  char job[72] = {};
  return prov::oaiDispatch(pd, "api.openai.com", "sk-fake-oai", "gpt-5.6", "openai", directive(), job);
}
// Managed Agents: the cached environment and agent, a session created, then the
// turn that starts the run is the request cut.
static FabricErr anthropicSub(FakeProviderDeps& rig) {
  rig.antEnv = "env_1";
  rig.antAgents = "claude-sonnet-5=ag_1;";
  rig.http.script.insert(rig.http.script.begin(),
                         Exchange{"", "/v1/sessions", 200, "{\"id\":\"sess_1\"}"});
  const prov::ProviderDeps pd = rig.contract();
  char job[72] = {};
  return prov::antDispatch(pd, "claude-sonnet-5", directive(), job);
}

struct Dispatcher {
  const char* name;
  FabricErr (*run)(FakeProviderDeps&);
  const char* paidPath;   // the request that starts the paid work
  uint32_t deadlineMs;    // its read deadline
};
static const Dispatcher kDispatchers[] = {
    {"zai", zaiSub, "/api/paas/v4/chat/completions", 50000},
    {"cumulo", cumuloSub, "/router/openai/v1/chat/completions", 120000},
    {"cumulo-messages", cumuloMessagesSub, "/router/anthropic/v1/messages", 120000},
    {"custom", customSub, "/v1/chat/completions", 30000},
    {"custom-messages", customMessagesSub, "/v1/messages", 30000},
    {"mistral", mistralSub, "/v1/conversations", 120000},   // the sync-sub ceiling (was 60 s)
    {"openai", openaiSub, "/v1/responses", 120000},
    {"anthropic", anthropicSub, "/v1/sessions/sess_1/events", 120000},
};

struct Cut {
  const char* err;    // the transport's words
  bool wholeDeadline; // the failure took the whole read deadline
  FabricErr want;
};
static const Cut kCuts[] = {
    {"no response", false, FabricErr::Timeout},   // the device: sent, then cut
    {"Server returned nothing (no headers, no data): no response", false, FabricErr::Timeout},  // hosted
    {"Timeout was reached: no response", true, FabricErr::Timeout},
    {"request body truncated (10/200 bytes written)", true, FabricErr::Network},  // never left
    {"head write failed", false, FabricErr::Network},
    {"connect failed: Could not connect to server", false, FabricErr::Network},
    {"connect failed: Could not connect to server", true, FabricErr::Timeout},   // the elapsed rule
};

static const char* errName(FabricErr e) {
  if (e == FabricErr::Timeout) return "Timeout";
  if (e == FabricErr::Network) return "Network";
  return "other";
}

// Every (dispatcher, cut) pair; the mismatches are listed together, so a failure
// names every dispatcher that breaks the rule, not just the first.
static void test_every_sub_dispatcher_reads_a_cut_by_one_rule() {
  std::string wrong;
  for (const Dispatcher& dp : kDispatchers) {
    for (const Cut& c : kCuts) {
      FakeProviderDeps rig;
      rig.http.script.push_back(transportCut(c.wholeDeadline ? dp.deadlineMs : 1000, c.err));
      const FabricErr got = dp.run(rig);
      const std::string what = std::string(dp.name) + " <- \"" + c.err + "\"" +
                               (c.wholeDeadline ? " (whole deadline)" : " (fast)");
      if (got != c.want) wrong += what + ": " + errName(got) + " want " + errName(c.want) + "; ";
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(rig.http.script.size(), rig.http.seen.size(), what.c_str());
      const agent::HttpRequest& paid = rig.http.seen.back();
      TEST_ASSERT_EQUAL_STRING_MESSAGE(dp.paidPath, paid.path.c_str(), what.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE("POST", paid.method.c_str(), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(dp.deadlineMs, paid.timeoutMs, what.c_str());
    }
  }
  TEST_ASSERT_EQUAL_STRING("", wrong.c_str());
}

// The Anthropic chain's earlier calls run nothing by themselves: a session create
// cut after it went out is "did not start" (no turn was sent, nothing runs).
static void test_anthropic_session_cut_did_not_start() {
  FakeProviderDeps rig;
  rig.antEnv = "env_1";
  rig.antAgents = "claude-sonnet-5=ag_1;";
  rig.http.script.push_back(transportCut(1000, "no response"));
  const prov::ProviderDeps pd = rig.contract();
  char job[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Network,
                    (int)prov::antDispatch(pd, "claude-sonnet-5", directive(), job));
  TEST_ASSERT_EQUAL_STRING("/v1/sessions", rig.http.seen.back().path.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_sub_dispatcher_reads_a_cut_by_one_rule);
  RUN_TEST(test_anthropic_session_cut_did_not_start);
  return UNITY_END();
}
