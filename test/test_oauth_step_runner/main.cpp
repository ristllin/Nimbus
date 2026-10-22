// test_oauth_step_runner - an MCP OAuth sign-in step never runs on the watched loop.
//
// Lane WDT traced Lumi's watchdog resets to the main loop (the only task the 8 s task
// watchdog watches) blocking past 8 s. The MCP OAuth flow ran each network step
// (HTTPS with 8-10 s timeouts behind a TLS-slot wait) inline in the main-loop pump;
// it was unreachable only because the generic /api/connectors handler swallowed the
// OAuth routes (CUM-718), so fixing the routes made the reset reachable. The loop now
// only decides and a short-lived worker runs the step. These cases pin the runner's
// rules and, as a class guard, that the loop pump calls no network step itself.
#include <unity.h>

#include <cstdio>
#include <string>

#include "nimbus/orch/oauth_step_runner.h"

using nimbus::orch::OauthStepRunner;

void setUp() {}
void tearDown() {}

// A due step starts exactly one worker; while it runs nothing else starts; done()
// frees the runner for the next step.
static void test_one_worker_at_a_time() {
  OauthStepRunner r;
  int spawns = 0;
  auto spawn = [&] { spawns++; return true; };
  TEST_ASSERT_TRUE(r.pump(true, 1000, spawn));
  TEST_ASSERT_TRUE(r.busy());
  for (uint32_t t = 1001; t < 1100; t++) TEST_ASSERT_FALSE(r.pump(true, t, spawn));
  TEST_ASSERT_EQUAL(1, spawns);
  r.done();
  TEST_ASSERT_FALSE(r.busy());
  TEST_ASSERT_TRUE(r.pump(true, 1200, spawn));
  TEST_ASSERT_EQUAL(2, spawns);
  TEST_ASSERT_EQUAL(2, (int)r.started());
}

// Nothing due, nothing started.
static void test_nothing_due_starts_nothing() {
  OauthStepRunner r;
  int spawns = 0;
  for (uint32_t t = 0; t < 50; t++) TEST_ASSERT_FALSE(r.pump(false, t, [&] { spawns++; return true; }));
  TEST_ASSERT_EQUAL(0, spawns);
  TEST_ASSERT_FALSE(r.busy());
}

// A refused start (a turn in flight, low memory) never runs the step and is asked
// again only after kRetryMs - never every loop pass - across the millis() wrap too.
static void test_refused_start_backs_off() {
  for (uint32_t base : {5000u, 0xFFFFF000u}) {
    OauthStepRunner r;
    int asks = 0;
    auto refuse = [&] { asks++; return false; };
    TEST_ASSERT_FALSE(r.pump(true, base, refuse));
    TEST_ASSERT_FALSE(r.busy());
    for (uint32_t dt = 1; dt < OauthStepRunner::kRetryMs; dt += 7) r.pump(true, base + dt, refuse);
    TEST_ASSERT_EQUAL(1, asks);
    r.pump(true, base + OauthStepRunner::kRetryMs, refuse);
    TEST_ASSERT_EQUAL(2, asks);
    TEST_ASSERT_EQUAL(2, (int)r.refused());
  }
}

static std::string readFile(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// The class guard: the main-loop pump in src/agent/connectors.cpp calls no network
// step and no HTTP request itself; it only hands a due step to the runner.
static void test_loop_pump_runs_no_network_step() {
  const std::string src = readFile("src/agent/connectors.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!src.empty(), "cannot read src/agent/connectors.cpp (run from repo root)");
  const size_t ns = src.find("namespace oauth {");
  const size_t at = src.find("\nvoid pump() {", ns);
  TEST_ASSERT_TRUE_MESSAGE(ns != std::string::npos && at != std::string::npos, "oauth pump() not found");
  int depth = 0;
  size_t end = src.find('{', at);
  for (size_t i = end; i < src.size(); i++) {
    if (src[i] == '{') depth++;
    if (src[i] == '}' && --depth == 0) { end = i; break; }
  }
  const std::string body = src.substr(at, end - at);
  for (const char* call : {"stepDiscoverPR(", "stepDiscoverAS(", "stepRegister(", "stepExchange(",
                           "httpDo(", "runStep(", "tryProtectedResource("}) {
    TEST_ASSERT_TRUE_MESSAGE(body.find(call) == std::string::npos, call);
  }
  TEST_ASSERT_TRUE_MESSAGE(body.find("s_runner.pump(") != std::string::npos,
                           "the loop pump no longer hands steps to the runner");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_one_worker_at_a_time);
  RUN_TEST(test_nothing_due_starts_nothing);
  RUN_TEST(test_refused_start_backs_off);
  RUN_TEST(test_loop_pump_runs_no_network_step);
  return UNITY_END();
}
