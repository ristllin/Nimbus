#include <unity.h>

#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/mem_config.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- the anchor invariant ----------------------------------------------------
// deriveBudget(200000, {}) MUST reproduce today's shipped constants exactly -
// this is what makes the derive change behavior-identical on the current fleet
// (Anthropic 200K head) until a different window is configured. If this test
// changes, the fleet's effective limits changed: mean it.
static void test_anchor_identity_at_200k() {
  ContextBudget b = deriveBudget(200000, {});
  TEST_ASSERT_EQUAL_INT(32768, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)b.chatSummaryBytes);
  TEST_ASSERT_EQUAL_UINT32(3000, (uint32_t)b.recentConvBytes);
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(65536, (uint32_t)b.toolTotalBytes);
  TEST_ASSERT_EQUAL_UINT32(1200, (uint32_t)b.briefBytes);
  TEST_ASSERT_EQUAL_UINT32(65536, (uint32_t)b.foldSliceBytes);
  TEST_ASSERT_EQUAL_UINT32(200000, b.ctxTokens);
}

// ---- floors bind on a small window -------------------------------------------
static void test_floors_bind_on_8k_window() {
  ContextBudget b = deriveBudget(8000, {});
  // 8K tokens = 32000 B window; every scaled value lands below its floor.
  TEST_ASSERT_EQUAL_INT(8192, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(2048, (uint32_t)b.chatSummaryBytes);
  TEST_ASSERT_EQUAL_UINT32(1500, (uint32_t)b.recentConvBytes);
  TEST_ASSERT_EQUAL_UINT32(512, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.toolTotalBytes);
  TEST_ASSERT_EQUAL_UINT32(600, (uint32_t)b.briefBytes);
  TEST_ASSERT_EQUAL_UINT32(16384, (uint32_t)b.foldSliceBytes);
}

// ---- caps bind on a huge window ----------------------------------------------
static void test_caps_bind_on_1m_window() {
  ContextBudget b = deriveBudget(1000000, {});
  TEST_ASSERT_EQUAL_INT(MemConfig::kContextMax, b.systemPromptBytes);  // 65536
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.chatSummaryBytes);
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.recentConvBytes);
  TEST_ASSERT_EQUAL_UINT32(40960, (uint32_t)b.toolResultBytes);   // 5x anchor, under cap
  TEST_ASSERT_EQUAL_UINT32(327680, (uint32_t)b.toolTotalBytes);   // 5x anchor, under cap
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)b.briefBytes);
  TEST_ASSERT_EQUAL_UINT32(131072, (uint32_t)b.foldSliceBytes);
}

// ---- overrides win verbatim (then clamp) -------------------------------------
static void test_overrides_win_and_clamp() {
  BudgetOverrides ov;
  ov.maxContextBytes = 20000;
  ov.toolResultCap = 2048;
  ov.toolTotalCap = 4096;
  ContextBudget b = deriveBudget(200000, ov);
  TEST_ASSERT_EQUAL_INT(20000, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(2048, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)b.toolTotalBytes);
  // Out-of-range overrides clamp to the SAME ranges the NVS setters enforce.
  ov.maxContextBytes = 1 << 20;
  ov.toolResultCap = 1 << 20;
  ov.toolTotalCap = 1 << 24;
  b = deriveBudget(200000, ov);
  TEST_ASSERT_EQUAL_INT(MemConfig::kContextMax, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(65536, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(1048576, (uint32_t)b.toolTotalBytes);
}

// ---- zero window falls back to the conservative default ----------------------
static void test_zero_window_uses_default() {
  ContextBudget b = deriveBudget(0, {});
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, b.ctxTokens);
  // 100K default = half the anchor: scaled values are half, floors bind where lower.
  TEST_ASSERT_EQUAL_INT(16384, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(32768, (uint32_t)b.toolTotalBytes);
}

// ---- monotonicity: a bigger window never shrinks an allocation ---------------
static void test_monotonic_in_window() {
  uint32_t windows[] = {8000, 32000, 100000, 128000, 200000, 272000, 1000000};
  ContextBudget prev = deriveBudget(windows[0], {});
  for (size_t i = 1; i < sizeof(windows) / sizeof(windows[0]); i++) {
    ContextBudget cur = deriveBudget(windows[i], {});
    TEST_ASSERT_TRUE(cur.systemPromptBytes >= prev.systemPromptBytes);
    TEST_ASSERT_TRUE(cur.chatSummaryBytes >= prev.chatSummaryBytes);
    TEST_ASSERT_TRUE(cur.recentConvBytes >= prev.recentConvBytes);
    TEST_ASSERT_TRUE(cur.toolResultBytes >= prev.toolResultBytes);
    TEST_ASSERT_TRUE(cur.toolTotalBytes >= prev.toolTotalBytes);
    TEST_ASSERT_TRUE(cur.briefBytes >= prev.briefBytes);
    TEST_ASSERT_TRUE(cur.foldSliceBytes >= prev.foldSliceBytes);
    prev = cur;
  }
}

// ---- Mistral 128K sanity: derived values sit between floor and anchor --------
static void test_mistral_128k_derivation() {
  ContextBudget b = deriveBudget(128000, {});
  // 128K/200K = 0.64x the anchor.
  TEST_ASSERT_EQUAL_INT(20971, b.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(5242, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(41943, (uint32_t)b.toolTotalBytes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_anchor_identity_at_200k);
  RUN_TEST(test_floors_bind_on_8k_window);
  RUN_TEST(test_caps_bind_on_1m_window);
  RUN_TEST(test_overrides_win_and_clamp);
  RUN_TEST(test_zero_window_uses_default);
  RUN_TEST(test_monotonic_in_window);
  RUN_TEST(test_mistral_128k_derivation);
  return UNITY_END();
}
