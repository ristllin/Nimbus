// Streaming filter-parse mechanics - the size-transparent response path. The
// device transport (src/agent/transport_tls.cpp) stream-parses the response off
// the socket into a filtered JsonDocument; here we exercise the exact ArduinoJson
// behaviour that path depends on (deserializeJson from a Stream + Filter +
// NestingLimit) at size, on the host, plus the HttpTransport::execJson default.
#include <unity.h>

#include <sstream>
#include <string>

#include <ArduinoJson.h>

#include "../support/fake_http.h"
#include "nimbus/harness/http.h"

using agent::HttpRequest;
using agent::HttpResponse;

void setUp() {}
void tearDown() {}

// A Mistral-style Conversations response: a FAT tool.execution payload (the part
// that OOMs the device when buffered whole) followed by the small message.output
// we actually want. The filter must drop the fat payload and keep only outputs.
static std::string bigConversationBody(int fatKilobytes) {
  std::string fat(fatKilobytes * 1024, 'x');
  std::string out = "{\"conversation_id\":\"conv_big\",\"outputs\":[";
  out += "{\"type\":\"tool.execution\",\"name\":\"notion\",\"result\":\"" + fat + "\"},";
  out += "{\"type\":\"message.output\",\"content\":\"Page created: abc-123\"}";
  out += "],\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":9}}";
  return out;
}

static JsonDocument conversationFilter() {
  JsonDocument f;
  f["conversation_id"] = true;
  JsonObject o = f["outputs"].add<JsonObject>();
  o["type"] = true;
  o["content"] = true;
  f["usage"] = true;
  return f;
}

// Stream-parse a large body and confirm ONLY the retained fields survive.
static void test_stream_filter_drops_fat_payload() {
  std::string body = bigConversationBody(64);  // 64 KB of junk we must not retain
  std::istringstream iss(body);
  JsonDocument filter = conversationFilter();
  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, iss, DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(16));
  TEST_ASSERT_EQUAL_MESSAGE(DeserializationError::Ok, err.code(), err.c_str());
  TEST_ASSERT_EQUAL_STRING("conv_big", doc["conversation_id"]);
  // The fat tool.execution payload is filtered out (result key not retained).
  TEST_ASSERT_FALSE(doc["outputs"][0]["result"].is<const char*>());
  // The message.output content survives.
  const char* content = nullptr;
  for (JsonObjectConst o : doc["outputs"].as<JsonArrayConst>())
    if (std::string(o["type"] | "") == "message.output") content = o["content"];
  TEST_ASSERT_NOT_NULL(content);
  TEST_ASSERT_EQUAL_STRING("Page created: abc-123", content);
}

// The retained document is a tiny fraction of the wire body (size-transparency).
static void test_retained_set_is_bounded() {
  std::string body = bigConversationBody(128);  // 128 KB on the wire
  std::istringstream iss(body);
  JsonDocument filter = conversationFilter();
  JsonDocument doc;
  deserializeJson(doc, iss, DeserializationOption::Filter(filter),
                  DeserializationOption::NestingLimit(16));
  // Whatever the wire size, the parsed doc holds only the small retained fields.
  TEST_ASSERT_TRUE(doc.memoryUsage() < 4096);
  TEST_ASSERT_TRUE(body.size() > 128 * 1024);
}

static std::string nestedObj(int depth) {
  std::string s;
  for (int i = 0; i < depth; i++) s += "{\"a\":";
  s += "1";
  for (int i = 0; i < depth; i++) s += "}";
  return s;
}

// A FILTERED-OUT subtree (outputs[].result) nested deeply must NOT abort the parse
// of the shallow retained field (outputs[].message.output.content). ArduinoJson
// counts nesting for skipped subtrees too, so the shared kResponseNestingLimit
// must clear real connector-response depth. (Regression guard: at the old limit 16
// this lost the reply the streaming path exists to deliver.)
static void test_deep_filtered_subtree_still_yields_reply() {
  std::string body =
      "{\"conversation_id\":\"c\",\"outputs\":["
      "{\"type\":\"tool.execution\",\"result\":" + nestedObj(40) + "},"
      "{\"type\":\"message.output\",\"content\":\"KEEP-ME\"}]}";
  std::istringstream iss(body);
  JsonDocument filter = conversationFilter();
  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, iss, DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(agent::kResponseNestingLimit));
  TEST_ASSERT_EQUAL_MESSAGE(DeserializationError::Ok, err.code(), err.c_str());
  const char* content = nullptr;
  for (JsonObjectConst o : doc["outputs"].as<JsonArrayConst>())
    if (std::string(o["type"] | "") == "message.output") content = o["content"];
  TEST_ASSERT_NOT_NULL(content);
  TEST_ASSERT_EQUAL_STRING("KEEP-ME", content);
}

// HttpTransport::execJson default (host/fake path) filter-parses the scripted body.
static void test_execjson_default_filters() {
  harness_test::FakeHttpTransport http;
  http.script.push_back({"", "", 200, bigConversationBody(8)});
  HttpRequest req;
  req.host = "api.mistral.ai";
  req.path = "/v1/conversations";
  JsonDocument filter = conversationFilter();
  JsonDocument doc;
  std::string err;
  int status = http.execJson(req, doc, filter, err);
  TEST_ASSERT_EQUAL(200, status);
  TEST_ASSERT_EQUAL_STRING("conv_big", doc["conversation_id"]);
  TEST_ASSERT_FALSE(doc["outputs"][0]["result"].is<const char*>());
}

// A transport error surfaces as status 0 through execJson (transport-fail contract).
static void test_execjson_transport_error() {
  harness_test::FakeHttpTransport http;
  http.script.push_back({"", "", 0, ""});  // scripted transport failure
  HttpRequest req;
  JsonDocument filter, doc;
  std::string err;
  int status = http.execJson(req, doc, filter, err);
  TEST_ASSERT_EQUAL(0, status);
  TEST_ASSERT_FALSE(err.empty());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_stream_filter_drops_fat_payload);
  RUN_TEST(test_retained_set_is_bounded);
  RUN_TEST(test_deep_filtered_subtree_still_yields_reply);
  RUN_TEST(test_execjson_default_filters);
  RUN_TEST(test_execjson_transport_error);
  return UNITY_END();
}
