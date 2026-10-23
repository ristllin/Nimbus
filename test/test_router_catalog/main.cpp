#include <unity.h>

#include <algorithm>
#include <set>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// CUM-530 and lane F (L19-49): the Cumulo router model list on the device - the
// store's rows and order, the short list, one slot per model across spellings, the
// router prices, and the model save rule. Pure headers (the device seams in
// provider_verify.cpp / webui.cpp only call them).
#include "../../src/agent/model_pick.h"
#include "../../src/agent/router_catalog.h"
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_selectable.h"

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

// The router rows a fetch keeps: a canned /router/models body with HTTP headers,
// pretty-printed. Non-chat ids keep no chat role (an embedding row stays one),
// the openai upstream's dated snapshots and the :batch price twins are no chat
// models, and the short list leads with the default and current generations.
static void test_router_body_rows_and_short_list() {
  const char* body =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
      "{\"object\": \"list\", \"data\": ["
      "{\"id\": \"anthropic/claude-3-5-haiku\", \"size_class\": \"small\"},"
      "{\"id\": \"anthropic/claude-opus-5\", \"size_class\": \"large\"},"
      "{\"id\": \"anthropic/claude-opus-5:batch\", \"size_class\": \"large\"},"
      "{\"id\": \"openai/gpt-4o\"}, {\"id\": \"openai/gpt-5.6\"},"
      "{\"id\": \"openai/gpt-4.1-2025-04-14\"}, {\"id\": \"openai/text-embedding-3-small\"},"
      "{\"id\": \"openai/o4-mini-deep-research\"}, {\"id\": \"openai/whisper-1\"},"
      "{\"id\": \"mistral/mistral-embed\"}, {\"id\": \"zai/glm-4.5\"}, {\"id\": \"zai/glm-5.3\"}]}";
  std::vector<ModelInfo> cat;
  nimbus::orch::parseModelsList("cumulo", body, cat);
  rc::protectHeads(cat, "gpt-5.6");
  std::string chat = ",", all = ",";
  for (const auto& m : cat) {
    all += rc::selectorOf(m) + ",";
    if (m.hasRole(nimbus::orch::RoleOrchestrator)) chat += rc::selectorOf(m) + ",";
  }
  TEST_ASSERT_EQUAL_STRING(",openai/gpt-5.6,anthropic/claude-opus-5,zai/glm-5.3,anthropic/claude-3-5-haiku,"
                           "zai/glm-4.5,openai/gpt-4o,", chat.c_str());
  TEST_ASSERT_TRUE(all.find(",openai/text-embedding-3-small,") != std::string::npos);   // an embedding row
  TEST_ASSERT_TRUE(all.find(":batch") == std::string::npos);
  TEST_ASSERT_TRUE(all.find("-2025-04-14") == std::string::npos);
  const std::string sl = rc::storeChoices(cat, "gpt-5.6", 0);
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", sl.substr(0, sl.find(',')).c_str());
  TEST_ASSERT_FALSE(csvHas(sl, "openai/text-embedding-3-small"));
  TEST_ASSERT_EQUAL(0, (int)nimbus::orch::parseModelsList("cumulo", "not a list", cat));
}

// The router body is untrusted: a list of thousands of ids must not cost seconds of
// CPU or an unbounded row list in the verify slot. The fetch holds at most
// kHarvestCollectMax rows, past which only current-generation chat rows are taken,
// so the default and each upstream's flagship listed after 5000 junk rows still
// lead; what was left out is counted (CUM-530).
static void test_harvest_is_bounded_on_a_huge_body() {
  std::string body = "{\"data\":[";
  for (int i = 0; i < 5000; ++i) body += "{\"id\":\"zai/m" + std::to_string(i) + "\"},";
  body += "{\"id\":\"anthropic/claude-opus-5\"},{\"id\":\"openai/gpt-5.6\"},"
          "{\"id\":\"zai/glm-5.3\"}]}";
  const auto t0 = std::chrono::steady_clock::now();
  nimbus::orch::ModelHarvest h;
  h.provider = "cumulo";
  nimbus::orch::harvestBody(h, body.data(), body.size());
  nimbus::orch::finishHarvest(h);
  TEST_ASSERT_EQUAL_UINT(nimbus::orch::kHarvestCollectMax + 3, h.rows.size());   // the bound + 3 flagships
  TEST_ASSERT_EQUAL_UINT(5003 - h.rows.size(), h.overflow);
  const size_t cut = rc::protectHeads(h.rows, "gpt-5.6");
  const std::string sl = rc::storeChoices(h.rows, "gpt-5.6", 0);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
  TEST_ASSERT_TRUE(h.rows.size() <= rc::kCatalogMaxRows);
  TEST_ASSERT_TRUE(cut > 0);
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", rc::selectorOf(h.rows[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", sl.substr(0, sl.find(',')).c_str());
  for (const char* id : {"anthropic/claude-opus-5", "zai/glm-5.3"}) TEST_ASSERT_TRUE_MESSAGE(csvHas(sl, id), id);
  TEST_ASSERT_TRUE_MESSAGE(ms < 3000, "a 5000-row router body took over 3 s");
}

// The candidate dedupe (sorted indexes + binary search for a dated twin's alias)
// keeps exactly what the plain pairwise rule keeps, in the same order: one copy of
// each model (the spellings of one version are one model, kept under the dash
// spelling when listed, else the first), minus empty ids and dated twins of a listed
// alias. The class, over generated lists mixing duplicates, spellings, twins,
// dated-only ids and empty ids.
static std::vector<size_t> pairwiseCandidates(const std::vector<std::string>& canon) {
  using nimbus::orch::sameModelKey;
  std::vector<size_t> cand;
  auto listed = [&](const std::string& id) {
    for (const std::string& e : canon)
      if (!e.empty() && sameModelKey(e) == sameModelKey(id)) return true;
    return false;
  };
  auto isRep = [&](size_t i) {
    const std::string k = sameModelKey(canon[i]);
    for (size_t j = 0; j < canon.size(); ++j)
      if (canon[j] == k) return j == i;   // the dash spelling, first copy
    for (size_t j = 0; j < canon.size(); ++j)
      if (sameModelKey(canon[j]) == k) return j == i;   // else the first copy
    return false;
  };
  for (size_t i = 0; i < canon.size(); ++i) {
    if (canon[i].empty() || !isRep(i)) continue;
    const std::string alias = rc::undatedAlias(canon[i]);
    if (!alias.empty() && listed(alias)) continue;
    cand.push_back(i);
  }
  return cand;
}
static void test_candidates_match_the_pairwise_rule() {
  const char* const kPool[] = {"zai/glm-5.3", "zai/glm-5.3", "", "anthropic/claude-opus-4-5",
                               "anthropic/claude-opus-4-5-20251101",
                               "anthropic/claude-haiku-4-5-20251001", "openai/gpt-4.1",
                               "openai/gpt-4.1-2025-04-14", "openai/gpt-4.1-2025-04-14",
                               "mistral/mistral-large", "openai/gpt-5.6",
                               "anthropic/claude-opus-5.5", "anthropic/claude-opus-5-5",
                               "anthropic/claude-sonnet-4.6", "openai/gpt-5.6-sol"};
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
// The web pick asks the ONE check (model_selectable.h) over the router's store.

// A router store from selectors (a verify's rows, protectHeads order).
static std::string routerStore(const std::vector<std::string>& sels) {
  std::vector<ModelInfo> cat;
  for (const std::string& sel : sels) cat.push_back(nimbus::orch::classifyCatalogEntry("cumulo", sel));
  rc::protectHeads(cat, "gpt-5.6");
  nimbus::orch::ModelStoreMeta meta;
  meta.choices = rc::storeChoices(cat, "gpt-5.6", 0);
  return nimbus::orch::serializeModelStore(meta, cat);
}
static nimbus::orch::SelectableSources cumuloSources(const std::string* text) {
  nimbus::orch::SelectableSources s;
  s.storeText = text;
  s.coldStart = NIMBUS_CUMULO_MODEL_CHOICES;
  s.defaultModel = "gpt-5.6";
  return s;
}
static bool pickOk(const std::string& text, const std::string& v) {
  return mp::allowed("cumulo", v, nimbus::orch::PickRole::Head, cumuloSources(&text));
}

// The bench: orchM_cumulo=gpt-5.6 answered 200 and changed nothing. The default is
// always a valid pick, in either spelling; so is every model the store lists, the
// ones past the 12-id short list included (the picker shows them all).
static void test_default_repicks_in_both_spellings() {
  const std::string text = routerStore(benchRouterList());
  TEST_ASSERT_TRUE(pickOk(text, "gpt-5.6"));
  TEST_ASSERT_TRUE(pickOk(text, "openai/gpt-5.6"));
  TEST_ASSERT_TRUE(pickOk(text, ""));                          // reset to default
  TEST_ASSERT_TRUE(pickOk(text, "anthropic/claude-opus-4-8"));
  TEST_ASSERT_TRUE(pickOk(text, "zai/glm-5.2"));
}

// A model past the short list is a valid pick because the store lists it; one the
// store does not list is refused, and a store-less device takes the cold-start list.
static void test_store_only_pick_is_allowed_by_the_store() {
  const std::string text = routerStore(benchRouterList());
  nimbus::orch::ModelStoreMeta meta;
  nimbus::orch::parseModelStoreHeader(text, meta);
  TEST_ASSERT_FALSE(csvHas(meta.choices, "anthropic/claude-haiku-4-5"));   // not in the short list
  TEST_ASSERT_TRUE(pickOk(text, "anthropic/claude-haiku-4-5"));
  TEST_ASSERT_FALSE(pickOk(text, "anthropic/claude-opus-9"));
  const std::string none;
  TEST_ASSERT_TRUE(pickOk(none, "openai/gpt-5.6-terra"));     // cold-start list
  TEST_ASSERT_FALSE(pickOk(none, "anthropic/claude-haiku-4-5"));
}

// A rejected choice: refused (never a silent no-op), and the copy names the
// allowed values, default included.
static void test_rejected_pick_names_the_allowed_values() {
  const std::string text = routerStore(benchRouterList());
  TEST_ASSERT_FALSE(pickOk(text, "anthropic/claude-opus-9"));
  TEST_ASSERT_FALSE(pickOk(text, "claude-opus-4-8"));   // bare = the openai upstream: not listed
  mp::PickLists l;
  l.choices = rc::shortlist(benchRouterList(), "gpt-5.6").csv;
  l.defaultModel = "gpt-5.6";
  l.router = true;
  const std::string msg = mp::rejectMessage("Cumulo Nimbus", l);
  TEST_ASSERT_TRUE(msg.rfind("That model isn't available for Cumulo Nimbus. Choose one of: ", 0) == 0);
  TEST_ASSERT_TRUE(msg.find("openai/gpt-5.6") != std::string::npos);
  TEST_ASSERT_TRUE(msg.find("anthropic/claude-opus-4-8") != std::string::npos);
  // A non-router provider compares exactly (no upstream folding) and names its
  // default even when the list omits it.
  nimbus::orch::SelectableSources o;
  o.coldStart = "gpt-6-astra,gpt-5.6-terra";
  o.defaultModel = "gpt-5.6";
  TEST_ASSERT_TRUE(mp::allowed("openai", "gpt-5.6", nimbus::orch::PickRole::Head, o));
  TEST_ASSERT_FALSE(mp::allowed("openai", "openai/gpt-5.6", nimbus::orch::PickRole::Head, o));
  mp::PickLists ol;
  ol.choices = "gpt-6-astra,gpt-5.6-terra";
  ol.defaultModel = "gpt-5.6";
  TEST_ASSERT_EQUAL_STRING(
      "That model isn't available for OpenAI. Choose one of: gpt-5.6, gpt-6-astra, gpt-5.6-terra.",
      mp::rejectMessage("OpenAI", ol).c_str());
}

// The picker's rows are the rows the save accepts: chat roles only while the check
// accepts them (usable, not a non-chat family), "<upstream>/<id>" for router rows.
static void test_picker_rows_follow_role_and_usable() {
  std::vector<ModelInfo> cat;
  for (const char* sel : {"openai/gpt-5.6", "anthropic/claude-opus-4-8", "zai/glm-5.2",
                          "openai/text-embedding-3-small"})
    cat.push_back(nimbus::orch::classifyCatalogEntry("cumulo", sel));
  cat[2].usable = false;
  cat[2].probed = true;
  const std::string text = nimbus::orch::serializeModelStore(nimbus::orch::ModelStoreMeta{}, cat);
  std::vector<std::string> heads;
  nimbus::orch::forEachStoreRow("cumulo", text, [&](const ModelInfo& m) {
    JsonDocument d;
    nimbus::orch::modelRowToJson("cumulo", m, d.to<JsonObject>(), 0);
    for (JsonVariantConst r : d["roles"].as<JsonArrayConst>())
      if (std::string(r | "") == "orchestrator") heads.push_back(nimbus::orch::modelSelector(m));
    return true;
  });
  TEST_ASSERT_EQUAL(2, (int)heads.size());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", heads[0].c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-4-8", heads[1].c_str());
  for (const std::string& id : heads) TEST_ASSERT_TRUE(pickOk(text, id));
  TEST_ASSERT_FALSE(pickOk(text, "zai/glm-5.2"));                    // probed unusable
  TEST_ASSERT_FALSE(pickOk(text, "openai/text-embedding-3-small"));  // not a chat model
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
  // The choice list, built from the raw list too (the keep order is the one rule).
  const rc::Shortlist sl = rc::shortlist(benchSlashList(), "gpt-5.6");
  TEST_ASSERT_TRUE_MESSAGE(sl.csv.find("mistral/mistral/") == std::string::npos, sl.csv.c_str());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", sl.csv.substr(0, sl.csv.find(',')).c_str());
  TEST_ASSERT_EQUAL_UINT(6, sl.total);   // 8 runnable ids, two of them a second spelling
  // The store's rows: a selector no upstream runs is not even held, the :batch twin
  // goes, and each model's two spellings are one row.
  std::vector<ModelInfo> cat;
  nimbus::orch::parseModelsList("cumulo", body, cat);
  rc::protectHeads(cat, "gpt-5.6");
  TEST_ASSERT_EQUAL_UINT(5, cat.size());
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

// CUM-530 gate: a device upgraded from v4.5.9 kept the Cumulo short list and catalog
// that build harvested (NVS mch_cumulo, /data/mcat_cumulo.json), slash ids included.
// The first boot of this build merges them into the store; the check, the picker
// rows and the refusal copy never offer or accept a slash id, whatever the merged
// lists say, and every runnable pick still saves.
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
  std::string mch;
  for (const std::string& id : benchSlashList()) mch += (mch.empty() ? "" : ",") + id;
  const std::string text = nimbus::orch::mergeLegacyLists("cumulo", "", v459Catalog(), mch, "v4.6.0");
  TEST_ASSERT_FALSE(text.empty());
  std::string accepted;
  for (const std::string& id : benchSlashList())
    if (!rc::routable(id) && pickOk(text, id)) accepted += id + " ";
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", accepted.c_str(), "a slash id from the stored list saved");
  for (const char* ok : {"openai/gpt-5.6", "gpt-5.6", "", "anthropic/claude-opus-5-5", "openai/gpt-6.1-sol"})
    TEST_ASSERT_TRUE_MESSAGE(pickOk(text, ok), ok);
  // The picker's rows: no slash id from either stored list, the runnable rows kept.
  std::string shown = ",";
  nimbus::orch::forEachStoreRow("cumulo", text, [&](const ModelInfo& m) {
    shown += nimbus::orch::modelSelector(m) + ",";
    return true;
  });
  TEST_ASSERT_TRUE_MESSAGE(shown.find("mistral/mistral/") == std::string::npos, shown.c_str());
  TEST_ASSERT_TRUE_MESSAGE(shown.find(":batch") == std::string::npos, shown.c_str());
  TEST_ASSERT_TRUE_MESSAGE(shown.find(",openai/gpt-5.6,") != std::string::npos, shown.c_str());
  TEST_ASSERT_TRUE_MESSAGE(shown.find(",anthropic/claude-opus-5-5,") != std::string::npos, shown.c_str());
  // The refusal copy never names one either.
  mp::PickLists l;
  l.choices = nimbus::orch::effectiveModelChoices("cumulo", mch);
  l.defaultModel = "gpt-5.6";
  l.router = true;
  const std::string msg = mp::rejectMessage("Cumulo Nimbus", l);
  TEST_ASSERT_TRUE_MESSAGE(msg.find("mistral/mistral/") == std::string::npos, msg.c_str());
  TEST_ASSERT_TRUE_MESSAGE(msg.find("anthropic/claude-opus-5-5") != std::string::npos, msg.c_str());
  // A pick such a build saved runs the default instead (every request on it would
  // be refused); a runnable pick runs as saved.
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", mp::runnableRouterPick("mistral/mistral/zai-glm-5-3", "gpt-5.6").c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5-5",
                           mp::runnableRouterPick("anthropic/claude-opus-5-5", "gpt-5.6").c_str());
  // A direct provider's ids are not router selectors: nothing is filtered there.
  nimbus::orch::SelectableSources d;
  d.coldStart = "ft:open-mistral-7b:acme/x,mistral-small-latest";
  d.defaultModel = "mistral-small-latest";
  TEST_ASSERT_TRUE(mp::allowed("mistral", "ft:open-mistral-7b:acme/x", nimbus::orch::PickRole::Head, d));
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
  // The fetch holds the bound, plus the three flagships listed after it.
  TEST_ASSERT_EQUAL_UINT(nimbus::orch::kHarvestCollectMax + 3, cat.size());
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

// ---- the real router list (L19-49) and its prices ---------------------------------
// The real /router/models body captured 2026-10-13 (623 rows, ~95 KB; ids and prices
// only, no credentials).
static std::string routerBody() {
  std::ifstream f("test/support/fixtures/router_models_2026-10-13.json", std::ios::binary);
  TEST_ASSERT_TRUE_MESSAGE(f.good(), "router fixture missing");
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A verify's store step over a body, as the device runs it (provider_verify.cpp
// storeModelHarvest): harvest, one row per model, the bound, the short list.
struct RouterStore {
  nimbus::orch::ModelHarvest h;
  nimbus::orch::ModelStoreMeta meta;
  size_t routable = 0;   // distinct models the router lists that the device can keep
};
static RouterStore storeRouterBody(const std::string& body, size_t chunk = 0) {
  RouterStore rs;
  rs.h.provider = "cumulo";
  if (!chunk) {
    nimbus::orch::harvestBody(rs.h, body.data(), body.size());
  } else {
    nimbus::orch::ModelsBodyReader rd(
        [](void* c, const char* o, size_t n) { nimbus::orch::harvestRow(*(nimbus::orch::ModelHarvest*)c, o, n); },
        &rs.h);
    for (size_t i = 0; i < body.size(); i += chunk) rd.feed(body.data() + i, std::min(chunk, body.size() - i));
    if (!rd.complete()) rs.h.cut = true;
  }
  nimbus::orch::finishHarvest(rs.h);
  const size_t more = rs.h.overflow + rc::protectHeads(rs.h.rows, "gpt-5.6") +
                      nimbus::orch::boundModelRows(rs.h.rows);
  rs.meta.total = (uint32_t)(rs.h.rows.size() + more);
  rs.meta.more = (uint32_t)more;
  rs.meta.cut = rs.h.cut || rs.h.hasMore;
  rs.meta.choices = rc::storeChoices(rs.h.rows, "gpt-5.6", 0);
  rs.routable = rs.h.rows.size() + more;
  return rs;
}

// The whole ~95 KB router list is read (the device used to stop at 64 KB and fall
// back to the 12-id list with no "more", CUM-564): the store holds every model the
// router lists that the device can run, one row each, under the stated bound - the
// count equals the routable total, so nothing says "more".
static void test_whole_router_body_reaches_the_store() {
  const std::string body = routerBody();
  TEST_ASSERT_TRUE(body.size() > 90000);
  const RouterStore rs = storeRouterBody(body, 1436);   // the TLS record size the device sees
  TEST_ASSERT_FALSE(rs.h.cut);
  TEST_ASSERT_EQUAL_UINT(623, rs.h.seen);
  TEST_ASSERT_TRUE(rs.h.rows.size() <= rc::kCatalogMaxRows);
  TEST_ASSERT_EQUAL_UINT(rs.meta.total, rs.h.rows.size());   // the routable total: no "more"
  TEST_ASSERT_EQUAL_UINT(0, rs.meta.more);
  const std::string text = nimbus::orch::serializeModelStore(rs.meta, rs.h.rows);
  TEST_ASSERT_TRUE(text.size() <= nimbus::orch::kModelStoreMaxBytes);
  // Rows listed past the first 64 KB are there (the old read window lost them).
  const size_t lunaAt = body.find("\"id\":\"openai/gpt-5.6-luna\"");
  TEST_ASSERT_TRUE(lunaAt > 65536);
  TEST_ASSERT_TRUE(text.find("openai/gpt-5.6-luna\t") != std::string::npos);
  // Every row the store holds is priced (the router prices every row it lists).
  for (const auto& m : rs.h.rows) TEST_ASSERT_TRUE_MESSAGE(m.priced, rc::selectorOf(m).c_str());
}

// A body cut short (the old 64 KB window, a dropped connection) keeps what arrived
// whole and SAYS so: the stored list is marked incomplete, the fetch's one log line
// names the cut, and a stored list is not replaced by it. Counter-test: the ~40 KB
// direct-provider lists read whole exactly as before (test_model_catalog pins them
// against the pre-lane-F parse).
static void test_cut_router_body_says_so() {
  const std::string body = routerBody().substr(0, 65535);
  const RouterStore rs = storeRouterBody(body, 1436);
  TEST_ASSERT_TRUE(rs.h.cut);
  TEST_ASSERT_TRUE(rs.meta.cut);
  TEST_ASSERT_TRUE(rs.h.rows.size() > 0);
  const std::string line = nimbus::orch::harvestLogLine(rs.h, rs.meta, body.size(), 1);
  TEST_ASSERT_TRUE_MESSAGE(line.find("body cut short") != std::string::npos, line.c_str());
  TEST_ASSERT_TRUE(line.rfind("verify: cumulo models read: ", 0) == 0);
  TEST_ASSERT_FALSE(nimbus::orch::harvestStorable(rs.h, /*haveStored=*/true));
  const std::string text = nimbus::orch::serializeModelStore(rs.meta, rs.h.rows);
  TEST_ASSERT_TRUE(text.find(" cut=1 ") != std::string::npos);
  for (const std::string p : {"openai", "mistral"}) {
    std::ifstream f("test/support/fixtures/models_2026-10/" + p + ".json", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    nimbus::orch::ModelHarvest h;
    h.provider = p;
    nimbus::orch::harvestBody(h, ss.str().data(), ss.str().size());
    TEST_ASSERT_FALSE_MESSAGE(h.cut, p.c_str());
  }
}

// 124 routable models (the four upstreams, priced and unpriced rows mixed): the store
// holds all 124, the picker offers all 124 for the head, and each says its price or
// "unknown". Counter-test: a BYOK list's store is its parse, row for row.
// 124 distinct current-generation router models, 31 per upstream, two thirds priced.
static std::string body124() {
  const std::pair<const char*, const char*> kUps[] = {
      {"openai", "gpt-7."}, {"anthropic", "claude-opus-7-"}, {"mistral", "mistral-large-7"}, {"zai", "glm-7."}};
  std::string body = "{\"object\":\"list\",\"data\":[";
  int n = 0;
  for (const auto& up : kUps) {
    for (int i = 0; i < 31; ++i, ++n) {
      body += "{\"id\":\"" + std::string(up.first) + "/" + up.second + std::to_string(i) +
              "\",\"size_class\":\"large\"";
      if (n % 3) body += ",\"in_micro_per_mtok\":" + std::to_string(1000 + n) + ",\"out_micro_per_mtok\":5000";
      body += "},";
    }
  }
  body.back() = ']';
  return body + "}";
}
static void test_124_models_all_stored_and_offered_with_a_price_or_unknown() {
  const RouterStore rs = storeRouterBody(body124());
  TEST_ASSERT_EQUAL_UINT(124, rs.h.rows.size());
  TEST_ASSERT_EQUAL_UINT(124, rs.meta.total);
  TEST_ASSERT_EQUAL_UINT(0, rs.meta.more);
  const std::string text = nimbus::orch::serializeModelStore(rs.meta, rs.h.rows);
  size_t offered = 0, priced = 0, unknown = 0;
  nimbus::orch::forEachStoreRow("cumulo", text, [&](const ModelInfo& m) {
    JsonDocument d;
    nimbus::orch::modelRowToJson("cumulo", m, d.to<JsonObject>(), 0);
    bool head = false;
    for (JsonVariantConst r : d["roles"].as<JsonArrayConst>()) head = head || std::string(r | "") == "orchestrator";
    offered += head && pickOk(text, nimbus::orch::modelSelector(m)) ? 1 : 0;
    const std::string src = d["priceSrc"] | "";
    priced += src == "router" ? 1 : 0;
    unknown += src == "unknown" ? 1 : 0;
    TEST_ASSERT_TRUE(src == "router" || src == "unknown");
    return true;
  });
  TEST_ASSERT_EQUAL_UINT(124, offered);
  TEST_ASSERT_EQUAL_UINT(124, priced + unknown);
  TEST_ASSERT_TRUE(priced > 0 && unknown > 0);
  // BYOK counter-test: the router rules never touch a direct provider's store.
  std::ifstream f("test/support/fixtures/models_2026-10/anthropic.json", std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  std::vector<ModelInfo> parsed;
  nimbus::orch::parseModelsList("anthropic", ss.str(), parsed);
  std::vector<ModelInfo> stored = parsed;
  TEST_ASSERT_EQUAL_UINT(0, nimbus::orch::boundModelRows(stored));
  TEST_ASSERT_EQUAL_UINT(parsed.size(), stored.size());
  for (size_t i = 0; i < parsed.size(); ++i) TEST_ASSERT_EQUAL_STRING(parsed[i].id.c_str(), stored[i].id.c_str());
}

// One slot per model across the dot and dash spellings: claude-opus-5-5 and
// claude-opus-5.5 are one row, counted once, kept under the dash spelling the
// Anthropic API uses and priced at the higher of the two. Counter-test: gpt-5.6 and
// gpt-5.6-sol are two models (the separator is not a variant).
static void test_dot_and_dash_spellings_are_one_slot() {
  const std::string body =
      "{\"data\":[{\"id\":\"anthropic/claude-opus-5.5\",\"in_micro_per_mtok\":4000000,\"out_micro_per_mtok\":21000000},"
      "{\"id\":\"anthropic/claude-opus-5-5\",\"in_micro_per_mtok\":4000000,\"out_micro_per_mtok\":20000000},"
      "{\"id\":\"openai/gpt-5.6\",\"in_micro_per_mtok\":1},{\"id\":\"openai/gpt-5.6-sol\",\"in_micro_per_mtok\":2}]}";
  const RouterStore rs = storeRouterBody(body);
  TEST_ASSERT_EQUAL_UINT(3, rs.h.rows.size());
  TEST_ASSERT_EQUAL_UINT(3, rs.meta.total);   // counted once
  std::string all = ",";
  for (const auto& m : rs.h.rows) all += rc::selectorOf(m) + ",";
  TEST_ASSERT_TRUE_MESSAGE(all.find(",anthropic/claude-opus-5-5,") != std::string::npos, all.c_str());
  TEST_ASSERT_TRUE_MESSAGE(all.find("claude-opus-5.5") == std::string::npos, all.c_str());
  TEST_ASSERT_TRUE(all.find(",openai/gpt-5.6,") != std::string::npos);
  TEST_ASSERT_TRUE(all.find(",openai/gpt-5.6-sol,") != std::string::npos);
  for (const auto& m : rs.h.rows)
    if (m.id == "claude-opus-5-5") TEST_ASSERT_EQUAL_UINT32(21000000, m.priceOut);   // the higher one
  // The real list: every spelling pair is one row.
  const RouterStore real = storeRouterBody(routerBody());
  std::set<std::string> keys;
  for (const auto& m : real.h.rows)
    TEST_ASSERT_TRUE_MESSAGE(keys.insert(nimbus::orch::sameModelKey(rc::selectorOf(m))).second,
                             rc::selectorOf(m).c_str());
  const std::string text = nimbus::orch::serializeModelStore(real.meta, real.h.rows);
  TEST_ASSERT_TRUE(pickOk(text, "anthropic/claude-opus-5.5"));   // either spelling picks the one row
  TEST_ASSERT_TRUE(pickOk(text, "anthropic/claude-opus-5-5"));
  TEST_ASSERT_EQUAL_STRING("claude-opus-5-5", nimbus::orch::sameModelKey("claude-opus-5.5").c_str());
  TEST_ASSERT_TRUE(nimbus::orch::sameModelKey("gpt-5.6") != nimbus::orch::sameModelKey("gpt-5.6-sol"));
}

static const nimbus::orch::ModelRate* priceOf(const std::vector<nimbus::orch::ModelRate>& v,
                                              const char* model) {
  for (const auto& r : v)
    if (std::strcmp(r.model, model) == 0) return &r;
  return nullptr;
}

// The spend ledger keeps router prices ONLY for the device's own models (the short
// list plus the head and sub-agent selectors), from the stored rows: keys are the
// usage keys ("<upstream>/<bare>"), prices the router's own, never a twin under
// another route or a variant.
static void test_router_rates_keep_only_the_devices_models() {
  const RouterStore rs = storeRouterBody(routerBody());
  const std::vector<std::string> keep = rc::priceKeepIds(
      NIMBUS_CUMULO_MODEL_CHOICES, "anthropic/claude-opus-5", "zai/glm-5.3");
  std::vector<nimbus::orch::ModelRate> out;
  std::vector<std::string> missing = {"stale"};
  TEST_ASSERT_EQUAL(5, (int)rc::routerRates(rs.h.rows, keep, out, &missing));
  TEST_ASSERT_EQUAL(0, (int)missing.size());
  const auto* g = priceOf(out, "openai/gpt-5.6");
  TEST_ASSERT_NOT_NULL(g);
  TEST_ASSERT_EQUAL_STRING("cumulo", g->prov);
  TEST_ASSERT_EQUAL_UINT32(4000000, g->microIn);
  TEST_ASSERT_EQUAL_UINT32(20000000, g->microOut);
  TEST_ASSERT_EQUAL_UINT32(200000, priceOf(out, "openai/gpt-5.6-luna")->microIn);   // listed past 64 KB
  TEST_ASSERT_EQUAL_UINT32(12000000, priceOf(out, "openai/gpt-5.6-terra")->microOut);
  TEST_ASSERT_EQUAL_UINT32(25000000, priceOf(out, "anthropic/claude-opus-5")->microOut);
  TEST_ASSERT_EQUAL_UINT32(79000, priceOf(out, "zai/glm-5.3")->microIn);
  for (const auto& r : out) TEST_ASSERT_NULL(std::strstr(r.model, "azure"));
  TEST_ASSERT_NULL(priceOf(out, "openai/gpt-5.6-sol"));
  // A model the router does not list keeps no router price, and is named.
  rc::routerRates(rs.h.rows, {"anthropic/claude-nope-9"}, out, &missing);
  TEST_ASSERT_EQUAL(0, (int)out.size());
  TEST_ASSERT_EQUAL(1, (int)missing.size());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-nope-9", missing[0].c_str());
}

// Bounded at the ledger's 24 rows whatever the keep list; two kept ids of one model
// (its two spellings) are one key at the higher price.
static void test_router_rates_are_bounded_and_merge_spellings() {
  const RouterStore rs = storeRouterBody(routerBody());
  std::vector<std::string> all;
  for (const auto& m : rs.h.rows) all.push_back(rc::selectorOf(m));
  TEST_ASSERT_TRUE(all.size() > 24);
  std::vector<nimbus::orch::ModelRate> out;
  TEST_ASSERT_EQUAL((int)nimbus::orch::ModelLedger::kMaxRouterRates, (int)rc::routerRates(rs.h.rows, all, out));
  std::vector<ModelInfo> two = {nimbus::orch::classifyCatalogEntry("cumulo", "mistral/mistral-large")};
  two[0].priced = true;
  two[0].priceIn = 2000000;
  two[0].priceOut = 6000000;
  rc::routerRates(two, {"mistral/mistral-large", "mistral/mistral-large"}, out);
  TEST_ASSERT_EQUAL(1, (int)out.size());
  TEST_ASSERT_EQUAL_UINT32(6000000, out[0].microOut);
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


// The peer review's case: an upstream whose non-chat row (an embedding) reads as a
// newer version than its chat model must never become that upstream's protected head.
static void test_non_chat_row_is_never_an_upstream_head() {
  const std::string body =
      "{\"data\":[{\"id\":\"mistral/mistral-embed-9\"},{\"id\":\"mistral/mistral-large\"},"
      "{\"id\":\"zai/glm-ocr-9\"},{\"id\":\"zai/glm-5.3\"},{\"id\":\"openai/gpt-5.6\"}]}";
  std::vector<ModelInfo> cat;
  nimbus::orch::parseModelsList("cumulo", body, cat);
  rc::protectHeads(cat, "gpt-5.6");
  std::vector<std::string> heads;
  std::vector<std::string> ups;
  for (const auto& m : cat) {
    if (std::find(ups.begin(), ups.end(), m.upstream) != ups.end()) continue;
    ups.push_back(m.upstream);
    heads.push_back(rc::selectorOf(m));
  }
  TEST_ASSERT_TRUE(std::find(heads.begin(), heads.end(), "mistral/mistral-large") != heads.end());
  TEST_ASSERT_TRUE(std::find(heads.begin(), heads.end(), "zai/glm-5.3") != heads.end());
  for (const std::string& h : heads) TEST_ASSERT_TRUE_MESSAGE(rc::routerChatModel(h), h.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_choice_list_keeps_default_and_each_upstream);
  RUN_TEST(test_every_upstream_survives_any_cap);
  RUN_TEST(test_dated_twins_dropped_dated_only_kept);
  RUN_TEST(test_each_upstream_keeps_its_newest_model);
  RUN_TEST(test_router_body_rows_and_short_list);
  RUN_TEST(test_harvest_is_bounded_on_a_huge_body);
  RUN_TEST(test_candidates_match_the_pairwise_rule);
  RUN_TEST(test_short_list_is_whole);
  RUN_TEST(test_catalog_protects_default_and_upstream_heads);
  RUN_TEST(test_default_repicks_in_both_spellings);
  RUN_TEST(test_store_only_pick_is_allowed_by_the_store);
  RUN_TEST(test_rejected_pick_names_the_allowed_values);
  RUN_TEST(test_picker_rows_follow_role_and_usable);
  RUN_TEST(test_catalog_from_a_huge_router_body_is_bounded);
  RUN_TEST(test_ids_no_upstream_runs_are_dropped);
  RUN_TEST(test_routable_rule);
  RUN_TEST(test_v459_stored_lists_never_offer_or_accept_slash_ids);
  RUN_TEST(test_catalog_of_many_upstreams_is_bounded);
  RUN_TEST(test_same_model_under_two_upstreams_is_two_rows);
  RUN_TEST(test_whole_router_body_reaches_the_store);
  RUN_TEST(test_cut_router_body_says_so);
  RUN_TEST(test_124_models_all_stored_and_offered_with_a_price_or_unknown);
  RUN_TEST(test_dot_and_dash_spellings_are_one_slot);
  RUN_TEST(test_router_rates_keep_only_the_devices_models);
  RUN_TEST(test_router_rates_are_bounded_and_merge_spellings);
  RUN_TEST(test_price_keep_ids_are_choices_plus_selectors);
  RUN_TEST(test_non_chat_row_is_never_an_upstream_head);
  UNITY_END();
  return 0;
}
