#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/fabric.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/provider_slots.h"

// First suite of the standalone-harness pyramid: the HeavyFabric registry -
// category binding, jobId "backend:remoteId" routing, unknown-backend
// handling, and the hlog seam (the portable library's only side channel).

using agent::Directive;
using agent::FabricErr;
using agent::HeavyFabric;
using agent::JobState;
using agent::ResultEnvelope;
using harness_test::FakeAdapter;
using harness_test::LogCapture;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static Directive directive(const char* cat, const char* task) {
  Directive d;
  d.category = cat;
  d.instruction = task;
  d.tag = "job0001";
  d.chatId = "1001";
  return d;
}

static void test_dispatch_routes_by_category_binding() {
  FakeAdapter a;
  a.backend = "anthropic";
  HeavyFabric f;
  f.registerAdapter(&a);
  f.bindCategory("research", "anthropic");

  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok,
                    (int)f.dispatch(directive("research", "dig"), jobId));
  TEST_ASSERT_EQUAL_STRING("anthropic:job-1", jobId);
  TEST_ASSERT_EQUAL(1, (int)a.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("dig", a.dispatched[0].instruction.c_str());
}

static void test_unbound_category_is_notfound_and_logged() {
  FakeAdapter a;
  HeavyFabric f;
  f.registerAdapter(&a);

  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::NotFound,
                    (int)f.dispatch(directive("ops", "x"), jobId));
  TEST_ASSERT_TRUE(LogCapture::contains("no backend for category ops"));
}

static void test_poll_routes_by_jobid_prefix() {
  FakeAdapter a, b;
  a.backend = "openai";
  b.backend = "mistral";
  b.pollScript = {JobState::Done};
  b.doneReply = "mistral says done";
  HeavyFabric f;
  f.registerAdapter(&a);
  f.registerAdapter(&b);

  ResultEnvelope env;
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)f.poll("mistral:xyz", env));
  TEST_ASSERT_EQUAL((int)JobState::Done, (int)env.state);
  TEST_ASSERT_EQUAL_STRING("mistral says done", env.reply);
  TEST_ASSERT_EQUAL(0, a.pollCount);   // openai adapter never touched
}

static void test_unknown_backend_prefix_is_notfound() {
  FakeAdapter a;
  a.backend = "openai";
  HeavyFabric f;
  f.registerAdapter(&a);
  ResultEnvelope env;
  TEST_ASSERT_EQUAL((int)FabricErr::NotFound, (int)f.poll("nope:123", env));
}

static void test_oversized_backend_prefix_logs_and_bails() {
  FakeAdapter a;
  HeavyFabric f;
  f.registerAdapter(&a);
  ResultEnvelope env;
  std::string longId(40, 'x');
  longId += ":1";
  TEST_ASSERT_EQUAL((int)FabricErr::NotFound, (int)f.poll(longId.c_str(), env));
  TEST_ASSERT_TRUE(LogCapture::contains("backend prefix too long"));
}

static void test_null_log_sink_never_crashes() {
  agent::hlog::setSink(nullptr);
  FakeAdapter a;
  HeavyFabric f;
  f.registerAdapter(&a);
  char jobId[72] = {};
  // Hits the "no backend for category" log path with no sink installed.
  TEST_ASSERT_EQUAL((int)FabricErr::NotFound,
                    (int)f.dispatch(directive("ops", "x"), jobId));
}

static void test_directive_skill_field_reaches_adapter() {
  // The Directive.skill hint must transit the fabric untouched - the skills-v1
  // per-spawn injection (roadmap P2) builds on this field.
  FakeAdapter a;
  a.backend = "custom";
  HeavyFabric f;
  f.registerAdapter(&a);
  f.bindCategory("code", "custom");
  Directive d = directive("code", "write it");
  d.skill = "deep_research";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)f.dispatch(d, jobId));
  TEST_ASSERT_EQUAL_STRING("deep_research", a.dispatched[0].skill.c_str());
}

static void test_every_device_backend_fits_the_table() {
  // The device registers every backend at boot through registerDeviceBackends,
  // keyed or not, so a key added later works without a restart (CUM-541). Each id
  // must stay reachable, and a seventh must be refused loudly, not in silence.
  FakeAdapter a[agent::kDeviceBackendCount];
  agent::ManagedAgentAdapter* list[agent::kDeviceBackendCount];
  for (int i = 0; i < agent::kDeviceBackendCount; i++) {
    a[i].backend = agent::kDeviceBackends[i];
    list[i] = &a[i];
  }
  HeavyFabric f;
  agent::ManagedAgentAdapter* const (&ref)[agent::kDeviceBackendCount] = list;
  TEST_ASSERT_NULL(agent::registerDeviceBackends(f, ref));
  for (int i = 0; i < agent::kDeviceBackendCount; i++)
    TEST_ASSERT_TRUE(f.adapterFor(agent::kDeviceBackends[i]) == &a[i]);
  TEST_ASSERT_FALSE(LogCapture::contains("adapter table full"));

  FakeAdapter extra;
  extra.backend = "seventh";
  f.registerAdapter(&extra);
  TEST_ASSERT_NULL(f.adapterFor("seventh"));
  TEST_ASSERT_TRUE(LogCapture::contains("adapter table full (6), seventh not registered"));
}

// A slot holding the wrong adapter, or none, is named instead of passing quietly.
static void test_device_backend_miswired_is_named() {
  FakeAdapter a[agent::kDeviceBackendCount];
  agent::ManagedAgentAdapter* list[agent::kDeviceBackendCount];
  for (int i = 0; i < agent::kDeviceBackendCount; i++) {
    a[i].backend = agent::kDeviceBackends[i];
    list[i] = &a[i];
  }
  list[4] = &a[5];   // "zai" slot holds the cumulo adapter
  HeavyFabric f;
  agent::ManagedAgentAdapter* const (&ref)[agent::kDeviceBackendCount] = list;
  TEST_ASSERT_EQUAL_STRING("zai", agent::registerDeviceBackends(f, ref));

  list[4] = nullptr;
  HeavyFabric g;
  TEST_ASSERT_EQUAL_STRING("zai", agent::registerDeviceBackends(g, ref));
  TEST_ASSERT_TRUE(LogCapture::contains("null adapter not registered"));
}

// The class: every first-class provider and the custom endpoint is a device
// backend, so none of them can again need a restart before its helpers run.
static void test_every_provider_is_a_device_backend() {
  auto listed = [](const char* id) {
    for (const char* b : agent::kDeviceBackends)
      if (std::strcmp(b, id) == 0) return true;
    return false;
  };
  for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; i++)
    TEST_ASSERT_TRUE_MESSAGE(listed(nimbus::orch::kProviderSlots[i].slug), nimbus::orch::kProviderSlots[i].slug);
  TEST_ASSERT_TRUE(listed("custom"));
}


// ---- LowMemory on evidence (heap F4) ---------------------------------------------
// The device transport names a memory-caused failure only on direct evidence: an
// allocation failed during the exchange (its error says "alloc failed"). Such a step
// is low memory: the fabric keeps its one same-host resend (the collapse is usually
// transient) and never switches hosts - the same request fails the same way on every
// provider, and a switch blamed the provider for a device fault (the 14 Oct openai ->
// anthropic switch after "0/71508").
static const char* kEvidenceErr =
    "request body truncated: write failed after 8192 of 71508 bytes: low memory (alloc failed "
    "1600 B DMA|INTERNAL)";

static std::string oaiDoneBody() {
  return "{\"id\":\"resp_f\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_f\","
         "\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}],"
         "\"usage\":{\"input_tokens\":60,\"output_tokens\":15}}";
}
static std::string antDoneBody() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_2\",\"name\":\"orch_turn\","
         "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
}

struct EvidenceRig {
  harness_test::FakeProviderDeps d;
  harness_test::FakeProviderDeps::ToolRig rig;
  agent::providers::ProviderDeps pd;
  int notifies = 0;
  EvidenceRig() {
    pd = d.contract();
    pd.delayMs = [this](uint32_t ms) { d.now += ms; };
    d.fillTools(rig);
    rig.ht.cfg.roundMinHeap = 12000;   // ORCH_LOOP_MIN_HEAP on the device
  }
  bool run(std::string& out, std::string& err) {
    return agent::providers::runFabricLoop(
        pd, {"openai", "anthropic"}, "SYS", "USER", out, err, rig.ht, nullptr,
        [this](const std::string&, const std::string&) { notifies++; });
  }
};

// The heap looks healthy again by the time the step returns (a transient collapse):
// the evidence alone makes it memory. One resend on the same host serves the round.
static void test_low_memory_with_evidence_resends_and_never_switches() {
  EvidenceRig r;
  r.d.http.script.push_back(harness_test::transportCut(0, kEvidenceErr));
  r.d.http.script.push_back({"api.openai.com", "/v1/responses", 200, oaiDoneBody()});
  std::string out, err;
  TEST_ASSERT_TRUE_MESSAGE(r.run(out, err), err.c_str());
  TEST_ASSERT_EQUAL(2, (int)r.d.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("api.openai.com", r.d.http.seen[1].host.c_str());
  TEST_ASSERT_EQUAL(0, r.notifies);
}

// Both attempts fail with evidence: the turn ends as low memory (the engine's
// memory reply), with no switch - an anthropic request here would be unscripted.
static void test_low_memory_with_evidence_ends_as_memory_without_a_switch() {
  EvidenceRig r;
  r.d.http.script.push_back(harness_test::transportCut(0, kEvidenceErr));
  r.d.http.script.push_back(harness_test::transportCut(0, kEvidenceErr));
  std::string out, err;
  TEST_ASSERT_FALSE(r.run(out, err));
  TEST_ASSERT_TRUE_MESSAGE(err.rfind("low memory", 0) == 0, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)r.d.http.seen.size());
  TEST_ASSERT_EQUAL(0, r.notifies);
  // Evidence on a request that DID go out (no response) is memory too: no switch,
  // and no same-host retry of a request the provider may be running.
  EvidenceRig s;
  s.d.http.script.push_back(harness_test::transportCut(0, "no response: low memory (alloc failed)"));
  TEST_ASSERT_FALSE(s.run(out, err));
  TEST_ASSERT_TRUE_MESSAGE(err.rfind("low memory", 0) == 0, err.c_str());
  TEST_ASSERT_EQUAL(1, (int)s.d.http.seen.size());
  TEST_ASSERT_EQUAL(0, s.notifies);
}

// CUM-404 v2 counter-test: the same write failure with NO allocation evidence, on a
// board whose largest block sat at 5,200 B (a CYD/Freenove idles there; just above
// the 5,000 B start floor, so no starved tag) and whose total free is healthy, is a
// network failure: it still fails over and the next provider serves the round. The
// largest block alone never makes a failure "low memory".
static void test_no_evidence_still_fails_over_cum404_counter_test() {
  EvidenceRig r;
  r.d.largest = 5200;
  r.d.http.script.push_back(
      harness_test::transportCut(0, "request body truncated: write failed after 0 of 71508 bytes"));
  r.d.http.script.push_back(
      harness_test::transportCut(0, "request body truncated: write failed after 0 of 71508 bytes"));
  r.d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antDoneBody()});
  std::string out, err;
  TEST_ASSERT_TRUE_MESSAGE(r.run(out, err), err.c_str());
  TEST_ASSERT_EQUAL(1, r.notifies);   // switched openai -> anthropic
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com", r.d.http.seen.back().host.c_str());
  // And the starved-start tag alone (": low memory", no "alloc failed") on a healthy
  // total is still not memory: it fails over too.
  EvidenceRig s;
  s.d.http.script.push_back(harness_test::transportCut(0, "connect failed: low memory"));
  s.d.http.script.push_back(harness_test::transportCut(0, "connect failed: low memory"));
  s.d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antDoneBody()});
  TEST_ASSERT_TRUE_MESSAGE(s.run(out, err), err.c_str());
  TEST_ASSERT_EQUAL(1, s.notifies);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_dispatch_routes_by_category_binding);
  RUN_TEST(test_unbound_category_is_notfound_and_logged);
  RUN_TEST(test_poll_routes_by_jobid_prefix);
  RUN_TEST(test_unknown_backend_prefix_is_notfound);
  RUN_TEST(test_oversized_backend_prefix_logs_and_bails);
  RUN_TEST(test_null_log_sink_never_crashes);
  RUN_TEST(test_directive_skill_field_reaches_adapter);
  RUN_TEST(test_every_device_backend_fits_the_table);
  RUN_TEST(test_device_backend_miswired_is_named);
  RUN_TEST(test_every_provider_is_a_device_backend);
  RUN_TEST(test_low_memory_with_evidence_resends_and_never_switches);
  RUN_TEST(test_low_memory_with_evidence_ends_as_memory_without_a_switch);
  RUN_TEST(test_no_evidence_still_fails_over_cum404_counter_test);
  UNITY_END();
  return 0;
}
