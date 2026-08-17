#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/orch_schema.h"

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

void setUp() { LogCapture::install(); }
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
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"max_tokens\":3000"));
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
  // FORCED tool use: tool_choice {type:"tool", name:"orch_turn"}.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"tool_choice\":{\"type\":\"tool\",\"name\":\"orch_turn\"}"));
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
  {  // truncation surfaces the stop_reason, not a blind "no tool_use"
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"content\":[{\"type\":\"text\"}],\"stop_reason\":\"max_tokens\"}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("no orch_turn tool_use (stop_reason=max_tokens)", err.c_str());
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
  //   [0] user seed (string content)
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
  TEST_ASSERT_EQUAL_STRING("USER", msgs[0]["content"] | "");
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

// Context Fabric: once the replayed conversation passes the fold trigger (a
// quarter of the model's window), rounds older than the newest 2 collapse to one
// "[earlier round N]" line each - the newest stay verbatim, and every surviving
// tool_use is still answered (the API's pairing invariant). Below the trigger the
// body is byte-identical, which the tests above pin.
static void test_loop_gradient_folds_old_rounds_over_trigger() {
  FakeProviderDeps d;
  // 5 tool rounds then the terminal. Distinct tool_use ids so the fold's pairing
  // can be checked precisely.
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
  // Each result: 60 KB of filler (UNDER ArduinoJson's 65535-byte string ceiling -
  // a longer string stores as null) plus a per-round TAIL marker. The fold keeps
  // only a ~160-char head gist, so a folded round LOSES its tail marker while a
  // verbatim round keeps it: that asymmetry is what this test asserts.
  int calls = 0;
  rig.ht.dispatch = [&rig, &calls](const nimbus::orch::HeadToolCall& c) {
    nimbus::orch::HeadToolResult r;
    r.id = c.id;
    r.name = c.name;
    r.output = std::string(60000, 'z') + "TAILMARK" + std::to_string(calls++);
    rig.dispatched.push_back(c);
    return r;
  };
  rig.ht.cfg.maxToolResultBytes = 0;   // isolate the fold from the clamp
  rig.ht.cfg.maxTotalToolBytes = 0;
  rig.ht.cfg.maxRounds = 8;
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_TRUE(d.http.seen.size() >= 5);
  const int last = (int)d.http.seen.size() - 1;
  const std::string& b = d.http.seen[last].body;
  // The oldest rounds collapsed to one line each...
  TEST_ASSERT_TRUE(bodyHas(d, last, "[earlier round 0] memory_search"));
  // ...losing their tails (the 60 KB payload is no longer replayed)...
  TEST_ASSERT_TRUE(b.find("TAILMARK0") == std::string::npos);
  // ...while the newest round is still there VERBATIM, tail and all.
  TEST_ASSERT_TRUE(b.find("TAILMARK4") != std::string::npos);
  // The seeded user turn is pinned (never folded).
  TEST_ASSERT_TRUE(bodyHas(d, last, "USER"));
  // Pairing invariant: every tool_use still in the body has its tool_result.
  for (int i = 1; i <= 5; i++) {
    const std::string id = "tu_" + std::to_string(i);
    const bool hasUse = b.find("\"id\":\"" + id + "\"") != std::string::npos;
    const bool hasRes = b.find("\"tool_use_id\":\"" + id + "\"") != std::string::npos;
    TEST_ASSERT_TRUE_MESSAGE(hasUse == hasRes, ("orphan tool_use/result for " + id).c_str());
  }
}

// The other half of R_ANT_echo_tool_use: a text-ONLY (stalled) round is NOT
// echoed - a trailing assistant message + the forced tool_choice would 400 -
// and the forced final round pins tool_choice {type:"tool"}.
static void test_loop_stall_no_echo_forced_final() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"just prose\"}],"
      "\"stop_reason\":\"end_turn\"}"});
  d.http.script.push_back({"", "", 200, finalToolBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  // The stalled prose was NOT echoed into round 1's messages[].
  TEST_ASSERT_FALSE(bodyHas(d, 1, "\"role\":\"assistant\""));
  TEST_ASSERT_FALSE(bodyHas(d, 1, "just prose"));
  // Forced final: tool_choice pins orch_turn, with the larger output headroom.
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"tool_choice\":{\"type\":\"tool\",\"name\":\"orch_turn\"}"));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"max_tokens\":4096"));
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
}

// A max_tokens-truncated round must FAIL with the real cause (never echoed or
// silently "recovered").
static void test_loop_max_tokens_truncation_fails_loud() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"trunc\"}],"
      "\"stop_reason\":\"max_tokens\"}"});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, &rig.ht, nullptr));
  TEST_ASSERT_EQUAL_STRING("truncated at max_tokens before a tool call", err.c_str());
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
  TEST_ASSERT_EQUAL((int)agent::FabricErr::RemoteFail,
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_shot_request_shape);
  RUN_TEST(test_nullable_enum_anyof_in_schema_source);
  RUN_TEST(test_single_shot_parse_usage_and_marker);
  RUN_TEST(test_error_mapping);
  RUN_TEST(test_loop_round2_body_rendered_from_transcript);
  RUN_TEST(test_loop_echoes_assistant_with_tool_use);
  RUN_TEST(test_loop_gradient_folds_old_rounds_over_trigger);
  RUN_TEST(test_loop_stall_no_echo_forced_final);
  RUN_TEST(test_loop_max_tokens_truncation_fails_loud);
  RUN_TEST(test_sub_dispatch_full_chain_and_caches);
  RUN_TEST(test_sub_dispatch_stale_404_clears_caches);
  RUN_TEST(test_sub_unsafe_model_name_falls_back);
  RUN_TEST(test_sub_poll_states);
  RUN_TEST(test_sub_cancel_sends_interrupt);
  UNITY_END();
  return 0;
}
