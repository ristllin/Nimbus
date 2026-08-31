// CUM-238: the provider-config read cache must keep NVS reads BOUNDED across a
// hot loop, not re-read every tick. These tests count the actual read-batches
// (each refresh == one round of getString() calls on the device) rather than
// only checking the returned values - the class rule the recurring hot-loop bug
// paid for: assert reads stay bounded, so a future change that reintroduces a
// per-tick NVS read FAILS here instead of shipping.

#include <unity.h>

#include <string>

#include "nimbus/store/provider_cache.h"

using namespace nimbus::store;

void setUp() {}
void tearDown() {}

// A fake NVS backend. `fills` counts how many times the cache actually went to
// "NVS" (invoked the fill) - the number the bug blew up to O(loop iterations).
struct FakeNvs {
  std::string openai, anthropic, mistral, orchHost, priority = "openai,anthropic,mistral";
  int fills = 0;
  ProviderCache::Fill fill() {
    return [this](ProviderSnapshot& s) {
      ++fills;
      s.openaiKey = openai;
      s.anthropicKey = anthropic;
      s.mistralKey = mistral;
      s.orchHost = orchHost;
      s.providerPriority = priority;
    };
  }
};

// The core CUM-238 assertion: N loop ticks reading the (unset) provider config
// must hit NVS ONCE, not N times. This is the read the fresh/wiped device spammed.
static void test_reads_bounded_over_loop() {
  FakeNvs nvs;  // fresh device: every key unset
  ProviderCache c;
  const int kTicks = 1000;  // ~a minute of ~60 ms render/poll ticks
  for (int i = 0; i < kTicks; ++i) {
    const ProviderSnapshot& s = c.snapshot(nvs.fill());
    TEST_ASSERT_FALSE(anyBuiltinKeyed(s));  // still unset - and served from RAM
  }
  TEST_ASSERT_EQUAL_INT(1, nvs.fills);  // refreshed once, not 1000x
}

// A config WRITE (setter -> markDirty) refreshes exactly once, then reads are
// bounded again. Invalidation is the ONLY thing that touches NVS.
static void test_write_triggers_exactly_one_refresh() {
  FakeNvs nvs;
  ProviderCache c;
  for (int i = 0; i < 50; ++i) c.snapshot(nvs.fill());
  TEST_ASSERT_EQUAL_INT(1, nvs.fills);

  nvs.anthropic = "sk-ant-abcd";  // owner pastes a key
  c.markDirty();                  // the setter invalidates
  for (int i = 0; i < 50; ++i) {
    const ProviderSnapshot& s = c.snapshot(nvs.fill());
    TEST_ASSERT_TRUE(anyBuiltinKeyed(s));
    TEST_ASSERT_EQUAL_STRING("sk-ant-abcd", s.anthropicKey.c_str());
  }
  TEST_ASSERT_EQUAL_INT(2, nvs.fills);  // one extra refresh total, not 50
}

// The NEGATIVE "still unset" result is cached: re-asking whether the device is
// configured does not re-read NVS. dirty() starts true (first read populates).
static void test_negative_result_is_cached() {
  FakeNvs nvs;
  ProviderCache c;
  TEST_ASSERT_TRUE(c.dirty());
  const ProviderSnapshot& s = c.snapshot(nvs.fill());
  TEST_ASSERT_FALSE(c.dirty());
  TEST_ASSERT_FALSE(anyBuiltinKeyed(s));
  for (int i = 0; i < 10; ++i) c.snapshot(nvs.fill());
  TEST_ASSERT_EQUAL_INT(1, nvs.fills);  // no NVS re-read to relearn "unset"
}

// anyBuiltinKeyed reflects the three built-in cloud providers only; the BYO
// hosts (zai/cumulo/custom) do not flip the built-in readiness predicate.
static void test_any_builtin_keyed_truth_table() {
  ProviderSnapshot s;
  TEST_ASSERT_FALSE(anyBuiltinKeyed(s));
  s.openaiKey = "x";    TEST_ASSERT_TRUE(anyBuiltinKeyed(s));  s.openaiKey = "";
  s.anthropicKey = "x"; TEST_ASSERT_TRUE(anyBuiltinKeyed(s));  s.anthropicKey = "";
  s.mistralKey = "x";   TEST_ASSERT_TRUE(anyBuiltinKeyed(s));  s.mistralKey = "";
  s.zaiKey = "x"; s.cumuloKey = "y"; s.customKey = "z";
  TEST_ASSERT_FALSE(anyBuiltinKeyed(s));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reads_bounded_over_loop);
  RUN_TEST(test_write_triggers_exactly_one_refresh);
  RUN_TEST(test_negative_result_is_cached);
  RUN_TEST(test_any_builtin_keyed_truth_table);
  return UNITY_END();
}
