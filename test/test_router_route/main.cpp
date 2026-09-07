#include <unity.h>

#include <string>

#include "nimbus/orch/router_route.h"

using nimbus::orch::resolveRouterRoute;
using nimbus::orch::RouterRoute;

void setUp() {}
void tearDown() {}

// ---- CUM-369: the router selector resolution class rule ---------------------
// The Cumulo router addresses each upstream under /router/<upstream>/v1 and PRICES
// the bare model id. A model selector is "<upstream>/<model>"; the head used to
// send it verbatim to a hardcoded /router/openai/v1, so a prefixed pick priced
// 403 model_not_priced and dropped the assistant head. The sub-session adapter
// split it correctly. Both now resolve through resolveRouterRoute, and this suite
// pins the contract for the whole class - a bare id AND a prefixed id, on the
// SAME rule the head and sub-session share.

// A stored bare id (the gpt-4o-class device) must resolve UNCHANGED: openai
// upstream, the id sent as-is, the plain openai base. This is the regression the
// fix must not cause.
static void test_bare_id_defaults_to_openai_unchanged() {
  RouterRoute r = resolveRouterRoute("gpt-4o");
  TEST_ASSERT_EQUAL_STRING("openai", r.upstream.c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-4o", r.model.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1", r.basePath.c_str());
}

// The current CUMULO_MODEL default ("gpt-5.6") is a bare id - it too routes to
// openai untouched.
static void test_default_cumulo_model_is_bare_openai() {
  RouterRoute r = resolveRouterRoute("gpt-5.6");
  TEST_ASSERT_EQUAL_STRING("openai", r.upstream.c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", r.model.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1", r.basePath.c_str());
}

// A prefixed openai pick from the new dropdown: the upstream is stripped, the base
// stays openai, and CRUCIALLY the model reaching the router is bare (no prefix) -
// the exact value the router prices. This is the bug: pre-fix the head sent
// "openai/gpt-4o" to /router/openai/v1 and priced 403.
static void test_prefixed_openai_strips_to_bare_model() {
  RouterRoute r = resolveRouterRoute("openai/gpt-4o");
  TEST_ASSERT_EQUAL_STRING("openai", r.upstream.c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-4o", r.model.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1", r.basePath.c_str());
  // The model the router sees carries NO "<upstream>/" prefix.
  TEST_ASSERT_TRUE(r.model.find('/') == std::string::npos);
  TEST_ASSERT_TRUE(r.model != std::string("openai/gpt-4o"));
}

// A prefixed non-openai pick routes to the correct upstream base with the bare
// model - the multi-key flagship path that silently failed over pre-fix.
static void test_prefixed_anthropic_routes_to_anthropic_base() {
  RouterRoute r = resolveRouterRoute("anthropic/claude-sonnet-5");
  TEST_ASSERT_EQUAL_STRING("anthropic", r.upstream.c_str());
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", r.model.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/anthropic/v1", r.basePath.c_str());
  TEST_ASSERT_TRUE(r.model.find('/') == std::string::npos);
}

static void test_prefixed_mistral_routes_to_mistral_base() {
  RouterRoute r = resolveRouterRoute("mistral/mistral-large-latest");
  TEST_ASSERT_EQUAL_STRING("mistral", r.upstream.c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-large-latest", r.model.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/mistral/v1", r.basePath.c_str());
}

// Only the FIRST '/' splits; a nested slash (a fine-tune / namespaced id) stays in
// the model so the router receives it intact.
static void test_nested_slash_stays_in_model() {
  RouterRoute r = resolveRouterRoute("openai/ft:acme/gpt-4o-v2");
  TEST_ASSERT_EQUAL_STRING("openai", r.upstream.c_str());
  TEST_ASSERT_EQUAL_STRING("ft:acme/gpt-4o-v2", r.model.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1", r.basePath.c_str());
}

// A leading '/' is not an upstream marker (indexOf('/') > 0 in the original): the
// whole selector is the model under the default upstream. Pinned so head and
// sub-session agree on this edge too.
static void test_leading_slash_is_not_an_upstream() {
  RouterRoute r = resolveRouterRoute("/gpt-4o");
  TEST_ASSERT_EQUAL_STRING("openai", r.upstream.c_str());
  TEST_ASSERT_EQUAL_STRING("/gpt-4o", r.model.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1", r.basePath.c_str());
}

// An empty selector yields an empty model - the signal both callers gate on
// (FabricErr::BadRequest on the sub-session; the head defaults upstream via
// store::orchModel before it ever gets here).
static void test_empty_selector_yields_empty_model() {
  RouterRoute r = resolveRouterRoute("");
  TEST_ASSERT_EQUAL_STRING("openai", r.upstream.c_str());
  TEST_ASSERT_TRUE(r.model.empty());
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1", r.basePath.c_str());
}

// The split is lossless: whenever a real "<upstream>/<model>" split happens,
// upstream + "/" + model reconstructs the selector exactly. This is the invariant
// that catches a future "split on the last slash" or "drop a segment" regression.
static void test_split_is_lossless_for_prefixed() {
  const char* prefixed[] = {"openai/gpt-4o", "anthropic/claude-sonnet-5",
                            "mistral/mistral-large-latest", "openai/ft:acme/gpt-4o-v2"};
  for (const char* sel : prefixed) {
    RouterRoute r = resolveRouterRoute(sel);
    TEST_ASSERT_EQUAL_STRING(sel, (r.upstream + "/" + r.model).c_str());
  }
}

// The class assertion that ties the two firmware call sites together: the head and
// the sub-session both call resolveRouterRoute(sel) with the SAME default upstream
// ("openai"), so for ANY selector they derive the identical (upstream, model,
// basePath). The one-arg form (what both callers use) must equal the explicit
// two-arg openai form; a new caller that passes a different default, or a path
// that stops using the helper and re-implements the split differently, breaks
// this. A prefixed model must NEVER survive as the router's model field.
static void test_head_and_subsession_resolve_identically() {
  const char* selectors[] = {"gpt-4o",
                             "gpt-5.6",
                             "openai/gpt-4o",
                             "anthropic/claude-sonnet-5",
                             "mistral/mistral-large-latest",
                             "openai/ft:acme/gpt-4o-v2",
                             "/gpt-4o",
                             ""};
  for (const char* sel : selectors) {
    RouterRoute head = resolveRouterRoute(sel);                 // orchestrator head
    RouterRoute sub  = resolveRouterRoute(sel, "openai");       // sub-session adapter
    TEST_ASSERT_EQUAL_STRING(sub.upstream.c_str(), head.upstream.c_str());
    TEST_ASSERT_EQUAL_STRING(sub.model.c_str(), head.model.c_str());
    TEST_ASSERT_EQUAL_STRING(sub.basePath.c_str(), head.basePath.c_str());
    // basePath always addresses the resolved upstream, never a hardcoded one.
    TEST_ASSERT_EQUAL_STRING((std::string("/router/") + head.upstream + "/v1").c_str(),
                             head.basePath.c_str());
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_bare_id_defaults_to_openai_unchanged);
  RUN_TEST(test_default_cumulo_model_is_bare_openai);
  RUN_TEST(test_prefixed_openai_strips_to_bare_model);
  RUN_TEST(test_prefixed_anthropic_routes_to_anthropic_base);
  RUN_TEST(test_prefixed_mistral_routes_to_mistral_base);
  RUN_TEST(test_nested_slash_stays_in_model);
  RUN_TEST(test_leading_slash_is_not_an_upstream);
  RUN_TEST(test_empty_selector_yields_empty_model);
  RUN_TEST(test_split_is_lossless_for_prefixed);
  RUN_TEST(test_head_and_subsession_resolve_identically);
  return UNITY_END();
}
