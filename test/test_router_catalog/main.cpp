#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
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

// CUM-530 gate: the harvest is bounded, but the catalog built from the SAME
// untrusted 64 KB router body was not: parseModelsList deduped pairwise
// (quadratic) and the store step re-serialized once per dropped row over every
// row. The parse now dedupes in one sort and protectHeads keeps at most
// kCatalogMaxRows, the default, each upstream's head and the flagships first,
// however late they are listed.
static void test_catalog_from_a_huge_router_body_is_bounded() {
  std::string body = "{\"object\":\"list\",\"data\":[";
  for (int i = 0; i < 4000; ++i) body += "{\"id\":\"zai/m" + std::to_string(i) + "\"},";
  body += "{\"id\":\"zai/m7\"},";   // a duplicate keeps one row
  body += "{\"id\":\"openai/gpt-5.6\"},{\"id\":\"anthropic/claude-opus-5\"},{\"id\":\"zai/glm-5.3\"}]}";
  std::vector<ModelInfo> cat;
  nimbus::orch::parseModelsList("cumulo", body, cat);
  size_t m7 = 0;
  for (const auto& m : cat) m7 += rc::selectorOf(m) == "zai/m7" ? 1 : 0;
  TEST_ASSERT_EQUAL_UINT(1, m7);
  const size_t cut = rc::protectHeads(cat, "gpt-5.6");
  TEST_ASSERT_TRUE(cat.size() <= rc::kCatalogMaxRows);
  TEST_ASSERT_TRUE(cut > 0);   // counted for the stored "<n> more" marker
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", rc::selectorOf(cat[0]).c_str());
  std::string all = ",";
  for (const auto& m : cat) all += rc::selectorOf(m) + ",";
  for (const char* want : {",openai/gpt-5.6,", ",anthropic/claude-opus-5,", ",zai/glm-5.3,"})
    TEST_ASSERT_TRUE_MESSAGE(all.find(want) != std::string::npos, want);
}

// Bench (nimbus-light, v4.5.10): 4 of the router's 12 ids carry a second '/' in
// the model part ("mistral/mistral/zai-glm-5-3"). The device offered them, a save
// answered 200, and the next turn got "Invalid model: mistral/zai-glm-5-3" from
// the upstream. No upstream runs such an id, so the harvest, the choice list and
// the catalog drop every one of them and keep the rest in order.
static std::vector<std::string> benchSlashList() {
  return {"openai/gpt-5.6",          "anthropic/claude-opus-5-5",    "mistral/mistral/zai-glm-5-3",
          "anthropic/claude-opus-5.5", "mistral/mistral/glm-5-2",    "openai/gpt-6.1-sol",
          "anthropic/claude-sonnet-5-5", "mistral/mistral/zai-glm-5-2", "openai/gpt-6.1-sol-pro",
          "anthropic/claude-sonnet-5.5", "mistral/mistral/zai-glm-5",  "openai/gpt-6.1-sol-pro:batch"};
}
static void test_ids_no_upstream_runs_are_dropped() {
  std::string body = "{\"object\":\"list\",\"data\":[";
  for (const std::string& id : benchSlashList()) body += "{\"id\":\"" + id + "\"},";
  body.back() = ']';
  body += "}";
  const std::vector<std::string> ids = rc::harvestIds(body.c_str());
  TEST_ASSERT_EQUAL_UINT(8, ids.size());
  for (const std::string& id : ids)
    TEST_ASSERT_TRUE_MESSAGE(id.find('/', id.find('/') + 1) == std::string::npos, id.c_str());
  // The choice list, built from the raw list too (the keep order is the one rule).
  const rc::Shortlist sl = rc::shortlist(benchSlashList(), "gpt-5.6");
  TEST_ASSERT_TRUE_MESSAGE(sl.csv.find("mistral/mistral/") == std::string::npos, sl.csv.c_str());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", sl.csv.substr(0, sl.csv.find(',')).c_str());
  TEST_ASSERT_EQUAL_UINT(8, sl.total);
  // The catalog: parseModelsList keeps "mistral/zai-glm-5-3" under upstream mistral;
  // protectHeads drops it.
  std::vector<ModelInfo> cat;
  nimbus::orch::parseModelsList("cumulo", body, cat);
  rc::protectHeads(cat, "gpt-5.6");
  TEST_ASSERT_EQUAL_UINT(8, cat.size());
  for (const auto& m : cat) TEST_ASSERT_TRUE_MESSAGE(m.id.find('/') == std::string::npos, m.id.c_str());
}
// The rule itself: a nested model part, an empty one, or a leading '/' on the
// model is not routable; a bare id and an ordinary selector are.
static void test_routable_rule() {
  TEST_ASSERT_FALSE(rc::routable("mistral/mistral/zai-glm-5-3"));
  TEST_ASSERT_FALSE(rc::routable("openai/"));
  TEST_ASSERT_FALSE(rc::routable("zai//glm-5"));
  TEST_ASSERT_TRUE(rc::routable("gpt-5.6"));
  TEST_ASSERT_TRUE(rc::routable("mistral/zai-glm-5-3"));
  // The rule reads the selector in place; it must agree with the route split the
  // head and the sub resolve through, for every shape.
  for (const char* sel : {"", "/", "//", "a", "/a", "a/", "a/b", "a//b", "a/b/c", "/a/b", "gpt-5.6",
                          "openai/gpt-5.6", "mistral/mistral/zai-glm-5-3", " openai/x", "openai/ x"}) {
    const nimbus::orch::RouterRoute r = nimbus::orch::resolveRouterRoute(sel);
    const bool split = !r.model.empty() && r.model.find('/') == std::string::npos;
    TEST_ASSERT_EQUAL_MESSAGE(split, rc::routable(sel), sel);
  }
}

// CUM-530 gate: a device upgraded from v4.5.9 keeps the Cumulo choice list and
// catalog that build harvested (NVS mch_cumulo, /data/mcat_cumulo.json), slash ids
// included, and nothing re-harvests until a Verify. The save rule, the copy that
// names the allowed values and the picker's catalog drop them on read, whatever
// the stored lists say; every runnable pick still saves.
static std::string v459Catalog() {
  // parseModelsList's shape: the upstream split off, a nested model part kept whole.
  return "[{\"id\":\"gpt-5.6\",\"upstream\":\"openai\",\"roles\":[\"orchestrator\",\"sub-agent\"],"
         "\"usable\":true},"
         "{\"id\":\"mistral/zai-glm-5-3\",\"upstream\":\"mistral\",\"roles\":[\"orchestrator\","
         "\"sub-agent\"],\"usable\":true},"
         "{\"id\":\"claude-opus-5-5\",\"upstream\":\"anthropic\",\"roles\":[\"orchestrator\"],"
         "\"usable\":true},"
         "{\"id\":\"mistral/glm-5-2\",\"upstream\":\"mistral\",\"roles\":[\"orchestrator\"],"
         "\"usable\":true},"
         "{\"id\":\"mistral-large-latest\",\"roles\":[\"orchestrator\"],\"usable\":true}]";
}
static void test_v459_stored_lists_never_offer_or_accept_slash_ids() {
  mp::PickLists l;
  for (const std::string& id : benchSlashList()) l.choices += (l.choices.empty() ? "" : ",") + id;
  l.catalog = mp::catalogSelectors(v459Catalog(), "orchestrator");
  l.defaultModel = "gpt-5.6";
  l.router = true;
  std::string accepted;
  for (const std::string& id : benchSlashList())
    if (!rc::routable(id) && mp::allowed(l, id)) accepted += id + " ";
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", accepted.c_str(), "a slash id from the stored list saved");
  for (const char* ok : {"openai/gpt-5.6", "gpt-5.6", "", "anthropic/claude-opus-5-5", "openai/gpt-6.1-sol"})
    TEST_ASSERT_TRUE_MESSAGE(mp::allowed(l, ok), ok);
  // The catalog the picker renders: the runnable rows only (a direct row is kept).
  TEST_ASSERT_EQUAL(3, (int)l.catalog.size());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", l.catalog[0].c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5-5", l.catalog[1].c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-large-latest", l.catalog[2].c_str());
  // The refusal copy never names one either.
  const std::string msg = mp::rejectMessage("Cumulo Nimbus", l);
  TEST_ASSERT_TRUE_MESSAGE(msg.find("mistral/mistral/") == std::string::npos, msg.c_str());
  TEST_ASSERT_TRUE_MESSAGE(msg.find("anthropic/claude-opus-5-5") != std::string::npos, msg.c_str());
  // A pick such a build saved runs the default instead (every request on it would
  // be refused); a runnable pick runs as saved.
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", mp::runnableRouterPick("mistral/mistral/zai-glm-5-3", "gpt-5.6").c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5-5",
                           mp::runnableRouterPick("anthropic/claude-opus-5-5", "gpt-5.6").c_str());
  // A direct provider's ids are not router selectors: nothing is filtered there.
  mp::PickLists d;
  d.choices = "ft:open-mistral-7b:acme/x,mistral-small-latest";
  d.defaultModel = "mistral-small-latest";
  TEST_ASSERT_TRUE(mp::allowed(d, "ft:open-mistral-7b:acme/x"));
}

// CUM-530 gate: the per-upstream grouping searched every upstream seen so far for
// each row and kept a queue per upstream, so a body of thousands of distinct
// upstream prefixes (each a one-row "upstream") was quadratic and held thousands
// of queues. The four known upstreams are always grouped, others only up to
// kOtherUpstreamsMax; the default and the flagships still lead.
static void test_catalog_of_many_upstreams_is_bounded() {
  std::string body = "{\"object\":\"list\",\"data\":[";
  for (int i = 0; i < 4000; ++i) body += "{\"id\":\"u" + std::to_string(i) + "/m" + std::to_string(i) + "\"},";
  body += "{\"id\":\"openai/gpt-5.6\"},{\"id\":\"anthropic/claude-opus-5\"},{\"id\":\"zai/glm-5.3\"}]}";
  std::vector<ModelInfo> cat;
  nimbus::orch::parseModelsList("cumulo", body, cat);
  TEST_ASSERT_TRUE(cat.size() > 4000);   // every distinct row reached the grouping
  const auto t0 = std::chrono::steady_clock::now();
  const size_t offered = cat.size();
  const size_t cut = rc::protectHeads(cat, "gpt-5.6");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  TEST_ASSERT_TRUE(cat.size() <= rc::kCatalogMaxRows);
  TEST_ASSERT_EQUAL_UINT(offered, cat.size() + cut);   // every row left out counts toward "<n> more"
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", rc::selectorOf(cat[0]).c_str());
  std::vector<std::string> ups;
  std::string all = ",";
  for (const auto& m : cat) {
    all += rc::selectorOf(m) + ",";
    if (std::find(ups.begin(), ups.end(), m.upstream) == ups.end()) ups.push_back(m.upstream);
  }
  for (const char* want : {",anthropic/claude-opus-5,", ",zai/glm-5.3,"})
    TEST_ASSERT_TRUE_MESSAGE(all.find(want) != std::string::npos, want);
  TEST_ASSERT_TRUE_MESSAGE(ups.size() <= 3 + rc::kOtherUpstreamsMax, all.substr(0, 200).c_str());
  TEST_ASSERT_TRUE_MESSAGE(ms < 2000, "grouping 4000 distinct upstreams took over 2 s");
  // The choice list built from the same rows is bounded the same way.
  std::vector<std::string> ids;
  for (int i = 0; i < 4000; ++i) ids.push_back("u" + std::to_string(i) + "/m" + std::to_string(i));
  ids.push_back("openai/gpt-5.6");
  const rc::Shortlist sl = rc::shortlist(ids, "gpt-5.6");
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", sl.csv.substr(0, sl.csv.find(',')).c_str());
  TEST_ASSERT_TRUE(sl.kept <= 1 + rc::kOtherUpstreamsMax);
  TEST_ASSERT_EQUAL_UINT(4001, sl.total);   // the left-out upstreams still count as offered
  TEST_ASSERT_TRUE(sl.truncated());
}

// The catalog deduped by the bare model id across upstreams, so a second upstream's
// model of the same name vanished from the catalog (and a catalog-only pick of it
// was refused). A router row's model is its upstream and its id.
static void test_same_model_under_two_upstreams_is_two_rows() {
  const std::string body =
      "{\"object\":\"list\",\"data\":[{\"id\":\"zai/glm-5.3\"},{\"id\":\"mistral/glm-5.3\"},"
      "{\"id\":\"zai/glm-5.3\"}]}";
  std::vector<ModelInfo> cat;
  nimbus::orch::parseModelsList("cumulo", body, cat);
  TEST_ASSERT_EQUAL_UINT(2, cat.size());   // the repeat of zai/glm-5.3 still goes
  rc::protectHeads(cat, "gpt-5.6");
  std::string all = ",";
  for (const auto& m : cat) all += rc::selectorOf(m) + ",";
  TEST_ASSERT_TRUE_MESSAGE(all.find(",zai/glm-5.3,") != std::string::npos, all.c_str());
  TEST_ASSERT_TRUE_MESSAGE(all.find(",mistral/glm-5.3,") != std::string::npos, all.c_str());
}

// ---- router prices (per-model prices, 2026-10) -----------------------------------
// The real /router/models body captured 2026-10-13 (623 rows, ~95 KB; ids and
// prices only, no credentials): the harvest keeps ONLY the device's own models.
static std::string routerBody() {
  std::ifstream f("test/support/fixtures/router_models_2026-10-13.json", std::ios::binary);
  TEST_ASSERT_TRUE_MESSAGE(f.good(), "router fixture missing");
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static const nimbus::orch::ModelRate* priceOf(const std::vector<nimbus::orch::ModelRate>& v,
                                              const char* model) {
  for (const auto& r : v)
    if (std::strcmp(r.model, model) == 0) return &r;
  return nullptr;
}

static void test_price_harvest_keeps_only_the_devices_models() {
  const std::string body = routerBody();
  TEST_ASSERT_TRUE(body.size() > 90000);
  const std::vector<std::string> keep = rc::priceKeepIds(
      NIMBUS_CUMULO_MODEL_CHOICES, "anthropic/claude-opus-5", "zai/glm-5.3");
  std::vector<nimbus::orch::ModelRate> out;
  TEST_ASSERT_EQUAL(5, (int)rc::harvestPrices(body.c_str(), keep, out));
  // Keys are the usage keys ("<upstream>/<bare>"), prices the router's own.
  const auto* g = priceOf(out, "openai/gpt-5.6");
  TEST_ASSERT_NOT_NULL(g);
  TEST_ASSERT_EQUAL_STRING("cumulo", g->prov);
  TEST_ASSERT_EQUAL_UINT32(4000000, g->microIn);
  TEST_ASSERT_EQUAL_UINT32(20000000, g->microOut);
  TEST_ASSERT_EQUAL_UINT32(200000, priceOf(out, "openai/gpt-5.6-luna")->microIn);
  TEST_ASSERT_EQUAL_UINT32(12000000, priceOf(out, "openai/gpt-5.6-terra")->microOut);
  TEST_ASSERT_EQUAL_UINT32(25000000, priceOf(out, "anthropic/claude-opus-5")->microOut);
  TEST_ASSERT_EQUAL_UINT32(79000, priceOf(out, "zai/glm-5.3")->microIn);
  // Never a twin under another route (openai/azure/gpt-5.6) or a variant (-sol).
  for (const auto& r : out) TEST_ASSERT_NULL(std::strstr(r.model, "azure"));
  TEST_ASSERT_NULL(priceOf(out, "openai/gpt-5.6-sol"));
  // The device reads at most 64 KB of the body: the rows past it are simply absent.
  const std::string cut = body.substr(0, 65535);
  TEST_ASSERT_TRUE(rc::harvestPrices(cut.c_str(), keep, out) >= 3);
  TEST_ASSERT_NULL(priceOf(out, "openai/gpt-5.6-luna"));   // its row sits past 64 KB
}

static void test_price_harvest_is_bounded_and_saturating() {
  const std::string body = routerBody();
  // Every routable id of the body as the keep list: still at most 24 rows.
  std::vector<std::string> all = rc::harvestIds(body.c_str());
  TEST_ASSERT_TRUE(all.size() > 24);
  std::vector<nimbus::orch::ModelRate> out;
  TEST_ASSERT_EQUAL((int)nimbus::orch::ModelLedger::kMaxRouterRates,
                    (int)rc::harvestPrices(body.c_str(), all, out));
  // An over-range price saturates (never wraps to a cheap one); a row with no price
  // is not kept; a body cut inside an object ends the scan without a crash.
  const char* odd =
      "{\"data\":[{\"id\":\"openai/gpt-5.6\",\"in_micro_per_mtok\":99999999999,"
      "\"out_micro_per_mtok\":5},{\"id\":\"openai/gpt-5.6-terra\"},"
      "{\"id\":\"openai/gpt-5.6-luna\",\"in_micro_per_mtok\":7";
  TEST_ASSERT_EQUAL(1, (int)rc::harvestPrices(odd, rc::priceKeepIds(NIMBUS_CUMULO_MODEL_CHOICES, "", ""), out));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, out[0].microIn);
  TEST_ASSERT_EQUAL_UINT32(5, out[0].microOut);
  TEST_ASSERT_EQUAL(0, (int)rc::harvestPrices(nullptr, all, out));
  TEST_ASSERT_EQUAL(0, (int)rc::harvestPrices("", all, out));
}

// Two router ids that share one usage key (an alias and its 4-digit build) keep the
// HIGHER price per field, whatever the body order; a body repeating a kept id stops
// after a bounded number of row scans (the verify task never spins on it).
static void test_price_harvest_shared_key_keeps_the_higher_price() {
  const char* body =
      "{\"data\":[{\"id\":\"mistral/mistral-large-2512\",\"in_micro_per_mtok\":500000,"
      "\"out_micro_per_mtok\":9000000},{\"id\":\"mistral/mistral-large\","
      "\"in_micro_per_mtok\":2000000,\"out_micro_per_mtok\":6000000}]}";
  std::vector<nimbus::orch::ModelRate> out;
  const std::vector<std::string> keep = {"mistral/mistral-large", "mistral/mistral-large-2512"};
  TEST_ASSERT_EQUAL(1, (int)rc::harvestPrices(body, keep, out));
  TEST_ASSERT_EQUAL_STRING("mistral/mistral-large", out[0].model);
  TEST_ASSERT_EQUAL_UINT32(2000000, out[0].microIn);
  TEST_ASSERT_EQUAL_UINT32(9000000, out[0].microOut);
  std::string rep = "{\"data\":[{";
  for (int i = 0; i < 500; ++i) rep += "\"id\":\"openai/gpt-5.6\",";
  rep += "\"id\":\"openai/gpt-5.6-terra\",\"in_micro_per_mtok\":1}]}";
  TEST_ASSERT_EQUAL(1, (int)rc::harvestPrices(rep.c_str(),
                                              rc::priceKeepIds(NIMBUS_CUMULO_MODEL_CHOICES, "", ""), out));
  TEST_ASSERT_NOT_NULL(priceOf(out, "openai/gpt-5.6"));
  TEST_ASSERT_NULL(priceOf(out, "openai/gpt-5.6-terra"));   // never reached: bounded
}

// CUM-564: the verify reads only the first 64 KB of the ~95 KB router list. A kept
// model listed later, or whose row the window cuts short, gets no router price (the
// next tier prices it), never a wrong one, and is named so the verify can log it.
static void test_price_harvest_names_models_past_the_read_window() {
  const std::string body = routerBody();
  const std::vector<std::string> keep = rc::priceKeepIds(
      NIMBUS_CUMULO_MODEL_CHOICES, "anthropic/claude-opus-5", "zai/glm-5.3");
  std::vector<nimbus::orch::ModelRate> whole, out;
  std::vector<std::string> missing = {"stale"};
  TEST_ASSERT_EQUAL(5, (int)rc::harvestPrices(body.c_str(), keep, whole, &missing));
  TEST_ASSERT_EQUAL(0, (int)missing.size());   // the whole list: every kept id found
  const std::string cut = body.substr(0, 65535);
  rc::harvestPrices(cut.c_str(), keep, out, &missing);
  TEST_ASSERT_TRUE(std::find(missing.begin(), missing.end(), "openai/gpt-5.6-luna") != missing.end());
  for (const std::string& m : missing) TEST_ASSERT_NULL(priceOf(out, m.c_str()));
  TEST_ASSERT_EQUAL((int)keep.size(), (int)(out.size() + missing.size()));
  for (const auto& r : out) {   // what the window holds is priced exactly as in the whole list
    const auto* w = priceOf(whole, r.model);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT32(w->microIn, r.microIn);
    TEST_ASSERT_EQUAL_UINT32(w->microOut, r.microOut);
  }
  // A row the window cuts mid-object: its id is in the window, its price is not.
  const size_t at = body.find("\"id\":\"anthropic/claude-haiku-4-5\"");
  TEST_ASSERT_TRUE(at != std::string::npos);
  const std::string mid = body.substr(0, at + 60);
  TEST_ASSERT_EQUAL(0, (int)rc::harvestPrices(mid.c_str(), {"anthropic/claude-haiku-4-5"}, out, &missing));
  TEST_ASSERT_EQUAL(1, (int)missing.size());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-haiku-4-5", missing[0].c_str());
}

static void test_price_keep_ids_are_choices_plus_selectors() {
  const std::vector<std::string> k = rc::priceKeepIds("a,b,,c", "anthropic/claude-opus-5", "");
  TEST_ASSERT_EQUAL(4, (int)k.size());
  TEST_ASSERT_EQUAL_STRING("c", k[2].c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5", k[3].c_str());
  std::string many;
  for (int i = 0; i < 30; ++i) many += (i ? "," : "") + std::string("m") + std::to_string(i);
  TEST_ASSERT_EQUAL((int)rc::kChoicesMax + 2, (int)rc::priceKeepIds(many, "x", "y").size());
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
  RUN_TEST(test_catalog_from_a_huge_router_body_is_bounded);
  RUN_TEST(test_ids_no_upstream_runs_are_dropped);
  RUN_TEST(test_routable_rule);
  RUN_TEST(test_v459_stored_lists_never_offer_or_accept_slash_ids);
  RUN_TEST(test_catalog_of_many_upstreams_is_bounded);
  RUN_TEST(test_same_model_under_two_upstreams_is_two_rows);
  RUN_TEST(test_price_harvest_keeps_only_the_devices_models);
  RUN_TEST(test_price_harvest_is_bounded_and_saturating);
  RUN_TEST(test_price_harvest_shared_key_keeps_the_higher_price);
  RUN_TEST(test_price_keep_ids_are_choices_plus_selectors);
  RUN_TEST(test_price_harvest_names_models_past_the_read_window);
  UNITY_END();
  return 0;
}
