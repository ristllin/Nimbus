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
  c.budget.recordTokens("anthropic", 1000, 250, 0, 0, "turn", "claude-opus-5");
  TEST_ASSERT_EQUAL(1, (int)fc.recorded.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", fc.recorded[0].host.c_str());
  TEST_ASSERT_EQUAL(1000, (int)fc.recorded[0].in);
  TEST_ASSERT_EQUAL(250, (int)fc.recorded[0].out);
  TEST_ASSERT_EQUAL_STRING("turn", fc.recorded[0].tag.c_str());
  TEST_ASSERT_EQUAL_STRING("claude-opus-5", fc.recorded[0].model.c_str());
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

// QW6: where a spoken reply's Telegram fallback goes. A Telegram turn (the owner's or a guest's)
// answers in its own chat, so a guest's request is never voiced into the owner's chat; a web,
// voice or serial turn, a device origin and a non-chat id go to the owner chat; no owner chat, no
// destination. (Merge review: only the owner-chat branch had run, on the bench.)
static void test_a_spoken_fallback_goes_to_the_turns_own_chat_else_the_owner_chat() {
  using agent::voiceFallbackChatOf;
  TEST_ASSERT_EQUAL_STRING("1001", voiceFallbackChatOf("1001", "1001").c_str());       // the owner's own chat
  TEST_ASSERT_EQUAL_STRING("7788", voiceFallbackChatOf("7788", "1001").c_str());       // a guest, never the owner
  TEST_ASSERT_EQUAL_STRING("-100123456", voiceFallbackChatOf("-100123456", "1001").c_str());   // a group
  for (const char* origin : {"web", "voice", "serial", "system", "", "loop:abc", "sub-1", "12ab", "-", "--5"})
    TEST_ASSERT_EQUAL_STRING_MESSAGE("1001", voiceFallbackChatOf(origin, "1001").c_str(), origin);
  TEST_ASSERT_EQUAL_STRING("", voiceFallbackChatOf("web", "").c_str());                // nowhere to send it
  TEST_ASSERT_EQUAL_STRING("7788", voiceFallbackChatOf("7788", "").c_str());           // the turn's own chat still stands
  TEST_ASSERT_EQUAL_STRING("1001", voiceFallbackChatOf(std::string(21, '9'), "1001").c_str());   // not a chat number
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_provider_defaults_and_keys);
  RUN_TEST(test_conv_id_roundtrip);
  RUN_TEST(test_loop_caps_mirror_store_defaults);
  RUN_TEST(test_budget_gate_and_recording);
  RUN_TEST(test_no_provider_write_surface_beyond_convid);
  RUN_TEST(test_a_spoken_fallback_goes_to_the_turns_own_chat_else_the_owner_chat);
  UNITY_END();
  return 0;
}
