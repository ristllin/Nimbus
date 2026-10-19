#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/wire_limits.h"

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
  // CUM-485: the recent window is a quarter of the system prompt now (was a
  // 3000 B dead knob - the device used a fixed constant that cut code replies).
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.recentConvBytes);
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
  TEST_ASSERT_EQUAL_UINT32(3000, (uint32_t)b.recentConvBytes);   // floor = the old shipped total
  TEST_ASSERT_EQUAL_UINT32(512, (uint32_t)b.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.toolTotalBytes);
  TEST_ASSERT_EQUAL_UINT32(600, (uint32_t)b.briefBytes);
  TEST_ASSERT_EQUAL_UINT32(16384, (uint32_t)b.foldSliceBytes);
}

// ---- caps bind on a huge window ----------------------------------------------
static void test_caps_bind_on_1m_window() {
  ContextBudget b = deriveBudget(1000000, {});
  TEST_ASSERT_EQUAL_INT(MemConfig::kContextMax, b.systemPromptBytes);  // 56000, under the string guard
  TEST_ASSERT_EQUAL_UINT32(8192, (uint32_t)b.chatSummaryBytes);
  TEST_ASSERT_EQUAL_UINT32(16384, (uint32_t)b.recentConvBytes);
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
  // The per-result clamp tops out under the provider string guard (wire_limits.h).
  TEST_ASSERT_EQUAL_UINT32(kToolResultCapMax, (uint32_t)b.toolResultBytes);
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

// ---- gate W2 #16: every per-turn size follows the head the engine runs --------
// The window, the sub-agent brief and the fold slice each derived their bytes in
// src/agent/orchestrator.cpp, and only the window used the engine's head
// (headHost, router fallback included); the brief and the fold kept the store's
// guess. Static guard over the device source (code lines, comments stripped):
// exactly ONE deriveBudget call and ONE store::resolvedOrchHost call, both in the
// shared headBudget() helper, so a new size site cannot drift to another head.
static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string all;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) all.append(buf, n);
  std::fclose(f);
  return all;
}
// Each line cut at its first "//" (comments carry the helper's name too).
static std::string stripLineComments(const std::string& src) {
  std::string out;
  for (size_t s = 0; s < src.size();) {
    size_t e = src.find('\n', s);
    if (e == std::string::npos) e = src.size();
    std::string line = src.substr(s, e - s);
    const size_t c = line.find("//");
    if (c != std::string::npos) line.erase(c);
    out += line + "\n";
    s = e + 1;
  }
  return out;
}
static int countOf(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}
static void test_orchestrator_sizes_off_one_head_budget() {
  const std::string src = stripLineComments(readFile("src/agent/orchestrator.cpp"));
  TEST_ASSERT_EQUAL_MESSAGE(1, countOf(src, "deriveBudget("), "a second budget derivation in orchestrator.cpp");
  TEST_ASSERT_EQUAL_MESSAGE(1, countOf(src, "resolvedOrchHost()"), "a size sited off the store's head guess");
  const size_t helper = src.find("ContextBudget headBudget()");
  TEST_ASSERT_TRUE(helper != std::string::npos);
  const size_t body = src.find("\n}\n", helper);
  const std::string fn = src.substr(helper, body - helper);
  TEST_ASSERT_TRUE(fn.find("g_engine->headHost()") != std::string::npos);
  TEST_ASSERT_TRUE(fn.find("resolvedOrchHost()") != std::string::npos);
  TEST_ASSERT_TRUE(fn.find("deriveBudget(") != std::string::npos);
  for (const char* use : {"headBudget().briefBytes", "headBudget().recentConvBytes", "headBudget().foldSliceBytes"})
    TEST_ASSERT_EQUAL_MESSAGE(1, countOf(src, use), use);
}

// ---- the fold input is ONE provider string ------------------------------------
// The fold sends summary + digest as one user string; at the 200K anchor the
// digest budget alone (65536) passed ArduinoJson's 65,535-byte ceiling, which
// stores the string as null (a 400). foldDigestCap bounds the digest so the whole
// input stays under the guard for every summary size the store can hold.
static void test_fold_input_fits_the_string_guard() {
  for (size_t sum : {(size_t)0, (size_t)1, (size_t)2048, kChatSummaryMax}) {
    const std::string summary(sum, 's');
    const ContextBudget b = deriveBudget(1000000, {});   // the largest slice there is
    const size_t cap = std::min(b.foldSliceBytes, foldDigestCap(summary.size()));
    const std::string in = buildCompactInputs(summary, std::string(cap, 'd'));
    TEST_ASSERT_TRUE(in.size() <= kJsonStrGuard);
  }
  // Counter-test: the unbounded anchor slice next to a full summary is over.
  const ContextBudget anchor = deriveBudget(200000, {});
  TEST_ASSERT_TRUE(buildCompactInputs(std::string(kChatSummaryMax, 's'),
                                      std::string(anchor.foldSliceBytes, 'd')).size() > kJsonStringCeiling);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)foldDigestCap(kJsonStrGuard));
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
  RUN_TEST(test_orchestrator_sizes_off_one_head_budget);
  RUN_TEST(test_fold_input_fits_the_string_guard);
  return UNITY_END();
}
