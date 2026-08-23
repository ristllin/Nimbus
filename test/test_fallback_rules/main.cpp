#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/fallback_rules.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static bool has(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }

// A rule set matching the BINDING v1 schema (CUM-105), including an unknown field
// the device must tolerate.
static const char* kRules = R"({
  "version": 1,
  "unknownTopLevel": true,
  "rules": [
    { "id":"large-rl", "enabled":true,
      "match": { "sizeClass":["large"], "errorClass":["rate_limited","server_error"] },
      "to": [ {"provider":"anthropic"}, {"provider":"mistral","model":"mistral-large-latest"} ] },
    { "id":"glob-openai", "enabled":true, "futField":"ignore-me",
      "match": { "provider":["openai"], "model":["gpt-4*"] },
      "to": [ {"provider":"zai","model":"glm-5"} ] },
    { "id":"disabled-rule", "enabled":false,
      "match": {}, "to": [ {"provider":"mistral"} ] }
  ]
})";

static void test_parse_tolerates_unknown_fields() {
  FallbackRuleSet rs;
  size_t n = parseFallbackRules(kRules, rs);
  TEST_ASSERT_EQUAL_UINT(3, n);
  TEST_ASSERT_EQUAL_INT(1, rs.version);
  TEST_ASSERT_EQUAL_STRING("large-rl", rs.rules[0].id.c_str());
  TEST_ASSERT_EQUAL_UINT(2, rs.rules[0].to.size());
  TEST_ASSERT_EQUAL_STRING("mistral-large-latest", rs.rules[0].to[1].model.c_str());
}

static void test_rule_match_predicates() {
  FallbackRuleSet rs;
  parseFallbackRules(kRules, rs);
  const FallbackRule& largeRl = rs.rules[0];
  TurnContext ctx;
  ctx.sizeClass = "large";
  ctx.errorClass = ErrorClass::RateLimited;
  TEST_ASSERT_TRUE(ruleMatches(largeRl, ctx));          // sizeClass + errorClass any-of
  ctx.errorClass = ErrorClass::Timeout;                 // not in the rule's errorClass list
  TEST_ASSERT_FALSE(ruleMatches(largeRl, ctx));
  ctx.sizeClass = "small";
  ctx.errorClass = ErrorClass::RateLimited;
  TEST_ASSERT_FALSE(ruleMatches(largeRl, ctx));         // sizeClass mismatch
  // model trailing-* glob
  const FallbackRule& glob = rs.rules[1];
  TurnContext g;
  g.provider = "openai";
  g.model = "gpt-4o-mini";
  TEST_ASSERT_TRUE(ruleMatches(glob, g));               // gpt-4* matches gpt-4o-mini
  g.model = "gpt-5.5";
  TEST_ASSERT_FALSE(ruleMatches(glob, g));
  // disabled rule never matches
  TurnContext any;
  TEST_ASSERT_FALSE(ruleMatches(rs.rules[2], any));
}

static void test_select_walks_targets_and_skips_failed() {
  FallbackRuleSet rs;
  parseFallbackRules(kRules, rs);
  TurnContext ctx;
  ctx.provider = "openai";
  ctx.model = "gpt-5.5";
  ctx.sizeClass = "large";
  ctx.errorClass = ErrorClass::ServerError;
  // Only anthropic is available -> first target chosen.
  auto onlyAnthropic = [](const std::string& p, const std::string&) { return p == "anthropic"; };
  FallbackChoice c = selectFallback(rs, ctx, onlyAnthropic);
  TEST_ASSERT_TRUE(c.found);
  TEST_ASSERT_EQUAL_STRING("large-rl", c.ruleId.c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic", c.target.provider.c_str());
  // anthropic unavailable -> walks to the second target (mistral).
  auto onlyMistral = [](const std::string& p, const std::string&) { return p == "mistral"; };
  FallbackChoice c2 = selectFallback(rs, ctx, onlyMistral);
  TEST_ASSERT_TRUE(c2.found);
  TEST_ASSERT_EQUAL_STRING("mistral", c2.target.provider.c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-large-latest", c2.target.model.c_str());
  // nothing available -> no choice.
  auto none = [](const std::string&, const std::string&) { return false; };
  TEST_ASSERT_FALSE(selectFallback(rs, ctx, none).found);
}

static void test_embeddings_and_parsefail_never_fall_back() {
  FallbackRuleSet rs;
  parseFallbackRules(kRules, rs);
  auto all = [](const std::string&, const std::string&) { return true; };
  TurnContext emb;
  emb.provider = "openai";
  emb.sizeClass = "large";
  emb.errorClass = ErrorClass::ServerError;
  emb.embeddings = true;
  TEST_ASSERT_FALSE(selectFallback(rs, emb, all).found);   // embeddings excluded
  TurnContext pf;
  pf.provider = "openai";
  pf.sizeClass = "large";
  pf.errorClass = ErrorClass::None;                        // parse_fail maps to None
  TEST_ASSERT_FALSE(selectFallback(rs, pf, all).found);
}

static void test_default_ruleset_walks_priority() {
  std::vector<std::string> prio = {"openai", "anthropic", "mistral"};
  FallbackRuleSet rs = defaultRuleSet(prio);
  TEST_ASSERT_EQUAL_UINT(3, rs.rules.size());  // small/medium/large
  TurnContext ctx;
  ctx.provider = "openai";       // the failed one
  ctx.model = "gpt-5.5";
  ctx.sizeClass = "large";
  ctx.errorClass = ErrorClass::Network;
  auto all = [](const std::string&, const std::string&) { return true; };
  FallbackChoice c = selectFallback(rs, ctx, all);
  TEST_ASSERT_TRUE(c.found);
  TEST_ASSERT_EQUAL_STRING("anthropic", c.target.provider.c_str());  // first non-failed in priority
}

static void test_fabric_error_mapping() {
  // FabricErr order: Ok0 Network1 Auth2 RateLimited3 BadRequest4 NotFound5
  // Unsupported6 Timeout7 RemoteFail8 ParseFail9
  TEST_ASSERT_TRUE(errorClassFromFabric(3) == ErrorClass::RateLimited);
  TEST_ASSERT_TRUE(errorClassFromFabric(7) == ErrorClass::Timeout);
  TEST_ASSERT_TRUE(errorClassFromFabric(8) == ErrorClass::ServerError);  // remote_fail
  TEST_ASSERT_TRUE(errorClassFromFabric(1) == ErrorClass::Network);
  TEST_ASSERT_TRUE(errorClassFromFabric(9) == ErrorClass::None);         // parse_fail -> hard
  TEST_ASSERT_EQUAL_STRING("server_error", errorClassToken(ErrorClass::ServerError));
  TEST_ASSERT_TRUE(errorClassFromToken("rate_limited") == ErrorClass::RateLimited);
}

static void test_size_class_word_and_note() {
  TEST_ASSERT_EQUAL_STRING("small", sizeClassWord('S').c_str());
  TEST_ASSERT_EQUAL_STRING("large", sizeClassWord('L').c_str());
  TEST_ASSERT_EQUAL_STRING("", sizeClassWord(0).c_str());
  std::string note = fallbackNote("openai", "gpt-5.5", "anthropic", "", ErrorClass::RateLimited);
  TEST_ASSERT_TRUE(has(note, "[FALLBACK]"));
  TEST_ASSERT_TRUE(has(note, "openai/gpt-5.5"));
  TEST_ASSERT_TRUE(has(note, "anthropic"));
  TEST_ASSERT_TRUE(has(note, "rate_limited"));
}

static void test_json_round_trip() {
  FallbackRuleSet rs;
  parseFallbackRules(kRules, rs);
  JsonDocument doc;
  fallbackRulesToJson(rs, doc.to<JsonObject>());
  std::string out;
  serializeJson(doc, out);
  FallbackRuleSet back;
  size_t n = parseFallbackRules(out, back);
  TEST_ASSERT_EQUAL_UINT(rs.rules.size(), n);
  TEST_ASSERT_EQUAL_STRING("large-rl", back.rules[0].id.c_str());
  TEST_ASSERT_EQUAL_UINT(2, back.rules[0].to.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_tolerates_unknown_fields);
  RUN_TEST(test_rule_match_predicates);
  RUN_TEST(test_select_walks_targets_and_skips_failed);
  RUN_TEST(test_embeddings_and_parsefail_never_fall_back);
  RUN_TEST(test_default_ruleset_walks_priority);
  RUN_TEST(test_fabric_error_mapping);
  RUN_TEST(test_size_class_word_and_note);
  RUN_TEST(test_json_round_trip);
  UNITY_END();
  return 0;
}
