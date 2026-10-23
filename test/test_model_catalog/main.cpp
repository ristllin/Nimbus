#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/orch/model_catalog.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// Fixtures are REAL /v1/models responses recorded 2026-08-23 (GET is free, no
// token spend), under test/support/fixtures/models/. The Anthropic capture had
// one preview model removed for public-repo hygiene; every other entry is verbatim.
static const char* kFixDir = "test/support/fixtures/models";

static std::string readFixture(const char* name) {
  std::string path = std::string(kFixDir) + "/" + name;
  FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static const ModelInfo* find(const std::vector<ModelInfo>& v, const std::string& id) {
  for (const ModelInfo& m : v)
    if (m.id == id) return &m;
  return nullptr;
}
static bool present(const std::vector<ModelInfo>& v, const std::string& id) {
  return find(v, id) != nullptr;
}

// ---- pure classifier --------------------------------------------------------
static void test_size_class_by_family() {
  TEST_ASSERT_EQUAL_CHAR('L', modelSizeClass("anthropic", "claude-opus-5"));
  TEST_ASSERT_EQUAL_CHAR('M', modelSizeClass("anthropic", "claude-sonnet-5"));
  TEST_ASSERT_EQUAL_CHAR('S', modelSizeClass("anthropic", "claude-haiku-4-5"));
  TEST_ASSERT_EQUAL_CHAR('L', modelSizeClass("mistral", "mistral-large-latest"));
  TEST_ASSERT_EQUAL_CHAR('M', modelSizeClass("mistral", "mistral-medium-latest"));
  TEST_ASSERT_EQUAL_CHAR('S', modelSizeClass("mistral", "mistral-small-latest"));
  // gpt-5.6 tier words (Sol/Terra/Luna) rank like the generic size words.
  TEST_ASSERT_EQUAL_CHAR('S', modelSizeClass("openai", "gpt-5.6-luna"));
  TEST_ASSERT_EQUAL_CHAR('M', modelSizeClass("openai", "gpt-5.6-terra"));
  TEST_ASSERT_EQUAL_CHAR('L', modelSizeClass("openai", "gpt-5.6-sol"));
  TEST_ASSERT_EQUAL_CHAR('L', modelSizeClass("openai", "gpt-5.6"));  // bare id = flagship default
  TEST_ASSERT_EQUAL_CHAR('S', modelSizeClass("openai", "o3-mini"));
  TEST_ASSERT_EQUAL_CHAR('S', modelSizeClass("zai", "glm-4.5-air"));
  TEST_ASSERT_EQUAL_CHAR('M', modelSizeClass("zai", "glm-5-turbo"));
  TEST_ASSERT_EQUAL_CHAR('L', modelSizeClass("zai", "glm-5"));
}

static void test_family_bucket() {
  TEST_ASSERT_EQUAL_STRING("claude-opus", modelFamily("anthropic", "claude-opus-5").c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-5", modelFamily("openai", "gpt-5.6-luna").c_str());
  TEST_ASSERT_EQUAL_STRING("o-series", modelFamily("openai", "o3").c_str());
  TEST_ASSERT_EQUAL_STRING("glm", modelFamily("zai", "glm-5").c_str());
}

static void test_role_classification_by_id() {
  ModelInfo chat = classifyModel("openai", "gpt-5.6-luna");
  TEST_ASSERT_TRUE(chat.hasRole(RoleOrchestrator));
  TEST_ASSERT_TRUE(chat.hasRole(RoleSubAgent));
  TEST_ASSERT_TRUE(chat.hasCap(CapTools));
  ModelInfo emb = classifyModel("openai", "text-embedding-3-large");
  TEST_ASSERT_TRUE(emb.hasRole(RoleEmbedding));
  TEST_ASSERT_FALSE(emb.hasRole(RoleOrchestrator));
  ModelInfo instruct = classifyModel("openai", "gpt-3.5-turbo-instruct");
  TEST_ASSERT_EQUAL_UINT16(0, instruct.roles);  // non-agentic: no role
}

// ---- GPT generation class (the invariant, not the instance) -----------------
// gpt-6-astra (OpenAI, 2026-09-03) shipped while every heuristic was keyed on the
// literal "gpt-5" prefix: no size class, no family, no vision role, the default
// context window, and - because the device harvest lists only preferred ids -
// absent from the orchestrator / sub-session dropdowns entirely. Encode the class
// rule: EVERY "gpt-<N>" generation from 5 up is a flagship-family, L-class,
// vision-capable chat model bucketed "gpt-<N>", so the next generation cannot
// silently regress to "unclassified" the way gpt-6 did.
static void test_gpt_generation_parse() {
  TEST_ASSERT_EQUAL_INT(6, gptGeneration("gpt-6-astra"));
  TEST_ASSERT_EQUAL_INT(5, gptGeneration("gpt-5.5"));
  TEST_ASSERT_EQUAL_INT(5, gptGeneration("GPT-5-chat-latest"));  // case-insensitive
  TEST_ASSERT_EQUAL_INT(4, gptGeneration("gpt-4o-mini"));
  TEST_ASSERT_EQUAL_INT(4, gptGeneration("gpt-4.1"));
  TEST_ASSERT_EQUAL_INT(3, gptGeneration("gpt-3.5-turbo"));
  TEST_ASSERT_EQUAL_INT(0, gptGeneration("gpt-realtime"));   // no generation digit
  TEST_ASSERT_EQUAL_INT(0, gptGeneration("gpt-image-1"));
  TEST_ASSERT_EQUAL_INT(0, gptGeneration("o4-mini"));
  TEST_ASSERT_EQUAL_INT(0, gptGeneration("claude-opus-5"));
  TEST_ASSERT_EQUAL_INT(0, gptGeneration(""));
  TEST_ASSERT_EQUAL_INT(0, gptGeneration("gpt-20260903"));  // a date run is not a generation
}

static void test_gpt_generation_class_rule() {
  for (int gen = 5; gen <= 9; ++gen) {
    const std::string id = "gpt-" + std::to_string(gen) + "-x";
    const std::string fam = "gpt-" + std::to_string(gen);
    TEST_ASSERT_TRUE_MESSAGE(isFlagshipFamily("openai", id), id.c_str());
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('L', modelSizeClass("openai", id), id.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(fam.c_str(), modelFamily("openai", id).c_str(), id.c_str());
    ModelInfo mi = classifyModel("openai", id);
    TEST_ASSERT_TRUE_MESSAGE(mi.hasRole(RoleOrchestrator), id.c_str());
    TEST_ASSERT_TRUE_MESSAGE(mi.hasRole(RoleSubAgent), id.c_str());
    TEST_ASSERT_TRUE_MESSAGE(mi.hasRole(RoleVision), id.c_str());
    TEST_ASSERT_TRUE_MESSAGE(mi.hasCap(CapTools), id.c_str());
    // A tier word still wins over the flagship default within the generation.
    TEST_ASSERT_EQUAL_CHAR('S', modelSizeClass("openai", fam + "-mini"));
  }
  // Pre-5 and non-gpt ids stay non-flagship (usable, just not preferred).
  TEST_ASSERT_FALSE(isFlagshipFamily("openai", "gpt-4o"));
  TEST_ASSERT_FALSE(isFlagshipFamily("openai", "gpt-4.1"));
  TEST_ASSERT_FALSE(isFlagshipFamily("openai", "o4-mini"));
  TEST_ASSERT_FALSE(isFlagshipFamily("openai", "gpt-realtime"));
  TEST_ASSERT_TRUE(isFlagshipFamily("zai", "glm-5-turbo"));
  TEST_ASSERT_FALSE(isFlagshipFamily("zai", "glm-4.6"));
  TEST_ASSERT_FALSE(isFlagshipFamily("anthropic", "claude-opus-5"));
}

// The instance: gpt-6-astra classifies as a fully usable flagship and sorts
// AHEAD of the gpt-5.x and gpt-4o ids in a harvested list, so the newest
// generation is the first orchestrator / sub-session candidate offered.
static void test_gpt6_astra_is_a_selectable_flagship() {
  const std::string body =
      "{\"data\":[{\"id\":\"gpt-4o\"},{\"id\":\"gpt-5.5\"},{\"id\":\"gpt-6-astra\"},"
      "{\"id\":\"gpt-4o-mini\"}]}";
  std::vector<ModelInfo> v;
  TEST_ASSERT_EQUAL_UINT(4, parseModelsList("openai", body, v));
  const ModelInfo* astra = find(v, "gpt-6-astra");
  TEST_ASSERT_NOT_NULL(astra);
  TEST_ASSERT_TRUE(astra->hasRole(RoleOrchestrator));
  TEST_ASSERT_TRUE(astra->hasRole(RoleSubAgent));
  TEST_ASSERT_TRUE(astra->hasRole(RoleVision));
  TEST_ASSERT_TRUE(astra->hasCap(CapTools));
  TEST_ASSERT_TRUE(astra->hasCap(CapJson));
  TEST_ASSERT_TRUE(astra->usable);
  TEST_ASSERT_EQUAL_CHAR('L', astra->size);
  TEST_ASSERT_EQUAL_STRING("gpt-6", astra->family.c_str());
  TEST_ASSERT_EQUAL_UINT32(922000, astra->ctxTokens);
  // Flagship-first: an L model leads, and gpt-6-astra precedes every non-L id
  // (gpt-4o-mini is S; bare gpt-4o carries no size signal and ranks last).
  TEST_ASSERT_EQUAL_CHAR('L', v.front().size);
  size_t iAstra = 0, iMini = 0, i4o = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i].id == "gpt-6-astra") iAstra = i;
    if (v[i].id == "gpt-4o-mini") iMini = i;
    if (v[i].id == "gpt-4o") i4o = i;
  }
  TEST_ASSERT_TRUE(iAstra < iMini);
  TEST_ASSERT_TRUE(iAstra < i4o);
}

// ---- Anthropic fixture: API-supplied capability fields ----------------------
static void test_anthropic_reads_api_capabilities() {
  std::vector<ModelInfo> v;
  size_t n = parseModelsList("anthropic", readFixture("anthropic.json"), v);
  TEST_ASSERT_EQUAL_UINT(9, n);
  const ModelInfo* opus = find(v, "claude-opus-5");
  TEST_ASSERT_NOT_NULL(opus);
  TEST_ASSERT_TRUE(opus->apiCaps);                       // came from the API, not heuristics
  TEST_ASSERT_EQUAL_UINT32(1000000, opus->ctxTokens);   // max_input_tokens
  TEST_ASSERT_EQUAL_UINT32(128000, opus->maxOutTokens); // max_tokens
  TEST_ASSERT_TRUE(opus->hasRole(RoleVision));           // capabilities.image_input
  TEST_ASSERT_TRUE(opus->hasCap(CapJson));               // capabilities.structured_outputs
  TEST_ASSERT_EQUAL_CHAR('L', opus->size);
  TEST_ASSERT_TRUE(opus->hasRole(RoleOrchestrator));
  const ModelInfo* haiku = find(v, "claude-haiku-4-5-20251001");
  TEST_ASSERT_NOT_NULL(haiku);
  TEST_ASSERT_EQUAL_CHAR('S', haiku->size);
  // No forbidden preview model leaked into the fixture.
  TEST_ASSERT_FALSE(present(v, "claude-fable-5"));
}

// ---- OpenAI fixture: id-family heuristics + role split ----------------------
static void test_openai_heuristics_and_roles() {
  std::vector<ModelInfo> v;
  parseModelsList("openai", readFixture("openai.json"), v);
  const ModelInfo* luna = find(v, "gpt-5.6-luna");
  TEST_ASSERT_NOT_NULL(luna);
  TEST_ASSERT_FALSE(luna->apiCaps);                  // OpenAI /v1/models is id-only
  TEST_ASSERT_TRUE(luna->hasRole(RoleOrchestrator));
  TEST_ASSERT_TRUE(luna->hasCap(CapTools));
  TEST_ASSERT_TRUE(luna->hasRole(RoleVision));       // gpt-5 multimodal heuristic
  TEST_ASSERT_EQUAL_CHAR('S', luna->size);  // Luna = the fastest/cheapest 5.6 tier
  // embedding / audio / image models land under their own roles...
  TEST_ASSERT_TRUE(find(v, "text-embedding-3-large")->hasRole(RoleEmbedding));
  TEST_ASSERT_TRUE(find(v, "gpt-4o-transcribe")->hasRole(RoleStt));
  TEST_ASSERT_TRUE(find(v, "tts-1-hd")->hasRole(RoleTts));
  TEST_ASSERT_TRUE(find(v, "gpt-image-1")->hasRole(RoleImage));
  const ModelInfo* rt = find(v, "gpt-realtime");
  TEST_ASSERT_NOT_NULL(rt);
  TEST_ASSERT_TRUE(rt->hasRole(RoleStt) && rt->hasRole(RoleTts));
  // ...and non-agentic text families are dropped entirely (no cap-8, but no junk).
  TEST_ASSERT_FALSE(present(v, "gpt-3.5-turbo-instruct"));
  TEST_ASSERT_FALSE(present(v, "o4-mini-deep-research"));
  // The 8-id cap is gone: many more than 8 orchestrator-capable ids survive.
  int orch = 0;
  for (const ModelInfo& m : v)
    if (m.hasRole(RoleOrchestrator)) orch++;
  TEST_ASSERT_TRUE(orch > 8);
}

// ---- Mistral fixture: capabilities metadata + alias dedup -------------------
static void test_mistral_metadata_and_alias_dedup() {
  std::vector<ModelInfo> v;
  parseModelsList("mistral", readFixture("mistral.json"), v);
  const ModelInfo* large = find(v, "mistral-large-latest");   // canonicalized from -2512
  TEST_ASSERT_NOT_NULL(large);
  TEST_ASSERT_TRUE(large->apiCaps);
  TEST_ASSERT_TRUE(large->hasRole(RoleVision));   // capabilities.vision
  TEST_ASSERT_TRUE(large->hasCap(CapTools));      // capabilities.function_calling
  TEST_ASSERT_EQUAL_CHAR('L', large->size);
  TEST_ASSERT_FALSE(present(v, "mistral-large-2512"));  // dated dupe folded into -latest
  // embeddings kept under their own role, chat-only OCR/instruct dropped.
  const ModelInfo* emb = find(v, "mistral-embed");
  TEST_ASSERT_NOT_NULL(emb);
  TEST_ASSERT_TRUE(emb->hasRole(RoleEmbedding));
  TEST_ASSERT_FALSE(emb->hasRole(RoleOrchestrator));
  TEST_ASSERT_FALSE(present(v, "mistral-ocr-latest"));  // ocr: not a chat turn
  // A single canonical entry per family - no duplicate -latest.
  int large_count = 0;
  for (const ModelInfo& m : v)
    if (m.id == "mistral-large-latest") large_count++;
  TEST_ASSERT_EQUAL_INT(1, large_count);
}

// ---- Z.ai (GLM) fixture: OpenAI-compatible id list --------------------------
static void test_zai_glm_roles_and_sizes() {
  std::vector<ModelInfo> v;
  size_t n = parseModelsList("zai", readFixture("zai.json"), v);
  TEST_ASSERT_EQUAL_UINT(9, n);
  const ModelInfo* glm5 = find(v, "glm-5");
  TEST_ASSERT_NOT_NULL(glm5);
  TEST_ASSERT_TRUE(glm5->hasRole(RoleOrchestrator));
  TEST_ASSERT_TRUE(glm5->hasCap(CapTools));
  TEST_ASSERT_EQUAL_CHAR('L', glm5->size);
  TEST_ASSERT_EQUAL_CHAR('S', find(v, "glm-4.5-air")->size);
  TEST_ASSERT_EQUAL_CHAR('M', find(v, "glm-5-turbo")->size);
}

// ---- flagship-first ordering ------------------------------------------------
static void test_flagship_first_ordering() {
  std::vector<ModelInfo> v;
  parseModelsList("anthropic", readFixture("anthropic.json"), v);
  // The first orchestrator-capable model must be an L-class flagship.
  bool sawL = false;
  for (const ModelInfo& m : v) {
    if (!m.hasRole(RoleOrchestrator)) continue;
    if (m.size == 'L') { sawL = true; break; }
    if (m.size == 'S') TEST_FAIL_MESSAGE("an S model preceded every L model");
    break;
  }
  TEST_ASSERT_TRUE(sawL);
}

// ---- Cumulo router upstream split -------------------------------------------
static void test_cumulo_upstream_split() {
  // Cumulo router ids arrive prefixed "<upstream>/<model>".
  std::string body =
      "{\"data\":[{\"id\":\"anthropic/claude-opus-5\"},{\"id\":\"openai/gpt-5.6-luna\"}]}";
  std::vector<ModelInfo> v;
  parseModelsList("cumulo", body, v);
  const ModelInfo* a = find(v, "claude-opus-5");
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL_STRING("anthropic", a->upstream.c_str());
  TEST_ASSERT_TRUE(a->hasRole(RoleOrchestrator));
}

// GET /router/models (the router's routable catalog) rides a size_class
// metadata field per model; API-supplied size overrides the id heuristic.
static void test_cumulo_size_class_metadata_overrides_heuristic() {
  std::string body =
      "{\"data\":["
      "{\"id\":\"openai/experimental-chat-x\",\"size_class\":\"medium\"},"
      "{\"id\":\"openai/gpt-5.6-luna\",\"size_class\":\"small\"},"
      "{\"id\":\"anthropic/claude-opus-5\"}]}";
  std::vector<ModelInfo> v;
  TEST_ASSERT_EQUAL_UINT(3, parseModelsList("cumulo", body, v));
  const ModelInfo* x = find(v, "experimental-chat-x");
  TEST_ASSERT_NOT_NULL(x);
  TEST_ASSERT_EQUAL_CHAR('M', x->size);  // the heuristic alone gives no size here
  TEST_ASSERT_TRUE(x->apiCaps);
  TEST_ASSERT_EQUAL_CHAR('S', find(v, "gpt-5.6-luna")->size);
  // No metadata -> the id heuristic still classifies (opus = L, not api-tagged).
  const ModelInfo* opus = find(v, "claude-opus-5");
  TEST_ASSERT_EQUAL_CHAR('L', opus->size);
  TEST_ASSERT_FALSE(opus->apiCaps);
}

// CUM-242 x1 item 5: the harvest cap (8) is an ORDERING for the choices CSV, NOT
// a truncation of the routable catalog. parseModelsList (which feeds mcat_<prov>
// / GET /api/models) keeps EVERY routable id - here a 14-model multi-upstream
// Cumulo list comes back whole, flagship-first, with each upstream tag intact.
static void test_cumulo_catalog_is_uncapped_not_truncated() {
  std::string body =
      "{\"data\":["
      "{\"id\":\"openai/gpt-5.6\",\"size_class\":\"large\"},"
      "{\"id\":\"openai/gpt-5.6-terra\",\"size_class\":\"medium\"},"
      "{\"id\":\"openai/gpt-5.6-luna\",\"size_class\":\"small\"},"
      "{\"id\":\"openai/gpt-4o\",\"size_class\":\"medium\"},"
      "{\"id\":\"anthropic/claude-opus-5\",\"size_class\":\"large\"},"
      "{\"id\":\"anthropic/claude-sonnet-5\",\"size_class\":\"medium\"},"
      "{\"id\":\"anthropic/claude-haiku-4-5\",\"size_class\":\"small\"},"
      "{\"id\":\"mistral/mistral-large-latest\",\"size_class\":\"large\"},"
      "{\"id\":\"mistral/mistral-small-latest\",\"size_class\":\"small\"},"
      "{\"id\":\"mistral/codestral-latest\",\"size_class\":\"medium\"},"
      "{\"id\":\"zai/glm-5.3\",\"size_class\":\"large\"},"
      "{\"id\":\"zai/glm-5.2\",\"size_class\":\"medium\"},"
      "{\"id\":\"zai/glm-5.3-flash\",\"size_class\":\"small\"},"
      "{\"id\":\"openai/gpt-5.5\",\"size_class\":\"large\"}]}";
  std::vector<ModelInfo> v;
  size_t n = parseModelsList("cumulo", body, v);
  // All 14 survive - well past the 8-id choices cap.
  TEST_ASSERT_EQUAL_UINT(14, n);
  TEST_ASSERT_TRUE(v.size() > 8);
  // A late-listed row (past index 8) is still present and correctly tagged.
  const ModelInfo* glm = find(v, "glm-5.3-flash");
  TEST_ASSERT_NOT_NULL(glm);
  TEST_ASSERT_EQUAL_STRING("zai", glm->upstream.c_str());
  // Flagship-first ordering holds across the whole (uncapped) list.
  TEST_ASSERT_EQUAL_CHAR('L', v.front().size);
}

// CUM-242 leg 1: rebuild a catalog from a bare id list (the harvested CSV) when a
// full /models body did not parse. classifyCatalogEntry honours the Cumulo
// "<upstream>/<model>" convention and classifies a bare id like classifyModel.
static void test_classify_catalog_entry_from_bare_ids() {
  ModelInfo prefixed = classifyCatalogEntry("cumulo", "anthropic/claude-opus-5");
  TEST_ASSERT_EQUAL_STRING("anthropic", prefixed.upstream.c_str());
  TEST_ASSERT_TRUE(prefixed.hasRole(RoleOrchestrator));
  // A bare router id (the OpenAI upstream lists undecorated ids) classifies as the
  // openai family and stays a usable chat/orchestrator model.
  ModelInfo bare = classifyCatalogEntry("cumulo", "gpt-4o");
  TEST_ASSERT_TRUE(bare.hasRole(RoleOrchestrator));
  TEST_ASSERT_TRUE(bare.hasCap(CapTools));
}

// ---- serialization to the /api/models shape --------------------------------
static void test_models_to_json_shape_and_usable_filter() {
  std::vector<ModelInfo> v;
  parseModelsList("anthropic", readFixture("anthropic.json"), v);
  // Mark one model unusable (the usability probe verdict).
  for (ModelInfo& m : v)
    if (m.id == "claude-sonnet-5") m.usable = false;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  modelsToJson(v, arr, /*includeUnusable=*/false);
  // Re-parse the emitted array and check the shape of the first entry.
  std::string out;
  serializeJson(doc, out);
  JsonDocument re;
  deserializeJson(re, out);
  JsonArrayConst ra = re.as<JsonArrayConst>();
  TEST_ASSERT_TRUE(ra.size() > 0);
  JsonObjectConst first = ra[0];
  TEST_ASSERT_TRUE(first["roles"].is<JsonArrayConst>());
  TEST_ASSERT_TRUE(first["caps"]["tools"].is<bool>());
  TEST_ASSERT_TRUE(first["ctxTokens"].as<uint32_t>() > 0);
  TEST_ASSERT_TRUE(first["size"].is<const char*>());
  // usable=false model is omitted by default.
  bool sawUnusable = false;
  for (JsonObjectConst o : ra)
    if (std::string((const char*)(o["id"] | "")) == "claude-sonnet-5") sawUnusable = true;
  TEST_ASSERT_FALSE(sawUnusable);
  // ...but included when asked.
  JsonDocument doc2;
  JsonArray arr2 = doc2.to<JsonArray>();
  modelsToJson(v, arr2, /*includeUnusable=*/true);
  bool nowSeen = false;
  for (JsonObject o : arr2)
    if (std::string((const char*)(o["id"] | "")) == "claude-sonnet-5") nowSeen = true;
  TEST_ASSERT_TRUE(nowSeen);
}

// ---- robustness: HTTP headers ahead of the JSON body ------------------------
static void test_parse_tolerates_http_headers() {
  std::string body =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
      "{\"data\":[{\"id\":\"glm-5\"}]}";
  std::vector<ModelInfo> v;
  size_t n = parseModelsList("zai", body, v);
  TEST_ASSERT_EQUAL_UINT(1, n);
  TEST_ASSERT_TRUE(present(v, "glm-5"));
}

// ---- usability probe verdict ------------------------------------------------
static void test_probe_verdict() {
  TEST_ASSERT_TRUE(probeVerdict(200, "") == ProbeVerdict::Usable);
  // model-specific rejections hide the model
  TEST_ASSERT_TRUE(probeVerdict(404, "{\"error\":{\"code\":\"model_not_found\"}}") ==
                   ProbeVerdict::Unusable);
  TEST_ASSERT_TRUE(probeVerdict(400, "The model `x` does not exist") == ProbeVerdict::Unusable);
  // Real error bodies observed live 2026-08-23 from each provider's probe wire:
  TEST_ASSERT_TRUE(probeVerdict(400, "The requested model 'gpt-5-x' does not exist.") ==
                   ProbeVerdict::Unusable);                                   // OpenAI
  TEST_ASSERT_TRUE(probeVerdict(404, "{\"error\":{\"type\":\"not_found_error\",\"message\":"
                                     "\"model: claude-x\"}}") == ProbeVerdict::Unusable);  // Anthropic
  TEST_ASSERT_TRUE(probeVerdict(400, "Invalid model: mistral-x") == ProbeVerdict::Unusable);  // Mistral
  TEST_ASSERT_TRUE(probeVerdict(403, "your key does not have access to this model") ==
                   ProbeVerdict::Unusable);
  TEST_ASSERT_TRUE(probeVerdict(401, "unauthorized") == ProbeVerdict::Unusable);
  // transient / unrelated faults never demote a model
  TEST_ASSERT_TRUE(probeVerdict(429, "rate limit exceeded") == ProbeVerdict::Unknown);
  TEST_ASSERT_TRUE(probeVerdict(500, "internal error") == ProbeVerdict::Unknown);
  TEST_ASSERT_TRUE(probeVerdict(0, "connection reset") == ProbeVerdict::Unknown);
  TEST_ASSERT_TRUE(probeVerdict(400, "missing required field: messages") == ProbeVerdict::Unknown);
}

// ---- NVS round-trip: modelsToJson -> modelsFromJson preserves the catalog ----
static void test_models_json_round_trip() {
  std::vector<ModelInfo> v;
  parseModelsList("mistral", readFixture("mistral.json"), v);
  parseModelsList("cumulo", "{\"data\":[{\"id\":\"anthropic/claude-opus-5\"}]}", v);
  for (ModelInfo& m : v)
    if (m.id == "mistral-embed") m.usable = false;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  modelsToJson(v, arr, /*includeUnusable=*/true);   // persist everything
  std::string blob;
  serializeJson(doc, blob);

  JsonDocument re;
  deserializeJson(re, blob);
  std::vector<ModelInfo> back;
  size_t n = modelsFromJson(re.as<JsonArrayConst>(), back);
  TEST_ASSERT_EQUAL_UINT(v.size(), n);
  const ModelInfo* large = find(back, "mistral-large-latest");
  TEST_ASSERT_NOT_NULL(large);
  TEST_ASSERT_TRUE(large->hasRole(RoleOrchestrator));
  TEST_ASSERT_TRUE(large->hasRole(RoleVision));
  TEST_ASSERT_TRUE(large->hasCap(CapTools));
  TEST_ASSERT_EQUAL_CHAR('L', large->size);
  TEST_ASSERT_TRUE(large->apiCaps);
  const ModelInfo* emb = find(back, "mistral-embed");
  TEST_ASSERT_NOT_NULL(emb);
  TEST_ASSERT_FALSE(emb->usable);                 // usability verdict survives the round-trip
  TEST_ASSERT_TRUE(emb->hasRole(RoleEmbedding));
  const ModelInfo* cum = find(back, "claude-opus-5");
  TEST_ASSERT_NOT_NULL(cum);
  TEST_ASSERT_EQUAL_STRING("anthropic", cum->upstream.c_str());  // upstream tag survives
}

// CUM-530: a Cumulo router pick ("<upstream>/<model>", a bare id is openai) is the
// upstream's model for its family and size class, so the fallback rules' size class
// and the served-by family check read it like the direct pick. Every upstream.
static void test_router_pick_reads_as_its_upstream_model() {
  struct Pair { const char* routerPick; const char* upstream; const char* model; };
  static const Pair kPairs[] = {
      {"openai/gpt-5.6", "openai", "gpt-5.6"},
      {"gpt-5.6", "openai", "gpt-5.6"},
      {"anthropic/claude-opus-5", "anthropic", "claude-opus-5"},
      {"anthropic/claude-haiku-4-5", "anthropic", "claude-haiku-4-5"},
      {"mistral/mistral-large-latest", "mistral", "mistral-large-latest"},
      {"zai/glm-5.3", "zai", "glm-5.3"},
  };
  for (const Pair& p : kPairs) {
    TEST_ASSERT_EQUAL_MESSAGE((int)modelSizeClass(p.upstream, p.model),
                              (int)modelSizeClass("cumulo", p.routerPick), p.routerPick);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(modelFamily(p.upstream, p.model).c_str(),
                                     modelFamily("cumulo", p.routerPick).c_str(), p.routerPick);
  }
  TEST_ASSERT_EQUAL('L', modelSizeClass("cumulo", "anthropic/claude-opus-5"));
}


// ==== lane F: the streamed fetch and the model store ===================================
// Oct 2026 fixtures: the provider lists recorded 2026-10-18 (GET is free) and, for
// the paged read, Anthropic's own pages at limit=5 recorded 2026-10-22.
static const char* kFix10 = "test/support/fixtures/models_2026-10";
static std::string readFix10(const char* name) {
  std::string path = std::string(kFix10) + "/" + name;
  FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}
// 2026-10-22T12:00:00Z: the day the lane ran; after gpt-5.1-codex's and
// gpt-5.3-chat-latest's shutdown dates, before gpt-5.4-nano's.
static const uint32_t kOct22 = 1792670400u;

static ModelHarvest harvestOf(const char* provider, const std::string& body, uint32_t now,
                              size_t chunk = 0) {
  ModelHarvest h;
  h.provider = provider;
  h.now = now;
  if (!chunk) {
    harvestBody(h, body.data(), body.size());
  } else {
    ModelsBodyReader rd([](void* c, const char* o, size_t n) { harvestRow(*(ModelHarvest*)c, o, n); }, &h);
    for (size_t i = 0; i < body.size(); i += chunk) rd.feed(body.data() + i, std::min(chunk, body.size() - i));
    if (!rd.complete()) h.cut = true;
  }
  finishHarvest(h);
  return h;
}
static bool csvHasId(const std::string& csv, const std::string& id) {
  return ("," + csv + ",").find("," + id + ",") != std::string::npos;
}

// The counter-test for the streamed reader (L19-49): on every BYOK list it yields
// exactly what the whole-body parse did before lane F (ids, order, roles, caps,
// size, window, deprecation), captured from the old parser into the golden.
static void test_streamed_parse_matches_the_old_parse_on_byok_lists() {
  FILE* g = std::fopen("test/test_model_catalog/byok_parse_golden_2026-10.txt", "rb");
  TEST_ASSERT_NOT_NULL(g);
  std::string golden;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), g)) > 0) golden.append(buf, n);
  std::fclose(g);
  std::string got;
  for (const char* p : {"openai", "anthropic", "mistral", "zai"}) {
    std::vector<ModelInfo> v;
    parseModelsList(p, readFix10((std::string(p) + ".json").c_str()), v);
    char line[256];
    std::snprintf(line, sizeof(line), "== %s %zu\n", p, v.size());
    got += line;
    for (const ModelInfo& m : v) {
      std::snprintf(line, sizeof(line), "%s|%s|r%u|c%u|%c|ctx%u|out%u|dep%d|api%d\n", m.id.c_str(),
                    m.upstream.c_str(), m.roles, m.caps, m.size ? m.size : '-', m.ctxTokens,
                    m.maxOutTokens, (int)m.deprecated, (int)m.apiCaps);
      got += line;
    }
  }
  TEST_ASSERT_EQUAL_STRING(golden.c_str(), got.c_str());
}

// The chunk size never changes what is read: 1 byte, a prime and one big feed agree.
static void test_streamed_parse_is_chunk_size_invariant() {
  for (const char* p : {"anthropic", "openai", "mistral"}) {
    const std::string body = readFix10((std::string(p) + ".json").c_str());
    const ModelHarvest whole = harvestOf(p, body, kOct22);
    for (size_t chunk : {(size_t)1, (size_t)7, (size_t)509, (size_t)4096}) {
      const ModelHarvest h = harvestOf(p, body, kOct22, chunk);
      TEST_ASSERT_EQUAL_UINT_MESSAGE(whole.rows.size(), h.rows.size(), p);
      TEST_ASSERT_FALSE(h.cut);
      for (size_t i = 0; i < h.rows.size(); ++i)
        TEST_ASSERT_EQUAL_STRING(whole.rows[i].id.c_str(), h.rows[i].id.c_str());
    }
  }
}

// The Oct 2026 Anthropic list: every model with its line, display name, release date
// and lifecycle; claude-sonnet-5-5 is there and the short list holds the current set.
static void test_oct_2026_anthropic_fixture() {
  const ModelHarvest h = harvestOf("anthropic", readFix10("anthropic.json"), kOct22);
  TEST_ASSERT_EQUAL_UINT(14, h.rows.size());
  TEST_ASSERT_EQUAL_UINT(0, h.hostile + h.retired);
  const ModelInfo* s55 = find(h.rows, "claude-sonnet-5-5");
  TEST_ASSERT_NOT_NULL(s55);
  TEST_ASSERT_EQUAL_STRING("Claude Sonnet 5.5", s55->name.c_str());
  TEST_ASSERT_EQUAL_STRING("sonnet", s55->line.c_str());
  TEST_ASSERT_EQUAL_STRING("active", s55->life.c_str());
  TEST_ASSERT_EQUAL_UINT32(parseIsoTime("2026-09-28T00:00:00Z"), s55->created);
  TEST_ASSERT_TRUE(s55->apiCaps && s55->hasCap(CapVision));
  const ModelInfo* fable = find(h.rows, "claude-fable-5-1");
  TEST_ASSERT_NOT_NULL(fable);
  TEST_ASSERT_EQUAL_STRING("fable", fable->line.c_str());
  const std::string shortList = directChoices("anthropic", h.rows, kOct22);
  for (const char* id : {"claude-fable-5-1", "claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5"})
    TEST_ASSERT_TRUE_MESSAGE(csvHasId(shortList, id), id);
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", shortList.substr(0, shortList.find(',')).c_str());   // newest first
}

// The Oct 2026 OpenAI list (critique E2): gpt-6-sol, gpt-6.1-sol and gpt-6-luna are
// there; a model past its shutdown date is gone once the clock knows the date, a model
// with a future date is kept and flagged but left off the short list. With the clock
// unset nothing is dropped by date.
static void test_oct_2026_openai_fixture_and_shutdown_dates() {
  const std::string body = readFix10("openai.json");
  const ModelHarvest h = harvestOf("openai", body, kOct22);
  for (const char* id : {"gpt-6-sol", "gpt-6.1-sol", "gpt-6-luna", "gpt-5.6-sol"})
    TEST_ASSERT_TRUE_MESSAGE(present(h.rows, id), id);
  TEST_ASSERT_FALSE(present(h.rows, "gpt-5.1-codex"));         // shut down 2026-07-23
  TEST_ASSERT_FALSE(present(h.rows, "gpt-5.3-chat-latest"));   // shut down 2026-08-10
  TEST_ASSERT_TRUE(h.retired > 0);
  const ModelInfo* nano = find(h.rows, "gpt-5.4-nano");        // shuts down 2027-04-01
  TEST_ASSERT_NOT_NULL(nano);
  TEST_ASSERT_TRUE(nano->deprecated);
  TEST_ASSERT_EQUAL_UINT32(parseIsoTime("2027-04-01"), nano->retires);
  const std::string shortList = directChoices("openai", h.rows, kOct22);
  TEST_ASSERT_TRUE(csvHasId(shortList, "gpt-6.1-sol"));
  TEST_ASSERT_TRUE(csvHasId(shortList, "gpt-5.6-sol"));         // critique C1
  TEST_ASSERT_FALSE(csvHasId(shortList, "gpt-5.4-nano"));
  const ModelHarvest unset = harvestOf("openai", body, 0);
  TEST_ASSERT_EQUAL_UINT(0, unset.retired);
  TEST_ASSERT_TRUE(present(unset.rows, "gpt-5.1-codex"));
  // A shutdown that passes after the fetch still bars the model at read time.
  TEST_ASSERT_TRUE(rowOffered(*nano, RoleOrchestrator, kOct22));
  TEST_ASSERT_FALSE(rowOffered(*nano, RoleOrchestrator, parseIsoTime("2027-04-02")));
}

// has_more: Anthropic's real pages at limit=5. Each page says whether more follow
// and the cursor; reading them in turn yields the whole list, the same 14 models as
// one limit=1000 read, in order.
static void test_has_more_pages_read_whole() {
  ModelHarvest h;
  h.provider = "anthropic";
  h.now = kOct22;
  std::string last;
  TEST_ASSERT_TRUE(harvestBody(h, readFix10("anthropic_page1_limit5.json").data(),
                               readFix10("anthropic_page1_limit5.json").size(), &last));
  TEST_ASSERT_EQUAL_STRING("claude-opus-5", last.c_str());
  TEST_ASSERT_TRUE(harvestBody(h, readFix10("anthropic_page2_limit5.json").data(),
                               readFix10("anthropic_page2_limit5.json").size(), &last));
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-4-6", last.c_str());
  TEST_ASSERT_FALSE(harvestBody(h, readFix10("anthropic_page3_limit5.json").data(),
                                readFix10("anthropic_page3_limit5.json").size(), &last));
  finishHarvest(h);
  const ModelHarvest one = harvestOf("anthropic", readFix10("anthropic.json"), kOct22);
  TEST_ASSERT_EQUAL_UINT(one.rows.size(), h.rows.size());
  for (size_t i = 0; i < h.rows.size(); ++i)
    TEST_ASSERT_EQUAL_STRING(one.rows[i].id.c_str(), h.rows[i].id.c_str());
  TEST_ASSERT_FALSE(h.cut);
}

// A body cut short (a dropped connection, the read deadline) keeps every row that
// arrived whole, says it was cut, and never replaces a stored list; with no list
// stored the partial one is better than nothing.
static void test_truncated_body_keeps_the_last_good_list() {
  const std::string body = readFix10("anthropic.json");
  for (size_t cutAt : {body.size() / 3, body.size() / 2, body.size() - 3}) {
    const ModelHarvest h = harvestOf("anthropic", body.substr(0, cutAt), kOct22, 512);
    TEST_ASSERT_TRUE(h.cut);
    TEST_ASSERT_TRUE(h.rows.size() < 14 || cutAt == body.size() - 3);
    TEST_ASSERT_FALSE(harvestStorable(h, /*haveStored=*/true));
    TEST_ASSERT_EQUAL(!h.rows.empty(), harvestStorable(h, /*haveStored=*/false));
    ModelStoreMeta meta;
    meta.total = (uint32_t)h.rows.size();
    TEST_ASSERT_TRUE(harvestLogLine(h, meta, cutAt, 1).find("body cut short") != std::string::npos);
  }
  const ModelHarvest whole = harvestOf("anthropic", body, kOct22);
  TEST_ASSERT_TRUE(harvestStorable(whole, true));
  TEST_ASSERT_FALSE(harvestStorable(ModelHarvest{}, false));   // empty never replaces
}

// Provider strings are untrusted: an id outside [A-Za-z0-9._:/-]{1,64} drops its row
// (a quote would break the probe's JSON, a tab or newline a store line), control bytes
// are stripped from names, and names are bounded on a UTF-8 boundary.
static void test_hostile_ids_and_names_are_cleaned() {
  std::string longId(65, 'a');
  std::string longName(200, 'n');
  longName += "\xc3\xa9";
  const std::string body = std::string("{\"data\":[") +
      "{\"id\":\"claude-ok-1\",\"display_name\":\"Fine\\u0007\\tName\"}," +
      "{\"id\":\"claude-\\\"quote\"}," +
      "{\"id\":\"claude-tab\\tid\"}," +
      "{\"id\":\"claude-nl\\nid\"}," +
      "{\"id\":\"claude space\"}," +
      "{\"id\":\"" + longId + "\"}," +
      "{\"id\":\"claude-long-name\",\"display_name\":\"" + longName + "\"}," +
      "{\"id\":7}],\"has_more\":false}";
  const ModelHarvest h = harvestOf("anthropic", body, kOct22);
  TEST_ASSERT_EQUAL_UINT(2, h.rows.size());
  TEST_ASSERT_EQUAL_UINT(6, h.hostile);
  TEST_ASSERT_EQUAL_STRING("FineName", find(h.rows, "claude-ok-1")->name.c_str());
  TEST_ASSERT_TRUE(find(h.rows, "claude-long-name")->name.size() <= kModelNameMax);
  for (const char* bad : {"", "a\"b", "a b", "a\tb", "x\x01", "\xc3\xa9"}) TEST_ASSERT_FALSE(cleanModelId(bad));
  for (const char* ok : {"gpt-5.6", "anthropic/claude-opus-5-5", "ft:gpt-4o:acme"}) TEST_ASSERT_TRUE(cleanModelId(ok));
  TEST_ASSERT_EQUAL_STRING("ab", cleanModelText("a\x7f\x1f" "b", 16).c_str());
}

// The store file round-trips every field, and its header carries what H's refresh
// and the freshness line read.
static void test_store_round_trip() {
  const ModelHarvest h = harvestOf("anthropic", readFix10("anthropic.json"), kOct22);
  ModelStoreMeta meta;
  meta.fetchedAt = kOct22;
  meta.fw = "v4.6.0";
  meta.total = (uint32_t)h.rows.size();
  meta.choices = directChoices("anthropic", h.rows, kOct22);
  const std::string text = serializeModelStore(meta, h.rows);
  TEST_ASSERT_TRUE(text.rfind("#models v1 fetchedAt=1792670400 fw=v4.6.0 ", 0) == 0);
  TEST_ASSERT_TRUE(text.size() < 90 * h.rows.size() + 300);   // about 85 B a model
  ModelStoreMeta back;
  std::vector<ModelInfo> rows;
  TEST_ASSERT_TRUE(parseModelStore("anthropic", text, back, &rows));
  TEST_ASSERT_EQUAL_UINT32(kOct22, back.fetchedAt);
  TEST_ASSERT_EQUAL_STRING("v4.6.0", back.fw.c_str());
  TEST_ASSERT_EQUAL_STRING(meta.choices.c_str(), back.choices.c_str());
  TEST_ASSERT_EQUAL_UINT(h.rows.size(), rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const ModelInfo &a = h.rows[i], &b = rows[i];
    TEST_ASSERT_EQUAL_STRING(a.id.c_str(), b.id.c_str());
    TEST_ASSERT_EQUAL_STRING(a.name.c_str(), b.name.c_str());
    TEST_ASSERT_EQUAL_STRING(a.line.c_str(), b.line.c_str());
    TEST_ASSERT_EQUAL_STRING(a.life.c_str(), b.life.c_str());
    TEST_ASSERT_EQUAL_STRING(a.family.c_str(), b.family.c_str());
    TEST_ASSERT_EQUAL(a.roles, b.roles);
    TEST_ASSERT_EQUAL(a.caps, b.caps);
    TEST_ASSERT_EQUAL(a.size, b.size);
    TEST_ASSERT_EQUAL_UINT32(a.created, b.created);
    TEST_ASSERT_EQUAL_UINT32(a.ctxTokens, b.ctxTokens);
    TEST_ASSERT_EQUAL_UINT32(a.maxOutTokens, b.maxOutTokens);
    TEST_ASSERT_EQUAL(a.usable, b.usable);
    TEST_ASSERT_EQUAL(a.apiCaps, b.apiCaps);
    TEST_ASSERT_EQUAL(a.deprecated, b.deprecated);
  }
  // A torn or foreign line is skipped, never misread; no header, no store.
  ModelStoreMeta m2;
  std::vector<ModelInfo> r2;
  TEST_ASSERT_TRUE(parseModelStore("anthropic", text + "claude-torn\tName\topus\n", m2, &r2));
  TEST_ASSERT_EQUAL_UINT(h.rows.size(), r2.size());
  TEST_ASSERT_FALSE(parseModelStore("anthropic", "[{\"id\":\"x\"}]", m2, &r2));
  TEST_ASSERT_EQUAL_STRING("/data/models_anthropic.txt", modelStorePath("anthropic").c_str());
  TEST_ASSERT_EQUAL_STRING("", modelStorePath("../x").c_str());
}

// Unset clock: a fetch made before SNTP stores fetchedAt=0, and the freshness line and
// GET /api/models say the date is unknown instead of comparing uptime with a date.
static void test_unset_clock_fetched_at_is_honest() {
  const ModelHarvest h = harvestOf("anthropic", readFix10("anthropic.json"), 0);
  ModelStoreMeta meta;
  meta.fetchedAt = h.now;
  const std::string text = serializeModelStore(meta, h.rows);
  ModelStoreMeta back;
  TEST_ASSERT_TRUE(parseModelStoreHeader(text, back));
  TEST_ASSERT_EQUAL_UINT32(0, back.fetchedAt);
  TEST_ASSERT_EQUAL_STRING("(list date unknown, may miss newer models)", modelListNote(true, back).c_str());
  back.fetchedAt = kOct22;
  TEST_ASSERT_EQUAL_STRING("(list from 2026-10-22, may miss newer models)", modelListNote(true, back).c_str());
  TEST_ASSERT_EQUAL_STRING("(built-in list, not fetched yet)", modelListNote(false, back).c_str());
  TEST_ASSERT_EQUAL_STRING("1970-01-01", isoDate(1).c_str());
  TEST_ASSERT_EQUAL_STRING("2024-02-29", isoDate(parseIsoTime("2024-02-29T23:59:59Z")).c_str());
  TEST_ASSERT_EQUAL_UINT32(0, parseIsoTime("2026-13-01"));
  TEST_ASSERT_EQUAL_UINT32(0, parseIsoTime("soon"));
}

// The bound trims by model line, round-robin: no line (Fable) goes first however the
// rows sort, and what was dropped is counted.
static void test_bound_trims_by_line_never_a_whole_line() {
  std::vector<ModelInfo> rows;
  for (int i = 0; i < 120; ++i) {
    ModelInfo m = classifyModel("anthropic", "claude-opus-9-" + std::to_string(i));
    m.line = "opus";
    rows.push_back(m);
  }
  for (int i = 0; i < 60; ++i) {
    ModelInfo m = classifyModel("anthropic", "claude-sonnet-9-" + std::to_string(i));
    m.line = "sonnet";
    rows.push_back(m);
  }
  for (int i = 0; i < 3; ++i) {
    ModelInfo m = classifyModel("anthropic", "claude-fable-9-" + std::to_string(i));
    m.line = "fable";   // no size class: sorts last
    rows.push_back(m);
  }
  const size_t dropped = boundModelRows(rows);
  TEST_ASSERT_EQUAL_UINT(kModelStoreMaxRows, rows.size());
  TEST_ASSERT_EQUAL_UINT(183 - kModelStoreMaxRows, dropped);
  for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(present(rows, "claude-fable-9-" + std::to_string(i)));
  TEST_ASSERT_TRUE(present(rows, "claude-sonnet-9-59"));
  TEST_ASSERT_EQUAL_STRING("claude-opus-9-0", rows[0].id.c_str());   // input order kept
  std::vector<ModelInfo> few(rows.begin(), rows.begin() + 10);
  TEST_ASSERT_EQUAL_UINT(0, boundModelRows(few));
}

// The legacy merge (critique C3): the older build's JSON catalog lost Fable in its
// size trim while the NVS short list still had it; the first boot's store holds both,
// says it is a legacy list of unknown date, and the JSON is only read.
static void test_legacy_lists_merge_into_the_store() {
  const std::string mcat =
      "[{\"id\":\"claude-opus-5\",\"roles\":[\"orchestrator\",\"sub-agent\"],\"usable\":true,"
      "\"size\":\"L\",\"family\":\"claude-opus\"},{\"id\":\"claude-sonnet-5\",\"roles\":"
      "[\"orchestrator\",\"sub-agent\"],\"usable\":true,\"size\":\"M\"}]";
  const std::string mch = "claude-fable-5-1, claude-opus-5,claude-sonnet-5,bad id";
  const std::string text = mergeLegacyLists("anthropic", "", mcat, mch, "v4.6.0");
  ModelStoreMeta meta;
  std::vector<ModelInfo> rows;
  TEST_ASSERT_TRUE(parseModelStore("anthropic", text, meta, &rows));
  TEST_ASSERT_EQUAL_STRING("legacy", meta.src.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, meta.fetchedAt);
  TEST_ASSERT_EQUAL_UINT(3, rows.size());
  TEST_ASSERT_TRUE(present(rows, "claude-fable-5-1"));
  TEST_ASSERT_EQUAL_STRING("claude-fable-5-1,claude-opus-5,claude-sonnet-5", meta.choices.c_str());
  // Idempotent: a second boot finds nothing new to write.
  TEST_ASSERT_EQUAL_STRING("", mergeLegacyLists("anthropic", text, mcat, mch, "v4.6.0").c_str());
  // A re-upgrade after a rollback: ids the older build harvested since are added.
  const std::string more = mergeLegacyLists("anthropic", text, "", "claude-haiku-5-5", "v4.6.0");
  TEST_ASSERT_TRUE(more.find("claude-haiku-5-5\t") != std::string::npos);
  // The erase rule: only on a boot after markValid, only once the store holds the list.
  TEST_ASSERT_FALSE(legacyListErasable(false, true, true));   // OTA trial boot: keep for a rollback
  TEST_ASSERT_FALSE(legacyListErasable(true, false, true));   // no store yet: keep
  TEST_ASSERT_FALSE(legacyListErasable(true, true, false));   // nothing to erase
  TEST_ASSERT_TRUE(legacyListErasable(true, true, true));
}

// GET /api/models rows: every row priced or saying so, with the store fields, and the
// chat roles only while the one check accepts the model.
// The usability probe's verdict on the live 22 Oct 2026 answers (POST /v1/messages,
// max_tokens 1): claude-haiku-5-5 200 is usable; an unknown model's 404
// not_found_error hides it.
static void test_probe_verdict_on_the_live_oct_2026_answers() {
  TEST_ASSERT_TRUE(probeVerdict(200, "") == ProbeVerdict::Usable);
  const char* notFound =
      "{\"type\":\"error\",\"error\":{\"type\":\"not_found_error\",\"message\":\"model: claude-imaginary-9\"}}";
  TEST_ASSERT_TRUE(probeVerdict(404, notFound) == ProbeVerdict::Unusable);
  TEST_ASSERT_TRUE(probeVerdict(529, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\"}}") ==
                   ProbeVerdict::Unknown);
}

static void test_api_rows_carry_price_or_unknown() {
  const ModelHarvest h = harvestOf("openai", readFix10("openai.json"), kOct22);
  JsonDocument d;
  modelsToJson("openai", h.rows, d.to<JsonArray>(), true, kOct22);
  bool sawBuiltin = false, sawUnknown = false;
  for (JsonObjectConst o : d.as<JsonArrayConst>()) {
    const std::string src = o["priceSrc"] | "";
    TEST_ASSERT_TRUE(src == "builtin" || src == "unknown");
    TEST_ASSERT_EQUAL(src != "unknown", o["priceIn"].is<uint32_t>());
    sawBuiltin = sawBuiltin || src == "builtin";
    sawUnknown = sawUnknown || src == "unknown";
    const std::string id = o["id"] | "";
    bool chat = false;
    for (JsonVariantConst r : o["roles"].as<JsonArrayConst>()) chat = chat || std::string(r | "") == "orchestrator";
    if (id.find("audio") != std::string::npos) TEST_ASSERT_FALSE_MESSAGE(chat, id.c_str());
  }
  TEST_ASSERT_TRUE(sawBuiltin && sawUnknown);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_probe_verdict);
  RUN_TEST(test_models_json_round_trip);
  RUN_TEST(test_size_class_by_family);
  RUN_TEST(test_family_bucket);
  RUN_TEST(test_gpt_generation_parse);
  RUN_TEST(test_gpt_generation_class_rule);
  RUN_TEST(test_gpt6_astra_is_a_selectable_flagship);
  RUN_TEST(test_role_classification_by_id);
  RUN_TEST(test_anthropic_reads_api_capabilities);
  RUN_TEST(test_openai_heuristics_and_roles);
  RUN_TEST(test_mistral_metadata_and_alias_dedup);
  RUN_TEST(test_zai_glm_roles_and_sizes);
  RUN_TEST(test_flagship_first_ordering);
  RUN_TEST(test_cumulo_upstream_split);
  RUN_TEST(test_cumulo_catalog_is_uncapped_not_truncated);
  RUN_TEST(test_cumulo_size_class_metadata_overrides_heuristic);
  RUN_TEST(test_classify_catalog_entry_from_bare_ids);
  RUN_TEST(test_models_to_json_shape_and_usable_filter);
  RUN_TEST(test_parse_tolerates_http_headers);
  RUN_TEST(test_router_pick_reads_as_its_upstream_model);
  RUN_TEST(test_streamed_parse_matches_the_old_parse_on_byok_lists);
  RUN_TEST(test_streamed_parse_is_chunk_size_invariant);
  RUN_TEST(test_oct_2026_anthropic_fixture);
  RUN_TEST(test_oct_2026_openai_fixture_and_shutdown_dates);
  RUN_TEST(test_has_more_pages_read_whole);
  RUN_TEST(test_truncated_body_keeps_the_last_good_list);
  RUN_TEST(test_hostile_ids_and_names_are_cleaned);
  RUN_TEST(test_store_round_trip);
  RUN_TEST(test_unset_clock_fetched_at_is_honest);
  RUN_TEST(test_bound_trims_by_line_never_a_whole_line);
  RUN_TEST(test_legacy_lists_merge_into_the_store);
  RUN_TEST(test_api_rows_carry_price_or_unknown);
  RUN_TEST(test_probe_verdict_on_the_live_oct_2026_answers);
  UNITY_END();
  return 0;
}
