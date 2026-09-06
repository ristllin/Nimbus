#include <unity.h>

#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"

// Stage H wire suite - the OpenAI Responses provider, host-tested for the first
// time over FakeHttpTransport: single-shot strict json_schema request shape,
// previous_response_id continuity (R_OAI_prev_response_id), parse/usage/error
// mapping, the tool loop's "required" tool_choice + function_call_output
// pairing, and the F20 chain-poisoning guards (R_F20): the 400 "No tool output
// found" self-heal AND the dirty-final-not-chained rule.

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::bodyHas;
using harness_test::headerOf;
using harness_test::reqBody;
namespace providers = agent::providers;
namespace orch = nimbus::orch;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static const char* kFinalTurn = "{\"reply\":\"hello there\",\"memory\":\"\",\"ask\":\"\"}";

// Canned single-shot success (output_text carries the turn JSON).
static std::string singleShotBody() {
  return "{\"id\":\"resp_1\",\"status\":\"completed\",\"output\":[{\"type\":\"message\","
         "\"content\":[{\"type\":\"output_text\",\"text\":"
         "\"{\\\"reply\\\":\\\"hello there\\\",\\\"memory\\\":\\\"\\\",\\\"ask\\\":\\\"\\\"}\"}]}],"
         "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":20}}";
}

// Canned loop rounds.
static std::string toolCallBody(const char* respId) {
  return std::string("{\"id\":\"") + respId + "\",\"output\":[{\"type\":\"function_call\","
         "\"call_id\":\"call_1\",\"name\":\"memory_search\","
         "\"arguments\":\"{\\\"q\\\":\\\"tea\\\"}\"}],"
         "\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":10}}";
}
static std::string finalCallBody(const char* respId) {
  return std::string("{\"id\":\"") + respId + "\",\"output\":[{\"type\":\"function_call\","
         "\"call_id\":\"call_f\",\"name\":\"orch_turn\","
         "\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}],"
         "\"usage\":{\"prompt_tokens\":60,\"completion_tokens\":15}}";
}

// ---- single-shot ------------------------------------------------------------

static void test_single_shot_request_shape() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.openai.com", "/v1/responses", 200, singleShotBody()});
  auto pd = d.contract();
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "SYS", "USER", out, err,
                                      nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(1, (int)d.http.seen.size());
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_EQUAL_STRING("POST", r.method.c_str());
  TEST_ASSERT_TRUE(r.tls);
  TEST_ASSERT_EQUAL(443, r.port);
  TEST_ASSERT_EQUAL_STRING("Bearer sk-fake-oai", headerOf(r, "Authorization").c_str());
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"model\":\"model-openai\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"instructions\":\"SYS\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"input\":\"USER\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"store\":true"));
  // Fresh conversation: NO previous_response_id.
  TEST_ASSERT_FALSE(bodyHas(d, 0, "previous_response_id"));
  // STRICT structured output rides text.format, schema descriptions intact
  // (OpenAI ENFORCES the schema - no description strip here, unlike Anthropic).
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"type\":\"json_schema\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"name\":\"orch_turn\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"strict\":true"));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "Text to send the owner now"));
  // Hosted connectors attach to the single-shot head turn.
  TEST_ASSERT_EQUAL(1, d.openAiAttaches);
  TEST_ASSERT_TRUE(bodyHas(d, 0, "fake-connector"));
}

static void test_single_shot_parse_conv_usage() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.openai.com", "/v1/responses", 200, singleShotBody()});
  auto pd = d.contract();
  std::string conv = "resp_prev", out, err;
  orch::TokenUsage u;
  bool ok = providers::orchTurnOpenAI(pd, conv, "SYS", "USER", out, err, nullptr, &u);
  TEST_ASSERT_TRUE(ok);
  // R_OAI_prev_response_id: a conv id already in the store still rides the
  // request, so a legacy value flushes naturally on its next use...
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"previous_response_id\":\"resp_prev\""));
  // ...but NO new chain head is kept.
  //
  // ⚠ Inverted deliberately (this asserted "resp_1" - the chain advancing).
  // OpenAI's server-side chain duplicates history this device already sends: the
  // system prompt carries the conversation summary and the per-chat recent
  // window every turn. Keeping the chain re-billed all of it - a Board 1
  // single-shot turn whose entire prompt is ~21.9 KB (~5.5 K tokens) was billed
  // 16,897 input tokens - and a stored id is exactly what goes stale and 400s
  // the following turn. Continuity is device-side now.
  TEST_ASSERT_EQUAL_STRING("", conv.c_str());
  TEST_ASSERT_EQUAL_STRING(kFinalTurn, out.c_str());
  TEST_ASSERT_EQUAL(100, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(20, (int)u.completionTokens);
}

static void test_no_key_refused() {
  FakeProviderDeps d;
  d.keys.erase("openai");
  auto pd = d.contract();
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, nullptr, nullptr));
  TEST_ASSERT_EQUAL_STRING("no OpenAI key", err.c_str());
  TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());
}

static void test_error_mapping() {
  {  // 401 with provider message
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 401, "{\"error\":{\"message\":\"bad key\"}}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("resp HTTP 401: bad key", err.c_str());
  }
  {  // transport failure
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 0, ""});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("network", err.c_str());
  }
  {  // 200 but no output_text
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200, "{\"id\":\"resp_1\",\"output\":[]}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("no output_text", err.c_str());
  }
  {  // 500
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 500, "{}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("resp HTTP 500", err.c_str());
  }
}

// ---- tool loop --------------------------------------------------------------

static void test_loop_round_shapes_and_pairing() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.openai.com", "/v1/responses", 200, toolCallBody("resp_1")});
  d.http.script.push_back({"api.openai.com", "/v1/responses", 200, finalCallBody("resp_2")});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  orch::TokenUsage u;
  bool ok = providers::orchTurnOpenAI(pd, conv, "SYS", "USER", out, err, &rig.ht, &u);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  // Round 0: tools rounds force "required" (the string, NOT an object) so the
  // model can't stall in prose; the terminal orch_turn is strict, registry
  // tools are not; instructions re-sent every round. Stage 2 phase 3: the input
  // is a MESSAGE ARRAY rendered from the canonical transcript (was a bare
  // string), and the request is stateless (store:false, no chain).
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"tool_choice\":\"required\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"name\":\"orch_turn\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"name\":\"memory_search\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"type\":\"message\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"role\":\"user\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"content\":\"USER\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"instructions\":\"SYS\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"store\":false"));
  TEST_ASSERT_EQUAL(2, d.openAiAttaches);   // hosted MCP rides EVERY tool round
  // The dispatched registry call reached the rig with its raw args.
  TEST_ASSERT_EQUAL(1, (int)rig.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("memory_search", rig.dispatched[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("call_1", rig.dispatched[0].id.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"q\":\"tea\"}", rig.dispatched[0].argsJson.c_str());
  // Round 1: the full transcript replays - the function_call and its paired
  // function_call_output ride the input; no previous_response_id anywhere.
  TEST_ASSERT_FALSE(bodyHas(d, 1, "previous_response_id"));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"type\":\"function_call\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"type\":\"function_call_output\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"call_id\":\"call_1\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"output\":\"tool-ok\""));
  // Outcome: the orch_turn arguments ARE the turn; usage summed across rounds.
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
  TEST_ASSERT_EQUAL(110, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(25, (int)u.completionTokens);
  // The persisted marker is a constant - never a resp_ id (nothing to poison).
  TEST_ASSERT_EQUAL_STRING("responses", conv.c_str());
}

static void test_loop_tool_error_encoded_in_output() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, toolCallBody("resp_1")});
  d.http.script.push_back({"", "", 200, finalCallBody("resp_2")});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  rig.result = "boom";
  rig.resultIsError = true;
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  // No is_error field exists on function_call_output - failure is text-encoded.
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"output\":\"ERROR: boom\""));
}

static void test_loop_forced_final_tool_choice() {
  FakeProviderDeps d;
  // Round 0 dispatches a tool; cap maxRounds=1 forces the next round tool-less.
  d.http.script.push_back({"", "", 200, toolCallBody("resp_1")});
  d.http.script.push_back({"", "", 200, finalCallBody("resp_2")});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  rig.ht.cfg.maxRounds = 1;
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  // Forced final: tool_choice is the pinned FUNCTION OBJECT, not "required".
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"tool_choice\":{\"type\":\"function\",\"name\":\"orch_turn\"}"));
}

// Glass Box A4 (OpenAI reasoning capture): a REASONING model requests a summary
// ("reasoning":{"summary":"auto"}) and a returned reasoning item's summary text
// flows to HeadTools.onRoundText (round-tagged). A non-reasoning model must NOT
// send the parameter (it 400s on chat models - the gate is the safety).
static void test_loop_reasoning_summary_capture() {
  FakeProviderDeps d;
  // Round 0 carries a reasoning item ahead of the tool call; round 1 finishes.
  d.http.script.push_back({"", "", 200,
      std::string("{\"id\":\"resp_1\",\"output\":[")
      + "{\"type\":\"reasoning\",\"summary\":[{\"type\":\"summary_text\","
        "\"text\":\"checking memory first\"}]},"
      + "{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"memory_search\","
        "\"arguments\":\"{}\"}],"
        "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2}}"});
  d.http.script.push_back({"", "", 200, finalCallBody("resp_2")});
  auto pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("o4-mini"); };  // reasoning family
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::vector<std::pair<std::string, int>> seenText;
  rig.ht.onRoundText = [&](const std::string& t, int round) {
    seenText.push_back({t, round});
  };
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"reasoning\":{\"summary\":\"auto\"}"));
  TEST_ASSERT_EQUAL(1, (int)seenText.size());
  TEST_ASSERT_EQUAL_STRING("checking memory first", seenText[0].first.c_str());
  TEST_ASSERT_EQUAL(0, seenText[0].second);
}

static void test_loop_no_reasoning_param_on_chat_model() {
  FakeProviderDeps d;   // default fake model "model-openai" - NOT a reasoning family
  d.http.script.push_back({"", "", 200, finalCallBody("resp_1")});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"reasoning\""));
}

// gpt-5-chat-latest is the NON-reasoning ChatGPT snapshot inside the gpt-5 prefix
// - the reasoning parameter hard-400s on it (prism riders finding: the bare
// starts("gpt-5") gate matched it and would have failed every turn on that model).
static void test_loop_no_reasoning_param_on_gpt5_chat_variant() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, finalCallBody("resp_1")});
  auto pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("gpt-5-chat-latest"); };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"reasoning\""));
}

// gpt-6-astra (2026-09) is a reasoning model like gpt-5.x: under the stateless
// store:false replay it needs the encrypted reasoning items included with its
// function calls, so the gate must recognise the NEW generation (the old literal
// "gpt-5" prefix left it ungated - a 400 on round 2 of every tool-calling turn).
// The class rule (every gpt-<N>, N >= 5) is what is asserted; the -chat carve-out
// still applies to the new generation.
static void test_loop_reasoning_param_on_every_gpt_generation() {
  for (int gen = 5; gen <= 9; ++gen) {
    const std::string model = "gpt-" + std::to_string(gen) + (gen == 6 ? "-astra" : "-x");
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200, finalCallBody("resp_1")});
    auto pd = d.contract();
    pd.orchModel = [model](const char*) { return model; };
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    std::string conv, out, err;
    bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
    TEST_ASSERT_TRUE_MESSAGE(ok, model.c_str());
    TEST_ASSERT_TRUE_MESSAGE(bodyHas(d, 0, "\"reasoning\":{\"summary\":\"auto\"}"), model.c_str());
    TEST_ASSERT_TRUE_MESSAGE(bodyHas(d, 0, "reasoning.encrypted_content"), model.c_str());
  }
  // The non-reasoning -chat snapshot of the new generation must stay ungated.
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, finalCallBody("resp_1")});
  auto pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("gpt-6-chat-latest"); };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"reasoning\""));
}

// Stage 2 phase 3: the loop is STATELESS - no previous_response_id ever, no
// store:true, and a stale chain id passed in as convId is simply ignored. The
// entire F20 chain-poisoning class (every successful turn used to poison the
// next; the self-heal then doubled round-0 cost) is structurally gone.
static void test_loop_is_stateless() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, toolCallBody("resp_1")});
  d.http.script.push_back({"", "", 200, finalCallBody("resp_2")});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv = "resp_stale_legacy_chain", out, err;   // must be ignored
  bool ok = providers::orchTurnOpenAI(pd, conv, "SYS", "USER", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  for (int i = 0; i < 2; i++) {
    TEST_ASSERT_FALSE(bodyHas(d, i, "previous_response_id"));
    TEST_ASSERT_FALSE(bodyHas(d, i, "\"store\":true"));
    TEST_ASSERT_TRUE(bodyHas(d, i, "\"store\":false"));
  }
  // The persisted marker is a constant, never a resp_ id - nothing to poison.
  TEST_ASSERT_EQUAL_STRING("responses", conv.c_str());
}

// Within-turn continuity is now FULL REPLAY: round 1's input[] carries the user
// seed + round 0's function_call AND its function_call_output (the canonical
// transcript rendered onto the wire), so the server needs no stored state.
static void test_loop_round2_replays_full_transcript() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, toolCallBody("resp_1")});
  d.http.script.push_back({"", "", 200, finalCallBody("resp_2")});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  TEST_ASSERT_TRUE(providers::orchTurnOpenAI(pd, conv, "SYS", "USER", out, err,
                                             &rig.ht, nullptr));
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  // Round 1 replays: the user message, the function_call it answers, its output.
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"type\":\"message\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"content\":\"USER\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"type\":\"function_call\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"name\":\"memory_search\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"type\":\"function_call_output\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"call_id\":\"call_1\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"output\":\"tool-ok\""));
}

// A final response that batched a registry call WITH orch_turn: the turn is
// over, the batched sibling is never dispatched (re-dispatch would double side
// effects), and - stateless - there is no chain head to worry about.
static void test_batched_sibling_with_orch_turn_dropped() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"id\":\"resp_dirty\",\"output\":["
      "{\"type\":\"function_call\",\"call_id\":\"call_f\",\"name\":\"orch_turn\","
      "\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"},"
      "{\"type\":\"function_call\",\"call_id\":\"call_x\",\"name\":\"memory_search\","
      "\"arguments\":\"{}\"}]}"});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
  TEST_ASSERT_EQUAL_STRING("responses", conv.c_str());     // marker, not an id
  TEST_ASSERT_EQUAL(0, (int)rig.dispatched.size());        // sibling dropped
}

// The loop gate itself: tools supplied but the master switch OFF => single-shot.
static void test_loop_gate_respects_toggle() {
  FakeProviderDeps d;
  d.toolLoop = false;
  d.http.script.push_back({"", "", 200, singleShotBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(1, (int)d.http.seen.size());
  TEST_ASSERT_TRUE(bodyHas(d, 0, "json_schema"));            // single-shot strict path
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"tool_choice\":\"required\""));
}

// ---- sub-session ------------------------------------------------------------

static void test_sub_dispatch_shape() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.openai.com", "/v1/responses", 200, "{\"id\":\"resp_9\"}"});
  auto pd = d.contract();
  agent::Directive dir;
  dir.category = "research";
  dir.instruction = "find the thing";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::oaiDispatch(pd, "api.openai.com", "sk-fake-oai",
                                                "gpt-5.5", "openai", dir, jobId));
  TEST_ASSERT_EQUAL_STRING("openai:resp_9", jobId);
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"background\":true"));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"store\":true"));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"model\":\"gpt-5.5\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"type\":\"web_search\""));  // default hosted web
  TEST_ASSERT_TRUE(bodyHas(d, 0, "autonomous research agent"));
  TEST_ASSERT_EQUAL(1, d.openAiAttaches);                      // connectors ride subs
}

static void test_sub_dispatch_error_mapping() {
  agent::Directive dir; dir.instruction = "x";
  char jobId[72];
  {
    FakeProviderDeps d; d.http.script.push_back({"", "", 401, "{}"});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Auth,
        (int)providers::oaiDispatch(pd, "h", "k", "m", "openai", dir, jobId));
  }
  {
    FakeProviderDeps d; d.http.script.push_back({"", "", 429, "{}"});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::RateLimited,
        (int)providers::oaiDispatch(pd, "h", "k", "m", "openai", dir, jobId));
  }
  {
    FakeProviderDeps d; d.http.script.push_back({"", "", 0, ""});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Network,
        (int)providers::oaiDispatch(pd, "h", "k", "m", "openai", dir, jobId));
  }
  {  // empty key: refused before any wire traffic
    FakeProviderDeps d;
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Auth,
        (int)providers::oaiDispatch(pd, "h", "", "m", "openai", dir, jobId));
    TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());
  }
}

static void test_sub_poll_states() {
  {  // completed with concatenated output_text
    FakeProviderDeps d;
    d.http.script.push_back({"", "/v1/responses/resp_9", 200,
        "{\"status\":\"completed\",\"output\":[{\"content\":["
        "{\"type\":\"output_text\",\"text\":\"part1 \"},"
        "{\"type\":\"output_text\",\"text\":\"part2\"}]}]}"});
    auto pd = d.contract();
    agent::ResultEnvelope env{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
        (int)providers::oaiPoll(pd, "h", "k", "openai", "openai:resp_9", env));
    TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
    TEST_ASSERT_EQUAL_STRING("part1 part2", env.reply);
  }
  {  // failed carries the provider error
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"status\":\"failed\",\"error\":{\"message\":\"exploded\"}}"});
    auto pd = d.contract();
    agent::ResultEnvelope env{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
        (int)providers::oaiPoll(pd, "h", "k", "openai", "openai:resp_9", env));
    TEST_ASSERT_EQUAL((int)agent::JobState::Error, (int)env.state);
    TEST_ASSERT_EQUAL_STRING("exploded", env.error);
  }
  {  // 404 = expired
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 404, "{}"});
    auto pd = d.contract();
    agent::ResultEnvelope env{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::NotFound,
        (int)providers::oaiPoll(pd, "h", "k", "openai", "openai:resp_9", env));
  }
}

// W7b: a code_interpreter run that wrote a file cites it as a
// container_file_citation annotation - the poll packs {container_id, file_id}
// into artifacts[] (url="cntr/cfile", label=filename), DEDUPED by file_id (the
// model cites one file repeatedly). Verified against a real response shape
// (live 2026-08-08).
static void test_sub_poll_captures_container_file_citation() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/responses/resp_9", 200,
      "{\"status\":\"completed\",\"output\":[{\"content\":["
      "{\"type\":\"output_text\",\"text\":\"made your pdf\","
      "\"annotations\":["
      "{\"type\":\"container_file_citation\",\"container_id\":\"cntr_abc\","
      "\"file_id\":\"cfile_123\",\"filename\":\"report.pdf\"},"
      "{\"type\":\"container_file_citation\",\"container_id\":\"cntr_abc\","
      "\"file_id\":\"cfile_123\",\"filename\":\"report.pdf\"}"   // duplicate cite
      "]}]}]}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
      (int)providers::oaiPoll(pd, "h", "k", "openai", "openai:resp_9", env));
  TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
  TEST_ASSERT_EQUAL_STRING("made your pdf", env.reply);
  TEST_ASSERT_EQUAL(1, env.artifactCount);              // deduped
  TEST_ASSERT_EQUAL_STRING("file", env.artifacts[0].type);
  TEST_ASSERT_EQUAL_STRING("cntr_abc/cfile_123", env.artifacts[0].url);
  TEST_ASSERT_EQUAL_STRING("report.pdf", env.artifacts[0].label);
}

// prism: a file beyond the cap that is cited REPEATEDLY must count ONCE - the
// note said "3 more files" when a single uncaptured file was mentioned 3 times.
static void test_over_cap_repeat_citations_count_once() {
  FakeProviderDeps d;
  std::string body = "{\"status\":\"completed\",\"output\":[{\"content\":["
      "{\"type\":\"output_text\",\"text\":\"done\",\"annotations\":[";
  // 3 distinct files fill kMaxArtifacts (3); a 4th is cited three times.
  for (int i = 1; i <= 3; i++)
    body += "{\"type\":\"container_file_citation\",\"container_id\":\"cntr\","
            "\"file_id\":\"cfile_" + std::to_string(i) + "\",\"filename\":\"f.png\"},";
  for (int i = 0; i < 3; i++)
    body += "{\"type\":\"container_file_citation\",\"container_id\":\"cntr\","
            "\"file_id\":\"cfile_over\",\"filename\":\"over.png\"}"
            + std::string(i < 2 ? "," : "");
  body += "]}]}]}";
  d.http.script.push_back({"", "", 200, body});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
      (int)providers::oaiPoll(pd, "h", "k", "openai", "openai:resp_9", env));
  TEST_ASSERT_EQUAL(3, env.artifactCount);
  TEST_ASSERT_TRUE(std::string(env.reply).find("1 more generated file(s)") != std::string::npos);
}

// No annotations => no artifacts (the common text-only run is unchanged).
static void test_sub_poll_no_annotations_no_artifacts() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"status\":\"completed\",\"output\":[{\"content\":["
      "{\"type\":\"output_text\",\"text\":\"just text\"}]}]}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
      (int)providers::oaiPoll(pd, "h", "k", "openai", "openai:resp_9", env));
  TEST_ASSERT_EQUAL(0, env.artifactCount);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_shot_request_shape);
  RUN_TEST(test_single_shot_parse_conv_usage);
  RUN_TEST(test_no_key_refused);
  RUN_TEST(test_error_mapping);
  RUN_TEST(test_loop_round_shapes_and_pairing);
  RUN_TEST(test_loop_tool_error_encoded_in_output);
  RUN_TEST(test_loop_forced_final_tool_choice);
  RUN_TEST(test_loop_reasoning_summary_capture);
  RUN_TEST(test_loop_no_reasoning_param_on_chat_model);
  RUN_TEST(test_loop_no_reasoning_param_on_gpt5_chat_variant);
  RUN_TEST(test_loop_reasoning_param_on_every_gpt_generation);
  RUN_TEST(test_loop_is_stateless);
  RUN_TEST(test_loop_round2_replays_full_transcript);
  RUN_TEST(test_batched_sibling_with_orch_turn_dropped);
  RUN_TEST(test_loop_gate_respects_toggle);
  RUN_TEST(test_sub_dispatch_shape);
  RUN_TEST(test_sub_dispatch_error_mapping);
  RUN_TEST(test_sub_poll_states);
  RUN_TEST(test_sub_poll_captures_container_file_citation);
  RUN_TEST(test_over_cap_repeat_citations_count_once);
  RUN_TEST(test_sub_poll_no_annotations_no_artifacts);
  UNITY_END();
  return 0;
}
