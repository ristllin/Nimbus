#include <unity.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

// Lane F: THE "is this model allowed" check. The parity class: every id the device
// lists - the prompt's short list, the picker's rows for each role - passes the
// orch_model and spawn checks (Deps::modelIsValid, PickRole::Any) and the web pick
// (model_pick::allowed for that role), and a model it does not list is refused, over
// every Oct 2026 provider list and the router's. The device asks the same function
// over its cached store text (store::modelSelectable).
#include "../../src/agent/agent_config.h"   // the compiled default per provider
#include "../../src/agent/model_pick.h"
#include "../../src/agent/router_catalog.h"
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_selectable.h"

using namespace nimbus::orch;
namespace rc = agent::router_catalog;

void setUp() {}
void tearDown() {}

static const uint32_t kOct22 = 1792670400u;   // 2026-10-22T12:00:00Z

static std::string readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static const char* defaultFor(const std::string& p) {
  if (p == "openai") return OPENAI_MODEL;
  if (p == "anthropic") return ANT_MODEL;
  if (p == "mistral") return MISTRAL_MODEL;
  if (p == "cumulo") return CUMULO_MODEL;
  if (p == "zai") return ZAI_MODEL;
  return "";
}

// The store a verify writes from a recorded list (the device's storeModelHarvest).
static std::string storeFrom(const std::string& provider, const std::string& body, uint32_t now) {
  ModelHarvest h;
  h.provider = provider;
  h.now = now;
  harvestBody(h, body.data(), body.size());
  finishHarvest(h);
  size_t more = provider == "cumulo" ? rc::protectHeads(h.rows, CUMULO_MODEL) : 0;
  more += boundModelRows(h.rows);
  ModelStoreMeta meta;
  meta.fetchedAt = now;
  meta.total = (uint32_t)(h.rows.size() + more);
  meta.more = (uint32_t)more;
  meta.choices = provider == "cumulo" ? rc::storeChoices(h.rows, CUMULO_MODEL, now)
                                      : directChoices(provider, h.rows, now);
  return serializeModelStore(meta, h.rows);
}

static SelectableSources sourcesFor(const std::string& provider, const std::string* text, uint32_t now) {
  SelectableSources s;
  s.storeText = text;
  s.coldStart = staticModelChoices(provider.c_str());
  s.defaultModel = defaultFor(provider);
  s.now = now;
  return s;
}

using nimbus::orch::choiceItems;

// The picker's ids for a role: GET /api/models rows (modelRowToJson) carrying it, as
// the web page picks them (a router row's pick is its selector).
static std::vector<std::string> pickerIds(const std::string& provider, const std::string& text,
                                          const char* role, uint32_t now) {
  std::vector<std::string> out;
  forEachStoreRow(provider, text, [&](const ModelInfo& m) {
    JsonDocument d;
    modelRowToJson(provider, m, d.to<JsonObject>(), now);
    for (JsonVariantConst r : d["roles"].as<JsonArrayConst>())
      if (std::string(r | "") == role && (d["usable"] | false)) out.push_back(modelSelector(m));
    return true;
  });
  return out;
}

struct Fixture {
  const char* provider;
  const char* path;
};
static const Fixture kFixtures[] = {
    {"anthropic", "test/support/fixtures/models_2026-10/anthropic.json"},
    {"openai", "test/support/fixtures/models_2026-10/openai.json"},
    {"mistral", "test/support/fixtures/models_2026-10/mistral.json"},
    {"zai", "test/support/fixtures/models_2026-10/zai.json"},
    {"cumulo", "test/support/fixtures/router_models_2026-10-13.json"},
};

// THE parity class: listed means accepted, by every check, for every provider.
static void test_every_listed_id_passes_every_check() {
  for (const Fixture& fx : kFixtures) {
    const std::string p = fx.provider;
    const std::string text = storeFrom(p, readFile(fx.path), kOct22);
    const SelectableSources src = sourcesFor(p, &text, kOct22);
    ModelStoreMeta meta;
    TEST_ASSERT_TRUE(parseModelStoreHeader(text, meta));
    // The prompt's [AVAILABLE MODELS] line: the effective short list.
    const std::string shortList = effectiveModelChoices(p.c_str(), meta.choices);
    TEST_ASSERT_FALSE_MESSAGE(shortList.empty(), fx.provider);
    for (const std::string& id : choiceItems(shortList)) {
      const std::string tag = p + ": " + id;
      TEST_ASSERT_TRUE_MESSAGE(modelSelectable(p, id, PickRole::Any, src), tag.c_str());    // orch_model, spawn
      TEST_ASSERT_TRUE_MESSAGE(agent::model_pick::allowed(p, id, PickRole::Head, src), tag.c_str());
      TEST_ASSERT_TRUE_MESSAGE(agent::model_pick::allowed(p, id, PickRole::Sub, src), tag.c_str());
    }
    // The picker, per role.
    const std::vector<std::string> heads = pickerIds(p, text, "orchestrator", kOct22);
    const std::vector<std::string> subs = pickerIds(p, text, "sub-agent", kOct22);
    TEST_ASSERT_TRUE_MESSAGE(heads.size() >= choiceItems(shortList).size(), fx.provider);
    for (const std::string& id : heads) {
      TEST_ASSERT_TRUE_MESSAGE(agent::model_pick::allowed(p, id, PickRole::Head, src), (p + ": " + id).c_str());
      TEST_ASSERT_TRUE_MESSAGE(modelSelectable(p, id, PickRole::Any, src), (p + ": " + id).c_str());
    }
    for (const std::string& id : subs)
      TEST_ASSERT_TRUE_MESSAGE(agent::model_pick::allowed(p, id, PickRole::Sub, src), (p + ": " + id).c_str());
    // The compiled default always (it can be re-picked by name).
    TEST_ASSERT_TRUE_MESSAGE(modelSelectable(p, defaultFor(p), PickRole::Head, src), fx.provider);
  }
}

// And the converse: what the store holds but the checks refuse is never listed, and
// for a direct provider a stored row is accepted exactly when rowOffered() says so.
static void test_accepted_if_and_only_if_offered() {
  for (const Fixture& fx : kFixtures) {
    const std::string p = fx.provider;
    if (p == "cumulo") continue;   // router rows compare by model across spellings (below)
    const std::string text = storeFrom(p, readFile(fx.path), kOct22);
    const SelectableSources src = sourcesFor(p, &text, kOct22);
    std::set<std::string> heads;
    for (const std::string& id : pickerIds(p, text, "orchestrator", kOct22)) heads.insert(id);
    forEachStoreRow(p, text, [&](const ModelInfo& m) {
      if (m.id == defaultFor(p)) return true;
      const bool ok = modelSelectable(p, m.id, PickRole::Head, src);
      TEST_ASSERT_EQUAL_MESSAGE(rowOffered(m, RoleOrchestrator, kOct22), ok, (p + ": " + m.id).c_str());
      TEST_ASSERT_EQUAL_MESSAGE(heads.count(m.id) > 0, ok, (p + ": " + m.id).c_str());
      return true;
    });
  }
}

// Refusals the old split checks let through or got wrong.
static void test_refusals() {
  const std::string openai = storeFrom("openai", readFile(kFixtures[1].path), kOct22);
  const SelectableSources so = sourcesFor("openai", &openai, kOct22);
  TEST_ASSERT_FALSE(modelSelectable("openai", "gpt-audio-mini", PickRole::Any, so));        // non-chat family
  TEST_ASSERT_FALSE(modelSelectable("openai", "text-embedding-3-small", PickRole::Head, so));
  TEST_ASSERT_FALSE(modelSelectable("openai", "gpt-5.1-codex", PickRole::Any, so));         // past shutdown
  TEST_ASSERT_FALSE(modelSelectable("openai", "gpt-9-imaginary", PickRole::Any, so));
  TEST_ASSERT_FALSE(modelSelectable("openai", "", PickRole::Any, so));
  TEST_ASSERT_TRUE(modelSelectable("openai", OPENAI_MODEL, PickRole::Any, so));            // the head default
  // A row the usability probe marked unusable is refused (and its picker row loses the role).
  ModelStoreMeta meta;
  std::vector<ModelInfo> rows;
  parseModelStore("openai", openai, meta, &rows);
  for (ModelInfo& m : rows)
    if (m.id == "gpt-6-sol") { m.usable = false; m.probed = true; }
  const std::string probed = serializeModelStore(meta, rows);
  const SelectableSources sp = sourcesFor("openai", &probed, kOct22);
  TEST_ASSERT_FALSE(modelSelectable("openai", "gpt-6-sol", PickRole::Any, sp));
  TEST_ASSERT_TRUE(modelSelectable("openai", "gpt-6.1-sol", PickRole::Any, sp));
  // The router: a selector no upstream runs, a :batch twin, and a model not listed.
  const std::string router = storeFrom("cumulo", readFile(kFixtures[4].path), kOct22);
  const SelectableSources sr = sourcesFor("cumulo", &router, kOct22);
  TEST_ASSERT_FALSE(modelSelectable("cumulo", "mistral/mistral/mistral-large-3", PickRole::Any, sr));
  TEST_ASSERT_FALSE(modelSelectable("cumulo", "anthropic/claude-opus-5:batch", PickRole::Any, sr));
  TEST_ASSERT_FALSE(modelSelectable("cumulo", "anthropic/claude-nope-1", PickRole::Any, sr));
  // One model, both spellings, and the bare default form.
  TEST_ASSERT_TRUE(modelSelectable("cumulo", "anthropic/claude-opus-5-5", PickRole::Head, sr));
  TEST_ASSERT_TRUE(modelSelectable("cumulo", "anthropic/claude-opus-5.5", PickRole::Head, sr));
  TEST_ASSERT_TRUE(modelSelectable("cumulo", "openai/gpt-5.6", PickRole::Head, sr));
  TEST_ASSERT_TRUE(modelSelectable("cumulo", "gpt-5.6", PickRole::Head, sr));
}

// Before the first fetch (fresh device, no store): the cold-start list and the default,
// nothing else; a store with nothing runnable for the role falls back the same way.
static void test_cold_start_without_a_store() {
  for (const Fixture& fx : kFixtures) {
    const std::string p = fx.provider;
    const SelectableSources none = sourcesFor(p, nullptr, kOct22);
    for (const std::string& id : choiceItems(staticModelChoices(p.c_str())))
      TEST_ASSERT_TRUE_MESSAGE(modelSelectable(p, id, PickRole::Any, none), (p + ": " + id).c_str());
    TEST_ASSERT_TRUE(modelSelectable(p, defaultFor(p), PickRole::Head, none));
    TEST_ASSERT_FALSE(modelSelectable(p, "not-a-model-x", PickRole::Any, none));
  }
  ModelInfo emb = classifyModel("openai", "text-embedding-3-small");
  const std::string onlyEmb = serializeModelStore(ModelStoreMeta{}, {emb});
  const SelectableSources se = sourcesFor("openai", &onlyEmb, kOct22);
  TEST_ASSERT_TRUE(modelSelectable("openai", "gpt-6-astra", PickRole::Any, se));   // cold-start list
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_listed_id_passes_every_check);
  RUN_TEST(test_accepted_if_and_only_if_offered);
  RUN_TEST(test_refusals);
  RUN_TEST(test_cold_start_without_a_store);
  UNITY_END();
  return 0;
}
