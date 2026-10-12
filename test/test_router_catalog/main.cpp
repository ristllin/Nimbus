#include <unity.h>

#include <chrono>
#include <string>
#include <vector>

// CUM-530: the Cumulo router model list on the device - the verify harvest's
// choice list, the catalog size trim, and the model save rule. Pure headers (the
// device seams in provider_verify.cpp / webui.cpp only call them).
#include "../../src/agent/model_pick.h"
#include "../../src/agent/router_catalog.h"
#include "nimbus/orch/model_catalog.h"

namespace rc = agent::router_catalog;
namespace mp = agent::model_pick;
using nimbus::orch::ModelInfo;

void setUp() {}
void tearDown() {}

// The router's real order (cumulo-nimbus routableCatalog: size class, then
// upstream name, then model): every large anthropic row sorts before the first
// openai one. The bench device kept exactly these first 8 opus ids.
static std::vector<std::string> benchRouterList() {
  return {
      "anthropic/claude-opus-4-5",  "anthropic/claude-opus-4-5-20251101",
      "anthropic/claude-opus-4-6",  "anthropic/claude-opus-4-6-20260205",
      "anthropic/claude-opus-4-7",  "anthropic/claude-opus-4-7-20260416",
      "anthropic/claude-opus-4-8",  "anthropic/claude-opus-4.1",
      "anthropic/claude-opus-5",    "mistral/mistral-large",
      "openai/gpt-5.5",             "openai/gpt-5.6",
      "openai/gpt-5.6-sol",         "openai/gpt-6-astra",
      "zai/glm-5",                  "zai/glm-5.3",
      "anthropic/claude-sonnet-5",  "mistral/mistral-small",
      "openai/gpt-5.6-terra",       "zai/glm-5.2",
      "anthropic/claude-haiku-4-5", "openai/gpt-5.6-luna",
      "zai/glm-5.3-flash",
  };
}

static bool csvHas(const std::string& csv, const char* id) {
  return nimbus::orch::modelInChoices(csv, id);
}

// The bench bug, as a test: the choice list keeps the router default and every
// upstream, not 8 opus rows.
static void test_choice_list_keeps_default_and_each_upstream() {
  const rc::Shortlist sl = rc::shortlist(benchRouterList(), "gpt-5.6");
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", sl.csv.substr(0, sl.csv.find(',')).c_str());
  for (const char* up : {"anthropic/", "openai/", "mistral/", "zai/"})
    TEST_ASSERT_TRUE_MESSAGE(sl.csv.find(up) != std::string::npos, up);
  TEST_ASSERT_TRUE(csvHas(sl.csv, "anthropic/claude-opus-4-8"));
  // prism (peer review): each upstream newest-first - the router sorts oldest
  // generation first within a size class, and a cut list must keep the newest.
  for (const char* id : {"anthropic/claude-opus-5", "anthropic/claude-sonnet-5",
                         "openai/gpt-6-astra", "zai/glm-5.3"})
    TEST_ASSERT_TRUE_MESSAGE(csvHas(sl.csv, id), id);
  TEST_ASSERT_FALSE(csvHas(sl.csv, "anthropic/claude-opus-4.1"));
  TEST_ASSERT_TRUE(sl.kept <= rc::kChoicesMax);
  TEST_ASSERT_TRUE(sl.csv.size() <= rc::kChoicesMaxBytes);
  // 23 router ids, 3 dated twins -> 20 distinct models; 12 kept: the cut is said.
  TEST_ASSERT_EQUAL_UINT(20, sl.total);
  TEST_ASSERT_TRUE(sl.truncated());
}

// The class: for EVERY upstream that appears anywhere in the list - even at the
// very end - its first model is kept, whatever the cap (down to default + one
// per upstream).
static void test_every_upstream_survives_any_cap() {
  std::vector<std::string> ids = benchRouterList();
  ids.push_back("groq/llama-5-large");   // a new upstream, listed last
  for (size_t cap = 6; cap <= rc::kChoicesMax; ++cap) {
    const rc::Shortlist sl = rc::shortlist(ids, "gpt-5.6", cap, rc::kChoicesMaxBytes);
    for (const char* up : {"anthropic/", "openai/", "mistral/", "zai/", "groq/"})
      TEST_ASSERT_TRUE_MESSAGE(sl.csv.find(up) != std::string::npos, up);
    TEST_ASSERT_TRUE(csvHas(sl.csv, "openai/gpt-5.6"));
  }
}

// The class: whatever the cap, each upstream's NEWEST version (by the numbers in
// its id) is the one kept when only one fits.
static void test_each_upstream_keeps_its_newest_model() {
  const rc::Shortlist sl = rc::shortlist(benchRouterList(), "gpt-5.6", 5, rc::kChoicesMaxBytes);
  TEST_ASSERT_EQUAL_STRING(
      "openai/gpt-5.6,anthropic/claude-opus-5,mistral/mistral-large,zai/glm-5.3,"
      "anthropic/claude-sonnet-5",
      sl.csv.c_str());
  // A parameter size or a date stamp is not a version.
  const rc::Shortlist m = rc::shortlist({"mistral/mistral-large", "mistral/pixtral-12b",
                                         "mistral/codestral-2501"}, "", 1, rc::kChoicesMaxBytes);
  TEST_ASSERT_EQUAL_STRING("mistral/mistral-large", m.csv.c_str());
  // A date stamp is never a version: a dated-only gpt-4o snapshot does not
  // outrank gpt-4.1 by reading its month and day as numbers.
  const rc::Shortlist d = rc::shortlist({"openai/gpt-4o-2024-08-06", "openai/gpt-4.1"}, "", 1,
                                        rc::kChoicesMaxBytes);
  TEST_ASSERT_EQUAL_STRING("openai/gpt-4.1", d.csv.c_str());
}

// The harvest filter (moved out of provider_verify.cpp so it is tested): a canned
// /router/models body with HTTP headers, pretty-printed: non-chat families and the
// openai upstream's dated snapshots are dropped; flagship families come first.
static void test_harvest_filters_a_router_body() {
  const char* body =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
      "{\"object\": \"list\", \"data\": ["
      "{\"id\": \"anthropic/claude-3-5-haiku\", \"size_class\": \"small\"},"
      "{\"id\": \"anthropic/claude-opus-5\", \"size_class\": \"large\"},"
      "{\"id\": \"openai/gpt-4o\"}, {\"id\": \"openai/gpt-5.6\"},"
      "{\"id\": \"openai/gpt-4.1-2025-04-14\"}, {\"id\": \"openai/text-embedding-3-small\"},"
      "{\"id\": \"openai/o4-mini-deep-research\"}, {\"id\": \"openai/whisper-1\"},"
      "{\"id\": \"mistral/mistral-embed\"}, {\"id\": \"zai/glm-4.5\"}, {\"id\": \"zai/glm-5.3\"}]}";
  const std::vector<std::string> ids = rc::harvestIds(body);
  const std::vector<std::string> want = {"anthropic/claude-opus-5", "openai/gpt-5.6", "zai/glm-5.3",
                                         "anthropic/claude-3-5-haiku", "openai/gpt-4o", "zai/glm-4.5"};
  TEST_ASSERT_EQUAL(want.size(), ids.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), ids[i].c_str());
  TEST_ASSERT_EQUAL(0, (int)rc::harvestIds("not a list").size());
}

// The router body is untrusted: a list of thousands of ids must not cost
// seconds of CPU and hundreds of KB of transient heap in the verify slot. The
// harvest keeps at most 128 chat ids (kHarvestMax), flagships first even when
// they come last, and the choice list built from it is still bounded and keeps
// the default and every upstream (CUM-530).
static void test_harvest_is_bounded_on_a_huge_body() {
  std::string body = "{\"data\":[";
  for (int i = 0; i < 5000; ++i) body += "{\"id\":\"zai/m" + std::to_string(i) + "\"},";
  body += "{\"id\":\"anthropic/claude-opus-5\"},{\"id\":\"openai/gpt-5.6\"},"
          "{\"id\":\"zai/glm-5.3\"}]}";
  static std::vector<std::string> logLines;
  logLines.clear();
  agent::hlog::setSink(+[](const char* l) { logLines.push_back(l); });
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<std::string> ids = rc::harvestIds(body.c_str());
  const rc::Shortlist sl = rc::shortlist(ids, "gpt-5.6");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
  TEST_ASSERT_EQUAL_UINT(128, ids.size());
  // The flagships listed after 5000 rows still lead the harvest.
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5", ids[0].c_str());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", ids[1].c_str());
  TEST_ASSERT_EQUAL_STRING("zai/glm-5.3", ids[2].c_str());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", sl.csv.substr(0, sl.csv.find(',')).c_str());
  for (const char* up : {"anthropic/", "zai/"})
    TEST_ASSERT_TRUE_MESSAGE(sl.csv.find(up) != std::string::npos, up);
  TEST_ASSERT_TRUE(sl.kept <= rc::kChoicesMax);
  TEST_ASSERT_TRUE(sl.csv.size() <= rc::kChoicesMaxBytes);
  TEST_ASSERT_TRUE_MESSAGE(ms < 2000, "harvest + shortlist of a 5000-id body took over 2 s");
  // One log line says the bound cut the list.
  TEST_ASSERT_EQUAL(1, (int)logLines.size());
  TEST_ASSERT_TRUE(logLines[0].find("harvest keeps 128 of 5003 router chat ids") != std::string::npos);
  // A short body is untouched by the bound, and logs nothing.
  logLines.clear();
  TEST_ASSERT_EQUAL_UINT(3, rc::harvestIds("{\"id\":\"zai/a1\"},{\"id\":\"zai/a2\"},"
                                           "{\"id\":\"openai/gpt-5.6\"}").size());
  TEST_ASSERT_EQUAL(0, (int)logLines.size());
  agent::hlog::setSink(nullptr);
}

// The candidate dedupe (sorted indexes + binary search for a dated twin's alias)
// keeps exactly what the plain pairwise rule keeps, in the same order: the first
// copy of each id, minus empty ids and dated twins of a listed alias. The class,
// over generated lists mixing duplicates, twins, dated-only ids and empty ids.
static std::vector<size_t> pairwiseCandidates(const std::vector<std::string>& canon) {
  std::vector<size_t> cand;
  std::vector<std::string> kept;
  auto has = [](const std::vector<std::string>& v, const std::string& x) {
    for (const std::string& e : v)
      if (e == x) return true;
    return false;
  };
  for (size_t i = 0; i < canon.size(); ++i) {
    if (canon[i].empty() || has(kept, canon[i])) continue;
    const std::string alias = rc::undatedAlias(canon[i]);
    if (!alias.empty() && has(canon, alias)) continue;
    kept.push_back(canon[i]);
    cand.push_back(i);
  }
  return cand;
}
static void test_candidates_match_the_pairwise_rule() {
  const char* const kPool[] = {"zai/glm-5.3", "zai/glm-5.3", "", "anthropic/claude-opus-4-5",
                               "anthropic/claude-opus-4-5-20251101",
                               "anthropic/claude-haiku-4-5-20251001", "openai/gpt-4.1",
                               "openai/gpt-4.1-2025-04-14", "openai/gpt-4.1-2025-04-14",
                               "mistral/mistral-large", "openai/gpt-5.6"};
  const size_t n = sizeof(kPool) / sizeof(kPool[0]);
  uint32_t seed = 12345;
  for (int round = 0; round < 200; ++round) {
    std::vector<std::string> canon;
    const size_t len = 1 + (size_t)(round % 40);
    for (size_t k = 0; k < len; ++k) {
      seed = seed * 1103515245u + 12345u;
      canon.push_back(kPool[(seed >> 16) % n]);
    }
    const std::vector<size_t> want = pairwiseCandidates(canon);
    const std::vector<size_t> got = rc::detail::candidates(canon);
    TEST_ASSERT_EQUAL_UINT(want.size(), got.size());
    for (size_t k = 0; k < want.size(); ++k) TEST_ASSERT_EQUAL_UINT(want[k], got[k]);
  }
  // And at size: 5002 ids with duplicates and a dated twin.
  std::vector<std::string> ids;
  for (int i = 0; i < 2500; ++i) {
    ids.push_back("zai/m" + std::to_string(i));
    ids.push_back("zai/m" + std::to_string(i));   // an exact duplicate
  }
  ids.push_back("anthropic/claude-opus-4-5");
  ids.push_back("anthropic/claude-opus-4-5-20251101");   // a dated twin of a listed alias
  const std::vector<size_t> order = rc::keepOrder(ids, "gpt-5.6");
  TEST_ASSERT_EQUAL_UINT(2501, order.size());
  for (size_t i : order) TEST_ASSERT_TRUE(ids[i] != "anthropic/claude-opus-4-5-20251101");
}

// Dated snapshots duplicating an undated alias are dropped; a dated-only id
// (no alias listed) is kept.
static void test_dated_twins_dropped_dated_only_kept() {
  const rc::Shortlist sl = rc::shortlist(
      {"anthropic/claude-opus-4-5", "anthropic/claude-opus-4-5-20251101",
       "anthropic/claude-haiku-4-5-20251001", "openai/gpt-4.1-2025-04-14", "openai/gpt-4.1"},
      "gpt-5.6");
  TEST_ASSERT_FALSE(csvHas(sl.csv, "anthropic/claude-opus-4-5-20251101"));
  TEST_ASSERT_FALSE(csvHas(sl.csv, "openai/gpt-4.1-2025-04-14"));
  TEST_ASSERT_TRUE(csvHas(sl.csv, "anthropic/claude-haiku-4-5-20251001"));
  TEST_ASSERT_TRUE(csvHas(sl.csv, "openai/gpt-4.1"));
  TEST_ASSERT_EQUAL_UINT(3, sl.total);
  TEST_ASSERT_FALSE(sl.truncated());
}

// A short list is kept whole and is not called truncated; a list without the
// router default simply starts with the first upstream.
static void test_short_list_is_whole() {
  const rc::Shortlist sl = rc::shortlist({"openai/gpt-5.6", "zai/glm-5.3"}, "gpt-5.6");
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6,zai/glm-5.3", sl.csv.c_str());
  TEST_ASSERT_FALSE(sl.truncated());
  const rc::Shortlist nd = rc::shortlist({"zai/glm-5.3", "anthropic/claude-opus-5"}, "gpt-5.6");
  TEST_ASSERT_EQUAL_STRING("zai/glm-5.3,anthropic/claude-opus-5", nd.csv.c_str());
}

static ModelInfo entry(const char* up, const char* id) {
  ModelInfo m = nimbus::orch::classifyCatalogEntry("cumulo", std::string(up) + "/" + id);
  return m;
}

// The catalog trim: protectHeads orders the router default, then one model per
// upstream, ahead of everything else - so popping from the back for the size bound
// never drops them - and it drops dated twins. Selectors are "<upstream>/<id>"
// (the catalog stores the bare id + upstream).
static void test_catalog_protects_default_and_upstream_heads() {
  std::vector<ModelInfo> cat;
  for (const std::string& sel : benchRouterList()) {
    const size_t sl = sel.find('/');
    cat.push_back(entry(sel.substr(0, sl).c_str(), sel.substr(sl + 1).c_str()));
  }
  rc::protectHeads(cat, "gpt-5.6");
  TEST_ASSERT_EQUAL_UINT(20, cat.size());   // 3 dated twins dropped
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", rc::selectorOf(cat[0]).c_str());
  // Trim to the default + one per upstream: all four upstreams still present.
  cat.resize(4);
  std::string ups;
  for (const auto& m : cat) ups += m.upstream + ",";
  for (const char* up : {"openai,", "anthropic,", "mistral,", "zai,"})
    TEST_ASSERT_TRUE_MESSAGE(ups.find(up) != std::string::npos, up);
}

// ---- the save rule ------------------------------------------------------------

static mp::PickLists cumuloLists() {
  mp::PickLists l;
  l.choices = rc::shortlist(benchRouterList(), "gpt-5.6").csv;
  // claude-haiku-4-5 is a catalog-only row: the router lists it, the 12-id
  // choice list does not keep it (pinned below), so only the catalog admits it.
  l.catalog = {"openai/gpt-5.6", "anthropic/claude-opus-4-8", "zai/glm-5.2",
               "anthropic/claude-haiku-4-5"};
  l.defaultModel = "gpt-5.6";
  l.router = true;
  return l;
}

// The bench: orchM_cumulo=gpt-5.6 answered 200 and changed nothing. The default
// is always a valid pick, in either spelling; so are list and catalog members.
static void test_default_repicks_in_both_spellings() {
  const mp::PickLists l = cumuloLists();
  TEST_ASSERT_TRUE(mp::allowed(l, "gpt-5.6"));
  TEST_ASSERT_TRUE(mp::allowed(l, "openai/gpt-5.6"));
  TEST_ASSERT_TRUE(mp::allowed(l, ""));                          // reset to default
  TEST_ASSERT_TRUE(mp::allowed(l, "anthropic/claude-opus-4-8"));
  TEST_ASSERT_TRUE(mp::allowed(l, "zai/glm-5.2"));               // a choice-list member
}

// The catalog branch of the save rule: a grouped picker row beyond the choice
// list is a valid pick only because the catalog lists it (CUM-530). Without the
// catalog the same pick is refused, so a regression in that branch fails here.
static void test_catalog_only_pick_is_allowed_by_the_catalog() {
  mp::PickLists l = cumuloLists();
  TEST_ASSERT_FALSE(csvHas(l.choices, "anthropic/claude-haiku-4-5"));   // not in the shortlist
  TEST_ASSERT_TRUE(mp::allowed(l, "anthropic/claude-haiku-4-5"));
  l.catalog.clear();
  TEST_ASSERT_FALSE(mp::allowed(l, "anthropic/claude-haiku-4-5"));
  // Catalog only (no choice list yet): the catalog row and the default are picks.
  mp::PickLists c;
  c.catalog = {"anthropic/claude-haiku-4-5"};
  c.defaultModel = "gpt-5.6";
  c.router = true;
  TEST_ASSERT_TRUE(mp::allowed(c, "anthropic/claude-haiku-4-5"));
  TEST_ASSERT_TRUE(mp::allowed(c, "openai/gpt-5.6"));
  TEST_ASSERT_FALSE(mp::allowed(c, "anthropic/claude-opus-5"));
}

// A rejected choice: refused (never a silent no-op), and the copy names the
// allowed values, default included.
static void test_rejected_pick_names_the_allowed_values() {
  const mp::PickLists l = cumuloLists();
  TEST_ASSERT_FALSE(mp::allowed(l, "anthropic/claude-opus-9"));
  TEST_ASSERT_FALSE(mp::allowed(l, "claude-opus-4-8"));   // bare = the openai upstream: not listed
  const std::string msg = mp::rejectMessage("Cumulo Nimbus", l);
  TEST_ASSERT_TRUE(msg.rfind("That model isn't available for Cumulo Nimbus. Choose one of: ", 0) == 0);
  TEST_ASSERT_TRUE(msg.find("openai/gpt-5.6") != std::string::npos);
  TEST_ASSERT_TRUE(msg.find("anthropic/claude-opus-4-8") != std::string::npos);
  // A non-router provider compares exactly (no upstream folding) and names its
  // default even when the list omits it.
  mp::PickLists o;
  o.choices = "gpt-6-astra,gpt-5.6-terra";
  o.defaultModel = "gpt-5.6";
  TEST_ASSERT_TRUE(mp::allowed(o, "gpt-5.6"));
  TEST_ASSERT_FALSE(mp::allowed(o, "openai/gpt-5.6"));
  TEST_ASSERT_EQUAL_STRING(
      "That model isn't available for OpenAI. Choose one of: gpt-5.6, gpt-6-astra, gpt-5.6-terra.",
      mp::rejectMessage("OpenAI", o).c_str());
}

// The catalog the picker renders is the catalog the save accepts: selectors for
// the role, usable only, "<upstream>/<id>" for router rows.
static void test_catalog_selectors_follow_role_and_usable() {
  const std::string blob =
      "[{\"id\":\"gpt-5.6\",\"roles\":[\"orchestrator\",\"sub-agent\"],\"usable\":true,"
      "\"upstream\":\"openai\"},"
      "{\"id\":\"claude-opus-4-8\",\"roles\":[\"orchestrator\"],\"usable\":true,"
      "\"upstream\":\"anthropic\"},"
      "{\"id\":\"glm-5.2\",\"roles\":[\"orchestrator\"],\"usable\":false,\"upstream\":\"zai\"},"
      "{\"id\":\"text-embedding-3-small\",\"roles\":[\"embedding\"],\"usable\":true,"
      "\"upstream\":\"openai\"}]";
  const auto orch = mp::catalogSelectors(blob, "orchestrator");
  TEST_ASSERT_EQUAL(2, (int)orch.size());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", orch[0].c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-4-8", orch[1].c_str());
  const auto sub = mp::catalogSelectors(blob, "sub-agent");
  TEST_ASSERT_EQUAL(1, (int)sub.size());
  TEST_ASSERT_EQUAL(0, (int)mp::catalogSelectors("not json", "orchestrator").size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_choice_list_keeps_default_and_each_upstream);
  RUN_TEST(test_every_upstream_survives_any_cap);
  RUN_TEST(test_dated_twins_dropped_dated_only_kept);
  RUN_TEST(test_each_upstream_keeps_its_newest_model);
  RUN_TEST(test_harvest_filters_a_router_body);
  RUN_TEST(test_harvest_is_bounded_on_a_huge_body);
  RUN_TEST(test_candidates_match_the_pairwise_rule);
  RUN_TEST(test_short_list_is_whole);
  RUN_TEST(test_catalog_protects_default_and_upstream_heads);
  RUN_TEST(test_default_repicks_in_both_spellings);
  RUN_TEST(test_catalog_only_pick_is_allowed_by_the_catalog);
  RUN_TEST(test_rejected_pick_names_the_allowed_values);
  RUN_TEST(test_catalog_selectors_follow_role_and_usable);
  UNITY_END();
  return 0;
}
