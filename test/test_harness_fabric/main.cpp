#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/fabric.h"

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
  // The device registers all six backends at boot, keyed or not, so a key added
  // later works without a restart. Each must stay reachable by id, and a seventh
  // must be refused loudly rather than dropped in silence.
  const char* ids[] = {"anthropic", "openai", "mistral", "custom", "zai", "cumulo"};
  FakeAdapter a[6];
  HeavyFabric f;
  for (int i = 0; i < 6; i++) { a[i].backend = ids[i]; f.registerAdapter(&a[i]); }
  for (int i = 0; i < 6; i++) TEST_ASSERT_TRUE(f.adapterFor(ids[i]) == &a[i]);
  TEST_ASSERT_FALSE(LogCapture::contains("adapter table full"));

  FakeAdapter extra;
  extra.backend = "seventh";
  f.registerAdapter(&extra);
  TEST_ASSERT_NULL(f.adapterFor("seventh"));
  TEST_ASSERT_TRUE(LogCapture::contains("adapter table full (6), seventh not registered"));
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
  UNITY_END();
  return 0;
}
