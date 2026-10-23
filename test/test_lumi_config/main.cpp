#include <unity.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Lane F2, the pre-release check before v4.6.0 reaches the owner's device: Lumi's
// stored model configuration (22 Oct 2026), run against the Oct 2026 provider lists
// (test/support/fixtures/models_2026-10, the router's router_models_2026-10-13.json)
// as the model store holds them after the first fetch.
//   openai    head gpt-5.6-sol           sub gpt-5.6-sol
//   cumulo    head gpt-5.6 (bare id)     sub gpt-5.6
//   mistral   head mistral-large-latest  sub mistral-large-latest
//   anthropic head claude-sonnet-5       sub claude-sonnet-5
//   zai       head glm-5.3               sub glm-5.3 (no key: never fetched)
//   provPrio openai,anthropic,mistral,cumulo; subPrio openai,mistral,anthropic
// Every pick passes the one check for its role, every spawn with no model named runs
// the stored sub (the cumulo spawn used to be coerced: its bare "gpt-5.6" is not
// spelled that way in the router's short list), a re-saved card is accepted, and a
// stored pick a later list drops keeps running (the owner is told once).
#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "../../src/agent/model_pick.h"
#include "../../src/agent/router_catalog.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/orch/journal.h"
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_defaults.h"
#include "nimbus/orch/model_selectable.h"

using namespace nimbus::orch;
namespace rc = agent::router_catalog;

void setUp() {}
void tearDown() {}

static const uint32_t kOct22 = 1792670400u;   // 2026-10-22T12:00:00Z

struct Pick {
  const char* provider;
  const char* fixture;   // nullptr: no key, never fetched
  const char* head;
  const char* sub;
};
static const Pick kLumi[] = {
    {"openai", "test/support/fixtures/models_2026-10/openai.json", "gpt-5.6-sol", "gpt-5.6-sol"},
    {"cumulo", "test/support/fixtures/router_models_2026-10-13.json", "gpt-5.6", "gpt-5.6"},
    {"mistral", "test/support/fixtures/models_2026-10/mistral.json", "mistral-large-latest", "mistral-large-latest"},
    {"anthropic", "test/support/fixtures/models_2026-10/anthropic.json", "claude-sonnet-5", "claude-sonnet-5"},
    {"zai", nullptr, "glm-5.3", "glm-5.3"},
};

static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// The store the device's first fetch writes (provider_verify.cpp storeModelHarvest).
static std::string storeFor(const Pick& p) {
  if (!p.fixture) return std::string();
  const std::string body = readFile(p.fixture);
  ModelHarvest h;
  h.provider = p.provider;
  h.now = kOct22;
  harvestBody(h, body.data(), body.size());
  finishHarvest(h);
  const bool router = std::string(p.provider) == "cumulo";
  size_t more = router ? rc::protectHeads(h.rows, "gpt-5.6") : 0;
  more += boundModelRows(h.rows);
  ModelStoreMeta meta;
  meta.fetchedAt = kOct22;
  meta.total = (uint32_t)(h.rows.size() + more);
  meta.more = (uint32_t)more;
  meta.choices = router ? rc::storeChoices(h.rows, "gpt-5.6", kOct22) : directChoices(p.provider, h.rows, kOct22);
  return serializeModelStore(meta, h.rows);
}

static std::string shortListOf(const Pick& p, const std::string& store) {
  ModelStoreMeta meta;
  parseModelStoreHeader(store, meta);
  return effectiveModelChoices(p.provider, meta.choices);
}

// The device's sources (store.cpp baseSources): the store, the cold-start list, the
// role defaults picked from the short list (lane DEF), the clock, the stored picks.
static SelectableSources sourcesFor(const Pick& p, const std::string* store, bool withPicks = true) {
  SelectableSources s;
  s.storeText = store;
  s.coldStart = staticModelChoices(p.provider);
  ModelStoreMeta meta;
  const bool have = parseModelStoreHeader(*store, meta);
  const std::string list = effectiveModelChoices(p.provider, have ? meta.choices : std::string());
  s.defaultModel = roleDefaultModel(p.provider, DefaultRole::Head, list, have && !meta.choices.empty());
  s.subDefaultModel = roleDefaultModel(p.provider, DefaultRole::Sub, list, have && !meta.choices.empty());
  s.now = kOct22;
  if (withPicks) s.configured = {p.head, p.sub};
  return s;
}

// (b) Every stored pick passes the check for its role after the first fetch, and the
// provider's list itself names it (so no "no longer names" notice is due for Lumi).
static void test_every_stored_pick_passes_for_its_role() {
  for (const Pick& p : kLumi) {
    const std::string store = storeFor(p);
    const SelectableSources withPicks = sourcesFor(p, &store);
    const SelectableSources listOnly = sourcesFor(p, &store, false);
    const std::string tag = std::string(p.provider) + " " + p.head + "/" + p.sub;
    TEST_ASSERT_TRUE_MESSAGE(modelSelectable(p.provider, p.head, PickRole::Head, withPicks), tag.c_str());
    TEST_ASSERT_TRUE_MESSAGE(modelSelectable(p.provider, p.sub, PickRole::Sub, withPicks), tag.c_str());
    TEST_ASSERT_TRUE_MESSAGE(modelSelectable(p.provider, p.sub, PickRole::Any, withPicks), tag.c_str());
    TEST_ASSERT_TRUE_MESSAGE(modelListed(p.provider, p.head, PickRole::Head, listOnly), tag.c_str());
    TEST_ASSERT_TRUE_MESSAGE(modelListed(p.provider, p.sub, PickRole::Sub, listOnly), tag.c_str());
    // A re-saved provider card posts both picks unchanged: accepted.
    TEST_ASSERT_TRUE_MESSAGE(agent::model_pick::allowed(p.provider, p.head, PickRole::Head, withPicks), tag.c_str());
    TEST_ASSERT_TRUE_MESSAGE(agent::model_pick::allowed(p.provider, p.sub, PickRole::Sub, withPicks), tag.c_str());
  }
  // Cumulo's bare id is the router's openai/gpt-5.6 row (and its role default).
  const std::string router = storeFor(kLumi[1]);
  TEST_ASSERT_TRUE(router.find("\nopenai/gpt-5.6\t") != std::string::npos);
}

// ---- the spawn path: JobEngine with Lumi's configuration ---------------------------
struct NullStore : JournalStore {
  std::map<int, std::string> slots;
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  bool put(int slot, const std::string& v) override {
    slots[slot] = v;
    return true;
  }
  bool remove(int slot) override {
    slots.erase(slot);
    return true;
  }
  void clearNs() override { slots.clear(); }
};

struct LumiJobs {
  harness_test::FakePlatform plat;
  harness_test::FakeAdapter adapters[4];
  agent::HeavyFabric fabric;
  NullStore store;
  Journal journal;
  std::map<std::string, std::string> stores;
  std::vector<std::pair<std::string, std::string>> delivered;
  std::unique_ptr<agent::JobEngine> eng;
  LumiJobs() {
    const char* backs[] = {"openai", "anthropic", "mistral", "cumulo"};
    for (int i = 0; i < 4; ++i) {
      adapters[i].backend = backs[i];
      fabric.registerAdapter(&adapters[i]);
    }
    for (const Pick& p : kLumi) stores[p.provider] = storeFor(p);
    journal.begin(&store);
    agent::JobEngine::Deps d;
    d.platform = plat.contract();
    d.fabric = &fabric;
    d.journal = &journal;
    d.deliver = [this](const std::string& c, const std::string& t) {
      delivered.push_back({c, t});
      return true;
    };
    d.backendHue = [](const char*) { return (uint8_t)1; };
    d.subPriority = [] { return std::string("openai,mistral,anthropic"); };
    d.providerHasKey = [](const std::string& p) { return p != "zai"; };
    d.subModel = [](const std::string& p) {
      for (const Pick& k : kLumi)
        if (p == k.provider) return std::string(k.sub);
      return std::string();
    };
    // The device's modelIsValid (orchestrator.cpp): the one check, PickRole::Any.
    d.modelIsValid = [this](const std::string& p, const std::string& m) {
      for (const Pick& k : kLumi)
        if (p == k.provider) return modelSelectable(p, m, PickRole::Any, sourcesFor(k, &stores[p]));
      return false;
    };
    d.modelChoices = [this](const std::string& p) {
      for (const Pick& k : kLumi)
        if (p == k.provider) return shortListOf(k, stores[p]);
      return std::string();
    };
    eng.reset(new agent::JobEngine(std::move(d)));
  }
  const harness_test::FakeAdapter& adapter(const std::string& b) const {
    for (const auto& a : adapters)
      if (a.backend == b) return a;
    return adapters[0];
  }
};

// (b) for the spawn path: a helper started with no model named runs the stored sub on
// every keyed provider. The cumulo case is the one that failed before lane F2: the
// spawn's default step compared the bare "gpt-5.6" with the router's short list
// ("openai/gpt-5.6") as text and ran the cheapest listed model instead.
static void test_spawns_with_no_model_named_run_the_stored_sub() {
  for (const char* prov : {"openai", "anthropic", "mistral", "cumulo"}) {
    LumiJobs j;
    Spawn s;
    s.task = "check the weather";
    s.name = "helper";
    s.provider = prov;
    j.eng->enqueueSpawn(s, "1001");
    j.eng->pump();
    const auto& got = j.adapter(prov).dispatched;
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)got.size(), prov);
    const Pick* k = nullptr;
    for (const Pick& p : kLumi)
      if (std::string(p.provider) == prov) k = &p;
    TEST_ASSERT_EQUAL_STRING_MESSAGE(k->sub, got[0].model.c_str(), prov);
    TEST_ASSERT_TRUE_MESSAGE(j.eng->spawnLedgerText("1001").find("cheapest model it lists") == std::string::npos,
                             j.eng->spawnLedgerText("1001").c_str());
  }
  // The pre-F2 rule, for the record: the bare id is not in the router short list.
  const std::string router = storeFor(kLumi[1]);
  TEST_ASSERT_FALSE(modelInChoices(shortListOf(kLumi[1], router), "gpt-5.6"));
  TEST_ASSERT_TRUE(modelInChoices(shortListOf(kLumi[1], router), "openai/gpt-5.6"));
}

// (c) A stored pick that a later list drops: it keeps running and passing (never
// refused: turns, spawns and a re-saved card), and modelListed says the list no longer
// names it - the condition for the one-time owner notice.
static void test_a_dropped_stored_pick_keeps_running() {
  const Pick& anth = kLumi[3];
  std::string store = storeFor(anth);
  const size_t at = store.find("\nclaude-sonnet-5\t");
  TEST_ASSERT_TRUE(at != std::string::npos);
  store.erase(at + 1, store.find('\n', at + 1) - at);   // the next fetch no longer lists it
  const SelectableSources withPicks = sourcesFor(anth, &store);
  SelectableSources listOnly = sourcesFor(anth, &store, false);
  listOnly.defaultModel.clear();   // the role default happens to be the same id: set it aside
  TEST_ASSERT_TRUE(modelSelectable("anthropic", "claude-sonnet-5", PickRole::Head, withPicks));
  TEST_ASSERT_TRUE(modelSelectable("anthropic", "claude-sonnet-5", PickRole::Sub, withPicks));
  TEST_ASSERT_TRUE(agent::model_pick::allowed("anthropic", "claude-sonnet-5", PickRole::Head, withPicks));
  TEST_ASSERT_FALSE(modelListed("anthropic", "claude-sonnet-5", PickRole::Head, listOnly));
  // Counter-test: a model the owner never stored and no list names is still refused.
  TEST_ASSERT_FALSE(modelSelectable("anthropic", "claude-imaginary-9", PickRole::Head, withPicks));
}

// The served-unlisted alias: OpenAI serves gpt-5.6 (answering as gpt-5.6-sol) but does
// not list it. Accepted while the store offers gpt-5.6-sol; refused when it does not
// (and the owner did not store it).
static void test_probed_alias_is_accepted_while_its_model_is_offered() {
  const Pick& oai = kLumi[0];
  std::string store = storeFor(oai);
  SelectableSources s = sourcesFor(oai, &store, false);
  s.defaultModel.clear();
  s.subDefaultModel.clear();
  TEST_ASSERT_TRUE(store.find("\ngpt-5.6\t") == std::string::npos);   // not listed
  TEST_ASSERT_TRUE(modelSelectable("openai", "gpt-5.6", PickRole::Head, s));
  const size_t at = store.find("\ngpt-5.6-sol\t");
  store.erase(at + 1, store.find('\n', at + 1) - at);
  TEST_ASSERT_FALSE(modelSelectable("openai", "gpt-5.6", PickRole::Head, s));
  // Through the router the alias is the upstream's: openai/gpt-5.6 is listed there anyway.
  const std::string router = storeFor(kLumi[1]);
  const SelectableSources r = sourcesFor(kLumi[1], &router, false);
  TEST_ASSERT_TRUE(modelSelectable("cumulo", "openai/gpt-5.6", PickRole::Head, r));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_stored_pick_passes_for_its_role);
  RUN_TEST(test_spawns_with_no_model_named_run_the_stored_sub);
  RUN_TEST(test_a_dropped_stored_pick_keeps_running);
  RUN_TEST(test_probed_alias_is_accepted_while_its_model_is_offered);
  UNITY_END();
  return 0;
}
