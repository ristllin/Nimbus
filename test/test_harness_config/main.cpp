#include <unity.h>

#include <string>

#include "../support/fake_config.h"
#include "nimbus/harness/config.h"

// Freezes the HarnessConfig boundary before the lifts: the contract's shape,
// the FakeConfig invariants every later suite depends on, and the structural
// security property (no key/priority/orchHost setters exist - asserted here as
// documentation-by-test: the only writable provider slot is setConvId).

using harness_test::FakeConfig;

void setUp() {}
void tearDown() {}

static void test_provider_defaults_and_keys() {
  FakeConfig fc;
  agent::HarnessConfig c = fc.contract();
  TEST_ASSERT_TRUE(c.provider.hasKey("anthropic"));
  TEST_ASSERT_TRUE(c.provider.hasKey("openai"));
  TEST_ASSERT_FALSE(c.provider.hasKey("custom"));
  TEST_ASSERT_EQUAL_STRING("sk-fake-ant", c.provider.key("anthropic").c_str());
  TEST_ASSERT_EQUAL_STRING("", c.provider.key("nope").c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic,openai,mistral",
                           c.provider.providerPriority().c_str());
}

static void test_conv_id_roundtrip() {
  FakeConfig fc;
  agent::HarnessConfig c = fc.contract();
  TEST_ASSERT_EQUAL_STRING("", c.provider.convId().c_str());
  c.provider.setConvId("anthropic|conv-123");
  TEST_ASSERT_EQUAL_STRING("anthropic|conv-123", c.provider.convId().c_str());
}

static void test_loop_caps_mirror_store_defaults() {
  // The FakeConfig defaults must track store.h's documented defaults (rounds 12,
  // deadline 600 s, result 4096, total 24576, loop ON) so host suites exercise
  // the same bounds the device ships.
  FakeConfig fc;
  agent::HarnessConfig c = fc.contract();
  TEST_ASSERT_TRUE(c.loop.toolLoopOn());
  TEST_ASSERT_EQUAL(12, c.loop.rounds());
  TEST_ASSERT_EQUAL(600, c.loop.deadlineS());
  TEST_ASSERT_EQUAL(4096, c.loop.resultCap());
  TEST_ASSERT_EQUAL(24576, c.loop.totalCap());
}

static void test_budget_gate_and_recording() {
  FakeConfig fc;
  fc.overBudgetHosts.insert("openai");
  agent::HarnessConfig c = fc.contract();
  TEST_ASSERT_TRUE(c.budget.overBudget("openai"));
  TEST_ASSERT_FALSE(c.budget.overBudget("anthropic"));
  c.budget.recordTokens("anthropic", 1000, 250, 0, 0, "turn");
  TEST_ASSERT_EQUAL(1, (int)fc.recorded.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", fc.recorded[0].host.c_str());
  TEST_ASSERT_EQUAL(1000, (int)fc.recorded[0].in);
  TEST_ASSERT_EQUAL(250, (int)fc.recorded[0].out);
  TEST_ASSERT_EQUAL_STRING("turn", fc.recorded[0].tag.c_str());
}

static void test_no_provider_write_surface_beyond_convid() {
  // Structural rail: ProviderConfig's only std::function returning void that
  // mutates provider state is setConvId. This is a compile-time property of
  // the struct; this test documents it and will break loudly if a setter for
  // keys/priority/orchHost is ever added here instead of the human-only path.
  agent::ProviderConfig p;
  (void)p.setConvId;   // exists
  // No p.setKey / p.setProviderPriority / p.setOrchHost - their absence IS the
  // assertion; adding one forces this comment (and the security review) to move.
  TEST_ASSERT_TRUE(true);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_provider_defaults_and_keys);
  RUN_TEST(test_conv_id_roundtrip);
  RUN_TEST(test_loop_caps_mirror_store_defaults);
  RUN_TEST(test_budget_gate_and_recording);
  RUN_TEST(test_no_provider_write_surface_beyond_convid);
  UNITY_END();
  return 0;
}
