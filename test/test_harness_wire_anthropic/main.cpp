#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/orch_schema.h"
#include "nimbus/orch/transcript.h"

// Stage H wire suite - the Anthropic provider: the Messages-API forced-tool
// single shot (ADVISORY schema - no strict:true, descriptions STRIPPED to fit
// the grammar budget), the nullable-enum anyOf pin (R_ANT_nullable_anyOf), the
// stateless messages[] tool loop with the echo-assistant-ONLY-with-tool_use
// rule (R_ANT_echo_tool_use), the beta/version headers, and the Managed-Agents
// sub-session chain (env/agent NVS caches, stale-404 clear, unsafe model names).

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::bodyHas;
using harness_test::headerOf;
using harness_test::reqBody;
namespace providers = agent::providers;
namespace orch = nimbus::orch;

void setUp() {
  LogCapture::install();
  providers::antForgetIds();   // the managed-agent ids live in RAM for a boot
}
void tearDown() { agent::hlog::setSink(nullptr); }

// Canned single-shot success: the tool_use input IS the turn.
static std::string singleShotBody() {
  return "{\"content\":[{\"type\":\"tool_use\",\"name\":\"orch_turn\","
         "\"input\":{\"reply\":\"hello there\",\"memory\":\"\"}}],"
         "\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":100,\"output_tokens\":20}}";
}
// Loop round: prose + a registry tool_use.
static std::string toolUseBody() {
  return "{\"content\":[{\"type\":\"text\",\"text\":\"let me check\"},"
         "{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
         "\"input\":{\"q\":\"tea\"}}],"
         "\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}";
}
static std::string finalToolBody() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
         "\"input\":{\"reply\":\"done\"}}],"
         "\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":60,\"output_tokens\":15}}";
}

// ---- single-shot ------------------------------------------------------------

static void test_single_shot_request_shape() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, singleShotBody()});
  auto pd = d.contract();
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  const agent::HttpRequest& r = d.http.seen[0];
  // Auth trio: x-api-key (NOT Bearer) + pinned version + the managed-agents beta.
  TEST_ASSERT_EQUAL_STRING("sk-fake-ant", headerOf(r, "x-api-key").c_str());
  TEST_ASSERT_EQUAL_STRING("2023-06-01", headerOf(r, "anthropic-version").c_str());
  TEST_ASSERT_EQUAL_STRING("managed-agents-2026-04-01", headerOf(r, "anthropic-beta").c_str());
  TEST_ASSERT_EQUAL_STRING("", headerOf(r, "Authorization").c_str());
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"model\":\"model-anthropic\""));
  // The output-cap table decides max_tokens (time-clamped for an unknown id's
  // slowest rate); no effort for an id the table does not know (the field 400s
  // on families without it).
  const providers::OutputCap cap =
      providers::outputCapFor("anthropic", "model-anthropic", providers::RoundKind::SingleShot, 120000);
  TEST_ASSERT_TRUE(bodyHas(d, 0, ("\"max_tokens\":" + std::to_string(cap.maxTokens)).c_str()));
  TEST_ASSERT_TRUE(cap.maxTokens > 3000);   // the old cap that cut thinking models' turns
  TEST_ASSERT_FALSE(bodyHas(d, 0, "output_config"));
  // v4.1.1 prompt caching: system rides as a content-block ARRAY so the block
  // can carry cache_control, and the tools prefix gets a breakpoint too. These
  // assertions pin the caching - if the shape reverts to a bare string, the
  // provider silently stops caching and every turn bills full price again.
  TEST_ASSERT_TRUE(bodyHas(d, 0,
      "\"system\":[{\"type\":\"text\",\"text\":\"SYS\",\"cache_control\":{\"type\":\"ephemeral\"}}]"));
  // The TOOL breakpoint: the tool object's trailing cache_control sits right
  // before the tools-array close + tool_choice. (Not an OR with a bare
  // "cache_control" - that would be satisfied by the system block alone.)
  TEST_ASSERT_TRUE(bodyHas(d, 0,
      "\"cache_control\":{\"type\":\"ephemeral\"}}],\"tool_choice\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"role\":\"user\",\"content\":\"USER\""));
  // NEVER forced: Sonnet 5.5, Opus 5.5 and Fable 5.1 answer a forced tool_choice
  // with HTTP 400 (verified live 18 Oct). auto + the orch_turn tool.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"tool_choice\":{\"type\":\"auto\"}"));
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"type\":\"tool\""));
  // ADVISORY schema: NO strict flag on Anthropic (grammar budget, verified live).
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"strict\":true"));
  // Descriptions are STRIPPED from the schema to fit the grammar budget (the
  // model still reads them via ORCH_FIELD_DOCS in the system prompt)...
  TEST_ASSERT_FALSE(bodyHas(d, 0, "Text to send the owner now"));
  // ...but the schema structure itself rides, incl. the nullable-enum anyOf form.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"input_schema\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "anyOf"));
}

// R_ANT_nullable_anyOf: nullable ENUMS must be anyOf[{enum},{null}] - the one
// form BOTH strict dialects accept - pinned on the canonical schema constant
// (the single source every provider embeds).
static void test_nullable_enum_anyof_in_schema_source() {
  const char* schema = orch::ORCH_SCHEMA_BODY;
  TEST_ASSERT_NOT_NULL(strstr(schema,
      "\"posture\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"dark\",\"calm\",\"full\"]},"
      "{\"type\":\"null\"}]}"));
  TEST_ASSERT_NOT_NULL(strstr(schema,
      "\"sfxTheme\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"pulse\"]},"
      "{\"type\":\"null\"}]}"));
}

static void test_single_shot_parse_usage_and_marker() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, singleShotBody()});
  auto pd = d.contract();
  std::string conv, out, err;
  orch::TokenUsage u;
  bool ok = providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, &u);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"hello there\",\"memory\":\"\"}", out.c_str());
  TEST_ASSERT_EQUAL_STRING("messages", conv.c_str());   // stateless marker
  TEST_ASSERT_EQUAL(100, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(20, (int)u.completionTokens);
}

static void test_error_mapping() {
  {  // 401 with message
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 401, "{\"error\":{\"message\":\"bad key\"}}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("messages HTTP 401: bad key", err.c_str());
  }
  {  // transport fail
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 0, ""});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("network", err.c_str());
  }
  {  // a cut reply is retried ONCE with more room, then fails naming the limit
    FakeProviderDeps d;
    for (int i = 0; i < 2; i++)
      d.http.script.push_back({"", "", 200,
          "{\"content\":[{\"type\":\"text\"}],\"stop_reason\":\"max_tokens\"}"});
    auto pd = d.contract();
    pd.orchModel = [](const char*) { return std::string("claude-haiku-5-5"); };
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("cut at the output limit (12000 tokens)", err.c_str());
    TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
    TEST_ASSERT_TRUE(bodyHas(d, 0, "\"max_tokens\":8192,\"output_config\":{\"effort\":\"medium\"}"));
    // Never the identical body: more room and one effort step down.
    TEST_ASSERT_TRUE(bodyHas(d, 1, "\"max_tokens\":12000,\"output_config\":{\"effort\":\"low\"}"));
  }
  {  // an empty reply still names its stop_reason, not a blind "no tool_use"
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"content\":[],\"stop_reason\":\"refusal\"}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("no orch_turn tool_use (stop_reason=refusal)", err.c_str());
  }
  {  // no key
    FakeProviderDeps d;
    d.keys.erase("anthropic");
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("no Anthropic key", err.c_str());
    TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());
  }
}

// ---- tool loop --------------------------------------------------------------

// Stage 2 phase 2: the round-2 request body is RENDERED from the canonical
// transcript (not an accumulator). Pin the full message SEQUENCE + shape so a
// renderer regression (dropped prose, reordered blocks, orphaned pairing)
// can't hide behind the coarser substring checks below.
static void test_loop_round2_body_rendered_from_transcript() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, toolUseBody()});
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, finalToolBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  // Parse round 2's body and assert the exact message sequence:
  //   [0] user seed (one text block, the same bytes on every round)
  //   [1] assistant: [text prose, tool_use tu_1] - prose PRESERVED in the render
  //   [2] user: [tool_result for tu_1]
  JsonDocument body;
  // The body embeds the orch_turn schema in tools[] - deeper than ArduinoJson's
  // default nesting limit (the documented NestingLimit(16) gotcha).
  TEST_ASSERT_EQUAL(DeserializationError::Ok,
                    deserializeJson(body, d.http.seen[1].body,
                                    DeserializationOption::NestingLimit(16)).code());
  JsonArrayConst msgs = body["messages"].as<JsonArrayConst>();
  TEST_ASSERT_EQUAL(3, (int)msgs.size());
  TEST_ASSERT_EQUAL_STRING("user", msgs[0]["role"] | "");
  TEST_ASSERT_EQUAL_STRING("text", msgs[0]["content"][0]["type"] | "");
  TEST_ASSERT_EQUAL_STRING("USER", msgs[0]["content"][0]["text"] | "");
  TEST_ASSERT_EQUAL_STRING("assistant", msgs[1]["role"] | "");
  JsonArrayConst ac = msgs[1]["content"].as<JsonArrayConst>();
  TEST_ASSERT_EQUAL(2, (int)ac.size());
  TEST_ASSERT_EQUAL_STRING("text", ac[0]["type"] | "");
  TEST_ASSERT_EQUAL_STRING("let me check", ac[0]["text"] | "");
  TEST_ASSERT_EQUAL_STRING("tool_use", ac[1]["type"] | "");
  TEST_ASSERT_EQUAL_STRING("tu_1", ac[1]["id"] | "");
  TEST_ASSERT_EQUAL_STRING("memory_search", ac[1]["name"] | "");
  TEST_ASSERT_EQUAL_STRING("tea", ac[1]["input"]["q"] | "");
  TEST_ASSERT_EQUAL_STRING("user", msgs[2]["role"] | "");
  JsonArrayConst uc = msgs[2]["content"].as<JsonArrayConst>();
  TEST_ASSERT_EQUAL(1, (int)uc.size());
  TEST_ASSERT_EQUAL_STRING("tool_result", uc[0]["type"] | "");
  TEST_ASSERT_EQUAL_STRING("tu_1", uc[0]["tool_use_id"] | "");
  TEST_ASSERT_EQUAL_STRING("tool-ok", uc[0]["content"] | "");
}

// R_ANT_echo_tool_use: when (and ONLY when) the round carried tool_use blocks,
// the assistant message is echoed verbatim into the replayed messages[] so the
// next round's tool_result blocks pair with the tool_use ids.
static void test_loop_echoes_assistant_with_tool_use() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, toolUseBody()});
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, finalToolBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  orch::TokenUsage u;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, &rig.ht, &u);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  // Round 0: tool rounds use tool_choice auto (may call a tool OR finish).
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"tool_choice\":{\"type\":\"auto\"}"));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"name\":\"memory_search\""));
  // Round 1: the assistant tool_use turn is echoed + answered with tool_result.
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"role\":\"assistant\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"id\":\"tu_1\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"type\":\"tool_result\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"tool_use_id\":\"tu_1\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"content\":\"tool-ok\""));
  // The dispatched call reached the rig.
  TEST_ASSERT_EQUAL(1, (int)rig.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("memory_search", rig.dispatched[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"q\":\"tea\"}", rig.dispatched[0].argsJson.c_str());
  // Outcome: usage summed; stateless marker convId.
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
  TEST_ASSERT_EQUAL(110, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(25, (int)u.completionTokens);
  TEST_ASSERT_EQUAL_STRING("messages", conv.c_str());
}

// Replay with one transcript-level fold and hysteresis (H6): a round's
// messages[] continues the previous round's byte for byte (the moving cache point
// aside) until the replay passes its budget; then the oldest whole rounds fold
// into one line each in the seeded message, down to half the budget, so the
// request is rewritten about once per half-budget of new results - and the
// newest result always reaches the model whole (no "[not shown]" late in a long
// loop). Every tool_use still on the wire stays answered.
static std::string messagesWithoutCachePoints(const std::string& body) {
  JsonDocument doc;
  deserializeJson(doc, body, DeserializationOption::NestingLimit(16));
  JsonArray msgs = doc["messages"].as<JsonArray>();
  for (JsonObject m : msgs)
    if (m["content"].is<JsonArray>())
      for (JsonObject b : m["content"].as<JsonArray>()) b.remove("cache_control");
  std::string out;
  serializeJson(msgs, out);
  return out;
}
static void test_loop_replay_folds_with_hysteresis_over_budget() {
  FakeProviderDeps d;
  for (int i = 1; i <= 5; i++) {
    std::string body = "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_" + std::to_string(i) +
                       "\",\"name\":\"memory_search\",\"input\":{\"q\":\"r" + std::to_string(i) +
                       "\"}}],\"stop_reason\":\"tool_use\"}";
    d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, body});
  }
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, finalToolBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  // Each result: 7 KB of filler (well under the 60,000-byte provider string guard)
  // plus a per-round TAIL marker, so the third result passes the 16 KB replay budget.
  int calls = 0;
  rig.ht.dispatch = [&rig, &calls](const nimbus::orch::HeadToolCall& c) {
    nimbus::orch::HeadToolResult r;
    r.id = c.id;
    r.name = c.name;
    r.output = std::string(7000, 'z') + "TAILMARK" + std::to_string(calls++);
    rig.dispatched.push_back(c);
    return r;
  };
  rig.ht.cfg.maxToolResultBytes = 0;   // isolate the replay budget from the clamp
  rig.ht.cfg.maxTotalToolBytes = 0;
  rig.ht.cfg.maxRounds = 8;
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(6, (int)d.http.seen.size());
  int rewrites = 0;
  for (size_t k = 1; k < d.http.seen.size(); k++) {
    std::string prev = messagesWithoutCachePoints(d.http.seen[k - 1].body);
    std::string next = messagesWithoutCachePoints(d.http.seen[k].body);
    prev.pop_back();   // drop the closing ']' - the next request continues the array
    if (next.compare(0, prev.size(), prev) != 0) rewrites++;
  }
  // 5 results of ~7 KB against the 16 KB budget: folds at rounds 3 and 5 only
  // (the T6 bound is ceil((35 - 16) / 8) + 1 = 4), never one per round.
  TEST_ASSERT_EQUAL(2, rewrites);
  const std::string& b = d.http.seen.back().body;
  TEST_ASSERT_TRUE(b.find("TAILMARK4") != std::string::npos);   // the newest result, whole
  TEST_ASSERT_TRUE(b.find("[not shown") == std::string::npos);
  TEST_ASSERT_TRUE(b.find("[cut:") == std::string::npos);
  TEST_ASSERT_TRUE(b.find("EARLIER TOOL ROUNDS OF THIS TURN") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("- round 0: memory_search") != std::string::npos);
  TEST_ASSERT_TRUE(bodyHas(d, 5, "USER"));
  for (int i = 1; i <= 5; i++) {
    const std::string id = "tu_" + std::to_string(i);
    const bool hasUse = b.find("\"id\":\"" + id + "\"") != std::string::npos;
    const bool hasRes = b.find("\"tool_use_id\":\"" + id + "\"") != std::string::npos;
    TEST_ASSERT_TRUE_MESSAGE(hasUse == hasRes, ("orphan tool_use/result for " + id).c_str());
  }
}

// The other half of R_ANT_echo_tool_use: a text-ONLY (stalled) round is NOT
// echoed on tool rounds. On the forced final round after the stall it IS: the
// prose rides back as an assistant message followed by a user nudge (the request
// still ends on a user turn - no prefill), so the final answer keeps it. And the
// final round is never forced: tool_choice auto (Sonnet 5.5, Opus 5.5 and Fable
// 5.1 answer a forced tool_choice with HTTP 400).
static void test_loop_stall_echoed_on_the_unforced_final_round() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"just prose\"}],"
      "\"stop_reason\":\"end_turn\"}"});
  d.http.script.push_back({"", "", 200, finalToolBody()});
  auto pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("claude-sonnet-5-5"); };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  // The nudge is the last message: one text block carrying the moving cache
  // point (prompt_layout.h); the echoed answer before it carries none.
  TEST_ASSERT_TRUE(bodyHas(d, 1,
      "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"just prose\"}]},"
      "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Now call orch_turn with your "
      "final complete turn, keeping the answer you gave above.\",\"cache_control\":{\"type\":\"ephemeral\"}}]}]"));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"tool_choice\":{\"type\":\"auto\"}"));
  TEST_ASSERT_FALSE(bodyHas(d, 1, "\"type\":\"tool\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "FINAL turn"));   // the final-round notice rides the system text
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
}

// The final round answered in prose: delivered, never "tool budget reached
// without a final answer". A prose round on a TOOL round is still a stall (the
// counter-test for the loosened acceptance).
static void test_loop_prose_final_accepted_only_on_the_final_round() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"Here is my answer.\"}],\"stop_reason\":\"end_turn\"}"});
  d.http.script.push_back({"", "", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"Here is my answer.\"}],\"stop_reason\":\"end_turn\"}"});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, &rig.ht, nullptr),
                           err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());   // round 0 was a stall, round 1 the final
  TEST_ASSERT_EQUAL_STRING("Here is my answer.", out.c_str());
  TEST_ASSERT_EQUAL(0, (int)rig.dispatched.size());
  TEST_ASSERT_TRUE(LogCapture::contains("final round answered in prose"));
}

// A cut round is retried ONCE in the step with a body that differs (more room,
// effort one step down), then fails as Cut with the real cause: never echoed,
// never "recovered", never resent identically.
static void test_loop_max_tokens_cut_retries_then_fails_loud() {
  FakeProviderDeps d;
  for (int i = 0; i < 2; i++)
    d.http.script.push_back({"", "", 200,
        "{\"content\":[{\"type\":\"text\",\"text\":\"trunc\"}],\"stop_reason\":\"max_tokens\"}"});
  auto pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("claude-haiku-5-5"); };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, &rig.ht, nullptr));
  TEST_ASSERT_EQUAL_STRING("cut at the output limit (12000 tokens)", err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_TRUE(reqBody(d, 0) != reqBody(d, 1));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"max_tokens\":8192,\"output_config\":{\"effort\":\"medium\"}"));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"max_tokens\":12000,\"output_config\":{\"effort\":\"low\"}"));
}

// CUM-526 (the CUM-506 class): a tool_use block batched with the final orch_turn
// runs, whichever side of orch_turn it sits on - it used to be dropped when it
// came AFTER, so the reply said "saved" with nothing written. The step carries
// the sibling as a tool call, the controller runs it exactly once before
// accepting a final answer, and the premature turn is replaced by the next
// round's (the replay carries the call and its result, never the premature
// orch_turn block).
static std::string finalWithSiblingBody(bool orchFirst) {
  const std::string fin =
      "{\"type\":\"tool_use\",\"id\":\"tu_fin\",\"name\":\"orch_turn\","
      "\"input\":{\"reply\":\"I've saved that permanently.\"}}";
  const std::string mw =
      "{\"type\":\"tool_use\",\"id\":\"tu_mw\",\"name\":\"memory_write\","
      "\"input\":{\"content\":\"Omar's research boat is called the Nereid II.\",\"permanent\":true}}";
  return "{\"content\":[" + (orchFirst ? fin + "," + mw : mw + "," + fin) +
         "],\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":70,\"output_tokens\":30}}";
}

static void test_loop_sibling_after_orch_turn_is_dispatched_once() {
  for (bool orchFirst : {true, false}) {
    FakeProviderDeps d;
    d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, finalWithSiblingBody(orchFirst)});
    d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, finalToolBody()});
    auto pd = d.contract();
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    rig.result = "stored memory m00000001";
    std::string conv, out, err;
    bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, &rig.ht, nullptr);
    TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)rig.dispatched.size(), orchFirst ? "orch_turn first" : "orch_turn last");
    TEST_ASSERT_EQUAL_STRING("memory_write", rig.dispatched[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("tu_mw", rig.dispatched[0].id.c_str());
    TEST_ASSERT_TRUE(rig.dispatched[0].argsJson.find("Nereid II") != std::string::npos);
    TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
    TEST_ASSERT_TRUE(bodyHas(d, 1, "\"tool_use_id\":\"tu_mw\""));
    TEST_ASSERT_TRUE(bodyHas(d, 1, "stored memory m00000001"));
    TEST_ASSERT_FALSE(bodyHas(d, 1, "tu_fin"));   // the premature final is gone
    TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
  }
}

// One antLoopStep round over a scripted /v1/messages response.
static nimbus::orch::HeadStep stepOver(const std::string& body, const char* user, int answers = 1) {
  FakeProviderDeps d;
  for (int i = 0; i < answers; i++) d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, body});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  nimbus::orch::Transcript tr;
  tr.addUser(user);
  const std::string instr = "SYS";
  return providers::antLoopStep(pd, instr, rig.ht, nullptr, tr)(true, {}, 60000, "");
}

// The step itself: orch_turn first, a memory_write after it - one finished step
// that carries the final turn AND the sibling call (0 calls before CUM-526).
static void test_loop_step_carries_sibling_after_orch_turn() {
  const nimbus::orch::HeadStep st = stepOver(finalWithSiblingBody(true), "remember my boat is the Nereid II");
  TEST_ASSERT_TRUE_MESSAGE(st.ok, st.error.c_str());
  TEST_ASSERT_TRUE(st.finished);
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"I've saved that permanently.\"}", st.finalTurn.c_str());
  TEST_ASSERT_EQUAL(1, (int)st.toolCalls.size());
  TEST_ASSERT_EQUAL_STRING("memory_write", st.toolCalls[0].name.c_str());
}

// Two orch_turn blocks in one response: the FIRST is the turn (the OpenAI and
// Mistral steps keep the first too); a later one never overwrites it.
static void test_loop_step_first_orch_turn_wins() {
  const nimbus::orch::HeadStep st = stepOver(
      "{\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"first\"}},"
      "{\"type\":\"tool_use\",\"id\":\"b\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"second\"}}],"
      "\"stop_reason\":\"tool_use\"}",
      "hi");
  TEST_ASSERT_TRUE(st.finished);
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"first\"}", st.finalTurn.c_str());
  TEST_ASSERT_EQUAL(0, (int)st.toolCalls.size());
}

// prism (roast + logic): a response cut by max_tokens can end INSIDE its last
// tool_use block; that call must be retried, never run (a half-written
// memory_write). Now ANY cut response - a call cut mid-input, or a complete call
// beside a cut - is retried with headroom and, if still cut, fails: nothing from a
// cut response is dispatched.
static void test_loop_call_cut_by_max_tokens_never_runs() {
  const std::string fin = "{\"type\":\"tool_use\",\"id\":\"tu_fin\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"saved\"}}";
  const std::string cut = "{\"type\":\"tool_use\",\"id\":\"tu_mw\",\"name\":\"memory_write\",\"input\":{\"content\":\"Omar's boat is the Ner\"}}";
  for (const std::string& blocks : {fin + "," + cut, cut}) {   // after a final, or a plain tool round
    FakeProviderDeps d;
    for (int i = 0; i < 2; i++)
      d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200,
                               "{\"content\":[" + blocks + "],\"stop_reason\":\"max_tokens\"}"});
    auto pd = d.contract();
    pd.orchModel = [](const char*) { return std::string("claude-haiku-5-5"); };
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, &rig.ht, nullptr));
    TEST_ASSERT_EQUAL_STRING("cut at the output limit (12000 tokens)", err.c_str());
    TEST_ASSERT_EQUAL(0, (int)rig.dispatched.size());
    // CUM-621: the per-host loop hands the cut orch_turn back for the salvage.
    if (blocks != cut) TEST_ASSERT_EQUAL_STRING("{\"reply\":\"saved\"}", out.c_str());
  }
  // A complete call before a trailing orch_turn, in a response that says
  // max_tokens: retried once with more room (every family has a retry since
  // L19-44), still cut, the step is cut - not finished, no calls, the orch_turn
  // kept for the reply salvage.
  const nimbus::orch::HeadStep st = stepOver(
      "{\"content\":[" +
          std::string("{\"type\":\"tool_use\",\"id\":\"tu_mw\",\"name\":\"memory_write\",\"input\":{\"content\":\"x\"}},") +
          fin + "],\"stop_reason\":\"max_tokens\"}",
      "hi", 2);
  TEST_ASSERT_FALSE(st.ok);
  TEST_ASSERT_FALSE(st.finished);
  TEST_ASSERT_EQUAL((int)nimbus::orch::StepErr::Cut, (int)st.kind);
  TEST_ASSERT_EQUAL_STRING("max_tokens", st.cut.c_str());
  TEST_ASSERT_EQUAL(0, (int)st.toolCalls.size());
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"saved\"}", st.finalTurn.c_str());
}

// INVERTED (was "an orch_turn whose response says stop_reason=max_tokens is still
// the turn"): a cut orch_turn is NOT the turn. Anthropic documents a tool_use cut
// by max_tokens as incomplete - its input may be a fragment or {} (16 Oct: "turn
// parse fail (reply) json={}"). It is retried with headroom; the complete answer
// of the retry is the turn.
static void test_loop_cut_orch_turn_is_retried_not_accepted() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200,
      "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
      "\"input\":{}}],\"stop_reason\":\"max_tokens\"}"});
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, finalToolBody()});
  auto pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("claude-opus-5"); };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, &rig.ht, nullptr),
                           err.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_TRUE(reqBody(d, 0) != reqBody(d, 1));
  TEST_ASSERT_EQUAL(0, (int)rig.dispatched.size());
}

// ---- sub-session (Managed Agents) -------------------------------------------

static void test_sub_dispatch_full_chain_and_caches() {
  FakeProviderDeps d;   // cold caches: env + agent created, then session + turn
  d.http.script.push_back({"api.anthropic.com", "/v1/environments", 200, "{\"id\":\"env_1\"}"});
  d.http.script.push_back({"api.anthropic.com", "/v1/agents", 200, "{\"id\":\"ag_1\"}"});
  d.http.script.push_back({"api.anthropic.com", "/v1/sessions", 200, "{\"id\":\"sess_1\"}"});
  d.http.script.push_back({"api.anthropic.com", "/v1/sessions/sess_1/events", 200, "{\"data\":[]}"});
  auto pd = d.contract();
  agent::Directive dir;
  dir.instruction = "do the task";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::antDispatch(pd, "claude-sonnet-4-6", dir, jobId));
  TEST_ASSERT_EQUAL_STRING("anthropic:sess_1", jobId);
  // NVS caches written once each.
  TEST_ASSERT_EQUAL_STRING("env_1", d.antEnv.c_str());
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-4-6=ag_1;", d.antAgents.c_str());
  // The agent creation pins the built-in toolset; the session binds agent + env.
  TEST_ASSERT_TRUE(bodyHas(d, 1, "agent_toolset_20260401"));
  // Owner connectors ride the agent as mcp_servers[] (attachAnthropic hook).
  TEST_ASSERT_TRUE(bodyHas(d, 1, "mcp_servers"));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "fake-mcp"));
  TEST_ASSERT_EQUAL(1, d.anthropicAttaches);
  TEST_ASSERT_TRUE(bodyHas(d, 2, "\"agent\":\"ag_1\""));
  TEST_ASSERT_TRUE(bodyHas(d, 2, "\"environment_id\":\"env_1\""));
  // The turn is one user.message event.
  TEST_ASSERT_TRUE(bodyHas(d, 3, "\"type\":\"user.message\""));
  TEST_ASSERT_TRUE(bodyHas(d, 3, "do the task"));
  // Warm dispatch: caches hit, only session + turn on the wire.
  d.http.script.push_back({"", "/v1/sessions", 200, "{\"id\":\"sess_2\"}"});
  d.http.script.push_back({"", "/v1/sessions/sess_2/events", 200, "{\"data\":[]}"});
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::antDispatch(pd, "claude-sonnet-4-6", dir, jobId));
  TEST_ASSERT_EQUAL(6, (int)d.http.seen.size());
}

static void test_sub_dispatch_stale_404_clears_caches() {
  FakeProviderDeps d;
  d.antEnv = "env_stale";
  d.antAgents = "claude-sonnet-4-6=ag_stale;";
  d.http.script.push_back({"", "/v1/sessions", 404, "{}"});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "x";
  char jobId[72];
  TEST_ASSERT_EQUAL((int)agent::FabricErr::NotFound,   // CUM-474: 404 classified
                    (int)providers::antDispatch(pd, "claude-sonnet-4-6", dir, jobId));
  // Both caches cleared so the next dispatch recreates them.
  TEST_ASSERT_EQUAL_STRING("", d.antEnv.c_str());
  TEST_ASSERT_EQUAL_STRING("", d.antAgents.c_str());
}

static void test_sub_unsafe_model_name_falls_back() {
  FakeProviderDeps d;
  d.antEnv = "env_1";
  d.http.script.push_back({"", "/v1/agents", 200, "{\"id\":\"ag_1\"}"});
  d.http.script.push_back({"", "/v1/sessions", 200, "{\"id\":\"sess_1\"}"});
  d.http.script.push_back({"", "/v1/sessions/sess_1/events", 200, "{\"data\":[]}"});
  auto pd = d.contract();
  agent::Directive dir;
  dir.instruction = "x";
  dir.model = "evil=model;inject";   // map delimiters -> rejected
  char jobId[72];
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::antDispatch(pd, "claude-sonnet-4-6", dir, jobId));
  TEST_ASSERT_TRUE(LogCapture::contains("unsafe model name"));
  // The agent was created for the DEFAULT model, and the map keys it.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"model\":\"claude-sonnet-4-6\""));
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-4-6=ag_1;", d.antAgents.c_str());
}

// ---- CUM-514: a sub-agent's saved report starts at the report ----------------
// Bench (nimbus-light, T08): the Sweden workstream's saved file began "I'll start
// with Boverket's own pages on the current rules and any transition from BBR.The
// new fire rules are BFS 2024:7. I'll read Boverket's guidance..." - every
// agent.message (narration before each tool call, then the report) glued together
// with no separator. Canned session transcript in the shape the events poll
// returns (one buffered agent.message per model request).
static std::string antMsg(const std::string& text) {
  return "{\"type\":\"agent.message\",\"content\":[{\"type\":\"text\",\"text\":\"" + text + "\"}]}";
}
// The report body at a realistic size (the bench file was ~11 KB; a few KB here):
// JSON-escaped, as the events poll returns it.
static const std::string kSwedenReport = [] {
  std::string r = "# Swedish fire rules for a 3-storey timber-frame home\\n\\n";
  for (int i = 0; i < 12; ++i)
    r += "BFS 2024:7 replaces BBR section 5 from 2026. A three-storey dwelling is building "
         "class Br1: load-bearing timber needs R60 and separating walls EI60. ";
  return r;
}();

static void test_cum514_artifact_drops_the_narration() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/sessions/sess_1/events", 200,
      "{\"data\":[" +
      antMsg("I'll start with Boverket's own pages on the current rules and any transition "
             "from BBR.") + "," +
      antMsg("The new fire rules are BFS 2024:7. I'll read Boverket's guidance and the "
             "regulation text.") + "," +
      antMsg("Those pages are mostly navigation. curl works. I'll write a helper.") + "," +
      antMsg(kSwedenReport) + "," +
      "{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(pd, "anthropic:sess_1", env));
  TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
  std::string want = kSwedenReport;
  size_t at;
  while ((at = want.find("\\n")) != std::string::npos) want.replace(at, 2, "\n");
  TEST_ASSERT_EQUAL_STRING(want.c_str(), env.reply);
  TEST_ASSERT_TRUE(std::string(env.reply).find("I'll start") == std::string::npos);
}

// The class around the rule: a closing note AFTER the report is kept (joined by a
// blank line); a one-message run (the recorded live fixture shape: one message,
// several text blocks) is byte-identical to before; a run of short messages only
// keeps the longest and what follows.
static void test_cum514_artifact_keeps_what_follows_the_report() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"data\":[" + antMsg("Checking the sources first.") + "," + antMsg(kSwedenReport) + "," +
      antMsg("Saved as sweden-rules.md.") + "," +
      "{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(pd, "anthropic:sess_1", env));
  const std::string r(env.reply);
  TEST_ASSERT_TRUE(r.rfind("# Swedish fire rules", 0) == 0);
  const std::string tail = "\n\nSaved as sweden-rules.md.";
  TEST_ASSERT_TRUE(r.size() > tail.size() && r.compare(r.size() - tail.size(), tail.size(), tail) == 0);

  FakeProviderDeps one;   // one message, several blocks: concatenated exactly as before
  one.http.script.push_back({"", "", 200,
      "{\"data\":[{\"type\":\"agent.message\",\"content\":["
      "{\"type\":\"text\",\"text\":\"curl 8.22.0\"},{\"type\":\"text\",\"text\":\", dated Sep 2\"}]},"
      "{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
  auto opd = one.contract();
  agent::ResultEnvelope oenv{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(opd, "anthropic:sess_1", oenv));
  TEST_ASSERT_EQUAL_STRING("curl 8.22.0, dated Sep 2", oenv.reply);
}

// prism: the narration rule never drops real work. A report written over two
// model requests (a long section, a tool call, a longer section) keeps both
// sections in order; only the short note before them goes.
static void test_cum514_artifact_keeps_a_long_earlier_section() {
  const std::string sectionA(1500, 'a');
  const std::string sectionB(3000, 'b');
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"data\":[" + antMsg("Let me check the sources.") + "," + antMsg(sectionA) + "," +
      antMsg("Fetching one more page.") + "," + antMsg(sectionB) + "," +
      "{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(pd, "anthropic:sess_1", env));
  TEST_ASSERT_EQUAL_STRING((sectionA + "\n\n" + sectionB).c_str(), env.reply);
}

// re-review: a short report keeps the narration floor - its 100-char working
// notes are still dropped.
static void test_cum514_short_report_still_drops_notes() {
  const std::string report(400, 'r');
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"data\":[" + antMsg("I'll start by searching the official register for the current rules, "
                               "then read the guidance pages and the regulation text, and check "
                               "which version applies to a three-storey house.") + "," +
      antMsg(report) + "," +
      "{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(pd, "anthropic:sess_1", env));
  TEST_ASSERT_EQUAL_STRING(report.c_str(), env.reply);
}

static void test_sub_poll_states() {
  {  // running: messages so far, no idle event
    FakeProviderDeps d;
    d.http.script.push_back({"", "/v1/sessions/sess_1/events", 200,
        "{\"data\":[{\"type\":\"agent.message\",\"content\":["
        "{\"type\":\"text\",\"text\":\"working...\"}]}]}"});
    auto pd = d.contract();
    agent::ResultEnvelope env{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                      (int)providers::antPoll(pd, "anthropic:sess_1", env));
    TEST_ASSERT_EQUAL((int)agent::JobState::Running, (int)env.state);
  }
  {  // done: idle end_turn; message chunks concatenate
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"data\":[{\"type\":\"agent.message\",\"content\":["
        "{\"type\":\"text\",\"text\":\"part1 \"},{\"type\":\"text\",\"text\":\"part2\"}]},"
        "{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
    auto pd = d.contract();
    agent::ResultEnvelope env{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                      (int)providers::antPoll(pd, "anthropic:sess_1", env));
    TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
    TEST_ASSERT_EQUAL_STRING("part1 part2", env.reply);
  }
  {  // error event
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"data\":[{\"type\":\"session.error\",\"error\":{\"message\":\"sandbox died\"}}]}"});
    auto pd = d.contract();
    agent::ResultEnvelope env{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                      (int)providers::antPoll(pd, "anthropic:sess_1", env));
    TEST_ASSERT_EQUAL((int)agent::JobState::Error, (int)env.state);
    TEST_ASSERT_EQUAL_STRING("sandbox died", env.error);
  }
  {  // 404 = session expired
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 404, "{}"});
    auto pd = d.contract();
    agent::ResultEnvelope env{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::NotFound,
                      (int)providers::antPoll(pd, "anthropic:sess_1", env));
  }
}

static void test_sub_cancel_sends_interrupt() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/sessions/sess_1/events", 200, "{\"data\":[]}"});
  auto pd = d.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::antCancel(pd, "anthropic:sess_1"));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"type\":\"user.interrupt\""));
}

// ---- the compaction fold: its own output budget, no tool, prose summary --------
static void test_fold_request_has_its_own_budget_and_no_tool() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"1. Owner intent: plan the trip.\"}],"
      "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":900,\"output_tokens\":40}}"});
  auto pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("claude-opus-5"); };
  std::string conv, out, err;
  orch::TokenUsage u;
  TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnAnthropic(pd, conv, orch::ORCH_COMPACT_PROMPT,
                                                        "[CONVERSATION SINCE]\nhi", out, err, nullptr, &u),
                           err.c_str());
  // The summary comes back as prose; the engine reads it as the reply.
  TEST_ASSERT_EQUAL_STRING("1. Owner intent: plan the trip.", out.c_str());
  // Its own budget from the table: Opus at its slowest rate fits 5500 tokens in
  // the 120 s round; the first attempt takes three quarters of that (the retry
  // keeps the rest, L19-44), at low effort (a summary needs little thinking).
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"max_tokens\":4125,\"output_config\":{\"effort\":\"low\"}"));
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"tools\""));        // no 10 KB schema on the fold
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"tool_choice\""));
}

// The 16 Oct fold on Opus: two of three calls returned no summary and the third
// was cut at 3000. Now a cut fold gets one retry with more room.
static void test_fold_cut_is_retried_with_more_room() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"content\":[{\"type\":\"thinking\"}],\"stop_reason\":\"max_tokens\"}"});
  d.http.script.push_back({"", "", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"summary\"}],\"stop_reason\":\"end_turn\"}"});
  auto pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("claude-haiku-5-5"); };
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnAnthropic(pd, conv, orch::ORCH_COMPACT_PROMPT, "x", out,
                                                        err, nullptr, nullptr),
                           err.c_str());
  TEST_ASSERT_EQUAL_STRING("summary", out.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_TRUE(reqBody(d, 0) != reqBody(d, 1));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"max_tokens\":12000"));
}

// A head single shot may answer in prose: it is delivered (the engine reads it as
// the reply only), never thrown away for a "no tool_use" error.
static void test_single_shot_prose_answer_is_accepted() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"It is 7 pm.\"}],\"stop_reason\":\"end_turn\"}"});
  auto pd = d.contract();
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, nullptr),
                           err.c_str());
  TEST_ASSERT_EQUAL_STRING("It is 7 pm.", out.c_str());
}


// L19-44: under the round's time clamp the retry after a cut keeps MORE room than
// the first attempt, for every Claude family (a no-effort id included), every
// capped round kind and every round length - a full 120 s round on claude-opus-5
// used to collapse both caps to 5,500, so the retry was effort-only and a no-effort
// id had none; under 50 s the cap fell below v4.5.13's 3,000.
static void test_cap_table_rows_retry_keeps_more_room_under_the_clamp() {
  static const char* const kFamilies[] = {
      "claude-haiku-5-5", "claude-sonnet-5-5", "claude-sonnet-5", "claude-opus-5-5", "claude-opus-5",
      "claude-fable-5-1", "claude-mythos-5-1", "claude-opus-4-8", "claude-sonnet-4-6",
      "claude-haiku-4-5",          // takes no effort
      "claude-3-5-haiku-latest",   // takes no effort, old id shape
      "model-anthropic",           // unknown: the slowest tier, no effort
  };
  // 8 s is the floor a near-exhausted final round still gets (kMinRoundMs): shorter
  // than the prefill, so the floors alone hold there.
  for (uint32_t roundMs : {8000u, 40000u, 60000u, 120000u}) {
    for (const char* id : kFamilies) {
      for (providers::RoundKind kind : {providers::RoundKind::ToolRound, providers::RoundKind::FinalRound,
                                        providers::RoundKind::SingleShot, providers::RoundKind::Fold}) {
        const providers::OutputCap c = providers::outputCapFor("anthropic", id, kind, roundMs);
        char what[128];
        snprintf(what, sizeof(what), "%s %s at %u ms: %u then %u", id, providers::roundKindSlug(kind),
                 (unsigned)roundMs, (unsigned)c.maxTokens, (unsigned)c.retryTokens);
        TEST_ASSERT_TRUE_MESSAGE(c.hasRetry(), what);
        TEST_ASSERT_TRUE_MESSAGE(c.retryTokens > c.maxTokens, what);
        TEST_ASSERT_TRUE_MESSAGE(c.retryTokens * 3 >= c.maxTokens * 4, what);   // at least a third more
        TEST_ASSERT_TRUE_MESSAGE(c.maxTokens >= 3000, what);                     // never under v4.5.13's cap
      }
    }
  }
  // The full round on opus (the 19 Oct case): 4,125 first, 5,500 on the retry.
  const providers::OutputCap opus =
      providers::outputCapFor("anthropic", "claude-opus-5", providers::RoundKind::ToolRound, 120000);
  TEST_ASSERT_EQUAL(4125, (int)opus.maxTokens);
  TEST_ASSERT_EQUAL(5500, (int)opus.retryTokens);
  // Mistral keeps the same shape.
  for (uint32_t roundMs : {40000u, 60000u, 120000u}) {
    const providers::OutputCap m =
        providers::outputCapFor("mistral", "mistral-large-latest", providers::RoundKind::ToolRound, roundMs);
    TEST_ASSERT_TRUE(m.hasRetry() && m.retryTokens > m.maxTokens && m.maxTokens >= 3000);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fold_request_has_its_own_budget_and_no_tool);
  RUN_TEST(test_fold_cut_is_retried_with_more_room);
  RUN_TEST(test_single_shot_prose_answer_is_accepted);
  RUN_TEST(test_single_shot_request_shape);
  RUN_TEST(test_nullable_enum_anyof_in_schema_source);
  RUN_TEST(test_single_shot_parse_usage_and_marker);
  RUN_TEST(test_error_mapping);
  RUN_TEST(test_loop_round2_body_rendered_from_transcript);
  RUN_TEST(test_loop_echoes_assistant_with_tool_use);
  RUN_TEST(test_loop_replay_folds_with_hysteresis_over_budget);
  RUN_TEST(test_loop_stall_echoed_on_the_unforced_final_round);
  RUN_TEST(test_loop_prose_final_accepted_only_on_the_final_round);
  RUN_TEST(test_loop_max_tokens_cut_retries_then_fails_loud);
  RUN_TEST(test_loop_sibling_after_orch_turn_is_dispatched_once);
  RUN_TEST(test_loop_step_carries_sibling_after_orch_turn);
  RUN_TEST(test_loop_step_first_orch_turn_wins);
  RUN_TEST(test_loop_call_cut_by_max_tokens_never_runs);
  RUN_TEST(test_loop_cut_orch_turn_is_retried_not_accepted);
  RUN_TEST(test_sub_dispatch_full_chain_and_caches);
  RUN_TEST(test_sub_dispatch_stale_404_clears_caches);
  RUN_TEST(test_sub_unsafe_model_name_falls_back);
  RUN_TEST(test_sub_poll_states);
  RUN_TEST(test_cum514_artifact_drops_the_narration);
  RUN_TEST(test_cum514_artifact_keeps_what_follows_the_report);
  RUN_TEST(test_cum514_artifact_keeps_a_long_earlier_section);
  RUN_TEST(test_cum514_short_report_still_drops_notes);
  RUN_TEST(test_sub_cancel_sends_interrupt);
  RUN_TEST(test_cap_table_rows_retry_keeps_more_room_under_the_clamp);
  UNITY_END();
  return 0;
}
