#include <unity.h>

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
  TEST_ASSERT_EQUAL_CHAR('L', modelSizeClass("openai", "gpt-5.6-luna"));
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
  TEST_ASSERT_EQUAL_CHAR('L', luna->size);
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_probe_verdict);
  RUN_TEST(test_models_json_round_trip);
  RUN_TEST(test_size_class_by_family);
  RUN_TEST(test_family_bucket);
  RUN_TEST(test_role_classification_by_id);
  RUN_TEST(test_anthropic_reads_api_capabilities);
  RUN_TEST(test_openai_heuristics_and_roles);
  RUN_TEST(test_mistral_metadata_and_alias_dedup);
  RUN_TEST(test_zai_glm_roles_and_sizes);
  RUN_TEST(test_flagship_first_ordering);
  RUN_TEST(test_cumulo_upstream_split);
  RUN_TEST(test_models_to_json_shape_and_usable_filter);
  RUN_TEST(test_parse_tolerates_http_headers);
  UNITY_END();
  return 0;
}
