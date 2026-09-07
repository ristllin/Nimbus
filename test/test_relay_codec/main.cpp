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
    } else if (v.type == 5) {  // ubegin
      TEST_ASSERT_EQUAL_STRING(v.id, f.upload.id);
      TEST_ASSERT_EQUAL_STRING(v.method, f.upload.method);
      TEST_ASSERT_EQUAL_STRING(v.path, f.upload.path);
      TEST_ASSERT_EQUAL_UINT32(v.totalLen, f.upload.totalLen);
      if (v.hdrKey) {
        TEST_ASSERT_FALSE(f.upload.headers.isNull());
        TEST_ASSERT_EQUAL_STRING(v.hdrVal, f.upload.headers[v.hdrKey].as<const char*>());
      }
    } else if (v.type == 6) {  // uchunk
      TEST_ASSERT_EQUAL_STRING(v.id, f.upload.id);
      TEST_ASSERT_EQUAL_STRING(v.bodyB64, f.upload.bodyB64);
      TEST_ASSERT_EQUAL_UINT32(v.seq, f.upload.seq);
      TEST_ASSERT_EQUAL_UINT32(v.off, f.upload.off);
    } else if (v.type == 7) {  // uend
      TEST_ASSERT_EQUAL_STRING(v.id, f.upload.id);
    } else if (v.type == 8) {  // uabort
      TEST_ASSERT_EQUAL_STRING(v.id, f.upload.id);
      TEST_ASSERT_EQUAL_STRING(v.byeReason, f.upload.reason);
    }
  }
}

// The upload frames each parse to their own FrameType and reject a missing id.
static void test_upload_frames() {
  struct {
    const char* json;
    int type;
  } ok[] = {
      {"{\"t\":\"ubegin\",\"id\":\"u\",\"method\":\"POST\",\"path\":\"/x\",\"totalLen\":9}", 5},
      {"{\"t\":\"uchunk\",\"id\":\"u\",\"seq\":3,\"off\":24,\"bodyB64\":\"AAAA\"}", 6},
      {"{\"t\":\"uend\",\"id\":\"u\"}", 7},
      {"{\"t\":\"uabort\",\"id\":\"u\",\"reason\":\"gone\"}", 8},
  };
  for (auto& c : ok) {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, c.json));
    RelayFrame f;
    TEST_ASSERT_TRUE_MESSAGE(parseRelayFrame(doc, f), c.json);
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.type, (int)f.type, c.json);
  }
  // Malformed: missing id (ubegin/uchunk/uend/uabort) or missing bodyB64 (uchunk).
  const char* bad[] = {
      "{\"t\":\"ubegin\",\"method\":\"POST\",\"path\":\"/x\"}",
      "{\"t\":\"uchunk\",\"id\":\"u\",\"seq\":0,\"off\":0}",
      "{\"t\":\"uend\"}",
      "{\"t\":\"uabort\"}",
  };
  for (const char* b : bad) {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, b));
    RelayFrame f;
    TEST_ASSERT_FALSE_MESSAGE(parseRelayFrame(doc, f), b);
  }
}

// buildUploadAck emits the exact device->relay shape the relay's zod expects.
static void test_build_upload_ack() {
  JsonDocument doc;
  buildUploadAck(doc, "u1", 8192);
  TEST_ASSERT_EQUAL_STRING("uack", doc["t"]);
  TEST_ASSERT_EQUAL_STRING("u1", doc["id"]);
  TEST_ASSERT_EQUAL_UINT32(8192, doc["off"].as<uint32_t>());
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

// CUM-182: the Welcome frame carries an optional `deviceId` (the relay's
// identity-bound hello-ack). Present -> parsed; absent -> "" (legacy relay).
static void test_welcome_device_id() {
  {
    JsonDocument doc;
    deserializeJson(doc, "{\"t\":\"welcome\",\"v\":1,\"heartbeatMs\":30000,\"deviceId\":\"dev_da5802\"}");
    RelayFrame f;
    TEST_ASSERT_TRUE(parseRelayFrame(doc, f));
    TEST_ASSERT_EQUAL_INT((int)FrameType::Welcome, (int)f.type);
    TEST_ASSERT_EQUAL_STRING("dev_da5802", f.deviceId);
  }
  {
    JsonDocument doc;
    deserializeJson(doc, "{\"t\":\"welcome\",\"v\":1,\"heartbeatMs\":30000}");
    RelayFrame f;
    TEST_ASSERT_TRUE(parseRelayFrame(doc, f));
    TEST_ASSERT_EQUAL_STRING("", f.deviceId);  // legacy relay: no echo
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_relay_vectors_decode);
  RUN_TEST(test_upload_frames);
  RUN_TEST(test_build_upload_ack);
  RUN_TEST(test_rejects_malformed);
  RUN_TEST(test_welcome_device_id);
  RUN_TEST(test_build_hello);
  RUN_TEST(test_build_res);
  RUN_TEST(test_build_res_no_body);
  RUN_TEST(test_base64_roundtrip);
  RUN_TEST(test_base64_rejects_garbage);
  return UNITY_END();
}
