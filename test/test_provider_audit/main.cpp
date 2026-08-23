// Host tests for the per-key access-audit parsers (lib/core/provider_audit).
// Bodies mirror the REAL provider API shapes observed 2026-08-19:
//   Mistral GET /v1/connectors -> {"items":[{id,name,protocol,...}],"pagination":{}}
//   GET /v1/models             -> {"data":[{"id":...,"capabilities":{...}}]}
// These lock down the "ask the provider what the key can reach" path so the device
// no longer assumes connector ids/access.

#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/provider_audit.h"

using core::AuditConnector;
using core::connectorIdByName;
using core::parseMistralConnectors;
using core::parseModelsList;

static const char* kConnectors = R"({
  "items": [
    {"id":"0198e70f-57b0-77f6-a752-0a7f5ea2da35","name":"atlassian","protocol":"mcp"},
    {"id":"0198f11d-493e-76a8-9c90-913b7462e7de","name":"notion","protocol":"mcp"},
    {"id":"019d8b52-6f1c-72db-97bf-7284f2bf5ead","name":"github_app","protocol":"mcp"},
    {"id":"019df75b-9673-72be-ba4f-033723c972ea","name":"gmail","protocol":"mcp"}
  ],
  "pagination": {"total": 4}
})";

static const char* kModels = R"({
  "data": [
    {"id":"mistral-large-latest","capabilities":{"completion_chat":true}},
    {"id":"codestral-latest","capabilities":{"completion_fim":true}},
    {"id":"mistral-embed"}
  ]
})";

// Connectors parse: real UUIDs extracted, name preserved.
void test_connectors_parse(void) {
  std::vector<AuditConnector> cs;
  int n = parseMistralConnectors(kConnectors, cs);
  TEST_ASSERT_EQUAL_INT(4, n);
  TEST_ASSERT_EQUAL_INT(4, (int)cs.size());
  TEST_ASSERT_EQUAL_STRING("atlassian", cs[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("github_app", cs[2].name.c_str());
  TEST_ASSERT_EQUAL_STRING("019d8b52-6f1c-72db-97bf-7284f2bf5ead", cs[2].id.c_str());
  TEST_ASSERT_EQUAL_STRING("mcp", cs[2].protocol.c_str());
}

// The self-heal lookup: github_app -> its real UUID (not the name).
void test_connector_id_by_name(void) {
  std::vector<AuditConnector> cs;
  parseMistralConnectors(kConnectors, cs);
  TEST_ASSERT_EQUAL_STRING("019d8b52-6f1c-72db-97bf-7284f2bf5ead",
                           connectorIdByName(cs, "github_app").c_str());
  TEST_ASSERT_EQUAL_STRING("", connectorIdByName(cs, "nope").c_str());
}

// Models parse (shape shared by all three providers).
void test_models_parse(void) {
  std::vector<std::string> ms;
  int n = parseModelsList(kModels, ms);
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_STRING("mistral-large-latest", ms[0].c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-embed", ms[2].c_str());
}

// Bare-array tolerance + garbage/empty degrade to 0 (no crash).
void test_tolerance(void) {
  std::vector<AuditConnector> cs;
  TEST_ASSERT_EQUAL_INT(1, parseMistralConnectors(R"([{"id":"x","name":"slack"}])", cs));
  std::vector<std::string> ms;
  TEST_ASSERT_EQUAL_INT(2, parseModelsList(R"([{"id":"a"},{"id":"b"}])", ms));
  std::vector<AuditConnector> c2;
  TEST_ASSERT_EQUAL_INT(0, parseMistralConnectors("not json", c2));
  TEST_ASSERT_EQUAL_INT(0, parseMistralConnectors(R"({"items":[]})", c2));
  std::vector<std::string> m2;
  TEST_ASSERT_EQUAL_INT(0, parseModelsList(R"({"data":[]})", m2));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_connectors_parse);
  RUN_TEST(test_connector_id_by_name);
  RUN_TEST(test_models_parse);
  RUN_TEST(test_tolerance);
  return UNITY_END();
}
