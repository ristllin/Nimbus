#include <unity.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "nimbus/orch/embedding.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static bool has(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }

// ---- request build ----------------------------------------------------------
static void test_build_request_with_dims() {
  std::string b = buildEmbeddingRequest("text-embedding-3-small", "hello world", 256);
  // re-parse to assert fields (order-independent)
  ArduinoJson::JsonDocument d;
  deserializeJson(d, b);
  TEST_ASSERT_EQUAL_STRING("text-embedding-3-small", d["model"]);
  TEST_ASSERT_EQUAL_STRING("hello world", d["input"]);
  TEST_ASSERT_EQUAL_INT(256, d["dimensions"].as<int>());
  TEST_ASSERT_EQUAL_STRING("float", d["encoding_format"]);
}

static void test_build_request_omits_dims_when_zero() {
  std::string b = buildEmbeddingRequest("mistral-embed", "x", 0);
  TEST_ASSERT_FALSE(has(b, "dimensions"));
}

// A memory's text is arbitrary and will contain quotes/backslashes/newlines. The
// request body must stay valid JSON with the input round-tripping verbatim -
// i.e. the input MUST be escaped (a hand-built body via string concat would
// corrupt the request, the same class of defect fixed once in the RPC id echo).
static void test_build_request_escapes_special_input() {
  const char* raw = "say \"hi\"\n\\path C:\\tmp\ttab";
  std::string b = buildEmbeddingRequest("m", raw, 0);
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
  // cumulo: the one-key router path, proxying OpenAI's embed models. Must NEVER be
  // a bare /v1/embeddings (that would hit a direct provider) and must route via
  // the router so the cumulo key + host are used.
  auto c = embedRouteFor("cumulo");
  TEST_ASSERT_TRUE(c.known);
  TEST_ASSERT_TRUE(c.viaCumuloRouter);
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1/embeddings", c.path);
  // unknown providers: refused, never a silent fallback.
  for (const char* p : {"anthropic", "zai", "", "openai/gpt-4o"}) {
    auto r = embedRouteFor(p);
    TEST_ASSERT_FALSE(r.known);
    TEST_ASSERT_FALSE(r.viaCumuloRouter);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_embed_route_per_provider);
  RUN_TEST(test_build_request_with_dims);
  RUN_TEST(test_build_request_omits_dims_when_zero);
  RUN_TEST(test_build_request_escapes_special_input);
  RUN_TEST(test_parse_ok);
  RUN_TEST(test_parse_dims_agnostic_when_zero);
  RUN_TEST(test_parse_dim_mismatch);
  RUN_TEST(test_parse_provider_error_object);
  RUN_TEST(test_parse_bad_json_and_missing_fields);
  RUN_TEST(test_parse_feeds_quantize_shape);
  return UNITY_END();
}
