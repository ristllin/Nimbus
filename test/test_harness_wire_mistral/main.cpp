#include <unity.h>

#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"

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
  TEST_ASSERT_EQUAL(0, d.mistralAttaches);
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
  UNITY_END();
  return 0;
}
