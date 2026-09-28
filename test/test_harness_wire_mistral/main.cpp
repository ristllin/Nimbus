#include <unity.h>

#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/rate_limit.h"
#include "nimbus/orch/orch_schema.h"
#include "nimbus/orch/turn.h"

// Stage H wire suite - the Mistral provider: the Conversations-API head turn
// (new-conversation pins model/instructions/connectors; continuation sends
// neither), strict response_format json_schema, conversation_id continuity, the
// tool loop's tool_choice pin (R_MIS_tool_choice: the Conversations API takes
// "required" - chat-completions' "any" 422s - and NO built-in connectors ride
// loop turns), function.result pairing, and the synchronous chat/completions
// sub-session + result cache.

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::bodyHas;
using harness_test::headerOf;
namespace providers = agent::providers;
namespace orch = nimbus::orch;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static std::string messageOutputBody(const char* cid) {
  return std::string("{\"conversation_id\":\"") + cid + "\",\"outputs\":["
         "{\"type\":\"message.output\",\"content\":"
         "\"{\\\"reply\\\":\\\"hello there\\\",\\\"memory\\\":\\\"\\\"}\"}],"
         "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":20}}";
}
static std::string functionCallBody(const char* cid, const char* name, const char* callId) {
  return std::string("{\"conversation_id\":\"") + cid + "\",\"outputs\":["
         "{\"type\":\"function.call\",\"name\":\"" + name + "\","
         "\"tool_call_id\":\"" + callId + "\","
         "\"arguments\":\"{\\\"q\\\":\\\"tea\\\"}\"}],"
         "\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":10}}";
}
static std::string finalCallBody(const char* cid) {
  return std::string("{\"conversation_id\":\"") + cid + "\",\"outputs\":["
         "{\"type\":\"function.call\",\"name\":\"orch_turn\",\"tool_call_id\":\"tc_f\","
         "\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}],"
         "\"usage\":{\"prompt_tokens\":60,\"completion_tokens\":15}}";
}
// ---- chat-completions canned bodies (the Stage 2 phase 4 loop wire) ---------
static std::string chatToolCallBody(const char* name, const char* id) {
  return std::string("{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":["
         "{\"id\":\"") + id + "\",\"function\":{\"name\":\"" + name + "\","
         "\"arguments\":\"{\\\"q\\\":\\\"tea\\\"}\"}}]}}],"
         "\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":10}}";
}
static std::string chatFinalBody() {
  return "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":["
         "{\"id\":\"abc123def\",\"function\":{\"name\":\"orch_turn\","
         "\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}}]}}],"
         "\"usage\":{\"prompt_tokens\":60,\"completion_tokens\":15}}";
}
static std::string chatProseBody() {
  return "{\"choices\":[{\"message\":{\"content\":\"just prose\"}}],"
         "\"usage\":{\"prompt_tokens\":40,\"completion_tokens\":5}}";
}

// ---- head single-shot -------------------------------------------------------

static void test_new_conversation_request_shape() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
                           messageOutputBody("conv_1")});
  auto pd = d.contract();
  std::string conv, out, err;
  orch::TokenUsage u;
  bool ok = providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, nullptr, &u);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_EQUAL_STRING("Bearer sk-fake-mis", headerOf(r, "Authorization").c_str());
  TEST_ASSERT_EQUAL_STRING("/v1/conversations", r.path.c_str());
  // New conversation pins model + instructions + the Studio built-in connectors.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"model\":\"model-mistral\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"instructions\":\"SYS\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"inputs\":\"USER\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"store\":true"));
  TEST_ASSERT_EQUAL(1, d.mistralAttaches);
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"type\":\"web_search\""));
  // Strict response_format json_schema with the canonical schema (descriptions
  // intact - Mistral ENFORCES it server-side).
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"response_format\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"type\":\"json_schema\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"strict\":true"));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "Text to send the owner now"));
  // Parse: conversation_id becomes the convId; the content is the turn.
  TEST_ASSERT_EQUAL_STRING("conv_1", conv.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"hello there\",\"memory\":\"\"}", out.c_str());
  TEST_ASSERT_EQUAL(100, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(20, (int)u.completionTokens);
}

static void test_continuation_sends_no_model_or_instructions() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.mistral.ai", "/v1/conversations/conv_1", 200,
                           messageOutputBody("conv_1")});
  auto pd = d.contract();
  std::string conv = "conv_1", out, err;
  bool ok = providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("/v1/conversations/conv_1", d.http.seen[0].path.c_str());
  // Continuation: model/instructions/connectors pinned at creation, NOT re-sent.
  // (Substring-match the exact top-level pairs - the embedded orch schema
  // legitimately contains a "model" property for spawns.)
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"model\":\"model-mistral\""));
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"instructions\":\"SYS\""));
  // CUM-460: the attach is consulted once as a PROBE (does the thread carry a
  // Studio connector?) but its tools are never re-sent on a continuation.
  TEST_ASSERT_EQUAL(1, d.mistralAttaches);
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"tools\""));
  // Built-ins only: the strict schema still rides the continued turn.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"strict\":true"));
}

static void test_error_mapping() {
  {  // 404 on a continued conversation: clear the convId (fresh next turn)
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 404, "{}"});
    auto pd = d.contract();
    std::string conv = "conv_gone", out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("conversation gone", err.c_str());
    TEST_ASSERT_EQUAL_STRING("", conv.c_str());
  }
  {  // error envelope message
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 401, "{\"message\":\"bad key\"}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("conversations HTTP 401: bad key", err.c_str());
  }
  {  // transport fail
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 0, ""});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("network", err.c_str());
  }
  {  // no message.output
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200, "{\"conversation_id\":\"c\",\"outputs\":[]}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("no message.output", err.c_str());
  }
  {  // no key
    FakeProviderDeps d;
    d.keys.erase("mistral");
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("no Mistral key", err.c_str());
  }
}

// ---- tool loop --------------------------------------------------------------

// Stage 2 phase 4: the loop runs on STATELESS /v1/chat/completions. Tool rounds
// send tool_choice "any" (chat-completions' force-a-tool value - NOT the
// Conversations dialect), NO Studio built-ins ride loop turns, and round 2
// replays the full transcript (system + user + assistant tool_calls + role:tool).
static void test_loop_chat_completions_stateless() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.mistral.ai", "/v1/chat/completions", 200,
                           chatToolCallBody("memory_search", "tc1tc1tc1")});
  d.http.script.push_back({"api.mistral.ai", "/v1/chat/completions", 200,
                           chatFinalBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv = "conv_old", out, err;   // a legacy convId must be ignored
  orch::TokenUsage u;
  bool ok = providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, &rig.ht, &u);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("/v1/chat/completions", d.http.seen[0].path.c_str());
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"tool_choice\":\"any\""));
  TEST_ASSERT_EQUAL(0, d.mistralAttaches);           // built-ins dropped in loop mode
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"role\":\"system\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"content\":\"SYS\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"role\":\"user\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"content\":\"USER\""));
  // Nested chat-completions tool shape: {type:function, function:{name...}}.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"function\":{\"name\":\"orch_turn\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"function\":{\"name\":\"memory_search\""));
  // Round 1: STATELESS full replay - assistant tool_calls + the role:tool answer.
  TEST_ASSERT_EQUAL_STRING("/v1/chat/completions", d.http.seen[1].path.c_str());
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"role\":\"assistant\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"id\":\"tc1tc1tc1\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"role\":\"tool\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"tool_call_id\":\"tc1tc1tc1\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"content\":\"tool-ok\""));
  // Dispatch reached the rig; convId is the stateless marker, never an id.
  TEST_ASSERT_EQUAL(1, (int)rig.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("memory_search", rig.dispatched[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
  TEST_ASSERT_EQUAL_STRING("chat", conv.c_str());
  TEST_ASSERT_EQUAL(110, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(25, (int)u.completionTokens);
}

// A stalled round (prose, no tool_calls) forces the final round. On
// chat-completions the named-function tool_choice object IS accepted (the
// Conversations-only 422 does not apply) - the forced final pins orch_turn
// directly instead of leaning on a nudge.
static void test_loop_stall_forces_named_final() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/chat/completions", 200, chatProseBody()});
  d.http.script.push_back({"", "/v1/chat/completions", 200, chatFinalBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnMistral(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"tool_choice\":{\"type\":\"function\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"name\":\"orch_turn\"}"));
  // The stalled prose round is NOT replayed (prose-only rounds don't render).
  TEST_ASSERT_FALSE(bodyHas(d, 1, "just prose"));
}

// The 422 validation detail shape surfaces in the loop error.
static void test_loop_422_detail_surfaces() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 422,
      "{\"detail\":[{\"msg\":\"Input should be 'auto', 'none' or 'required'\"}]}"});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, &rig.ht, nullptr));
  TEST_ASSERT_EQUAL_STRING(
      "chat HTTP 422: Input should be 'auto', 'none' or 'required'", err.c_str());
}

// Foreign tool_call ids (a transcript carried over by mid-turn failover) are
// normalized to chat-completions' required 9-alphanumeric shape,
// DETERMINISTICALLY and consistently across the call and its paired answer.
static void test_foreign_call_ids_normalized() {
  TEST_ASSERT_EQUAL_STRING("tc1tc1tc1",
                           providers::mistralCallId("tc1tc1tc1").c_str());  // already valid
  std::string a = providers::mistralCallId("toolu_01AbCdEfGh");
  std::string b = providers::mistralCallId("toolu_01AbCdEfGh");
  std::string c = providers::mistralCallId("call_XYZ123");
  TEST_ASSERT_EQUAL(9, (int)a.size());
  TEST_ASSERT_EQUAL_STRING(a.c_str(), b.c_str());   // deterministic
  TEST_ASSERT_TRUE(a != c);                          // distinct inputs stay distinct
  for (char ch : a) TEST_ASSERT_TRUE(isalnum((unsigned char)ch));
}

// ---- sub-session ------------------------------------------------------------

static void test_sub_dispatch_and_poll_cache() {
  FakeProviderDeps d;
  // Sub-agents now run over the Conversations API (so Studio connectors ride the
  // call). Result is the last outputs[].message.output content.
  d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
      "{\"conversation_id\":\"c1\",\"outputs\":[{\"type\":\"message.output\","
      "\"content\":\"the result\"}]}"});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "do it";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::mistralDispatch(pd, "sub-mistral", dir, jobId));
  // The wire: a Conversations body with the sub model, the agent instructions, the
  // task as `inputs`, and the owner's Studio connectors attached (run server-side).
  TEST_ASSERT_EQUAL_STRING("/v1/conversations", d.http.seen[0].path.c_str());
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"model\":\"sub-mistral\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "autonomous assistant agent"));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"inputs\":\"do it\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"tools\""));   // connectors ride the sub-agent
  TEST_ASSERT_EQUAL(1, d.mistralAttaches);
  // Poll serves the cached result as Done exactly once.
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::mistralPoll(pd, jobId, env));
  TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
  TEST_ASSERT_EQUAL_STRING("the result", env.reply);
  TEST_ASSERT_EQUAL((int)agent::FabricErr::NotFound, (int)providers::mistralPoll(pd, jobId, env));
}

// Free-text sub-agent output (no response_format schema) can return message.output
// `content` as an ARRAY of text chunks, not a bare string. The result must still
// extract (regression: string-only extraction returned "" -> RemoteFail on success).
static void test_sub_dispatch_array_content() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/conversations", 200,
      "{\"outputs\":[{\"type\":\"message.output\",\"content\":["
      "{\"type\":\"text\",\"text\":\"part one \"},"
      "{\"type\":\"text\",\"text\":\"part two\"}]}]}"});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "x";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::mistralDispatch(pd, "m", dir, jobId));
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::mistralPoll(pd, jobId, env));
  TEST_ASSERT_EQUAL_STRING("part one part two", env.reply);
}

// v4.1 code_interpreter file capture. A sub-agent that runs code_interpreter and
// writes a file emits it as a `tool_file` chunk inside a message.output `content`
// ARRAY (VERIFIED against the live Conversations API 2026-08-08). The dispatch
// filter keeps content whole, so the file reference must survive to poll and land
// in ResultEnvelope.artifacts[] (url=file_id, label=file_name) while the prose
// still lands in env.reply and the bytes NEVER do.
static void test_sub_dispatch_captures_code_interpreter_file() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
      "{\"conversation_id\":\"c1\",\"outputs\":["
      // the tool.execution entry (no content key) is ignored by the parse
      "{\"type\":\"tool.execution\",\"name\":\"code_interpreter\",\"info\":{\"result\":["
      "{\"type\":\"file_url\",\"file_url\":\"https://blob/x?sig=y\","
      "\"file_name\":\"report.pdf\",\"file_type\":\"pdf\"}]}},"
      // the message.output carries the durable tool_file (file_id) + the prose
      "{\"type\":\"message.output\",\"content\":["
      "{\"type\":\"tool_file\",\"tool\":\"code_interpreter\","
      "\"file_id\":\"a92205d6-fbdb-408a-9fdf-98f74f03cfdd\","
      "\"file_name\":\"report.pdf\",\"file_type\":\"pdf\"},"
      "{\"type\":\"text\",\"text\":\"Your PDF is ready.\"}]}]}"});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "make a pdf";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::mistralDispatch(pd, "m", dir, jobId));
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::mistralPoll(pd, jobId, env));
  TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
  // The prose lands in reply; the file reference lands in artifacts[].
  TEST_ASSERT_EQUAL_STRING("Your PDF is ready.", env.reply);
  TEST_ASSERT_EQUAL(1, env.artifactCount);
  TEST_ASSERT_EQUAL_STRING("file", env.artifacts[0].type);
  TEST_ASSERT_EQUAL_STRING("a92205d6-fbdb-408a-9fdf-98f74f03cfdd", env.artifacts[0].url);
  TEST_ASSERT_EQUAL_STRING("report.pdf", env.artifacts[0].label);
}

// A file-only run (no prose message) still succeeds and yields the artifact - the
// bytes are the payload, so an empty reply must not be read as RemoteFail.
static void test_sub_dispatch_file_only_no_prose() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/conversations", 200,
      "{\"outputs\":[{\"type\":\"message.output\",\"content\":["
      "{\"type\":\"tool_file\",\"file_id\":\"fid-123\",\"file_name\":\"chart.png\","
      "\"file_type\":\"png\"}]}]}"});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "plot it";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::mistralDispatch(pd, "m", dir, jobId));
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::mistralPoll(pd, jobId, env));
  TEST_ASSERT_EQUAL(1, env.artifactCount);
  TEST_ASSERT_EQUAL_STRING("fid-123", env.artifacts[0].url);
  TEST_ASSERT_EQUAL_STRING("chart.png", env.artifacts[0].label);
}

// A plain text run produces NO artifacts (no false positives from ordinary
// message.output content).
static void test_sub_dispatch_text_only_no_artifacts() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/conversations", 200,
      "{\"outputs\":[{\"type\":\"message.output\",\"content\":\"just text\"}]}"});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "chat";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::mistralDispatch(pd, "m", dir, jobId));
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::mistralPoll(pd, jobId, env));
  TEST_ASSERT_EQUAL_STRING("just text", env.reply);
  TEST_ASSERT_EQUAL(0, env.artifactCount);
}

static void test_sub_per_dispatch_model_override() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"outputs\":[{\"type\":\"message.output\",\"content\":\"ok\"}]}"});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "x"; dir.model = "mistral-small-latest";
  char jobId[72];
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::mistralDispatch(pd, "sub-mistral", dir, jobId));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"model\":\"mistral-small-latest\""));
  agent::ResultEnvelope env{};
  providers::mistralPoll(pd, jobId, env);   // vacate the cache slot for later tests
}

static void test_sub_error_mapping() {
  agent::Directive dir; dir.instruction = "x";
  char jobId[72];
  {
    FakeProviderDeps d; d.http.script.push_back({"", "", 401, "{}"});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Auth,
                      (int)providers::mistralDispatch(pd, "m", dir, jobId));
  }
  {
    FakeProviderDeps d; d.http.script.push_back({"", "", 429, "{}"});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::RateLimited,
                      (int)providers::mistralDispatch(pd, "m", dir, jobId));
  }
  {
    FakeProviderDeps d; d.http.script.push_back({"", "", 0, ""});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Network,
                      (int)providers::mistralDispatch(pd, "m", dir, jobId));
  }
  {  // no key: refused before the wire
    FakeProviderDeps d;
    d.keys.erase("mistral");
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Auth,
                      (int)providers::mistralDispatch(pd, "m", dir, jobId));
    TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());
  }
}

// R_MIS_reserved_toolname (live drift 2026-07-18): Mistral's Conversations API
// RESERVES its built-in connector names - advertising a user tool named
// web_search 422s the WHOLE request ("protected function name: web_search"),
// which broke every mistral head turn with the loop on (our Tavily tool is
// web.search -> web_search). The wire must rename reserved collisions and
// invert the rename before dispatch so the registry still sees the real name.
static void test_reserved_toolname_renamed_and_inverted() {
  FakeProviderDeps d;
  // Round 0 returns a call to the RENAMED tool (reg_web_search) - the model
  // only ever sees the safe name; round 1 finalizes. (Loop = chat-completions.)
  d.http.script.push_back({"api.mistral.ai", "/v1/chat/completions", 200,
                           chatToolCallBody("reg_web_search", "tc9tc9tc9")});
  d.http.script.push_back({"api.mistral.ai", "/v1/chat/completions", 200,
                           chatFinalBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  rig.ht.specs.push_back(nimbus::orch::ToolRegistry::Spec{
      "web_search", "search the web (Tavily)",
      "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}"});
  rig.ht.dispatch = [&rig](const nimbus::orch::HeadToolCall& c) {
    rig.dispatched.push_back(c);
    nimbus::orch::HeadToolResult r; r.id = c.id; r.name = c.name;
    r.output = "web-ok"; return r;
  };
  rig.ht.cfg.maxRounds = 12; rig.ht.cfg.deadlineMs = 600000;
  rig.ht.cfg.roundMinHeap = 28000; rig.ht.cfg.maxToolResultBytes = 4096;
  rig.ht.cfg.maxTotalToolBytes = 24576;
  std::string conv, out, err;
  bool ok = providers::orchTurnMistral(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  // The wire NEVER advertises the reserved name; it advertises the renamed one.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"function\":{\"name\":\"reg_web_search\""));
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"function\":{\"name\":\"web_search\""));
  // Dispatch sees the ORIGINAL registry name (inverse applied).
  TEST_ASSERT_EQUAL(1, (int)rig.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("web_search", rig.dispatched[0].name.c_str());
  // The pure bijection round-trips; a non-reserved name is untouched.
  TEST_ASSERT_EQUAL_STRING("reg_web_search", providers::mistralSafeName("web_search").c_str());
  TEST_ASSERT_EQUAL_STRING("web_search", providers::mistralUnsafeName("reg_web_search").c_str());
  TEST_ASSERT_EQUAL_STRING("memory_search", providers::mistralSafeName("memory_search").c_str());
  TEST_ASSERT_EQUAL_STRING("memory_search", providers::mistralUnsafeName("memory_search").c_str());
}

// ---- CUM-460a: Studio connector on a single-shot head turn ------------------
// Measured live: a Conversations call with a Studio connector AND the strict
// orch_turn response_format is cut at the 60 s deadline (3 of 3); without the
// response_format it answers in ~5 s. The head turn that carries a Studio
// connector therefore sends NO strict-schema field; every other head turn is
// byte-for-byte unchanged.

// The fake's default attach is built-ins only; these add a Studio connector.
static void attachStudio(FakeProviderDeps& d, agent::providers::ProviderDeps& pd) {
  pd.attachMistral = [&d](JsonDocument& doc) {
    d.mistralAttaches++;
    doc["tools"].add<JsonObject>()["type"] = "web_search";
    JsonObject t = doc["tools"].add<JsonObject>();
    t["type"] = "connector";
    t["connector_id"] = "google_calendar";
  };
}

static std::string freeTextBody(const char* cid, const char* text) {
  JsonDocument d;
  d["conversation_id"] = cid;
  JsonObject o = d["outputs"].add<JsonObject>();
  o["type"] = "message.output";
  JsonObject c = o["content"].add<JsonObject>();
  c["type"] = "text";
  c["text"] = text;
  std::string s;
  serializeJson(d, s);
  return s;
}

static bool hasStrictSchema(const FakeProviderDeps& d, size_t i) {
  return bodyHas(d, i, "\"response_format\"") || bodyHas(d, i, "\"json_schema\"") ||
         bodyHas(d, i, "\"strict\"");
}

// The exact pre-CUM-460 body of a new conversation (built-ins attached): the
// shape the no-Studio path must keep, byte for byte.
static std::string strictNewConversationBody() {
  JsonDocument e;
  e["store"] = true;
  e["inputs"] = "USER";
  e["model"] = "model-mistral";
  e["instructions"] = "SYS";
  e["tools"].add<JsonObject>()["type"] = "web_search";
  JsonObject rf = e["completion_args"]["response_format"].to<JsonObject>();
  rf["type"] = "json_schema";
  rf["json_schema"]["name"] = "orch_turn";
  rf["json_schema"]["strict"] = true;
  JsonDocument sd;
  deserializeJson(sd, orch::ORCH_SCHEMA_BODY, DeserializationOption::NestingLimit(16));
  rf["json_schema"]["schema"] = sd;
  std::string s;
  serializeJson(e, s);
  return s;
}

static void test_head_without_studio_connector_is_unchanged() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/conversations", 200, messageOutputBody("conv_1")});
  auto pd = d.contract();
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(
      providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, nullptr, nullptr),
      err.c_str());
  TEST_ASSERT_EQUAL_STRING(strictNewConversationBody().c_str(), harness_test::reqBody(d, 0).c_str());
  TEST_ASSERT_FALSE(bodyHas(d, 0, "OUTPUT FORMAT"));
  // The reply is the schema-pinned content, verbatim (no lenient rewrite).
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"hello there\",\"memory\":\"\"}", out.c_str());
}

static void test_head_with_studio_connector_sends_no_strict_schema() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
      freeTextBody("conv_s", "| 19:00 | Standup |\n| 21:00 | School email |")});
  auto pd = d.contract();
  attachStudio(d, pd);
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(
      providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, nullptr, nullptr),
      err.c_str());
  // The connector and the built-in still ride the new conversation...
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"connector_id\":\"google_calendar\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"type\":\"web_search\""));
  // ...but no strict-schema field does; the instructions ask for the object.
  TEST_ASSERT_FALSE(hasStrictSchema(d, 0));
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"completion_args\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"instructions\":\"SYS\\n\\n[OUTPUT FORMAT]"));
  // The instruction carries the canonical schema's SHAPES (item keys such as a
  // mem_write's content), without the descriptions the prompt already has.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\\\"mem_write\\\":{\\\"type\\\":\\\"array\\\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\\\"required\\\":[\\\"content\\\""));
  TEST_ASSERT_FALSE(bodyHas(d, 0, "description"));
  TEST_ASSERT_EQUAL_STRING("conv_s", conv.c_str());
  // Free text (the measured reply shape: a markdown table) becomes the reply of a
  // turn the portable parser accepts, and the log says the turn carried nothing else.
  orch::Turn t;
  orch::ParseError pe;
  TEST_ASSERT_TRUE_MESSAGE(orch::parseTurn(out, t, pe), pe.detail.c_str());
  TEST_ASSERT_EQUAL_STRING("| 19:00 | Standup |\n| 21:00 | School email |", t.reply.c_str());
  TEST_ASSERT_EQUAL_STRING("", t.memory.c_str());
  TEST_ASSERT_EQUAL_STRING("", t.ask.c_str());
  TEST_ASSERT_TRUE(LogCapture::contains("schema-less reply was prose"));
}

// A CONTINUED conversation carries the connectors pinned at its creation, so the
// shape follows the attach probe even though no tools are re-sent.
static void test_head_studio_continuation_sends_no_strict_schema() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/conversations/conv_s", 200,
      freeTextBody("conv_s",
                   "```json\n{\"reply\":\"Two events today.\",\"memory\":\"cal checked\","
                   "\"device\":[]}\n```")});
  auto pd = d.contract();
  attachStudio(d, pd);
  std::string conv = "conv_s", out, err;
  TEST_ASSERT_TRUE_MESSAGE(
      providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, nullptr, nullptr),
      err.c_str());
  TEST_ASSERT_EQUAL_STRING("/v1/conversations/conv_s", d.http.seen[0].path.c_str());
  TEST_ASSERT_FALSE(hasStrictSchema(d, 0));
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"tools\""));          // pinned, never re-sent
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"instructions\""));   // pinned at creation
  // The fenced orch_turn object is the turn; the missing "ask" is filled in.
  orch::Turn t;
  orch::ParseError pe;
  TEST_ASSERT_TRUE_MESSAGE(orch::parseTurn(out, t, pe), pe.detail.c_str());
  TEST_ASSERT_EQUAL_STRING("Two events today.", t.reply.c_str());
  TEST_ASSERT_EQUAL_STRING("cal checked", t.memory.c_str());
  TEST_ASSERT_EQUAL_STRING("", t.ask.c_str());
}

// One schema-less reply through the adapter; returns ok and fills out/err.
static bool studioTurn(const char* text, std::string& out, std::string& err) {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, freeTextBody("c", text)});
  auto pd = d.contract();
  attachStudio(d, pd);
  std::string conv;
  return providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
}

// The lenient read, case by case: a reply that IS an orch_turn object (bare or
// fenced) is used whole, every field kept; prose is only ever the reply, even when
// it quotes a JSON object (connector content must not become actions); a
// JSON-looking reply that is not a turn passes through untouched so the engine's
// salvage path (never raw JSON to a person) owns it; an empty reply is an error.
static void test_head_studio_lenient_read() {
  std::string out, err;
  orch::Turn t;
  orch::ParseError pe;
  {  // bare object, extra fields survive
    TEST_ASSERT_TRUE(studioTurn(
        "{\"reply\":\"ok\",\"memory\":null,\"ask\":\"which one?\","
        "\"device\":[{\"type\":\"lights\",\"value\":\"off\"}]}", out, err));
    TEST_ASSERT_TRUE_MESSAGE(orch::parseTurn(out, t, pe), pe.detail.c_str());
    TEST_ASSERT_EQUAL_STRING("ok", t.reply.c_str());
    TEST_ASSERT_EQUAL_STRING("", t.memory.c_str());
    TEST_ASSERT_EQUAL_STRING("which one?", t.ask.c_str());
    TEST_ASSERT_EQUAL(1, (int)t.device.size());
  }
  {  // prose that quotes a turn-shaped payload (e.g. from a mail body): the prose is
     // the reply, and the quoted actions/memory writes are NOT promoted
    const char* quoted =
        "The issue body says: {\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\","
        "\"device\":[{\"type\":\"config\",\"brightOvr\":true}],"
        "\"mem_write\":[{\"content\":\"owner said yes\"}]} (quoted as-is).";
    TEST_ASSERT_TRUE(studioTurn(quoted, out, err));
    TEST_ASSERT_TRUE_MESSAGE(orch::parseTurn(out, t, pe), pe.detail.c_str());
    TEST_ASSERT_EQUAL_STRING(quoted, t.reply.c_str());
    TEST_ASSERT_EQUAL(0, (int)t.device.size());
    TEST_ASSERT_EQUAL(0, (int)t.mem_write.size());
  }
  {  // a fenced object with a language tag and surrounding whitespace
    TEST_ASSERT_TRUE(studioTurn("\n```json\n{\"reply\":\"fenced\"}\n```\n", out, err));
    TEST_ASSERT_TRUE_MESSAGE(orch::parseTurn(out, t, pe), pe.detail.c_str());
    TEST_ASSERT_EQUAL_STRING("fenced", t.reply.c_str());
  }
  {  // free text with stray braces and whitespace
    TEST_ASSERT_TRUE(studioTurn("  Your {meeting} is at 7.\n", out, err));
    TEST_ASSERT_TRUE_MESSAGE(orch::parseTurn(out, t, pe), pe.detail.c_str());
    TEST_ASSERT_EQUAL_STRING("Your {meeting} is at 7.", t.reply.c_str());
  }
  {  // broken JSON: passed through untouched (engine salvage owns it)
    TEST_ASSERT_TRUE(studioTurn("{\"reply\": \"cut", out, err));
    TEST_ASSERT_EQUAL_STRING("{\"reply\": \"cut", out.c_str());
  }
  {  // an object that is not a turn: passed through, never wrapped as a reply
    TEST_ASSERT_TRUE(studioTurn("```\n{\"events\":[1,2]}\n```", out, err));
    TEST_ASSERT_EQUAL_STRING("```\n{\"events\":[1,2]}\n```", out.c_str());
  }
  {  // a fenced markdown table is prose, not JSON: it is the reply
    TEST_ASSERT_TRUE(studioTurn("```\n| 19:00 | Standup |\n```", out, err));
    TEST_ASSERT_TRUE_MESSAGE(orch::parseTurn(out, t, pe), pe.detail.c_str());
    TEST_ASSERT_EQUAL_STRING("```\n| 19:00 | Standup |\n```", t.reply.c_str());
  }
  {  // whitespace only: no turn
    TEST_ASSERT_FALSE(studioTurn(" \n ", out, err));
    TEST_ASSERT_EQUAL_STRING("no message.output", err.c_str());
  }
}

// The CLASS rule over every attach shape and both conversation states: the strict
// schema rides a head turn exactly when no Studio connector does. A new attach
// shape that forgets the rule fails here, not on the 60 s cutoff in the field.
static void test_head_schema_xor_studio_connector_class() {
  struct Shape { const char* name; bool builtin, studio; };
  const Shape shapes[] = {{"none", false, false},   {"builtin", true, false},
                          {"studio", false, true},  {"both", true, true}};
  for (const Shape& sh : shapes) {
    for (int cont = 0; cont < 2; cont++) {
      FakeProviderDeps d;
      d.http.script.push_back({"", "", 200, messageOutputBody("c")});
      auto pd = d.contract();
      pd.attachMistral = [sh](JsonDocument& doc) {
        if (sh.builtin) doc["tools"].add<JsonObject>()["type"] = "code_interpreter";
        if (sh.studio) {
          JsonObject t = doc["tools"].add<JsonObject>();
          t["type"] = "connector";
          t["connector_id"] = "notion";
        }
      };
      std::string conv = cont ? "c" : "", out, err;
      TEST_ASSERT_TRUE_MESSAGE(
          providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, nullptr, nullptr),
          sh.name);
      TEST_ASSERT_EQUAL_MESSAGE(!sh.studio, hasStrictSchema(d, 0), sh.name);
      TEST_ASSERT_EQUAL_MESSAGE(sh.studio && !cont, bodyHas(d, 0, "OUTPUT FORMAT"), sh.name);
    }
  }
}

// ---- CUM-460b: a 429 names the quota window that refused ---------------------
// Mistral's 429 body reads the same for a per-minute limit, a spent daily
// connector quota and a 0-per-minute plan; the x-ratelimit-* headers are what
// differ, so the adapter carries the window as a tag the engine turns into copy.
static void test_head_429_carries_the_quota_window() {
  {  // spent daily Studio connector quota (the live 2026-09-27 shape)
    FakeProviderDeps d;
    harness_test::Exchange e;
    e.status = 429;
    e.body = "{\"detail\":\"Custom connector rate limit reached.\"}";
    e.headers = {{"X-RateLimit-Limit-Custom-Minute", "5"},
                 {"x-ratelimit-limit-custom-day", "50"},
                 {"x-ratelimit-remaining-custom-day", "0"},
                 {"content-type", "application/json"}};
    d.http.script.push_back(e);
    auto pd = d.contract();
    attachStudio(d, pd);
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING(
        "conversations HTTP 429 [rl:day-utc]: Custom connector rate limit reached.",
        err.c_str());
    TEST_ASSERT_EQUAL((int)agent::RateLimit::DailyUtc, (int)agent::rateLimitFromError(err));
  }
  {  // no headers (a transport without capture): no tag, the text still quoted
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 429, "{\"message\":\"Requests rate limit exceeded\"}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("conversations HTTP 429: Requests rate limit exceeded", err.c_str());
  }
  {  // a non-429 error never carries a window, even with quota headers present
    FakeProviderDeps d;
    harness_test::Exchange e;
    e.status = 503;
    e.body = "{\"message\":\"busy\"}";
    e.headers = {{"x-ratelimit-remaining-custom-day", "0"}};
    d.http.script.push_back(e);
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("conversations HTTP 503: busy", err.c_str());
  }
}

// The tool loop (chat/completions) on a key whose plan allows 0 requests/min there:
// the live personal-key shape. Waiting never helps, and the error says so.
static void test_loop_429_zero_per_minute_plan() {
  FakeProviderDeps d;
  harness_test::Exchange e;
  e.status = 429;
  e.body = "{\"message\":\"Requests rate limit exceeded\"}";
  e.headers = {{"x-ratelimit-limit-req-minute", "0"},
               {"x-ratelimit-remaining-req-minute", "0"}};
  d.http.script.push_back(e);
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, &rig.ht, nullptr));
  TEST_ASSERT_TRUE_MESSAGE(err.find("chat HTTP 429 [rl:plan]") != std::string::npos, err.c_str());
  TEST_ASSERT_EQUAL((int)agent::RateLimit::NotAllowed, (int)agent::rateLimitFromError(err));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reserved_toolname_renamed_and_inverted);
  RUN_TEST(test_new_conversation_request_shape);
  RUN_TEST(test_continuation_sends_no_model_or_instructions);
  RUN_TEST(test_error_mapping);
  RUN_TEST(test_loop_chat_completions_stateless);
  RUN_TEST(test_loop_stall_forces_named_final);
  RUN_TEST(test_loop_422_detail_surfaces);
  RUN_TEST(test_foreign_call_ids_normalized);
  RUN_TEST(test_sub_dispatch_and_poll_cache);
  RUN_TEST(test_sub_dispatch_array_content);
  RUN_TEST(test_sub_dispatch_captures_code_interpreter_file);
  RUN_TEST(test_sub_dispatch_file_only_no_prose);
  RUN_TEST(test_sub_dispatch_text_only_no_artifacts);
  RUN_TEST(test_sub_per_dispatch_model_override);
  RUN_TEST(test_sub_error_mapping);
  RUN_TEST(test_head_without_studio_connector_is_unchanged);
  RUN_TEST(test_head_with_studio_connector_sends_no_strict_schema);
  RUN_TEST(test_head_studio_continuation_sends_no_strict_schema);
  RUN_TEST(test_head_studio_lenient_read);
  RUN_TEST(test_head_schema_xor_studio_connector_class);
  RUN_TEST(test_head_429_carries_the_quota_window);
  RUN_TEST(test_loop_429_zero_per_minute_plan);
  UNITY_END();
  return 0;
}
