#include <unity.h>

#include <string>

#include "../support/recorded_models.h"
#include "nimbus/orch/model_defaults.h"
#include "nimbus/orch/model_window.h"

// test_model_window - the per-model context window table (model_window.h). These
// assertions moved here verbatim from test_orch_compact (lane P0 of the history fix,
// 16 Nov 2026), so the window's owner changes them without touching the fold's suite.

using namespace nimbus::orch;

void setUp() {}
// A failed assertion leaves its test early: never let its listed lookup leak into the next.
void tearDown() { installListedCtxLookup(nullptr); }

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


// ---- the head's real window (lane B of the history fix) -----------------------
// The class: with the device's listed lookup installed over the recorded provider lists,
// every recorded model's window is its listed window where a list says one, else the
// corrected table; and the table never exceeds what any recorded list says (so a fresh
// device, before its first list refresh, never assumes a bigger window than the
// provider's).
static void test_head_window_is_listed_else_table_for_every_recorded_model() {
  const auto& models = recorded::models();
  TEST_ASSERT_TRUE(models.size() > 200);
  installListedCtxLookup(&recorded::listedLookup);
  size_t listed = 0;
  for (const recorded::Model& m : models) {
    const std::string what = m.provider + "/" + m.id;
    const uint32_t l = recorded::listedLookup(m.provider.c_str(), m.id.c_str());
    const uint32_t table = tableWindowTokens(m.provider, m.id);
    TEST_ASSERT_TRUE_MESSAGE(table > 0, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(l ? l : table, headWindowTokens(m.provider, m.id), what.c_str());
    for (bool october : {false, true}) {
      const uint32_t said = recorded::listedIn(m.provider, m.id, october);
      if (said) TEST_ASSERT_TRUE_MESSAGE(table <= said, what.c_str());
    }
    listed += l ? 1 : 0;
  }
  TEST_ASSERT_TRUE_MESSAGE(listed >= 60, std::to_string(listed).c_str());   // the Mistral and Anthropic rows say it
  installListedCtxLookup(nullptr);
}

// The table's rows, one per family (plan v3 lane B), and OpenAI unchanged for every
// recorded id.
static void test_head_window_table_rows() {
  struct Row { const char* host; const char* model; uint32_t window; };
  static const Row kRows[] = {
      {"anthropic", "claude-sonnet-5-5", 1000000},       {"anthropic", "claude-haiku-5-5", 1000000},
      {"anthropic", "claude-opus-5", 1000000},           {"anthropic", "claude-mythos-5-1", 1000000},
      {"anthropic", "claude-sonnet-4-6", 1000000},       {"anthropic", "claude-opus-4-8", 1000000},
      {"anthropic", "claude-opus-4-5-20251101", 200000}, {"anthropic", "claude-haiku-4-5", 200000},
      {"anthropic", "claude-3-5-haiku-latest", 200000},  {"anthropic", "claude-quill", 200000},
      {"mistral", "mistral-large-4-0", 1048576},         {"mistral", "mistral-large-4", 1048576},
      {"mistral", "mistral-large-2512", 262144},         {"mistral", "mistral-large-latest", 262144},
      {"mistral", "mistral-large-2411", 128000},         {"mistral", "mistral-medium-latest", 262144},
      {"mistral", "mistral-small-latest", 262144},       {"mistral", "magistral-medium-latest", 262144},
      {"mistral", "ministral-8b-latest", 262144},        {"mistral", "ministral-14b-2512", 262144},
      {"mistral", "ministral-3b-latest", 131072},        {"mistral", "devstral-medium-latest", 262144},
      {"mistral", "mistral-vibe-cli-latest", 262144},    {"mistral", "labs-leanstral-1-5", 262144},
      {"mistral", "codestral-latest", 256000},           {"mistral", "voxtral-small-latest", 32768},
      {"mistral", "voxtral-mini-latest", 16384},         {"mistral", "voxtral-mini-tts-latest", 4096},
      {"mistral", "mistral-ocr-latest", 16384},          {"mistral", "mistral-embed", 8192},
      {"mistral", "zai-glm-5-3", 917504},                {"mistral", "glm-5-2", 917504},
      {"zai", "glm-5.3", 917504},                        {"zai", "glm-5", 917504},
      {"zai", "glm-5.3-flash", 917504},                  {"zai", "glm-4.6", 128000},
      {"zai", "glm-4.5v", 128000},                       {"zai", "glm-4.6v", 128000},
      {"cumulo", "zai/glm-5.3", 917504},                 {"cumulo", "anthropic/claude-sonnet-5-5", 1000000},
      {"cumulo", "mistral/mistral-large-4-0", 1048576},  {"cumulo", "openai/gpt-5.6", 272000},
      {"custom", "gpt-5.6-luna", 272000},                {"custom", "meta-llama/llama-3-70b", kCtxDefaultTokens},
  };
  for (const Row& r : kRows) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.window, tableWindowTokens(r.host, r.model), r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.window, headWindowTokens(r.host, r.model), r.model);   // nothing listed
  }
  for (const recorded::Model& m : recorded::models())
    if (m.provider == "openai")
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(legacyCtxTokens("openai", m.id), tableWindowTokens("openai", m.id), m.id.c_str());
}

// Counter-test: Lumi's head is 1,048,576 tokens here and 128,000 in the legacy table,
// which stays v5.1.1's until lane H switches modelCtxTokens.
static void test_large4_window_here_and_legacy() {
  TEST_ASSERT_EQUAL_UINT32(1048576, headWindowTokens("mistral", "mistral-large-4-0"));
  TEST_ASSERT_EQUAL_UINT32(128000, modelCtxTokens("mistral", "mistral-large-4-0"));
  TEST_ASSERT_EQUAL_UINT32(128000, legacyCtxTokens("mistral", "mistral-large-4-0"));
  for (const recorded::Model& m : recorded::models())
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(legacyCtxTokens(m.provider, m.id), modelCtxTokens(m.provider, m.id), m.id.c_str());
}

// Fresh device (no model list fetched yet): every default head model, Lumi's
// mistral-large-4-0 and the ruled Anthropic head claude-sonnet-5-5 get from the table
// exactly what the provider's list says once it is fetched.
static void test_fresh_device_table_equals_listed_for_default_heads() {
  std::vector<recorded::Model> heads = {{"mistral", "mistral-large-4-0"}, {"anthropic", "claude-sonnet-5-5"}};
  for (const RoleDefault& r : kRoleDefaults) heads.push_back({r.slot, r.fallback});
  size_t checked = 0;
  for (const recorded::Model& h : heads) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(tableWindowTokens(h.provider, h.id), headWindowTokens(h.provider, h.id),
                                     h.id.c_str());   // no lookup installed
    const uint32_t listed = recorded::listedLookup(h.provider.c_str(), h.id.c_str());
    if (!listed) continue;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(listed, tableWindowTokens(h.provider, h.id), h.id.c_str());
    checked++;
  }
  TEST_ASSERT_TRUE(checked >= 6);
}

// The router class for the new table: a selector reads its upstream's window, listed or
// table, and a bare id on the router reads the default upstream's.
static void test_head_window_router_selector_class() {
  installListedCtxLookup(&recorded::listedLookup);
  struct Pick { const char* up; const char* id; };
  static const Pick kPicks[] = {
      {"openai", "gpt-5.6"},           {"openai", "gpt-6-astra"},          {"anthropic", "claude-sonnet-5-5"},
      {"anthropic", "claude-haiku-4-5"}, {"mistral", "mistral-large-4-0"}, {"mistral", "mistral-medium-latest"},
      {"zai", "glm-5.3"},              {"zai", "glm-4.6v"},
  };
  for (const Pick& k : kPicks) {
    const std::string sel = std::string(k.up) + "/" + k.id;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(headWindowTokens(k.up, k.id), headWindowTokens("cumulo", sel), sel.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(headWindowTokens(k.up, k.id), headWindowTokens(k.up, sel), sel.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(headWindowTokens("openai", k.id), headWindowTokens("cumulo", k.id), k.id);
  }
  installListedCtxLookup(nullptr);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ctx_table_families);
  RUN_TEST(test_ctx_table_every_gpt_generation_is_known);
  RUN_TEST(test_ctx_table_unknown_is_conservative);
  RUN_TEST(test_ctx_router_selectors_resolve_to_the_upstream_window);
  RUN_TEST(test_ctx_router_selector_class);
  RUN_TEST(test_head_window_is_listed_else_table_for_every_recorded_model);
  RUN_TEST(test_head_window_table_rows);
  RUN_TEST(test_large4_window_here_and_legacy);
  RUN_TEST(test_fresh_device_table_equals_listed_for_default_heads);
  RUN_TEST(test_head_window_router_selector_class);
  return UNITY_END();
}
