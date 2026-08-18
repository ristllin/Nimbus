// test_relay_codec - the cloud relay frame codec, byte-locked to protocol.ts via
// relay_vectors.h (regenerate with tools/gen_relay_vectors.mjs). Proves the device
// decodes exactly what the reference relay encodes, builds wire-valid device frames,
// and round-trips base64.
#include <unity.h>

#include <ArduinoJson.h>
#include <string>
#include <vector>

#include "nimbus/cloud/relay_codec.h"
#include "relay_vectors.h"

using namespace nimbus::cloud;

void setUp() {}
void tearDown() {}

// Every reference-encoded relay->device frame decodes to the expected fields.
static void test_relay_vectors_decode() {
  for (int i = 0; i < kRelayVectorCount; i++) {
    const RelayVec& v = kRelayVectors[i];
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, v.json);
    TEST_ASSERT_FALSE_MESSAGE(err, v.name);
    RelayFrame f;
    TEST_ASSERT_TRUE_MESSAGE(parseRelayFrame(doc, f), v.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(v.type, (int)f.type, v.name);
    if (v.type == 1) {
      TEST_ASSERT_EQUAL_UINT32(v.heartbeatMs, f.heartbeatMs);
    } else if (v.type == 2) {
      TEST_ASSERT_EQUAL_STRING(v.id, f.req.id);
      TEST_ASSERT_EQUAL_STRING(v.method, f.req.method);
      TEST_ASSERT_EQUAL_STRING(v.path, f.req.path);
      if (v.bodyB64)
        TEST_ASSERT_EQUAL_STRING(v.bodyB64, f.req.bodyB64);
      else
        TEST_ASSERT_NULL(f.req.bodyB64);
      if (v.hdrKey) {
        TEST_ASSERT_FALSE(f.req.headers.isNull());
        TEST_ASSERT_EQUAL_STRING(v.hdrVal, f.req.headers[v.hdrKey].as<const char*>());
      }
    } else if (v.type == 4) {
      TEST_ASSERT_EQUAL_STRING(v.byeReason, f.byeReason);
    }
  }
}

// Malformed / unknown frames never parse as trusted.
static void test_rejects_malformed() {
  const char* bad[] = {
      "not json", "{}", "[]", "\"str\"", "42",
      "{\"t\":\"nope\"}",
      "{\"t\":\"req\",\"id\":\"x\"}",            // missing method/path
      "{\"t\":\"req\",\"method\":\"GET\",\"path\":\"/\"}",  // missing id
  };
  for (const char* b : bad) {
    JsonDocument doc;
    RelayFrame f;
    // A parse error OR a schema miss both must yield Unknown/false.
    if (deserializeJson(doc, b)) {
      // JSON itself invalid -> nothing to feed; that is a reject by construction.
      continue;
    }
    TEST_ASSERT_FALSE_MESSAGE(parseRelayFrame(doc, f), b);
    TEST_ASSERT_EQUAL_INT((int)FrameType::Unknown, (int)f.type);
  }
}

// Device->relay builders produce the exact field shapes the relay's zod expects.
static void test_build_hello() {
  JsonDocument doc;
  buildHello(doc, "dev_abc", "tok.xyz", "v4.2.0");
  TEST_ASSERT_EQUAL_STRING("hello", doc["t"]);
  TEST_ASSERT_EQUAL_INT(kProtocolVersion, doc["v"].as<int>());
  TEST_ASSERT_EQUAL_STRING("dev_abc", doc["deviceId"]);
  TEST_ASSERT_EQUAL_STRING("tok.xyz", doc["connectToken"]);
  TEST_ASSERT_EQUAL_STRING("v4.2.0", doc["fw"]);
}

static void test_build_res() {
  JsonDocument doc;
  JsonObject h = startRes(doc, "r1", 200);
  h["content-type"] = "application/json";
  setResBody(doc, "eyJvayI6dHJ1ZX0=");
  TEST_ASSERT_EQUAL_STRING("res", doc["t"]);
  TEST_ASSERT_EQUAL_STRING("r1", doc["id"]);
  TEST_ASSERT_EQUAL_INT(200, doc["status"].as<int>());
  TEST_ASSERT_EQUAL_STRING("application/json", doc["headers"]["content-type"]);
  TEST_ASSERT_EQUAL_STRING("eyJvayI6dHJ1ZX0=", doc["bodyB64"]);
}

static void test_build_res_no_body() {
  JsonDocument doc;
  startRes(doc, "r3", 204);
  setResBody(doc, nullptr);  // no body
  TEST_ASSERT_FALSE(doc["bodyB64"].is<const char*>());
}

// base64 round-trips, including the 3 residue classes and binary bytes.
static void test_base64_roundtrip() {
  const char* samples[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar",
                           "{\"devName\":\"Nimbus\"}"};
  for (const char* s : samples) {
    std::string enc;
    b64Encode(reinterpret_cast<const uint8_t*>(s), strlen(s), enc);
    std::vector<uint8_t> dec;
    TEST_ASSERT_TRUE(b64Decode(enc.c_str(), enc.size(), dec));
    TEST_ASSERT_EQUAL_UINT(strlen(s), dec.size());
    if (!dec.empty()) TEST_ASSERT_EQUAL_MEMORY(s, dec.data(), dec.size());
  }
  // Known-answer vectors.
  std::string e;
  b64Encode(reinterpret_cast<const uint8_t*>("foobar"), 6, e);
  TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", e.c_str());
  // All 256 byte values round-trip.
  std::vector<uint8_t> all(256);
  for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
  std::string enc;
  b64Encode(all.data(), all.size(), enc);
  std::vector<uint8_t> dec;
  TEST_ASSERT_TRUE(b64Decode(enc.c_str(), enc.size(), dec));
  TEST_ASSERT_EQUAL_UINT(256, dec.size());
  TEST_ASSERT_EQUAL_MEMORY(all.data(), dec.data(), 256);
}

static void test_base64_rejects_garbage() {
  std::vector<uint8_t> out;
  TEST_ASSERT_FALSE(b64Decode("!!!!", 4, out));
  TEST_ASSERT_FALSE(b64Decode("A", 1, out));  // single leftover sextet is impossible
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_relay_vectors_decode);
  RUN_TEST(test_rejects_malformed);
  RUN_TEST(test_build_hello);
  RUN_TEST(test_build_res);
  RUN_TEST(test_build_res_no_body);
  RUN_TEST(test_base64_roundtrip);
  RUN_TEST(test_base64_rejects_garbage);
  return UNITY_END();
}
