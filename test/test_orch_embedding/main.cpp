#include <unity.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "nimbus/orch/embedding.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/vector_memory.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static bool has(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }

// ---- request build ----------------------------------------------------------
static void test_build_request_with_dims() {
  std::string b = buildEmbeddingRequest("openai", "text-embedding-3-small", "hello world", 256);
  // re-parse to assert fields (order-independent)
  ArduinoJson::JsonDocument d;
  deserializeJson(d, b);
  TEST_ASSERT_EQUAL_STRING("text-embedding-3-small", d["model"]);
  TEST_ASSERT_EQUAL_STRING("hello world", d["input"]);
  TEST_ASSERT_EQUAL_INT(256, d["dimensions"].as<int>());
  TEST_ASSERT_EQUAL_STRING("float", d["encoding_format"]);
}

static void test_build_request_omits_dims_when_zero() {
  std::string b = buildEmbeddingRequest("openai", "text-embedding-3-small", "x", 0);
  TEST_ASSERT_FALSE(has(b, "dimensions"));
  TEST_ASSERT_TRUE(has(b, "encoding_format"));   // the OpenAI dialect keeps it
}

// ---- CUM-469: the body is provider-aware -------------------------------------
// One OpenAI-shaped body went to every provider, and Mistral's /v1/embeddings
// schema has no `dimensions` field, so every mistral embed call 422'd live (bench
// unit A, 2026-10-04: embedcfg provider=mistral model=mistral-embed dims=1024).
// Each provider's EXACT body is pinned as a golden, built from the config that
// provider is really run with.
struct BodyGolden {
  const char* provider;
  const char* model;
  int dims;
  const char* body;
};
static const BodyGolden kBodyGoldens[] = {
    {"openai", "text-embedding-3-small", 256,
     R"({"model":"text-embedding-3-small","input":"hello world","dimensions":256,"encoding_format":"float"})"},
    // Mistral: no width field and no encoding_format. mistral-embed is fixed at 1024.
    {"mistral", "mistral-embed", 1024, R"({"model":"mistral-embed","input":"hello world"})"},
    // cumulo proxies OpenAI's embed models through the router: the OpenAI body, byte
    // for byte, so vectors stay comparable across a direct-openai <-> cumulo switch.
    {"cumulo", "text-embedding-3-small", 256,
     R"({"model":"text-embedding-3-small","input":"hello world","dimensions":256,"encoding_format":"float"})"},
};

static const BodyGolden* findBodyGolden(const char* provider) {
  for (const BodyGolden& g : kBodyGoldens)
    if (std::string(g.provider) == provider) return &g;
  return nullptr;
}

static void test_body_golden_openai() {
  const BodyGolden* g = findBodyGolden("openai");
  TEST_ASSERT_EQUAL_STRING(g->body,
                           buildEmbeddingRequest("openai", g->model, "hello world", g->dims).c_str());
}

// The bench config, exactly: the body Mistral is sent must carry neither OpenAI-only
// field, whatever dims the owner stored (0, a stale 256, or the native 1024).
static void test_body_golden_mistral_has_no_openai_fields() {
  const BodyGolden* g = findBodyGolden("mistral");
  TEST_ASSERT_EQUAL_STRING(g->body,
                           buildEmbeddingRequest("mistral", g->model, "hello world", g->dims).c_str());
  for (int dims : {0, 256, 1024, 1536}) {
    std::string b = buildEmbeddingRequest("mistral", "mistral-embed", "hello world", dims);
    TEST_ASSERT_FALSE_MESSAGE(has(b, "dimensions"), b.c_str());
    TEST_ASSERT_FALSE_MESSAGE(has(b, "encoding_format"), b.c_str());
    TEST_ASSERT_EQUAL_STRING(g->body, b.c_str());
  }
}

static void test_body_golden_cumulo_is_the_openai_body() {
  const BodyGolden* g = findBodyGolden("cumulo");
  TEST_ASSERT_EQUAL_STRING(g->body,
                           buildEmbeddingRequest("cumulo", g->model, "hello world", g->dims).c_str());
  TEST_ASSERT_EQUAL_STRING(
      buildEmbeddingRequest("openai", g->model, "hello world", g->dims).c_str(),
      buildEmbeddingRequest("cumulo", g->model, "hello world", g->dims).c_str());
}

// The class rule, over the canonical provider registry (not a list copied here):
// every provider with an embeddings route has a pinned golden body, so a provider
// given a route without one FAILS; every provider without a route builds NO body
// (an explicit not-supported, never a silent OpenAI-shaped fallback); and no golden
// outlives its route.
static void test_every_routed_provider_has_a_pinned_body() {
  for (const nimbus::orch::ProviderSlot& slot : nimbus::orch::kProviderSlots) {
    const EmbedRoute r = embedRouteFor(slot.slug);
    const BodyGolden* g = findBodyGolden(slot.slug);
    if (!r.known) {
      TEST_ASSERT_TRUE_MESSAGE(r.wire == EmbedWire::None, slot.slug);
      TEST_ASSERT_NULL_MESSAGE(g, slot.slug);   // a golden for an unrouted provider is stale
      TEST_ASSERT_TRUE_MESSAGE(buildEmbeddingRequest(slot.slug, "m", "x", 256).empty(), slot.slug);
      continue;
    }
    TEST_ASSERT_TRUE_MESSAGE(r.wire != EmbedWire::None, slot.slug);
    TEST_ASSERT_NOT_NULL_MESSAGE(g, slot.slug);   // routed with no golden: pin its body
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        g->body, buildEmbeddingRequest(slot.slug, g->model, "hello world", g->dims).c_str(),
        slot.slug);
  }
  for (const BodyGolden& g : kBodyGoldens) {
    TEST_ASSERT_NOT_NULL_MESSAGE(nimbus::orch::findProviderSlot(g.provider), g.provider);
    TEST_ASSERT_TRUE_MESSAGE(embedRouteFor(g.provider).known, g.provider);
  }
  // Off-registry slugs get no body either.
  for (const char* p : {"", "custom", "openai/gpt-4o"})
    TEST_ASSERT_TRUE_MESSAGE(buildEmbeddingRequest(p, "m", "x", 256).empty(), p);
}

// A memory's text is arbitrary and will contain quotes/backslashes/newlines. The
// request body must stay valid JSON with the input round-tripping verbatim -
// i.e. the input MUST be escaped (a hand-built body via string concat would
// corrupt the request, the same class of defect fixed once in the RPC id echo).
static void test_build_request_escapes_special_input() {
  const char* raw = "say \"hi\"\n\\path C:\\tmp\ttab";
  std::string b = buildEmbeddingRequest("openai", "m", raw, 0);
  ArduinoJson::JsonDocument d;
  TEST_ASSERT_TRUE(deserializeJson(d, b) == ArduinoJson::DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING(raw, d["input"]);  // round-trips exactly
}

// ---- response parse ---------------------------------------------------------
static void test_parse_ok() {
  const char* json = R"({"data":[{"embedding":[0.1,-0.2,0.3,1.0]}],"model":"m"})";
  std::vector<float> out; std::string err;
  TEST_ASSERT_TRUE(parseEmbeddingResponse(json, 4, out, err));
  TEST_ASSERT_EQUAL_INT(4, (int)out.size());
  TEST_ASSERT_FLOAT_WITHIN(1e-5, 0.1f, out[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-5, 1.0f, out[3]);
}

static void test_parse_dims_agnostic_when_zero() {
  const char* json = R"({"data":[{"embedding":[1,2,3]}]})";
  std::vector<float> out; std::string err;
  TEST_ASSERT_TRUE(parseEmbeddingResponse(json, 0, out, err));  // expectedDims 0 => any length
  TEST_ASSERT_EQUAL_INT(3, (int)out.size());
}

static void test_parse_dim_mismatch() {
  const char* json = R"({"data":[{"embedding":[1,2,3]}]})";
  std::vector<float> out; std::string err;
  TEST_ASSERT_FALSE(parseEmbeddingResponse(json, 256, out, err));
  TEST_ASSERT_TRUE(has(err, "dim mismatch"));
  TEST_ASSERT_EQUAL_INT(0, (int)out.size());
}

static void test_parse_provider_error_object() {
  const char* json = R"({"error":{"message":"invalid api key","type":"auth"}})";
  std::vector<float> out; std::string err;
  TEST_ASSERT_FALSE(parseEmbeddingResponse(json, 256, out, err));
  TEST_ASSERT_TRUE(has(err, "invalid api key"));
}

static void test_parse_bad_json_and_missing_fields() {
  std::vector<float> out; std::string err;
  TEST_ASSERT_FALSE(parseEmbeddingResponse("{not json", 0, out, err));
  TEST_ASSERT_TRUE(has(err, "bad json"));
  TEST_ASSERT_FALSE(parseEmbeddingResponse(R"({"data":[]})", 0, out, err));
  TEST_ASSERT_TRUE(has(err, "no data"));
  TEST_ASSERT_FALSE(parseEmbeddingResponse(R"({"data":[{"x":1}]})", 0, out, err));
  TEST_ASSERT_TRUE(has(err, "no embedding"));
}

// ---- round-trip into the vector engine's quantizer --------------------------
static void test_parse_feeds_quantize_shape() {
  const char* json = R"({"data":[{"embedding":[1.0,-1.0,0.5,0.0]}]})";
  std::vector<float> out; std::string err;
  TEST_ASSERT_TRUE(parseEmbeddingResponse(json, 4, out, err));
  // sanity: values in a range the int8 quantizer expects ([-1,1] typical)
  for (float f : out) TEST_ASSERT_TRUE(f >= -1.0f && f <= 1.0f);
}

// CUM-302: the embed endpoint routing per provider is a class rule, not one
// point. A prefixed / cumulo pick must go to the router, never a direct provider,
// and every known provider must resolve to a real path; an unknown one must fail
// (not silently fall through to openai). If a new embed provider is added without
// a route, `known` stays false and this test forces the coverage.
static void test_embed_route_per_provider() {
  using nimbus::orch::embedRouteFor;
  // openai + mistral: direct provider, OpenAI-compatible /v1/embeddings.
  for (const char* p : {"openai", "mistral"}) {
    auto r = embedRouteFor(p);
    TEST_ASSERT_TRUE(r.known);
    TEST_ASSERT_FALSE(r.viaCumuloRouter);
    TEST_ASSERT_EQUAL_STRING("/v1/embeddings", r.path);
  }
  // Same path, different body dialects (CUM-469).
  TEST_ASSERT_TRUE(embedRouteFor("openai").wire == EmbedWire::OpenAI);
  TEST_ASSERT_TRUE(embedRouteFor("mistral").wire == EmbedWire::Mistral);
  // cumulo: the one-key router path, proxying OpenAI's embed models. Must NEVER be
  // a bare /v1/embeddings (that would hit a direct provider) and must route via
  // the router so the cumulo key + host are used.
  auto c = embedRouteFor("cumulo");
  TEST_ASSERT_TRUE(c.known);
  TEST_ASSERT_TRUE(c.viaCumuloRouter);
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1/embeddings", c.path);
  TEST_ASSERT_TRUE(c.wire == EmbedWire::OpenAI);   // the router proxies OpenAI's body
  // unknown providers: refused, never a silent fallback.
  for (const char* p : {"anthropic", "zai", "", "openai/gpt-4o"}) {
    auto r = embedRouteFor(p);
    TEST_ASSERT_FALSE(r.known);
    TEST_ASSERT_FALSE(r.viaCumuloRouter);
    TEST_ASSERT_TRUE(r.wire == EmbedWire::None);
  }
}

// ---- CUM-469: the width contract, end to end on the host ----------------------
// A Mistral response in its documented shape (object/model/id/usage beside data[]),
// `n` floats wide.
static std::string mistralResponse(int n) {
  std::string j = R"({"id":"e1","object":"list","model":"mistral-embed","data":[{"object":"embedding","index":0,"embedding":[)";
  for (int i = 0; i < n; i++) {
    if (i) j += ",";
    j += (i % 2) ? "-0.0216064453125" : "0.0123291015625";
  }
  j += R"(]}],"usage":{"prompt_tokens":3,"total_tokens":3,"completion_tokens":0}})";
  return j;
}

// mistral-embed returns 1024 floats whatever is asked; with stored dims=1024 that
// parses, quantizes and lands in a 1024-wide store: the bench config works.
static void test_mistral_1024_response_fills_a_1024_store() {
  const std::string json = mistralResponse(1024);
  std::vector<float> f; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parseEmbeddingResponse(json.c_str(), 1024, f, err), err.c_str());
  TEST_ASSERT_EQUAL_INT(1024, (int)f.size());
  std::vector<int8_t> q = VectorMemory::quantize(f);
  TEST_ASSERT_EQUAL_INT(1024, (int)q.size());
  VectorMemory vm;
  vm.configure(1024);
  VecEntry e;
  e.id = "m1"; e.content = "the bench fact"; e.vec = q;
  TEST_ASSERT_TRUE(vm.add(e));
  TEST_ASSERT_EQUAL_INT(1, vm.size());
}

// Stored dims that disagree with the model's native width are REFUSED with the
// "dim mismatch" token (which the memory tools map to honest words), never
// truncated: a 1024-wide mistral-embed vector cut to 256 would compare as noise.
static void test_width_mismatch_is_refused_not_truncated() {
  const std::string json = mistralResponse(1024);
  std::vector<float> f; std::string err;
  TEST_ASSERT_FALSE(parseEmbeddingResponse(json.c_str(), 256, f, err));
  TEST_ASSERT_EQUAL_STRING("dim mismatch: got 1024 expected 256", err.c_str());
  TEST_ASSERT_EQUAL_INT(0, (int)f.size());
  // And the store itself holds one width: a 1024 vector never enters a 256 store.
  VectorMemory vm;
  vm.configure(256);
  VecEntry e;
  e.id = "m2"; e.content = "x"; e.vec.assign(1024, 1);
  TEST_ASSERT_FALSE(vm.add(e));
  TEST_ASSERT_EQUAL_INT(0, vm.size());
}

// Boot rule for the width contract: a wiped store persisted under the OLD width
// (embedcfg reset=1 persists before the new width is applied) must not pin the
// store to that width after a reboot, or every 1024-wide mistral write is refused.
// A store that holds vectors is never re-widened.
static void test_empty_store_adopts_configured_width_after_reload() {
  VectorMemory old;
  old.configure(256);                       // the replaced config's width
  const std::string wipedBlob = old.serialize();
  VectorMemory vm;
  vm.configure(1024);                       // boot: configured from the new embed config
  TEST_ASSERT_TRUE(vm.deserialize(wipedBlob));
  TEST_ASSERT_EQUAL_INT(256, vm.dims());    // the header won: the bug's starting point
  TEST_ASSERT_TRUE(vm.adoptWidthIfEmpty(1024));
  TEST_ASSERT_EQUAL_INT(1024, vm.dims());
  VecEntry e;
  e.id = "m"; e.content = "after reboot"; e.vec.assign(1024, 7);
  TEST_ASSERT_TRUE(vm.add(e));
  // Non-empty: the width is the vectors' width and stays put.
  TEST_ASSERT_FALSE(vm.adoptWidthIfEmpty(256));
  TEST_ASSERT_EQUAL_INT(1024, vm.dims());
  // No-ops: same width, or a non-positive width.
  VectorMemory fresh;
  fresh.configure(1024);
  TEST_ASSERT_FALSE(fresh.adoptWidthIfEmpty(1024));
  TEST_ASSERT_FALSE(fresh.adoptWidthIfEmpty(0));
  TEST_ASSERT_EQUAL_INT(1024, fresh.dims());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_embed_route_per_provider);
  RUN_TEST(test_build_request_with_dims);
  RUN_TEST(test_build_request_omits_dims_when_zero);
  RUN_TEST(test_body_golden_openai);
  RUN_TEST(test_body_golden_mistral_has_no_openai_fields);
  RUN_TEST(test_body_golden_cumulo_is_the_openai_body);
  RUN_TEST(test_every_routed_provider_has_a_pinned_body);
  RUN_TEST(test_mistral_1024_response_fills_a_1024_store);
  RUN_TEST(test_width_mismatch_is_refused_not_truncated);
  RUN_TEST(test_empty_store_adopts_configured_width_after_reload);
  RUN_TEST(test_build_request_escapes_special_input);
  RUN_TEST(test_parse_ok);
  RUN_TEST(test_parse_dims_agnostic_when_zero);
  RUN_TEST(test_parse_dim_mismatch);
  RUN_TEST(test_parse_provider_error_object);
  RUN_TEST(test_parse_bad_json_and_missing_fields);
  RUN_TEST(test_parse_feeds_quantize_shape);
  return UNITY_END();
}
