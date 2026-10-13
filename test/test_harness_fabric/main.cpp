#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/fabric.h"
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
  UNITY_END();
  return 0;
}
