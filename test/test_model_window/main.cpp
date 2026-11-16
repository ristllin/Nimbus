#include <unity.h>

#include <string>

#include "nimbus/orch/model_window.h"

// test_model_window - the per-model context window table (model_window.h). These
// assertions moved here verbatim from test_orch_compact (lane P0 of the history fix,
// 16 Nov 2026), so the window's owner changes them without touching the fold's suite.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- model context table -----------------------------------------------------
static void test_ctx_table_families() {
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("anthropic", "claude-sonnet-4-6"));
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("anthropic", "claude-sonnet-4-7"));  // unseen snapshot, family match
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("openai", "gpt-5.5"));
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("openai", "gpt-5.4-mini"));
  // gpt-6-astra (2026-09): 1,050,000 published context minus the 128,000 max
  // output = the 922K INPUT window LiteLLM reports. Later generations inherit it.
  TEST_ASSERT_EQUAL_UINT32(922000, modelCtxTokens("openai", "gpt-6-astra"));
  TEST_ASSERT_EQUAL_UINT32(922000, modelCtxTokens("openai", "gpt-7-x"));
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("openai", "o4-mini-deep-research"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("mistral", "mistral-large-latest"));
  // CUM-288 class rule: whatever model the Cumulo router head resolves to must
  // get its real family window, never the conservative 100K default a "" model
  // or an unknown family would produce (which compacted the router head early).
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "gpt-4o"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "gpt-4o-mini"));
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("cumulo", "gpt-5.6"));  // the 2026-09 default
}

// Class rule: no gpt-5-or-newer generation may fall through to the conservative
// default. gpt-6-astra did (it was keyed on the literal "gpt-5" prefix), which
// would have compacted a 922K-window model at 100K.
static void test_ctx_table_every_gpt_generation_is_known() {
  for (int gen = 5; gen <= 9; ++gen) {
    const std::string id = "gpt-" + std::to_string(gen) + "-x";
    TEST_ASSERT_TRUE_MESSAGE(modelCtxTokens("openai", id) >= 272000, id.c_str());
  }
}

// CUM-530: the Cumulo picker stores every pick as a router selector
// ("<upstream>/<id>"; re-picking the default stores "openai/gpt-5.6"). The window
// lookup resolves it through the ONE router rule (resolveRouterRoute) and reads
// the upstream's window for the bare id: the prefixed default fell to the 100K
// default, a 2.7x smaller recent window and compaction at a third of the window.
static void test_ctx_router_selectors_resolve_to_the_upstream_window() {
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("cumulo", "openai/gpt-5.6"));
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("cumulo", "anthropic/claude-opus-5"));
  TEST_ASSERT_EQUAL_UINT32(922000, modelCtxTokens("cumulo", "openai/gpt-6-astra"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "mistral/mistral-large-latest"));
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("cumulo", "OpenAI/GPT-5.6"));   // case-insensitive
  // The bare spelling keeps the router's default upstream (openai): unchanged.
  TEST_ASSERT_EQUAL_UINT32(272000, modelCtxTokens("cumulo", "gpt-5.6"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("cumulo", "gpt-4o"));
  // Another provider's id is split only when its prefix is a router upstream; a
  // LAN server's namespaced id keeps the conservative default (never a bigger
  // family window: "mistralai/Mistral-7B" is a 32K model, not mistral-* 128K).
  TEST_ASSERT_EQUAL_UINT32(200000, modelCtxTokens("custom", "anthropic/claude-sonnet-5"));
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "meta-llama/llama-3-70b"));
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens,
                           modelCtxTokens("custom", "mistralai/Mistral-7B-Instruct-v0.3"));
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "TheBloke/Mistral-7B-GGUF"));
  // A leading '/' is not an upstream marker (the router rule's own semantics).
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "/gpt-5.6"));
}

// The class: whatever (upstream, model) the router serves, the selector's window
// equals the direct upstream's window for the bare id, and a bare id on the router
// equals the default upstream's. A new family added to the table is covered here
// with no per-selector edit.
static void test_ctx_router_selector_class() {
  struct Pick { const char* up; const char* id; };
  static const Pick kPicks[] = {
      {"openai", "gpt-5.6"},          {"openai", "gpt-5.6-terra"},   {"openai", "gpt-6-astra"},
      {"openai", "gpt-4o-mini"},      {"openai", "o4-mini"},         {"anthropic", "claude-opus-5"},
      {"anthropic", "claude-haiku-4-5"}, {"mistral", "mistral-large-latest"},
      {"mistral", "mistral-small-2"}, {"zai", "glm-5.3"},            {"zai", "glm-4.5-flash"},
  };
  for (const Pick& k : kPicks) {
    const std::string sel = std::string(k.up) + "/" + k.id;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(modelCtxTokens(k.up, k.id), modelCtxTokens("cumulo", sel),
                                     sel.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(modelCtxTokens("openai", k.id), modelCtxTokens("cumulo", k.id),
                                     k.id);
  }
}

static void test_ctx_table_unknown_is_conservative() {
  // Unknown family -> the conservative default (compacts EARLY, never late).
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "llama-3-70b"));
  // "openai"/"omni"-style names must not match the o-series rule.
  TEST_ASSERT_EQUAL_UINT32(kCtxDefaultTokens, modelCtxTokens("custom", "omni-chat"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ctx_table_families);
  RUN_TEST(test_ctx_table_every_gpt_generation_is_known);
  RUN_TEST(test_ctx_table_unknown_is_conservative);
  RUN_TEST(test_ctx_router_selectors_resolve_to_the_upstream_window);
  RUN_TEST(test_ctx_router_selector_class);
  return UNITY_END();
}
