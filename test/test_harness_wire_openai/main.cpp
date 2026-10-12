#include <unity.h>

#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/rate_limit.h"

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

// A final response that batched a registry call WITH orch_turn (here AFTER it).
// CUM-506 reversed the old "drop the sibling" rule: dropping lost real side
// effects (five memory writes) while the reply claimed them. The sibling now runs
// exactly ONCE before any reply is delivered, and the next round - which replays
// the call and its output, never the premature final - writes the reply.
static void test_batched_sibling_with_orch_turn_runs_once() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"id\":\"resp_dirty\",\"output\":["
      "{\"type\":\"function_call\",\"call_id\":\"call_f\",\"name\":\"orch_turn\","
      "\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"},"
      "{\"type\":\"function_call\",\"call_id\":\"call_x\",\"name\":\"memory_search\","
      "\"arguments\":\"{}\"}]}"});
  d.http.script.push_back({"", "", 200, finalCallBody("resp_2")});   // the re-asked reply
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
  TEST_ASSERT_EQUAL_STRING("responses", conv.c_str());     // marker, not an id
  TEST_ASSERT_EQUAL(1, (int)rig.dispatched.size());        // the sibling ran, once
  TEST_ASSERT_EQUAL_STRING("memory_search", rig.dispatched[0].name.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());           // the reply is re-asked
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"call_id\":\"call_x\""));
  TEST_ASSERT_FALSE(bodyHas(d, 1, "\"call_id\":\"call_f\""));
}

// ...and when the batched call FAILS, the premature final is discarded and the
// model answers again with the failure in hand (the replay carries the call and
// its output), so the delivered reply cannot claim what did not happen.
static void test_batched_sibling_failure_reasks_with_the_result() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"id\":\"resp_dirty\",\"output\":["
      "{\"type\":\"function_call\",\"call_id\":\"call_x\",\"name\":\"memory_write\","
      "\"arguments\":\"{}\"},"
      "{\"type\":\"function_call\",\"call_id\":\"call_f\",\"name\":\"orch_turn\","
      "\"arguments\":\"{\\\"reply\\\":\\\"saved\\\"}\"}]}"});
  d.http.script.push_back({"", "", 200, finalCallBody("resp_2")});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  rig.resultIsError = true;
  rig.result = "store full";
  std::string conv, out, err;
  TEST_ASSERT_TRUE(providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, &rig.ht, nullptr));
  TEST_ASSERT_EQUAL(1, (int)rig.dispatched.size());   // ran once, never re-dispatched
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_TRUE(bodyHas(d, 1, "\"call_id\":\"call_x\""));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "store full"));
  TEST_ASSERT_FALSE(bodyHas(d, 1, "\"call_id\":\"call_f\""));   // the premature final is gone
  TEST_ASSERT_TRUE(out.find("saved") == std::string::npos);
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

// CUM-465: a background dispatch refused with a 429 hands back the window its text
// names (OpenAI says it in the message), exactly as a head turn's 429 reads it.
static void test_sub_dispatch_429_names_the_window() {
  struct Case { const char* body; agent::RateLimit want; };
  const Case cases[] = {
      {"{\"error\":{\"message\":\"Rate limit reached for gpt-4o-mini in organization org-x on "
       "requests per day (RPD): Limit 200, Used 200, Requested 1.\",\"code\":\"rate_limit_exceeded\"}}",
       agent::RateLimit::Daily},
      {"{\"error\":{\"message\":\"Rate limit reached for gpt-4o on tokens per min (TPM): Limit "
       "30000, Used 29000, Requested 2000.\"}}",
       agent::RateLimit::PerMinute},
      {"{\"error\":{\"message\":\"You exceeded your current quota, please check your plan and "
       "billing details.\",\"code\":\"insufficient_quota\"}}",
       agent::RateLimit::Quota},
      {"{}", agent::RateLimit::Unknown},
  };
  for (const Case& k : cases) {
    FakeProviderDeps d;
    d.http.script.push_back({"", "/v1/responses", 429, k.body});
    auto pd = d.contract();
    agent::RateLimit window = agent::RateLimit::kCount;   // sentinel: never written
    agent::Directive dir; dir.instruction = "x"; dir.rateLimit = &window;
    char jobId[72];
    TEST_ASSERT_EQUAL((int)agent::FabricErr::RateLimited,
        (int)providers::oaiDispatch(pd, "h", "k", "m", "openai", dir, jobId));
    TEST_ASSERT_EQUAL_MESSAGE((int)k.want, (int)window, k.body);
  }
  {  // any other failure writes no window
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 500, "{\"error\":{\"message\":\"requests per day\"}}"});
    auto pd = d.contract();
    agent::RateLimit window = agent::RateLimit::kCount;
    agent::Directive dir; dir.instruction = "x"; dir.rateLimit = &window;
    char jobId[72];
    TEST_ASSERT_EQUAL((int)agent::FabricErr::RemoteFail,
        (int)providers::oaiDispatch(pd, "h", "k", "m", "openai", dir, jobId));
    TEST_ASSERT_EQUAL((int)agent::RateLimit::kCount, (int)window);
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

// ---- CUM-477: the router loop resends a request the device never finished ------
// Bench (nimbus-light, Cumulo head, C2): round 1's request hit "short body write
// 0/66852 ... heap=9940"; the router loop (orchTurnOpenAI's own loop - the Cumulo
// head never runs the fabric) had no step retry, the turn failed after a tool had
// run, and the owner read "the provider could not be reached (check the
// connection)". A fake transport fails the round-1 write under heap pressure.

struct RouterRig {
  FakeProviderDeps d;
  FakeProviderDeps::ToolRig rig;
  agent::providers::ProviderDeps pd;
  bool recovered = false;
  int delays = 0;
  uint32_t delayed = 0;
  explicit RouterRig(bool recovers) {
    harness_test::Exchange r0;
    r0.expectHost = "app.cumulo-nimbus.ai";
    r0.expectPathContains = "/router/openai/v1/responses";
    r0.body = toolCallBody("resp_1");
    d.http.script.push_back(r0);
    harness_test::Exchange unsent;   // the round-1 write that never left the device
    unsent.status = 0;
    unsent.err = "request body truncated (0/66852 bytes written)";
    d.http.script.push_back(unsent);
    harness_test::Exchange r1 = r0;
    r1.body = finalCallBody("resp_2");
    d.http.script.push_back(r1);
    pd = d.contract();
    pd.routerBase = [] { return std::string("https://app.cumulo-nimbus.ai"); };
    pd.routerKey  = [] { return std::string("cumulo_sk_test"); };
    // Free heap: healthy until the failed write, then 9940 (the bench number)
    // until the heap recovers (the first wait, when `recovers`).
    pd.freeHeap = [this] { return (d.http.idx == 2 && !recovered) ? 9940u : 100000u; };
    pd.delayMs = [this, recovers](uint32_t ms) {
      ++delays;
      delayed += ms;
      d.now += ms;   // the wait passes on the device clock too
      if (recovers) recovered = true;
    };
    d.fillTools(rig);
    rig.ht.cfg.roundMinHeap = 12000;   // ORCH_LOOP_MIN_HEAP on the device
  }
};

static void test_cum477_router_loop_resends_after_heap_recovers() {
  RouterRig r(/*recovers=*/true);
  std::string conv, out, err;
  const bool ok = providers::orchTurnOpenAI(r.pd, conv, "SYS", "USER", out, err, &r.rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(3, (int)r.d.http.seen.size());   // round 0, the unsent round 1, its resend
  TEST_ASSERT_EQUAL_STRING(reqBody(r.d, 1).c_str(), reqBody(r.d, 2).c_str());   // same request
  TEST_ASSERT_EQUAL(1, (int)r.rig.dispatched.size());   // the round-0 tool ran ONCE
  TEST_ASSERT_TRUE(r.delays >= 1);                       // it waited for the heap, on this task
  TEST_ASSERT_TRUE(LogCapture::contains("orchTurn(oai-loop): request not sent (heap 9940) - one "
                                        "same-host resend after the heap recovered"));
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
}

// The heap never comes back: no resend (it would fail the same way), a bounded
// wait, and an error the engine reads as low memory - never "network".
static void test_cum477_no_recovery_fails_as_low_memory_not_network() {
  RouterRig r(/*recovers=*/false);
  r.d.http.script.pop_back();   // a resend would be unscripted: the test fails loudly
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(r.pd, conv, "SYS", "USER", out, err, &r.rig.ht, nullptr));
  TEST_ASSERT_EQUAL(2, (int)r.d.http.seen.size());
  TEST_ASSERT_TRUE(err.rfind("low memory", 0) == 0);
  TEST_ASSERT_TRUE(r.delayed >= 3000u && r.delayed <= 3200u);   // bounded wait
  TEST_ASSERT_TRUE(LogCapture::contains("request not sent and the heap did not recover"));
}

// The resend is refused because the heap stayed under the resend threshold (the
// round floor + its margin) for the whole wait: total free 14000 with the 12000
// floor sits above the floor but inside the margin. The refusal reason is memory,
// so the step says memory; "network" told the owner to check the connection
// after a tool had already run.
static void test_cum477_refused_in_the_margin_band_reports_memory() {
  RouterRig r(/*recovers=*/false);
  r.d.http.script.pop_back();   // a resend would be unscripted: the test fails loudly
  r.pd.freeHeap = [] { return 14000u; };
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(r.pd, conv, "SYS", "USER", out, err, &r.rig.ht, nullptr));
  TEST_ASSERT_EQUAL(2, (int)r.d.http.seen.size());   // no third exchange
  TEST_ASSERT_EQUAL(1, (int)r.rig.dispatched.size());
  TEST_ASSERT_TRUE(r.delayed >= 3000u && r.delayed <= 3200u);
  TEST_ASSERT_TRUE_MESSAGE(err.rfind("low memory", 0) == 0, err.c_str());
}

// Counter-test (CUM-404 v2): the largest block alone under its floor for the whole
// wait, with total free fine, refuses the resend but is NOT reported as memory - a
// Freenove pins its largest block near that floor, and "low memory" would skip the
// engine's retry and failover for what may be a genuine outage.
static void test_cum477_largest_block_alone_stays_network() {
  RouterRig r(/*recovers=*/false);
  r.d.http.script.pop_back();
  r.pd.freeHeap = [] { return 100000u; };
  r.rig.ht.cfg.roundMinLargest = 5000;   // ORCH_TURN_MIN_LARGEST_BLOCK
  r.pd.largestBlock = [&r] { return r.d.http.idx >= 2 ? 4000u : 60000u; };
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(r.pd, conv, "SYS", "USER", out, err, &r.rig.ht, nullptr));
  TEST_ASSERT_EQUAL(2, (int)r.d.http.seen.size());   // refused: no resend
  TEST_ASSERT_TRUE(r.delayed >= 3000u);
  TEST_ASSERT_EQUAL_STRING("network", err.c_str());
}

// Counter-test: with no way to wait (no delay seam) a reading taken the instant
// after the failure is not evidence - the failed connection's buffers may not be
// back yet - so the margin band stays the plain network failure it was.
static void test_cum477_no_wait_seam_keeps_network_in_the_band() {
  RouterRig r(/*recovers=*/false);
  r.d.http.script.pop_back();
  r.pd.freeHeap = [] { return 14000u; };
  r.pd.delayMs = nullptr;
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(r.pd, conv, "SYS", "USER", out, err, &r.rig.ht, nullptr));
  TEST_ASSERT_EQUAL(2, (int)r.d.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("network", err.c_str());
}

// Counter-tests in the dimensions the resend now reads: a request that WAS sent
// and got no answer is never resent (the provider may have acted on it), and an
// unsent request with a healthy heap is resent at once, without waiting.
static void test_cum477_only_unsent_requests_are_resent() {
  RouterRig sent(/*recovers=*/true);
  sent.d.http.script[1].err = "no response";
  sent.d.http.script.pop_back();
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(sent.pd, conv, "S", "U", out, err, &sent.rig.ht, nullptr));
  TEST_ASSERT_EQUAL(2, (int)sent.d.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("network", err.c_str());

  RouterRig healthy(/*recovers=*/true);
  healthy.pd.freeHeap = [] { return 100000u; };
  TEST_ASSERT_TRUE(providers::orchTurnOpenAI(healthy.pd, conv, "S", "U", out, err, &healthy.rig.ht, nullptr));
  TEST_ASSERT_EQUAL(3, (int)healthy.d.http.seen.size());
  TEST_ASSERT_EQUAL(0, healthy.delays);
}

// prism: the resend runs on what is left of the round's budget (F25), so its
// socket timeout is shorter than the first attempt's, never a fresh full round.
static void test_cum477_resend_gets_the_rest_of_the_round_budget() {
  RouterRig r(/*recovers=*/true);
  r.rig.ht.cfg.deadlineMs = 40000;   // a round budget under the 120 s cap
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnOpenAI(r.pd, conv, "S", "U", out, err, &r.rig.ht, nullptr),
                           err.c_str());
  TEST_ASSERT_TRUE(r.d.http.seen[2].timeoutMs < r.d.http.seen[1].timeoutMs);
}

// prism: a step that fails before reaching the transport never inherits an older
// exchange's "unsent" flag (exchange() resets the record on entry).
static void test_cum477_no_transport_no_resend() {
  RouterRig r(/*recovers=*/true);
  r.d.http.script.erase(r.d.http.script.begin() + 1, r.d.http.script.end());
  std::string conv, out, err;
  // First a real unsent failure on another turn leaves the record set...
  RouterRig prev(/*recovers=*/false);
  prev.d.http.script.pop_back();
  providers::orchTurnOpenAI(prev.pd, conv, "S", "U", out, err, &prev.rig.ht, nullptr);
  // ...then a turn whose round 1 fails with no transport call at all.
  r.pd.http = nullptr;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(r.pd, conv, "S", "U", out, err, &r.rig.ht, nullptr));
  TEST_ASSERT_EQUAL(0, r.delays);
  TEST_ASSERT_EQUAL_STRING("network", err.c_str());
}

// prism (peer review): a transport failure the transport itself tagged as memory
// pressure ("... : low memory", the exchange began starved) is reported as low
// memory by the single-shot head too, so the engine gives the honest memory reply
// instead of a retry, a failover and "check the connection". Untagged stays network.
static void test_cum477_tagged_low_memory_reaches_the_single_shot_head() {
  FakeProviderDeps d;
  harness_test::Exchange starved;
  starved.status = 0;
  starved.err = "connect failed: low memory";
  d.http.script.push_back(starved);
  d.heap = 9000;   // and total free is genuinely short
  auto pd = d.contract();
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, nullptr, nullptr));
  TEST_ASSERT_TRUE(err.rfind("low memory", 0) == 0);

  // The tag alone (a fragmented-but-healthy heap, the Freenove's normal state) is
  // NOT memory: it stays network so the failover ladder still runs (CUM-404 v2).
  FakeProviderDeps f;
  f.http.script.push_back(starved);
  auto fpd = f.contract();   // heap 100000
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(fpd, conv, "S", "U", out, err, nullptr, nullptr));
  TEST_ASSERT_EQUAL_STRING("network", err.c_str());

  FakeProviderDeps n;
  harness_test::Exchange down;
  down.status = 0;
  down.err = "connect failed";
  n.http.script.push_back(down);
  auto npd = n.contract();
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(npd, conv, "S", "U", out, err, nullptr, nullptr));
  TEST_ASSERT_EQUAL_STRING("network", err.c_str());
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
  RUN_TEST(test_batched_sibling_with_orch_turn_runs_once);
  RUN_TEST(test_batched_sibling_failure_reasks_with_the_result);
  RUN_TEST(test_loop_gate_respects_toggle);
  RUN_TEST(test_sub_dispatch_shape);
  RUN_TEST(test_sub_dispatch_error_mapping);
  RUN_TEST(test_sub_dispatch_429_names_the_window);
  RUN_TEST(test_sub_poll_states);
  RUN_TEST(test_sub_poll_captures_container_file_citation);
  RUN_TEST(test_over_cap_repeat_citations_count_once);
  RUN_TEST(test_sub_poll_no_annotations_no_artifacts);
  RUN_TEST(test_cum477_router_loop_resends_after_heap_recovers);
  RUN_TEST(test_cum477_no_recovery_fails_as_low_memory_not_network);
  RUN_TEST(test_cum477_refused_in_the_margin_band_reports_memory);
  RUN_TEST(test_cum477_largest_block_alone_stays_network);
  RUN_TEST(test_cum477_no_wait_seam_keeps_network_in_the_band);
  RUN_TEST(test_cum477_only_unsent_requests_are_resent);
  RUN_TEST(test_cum477_resend_gets_the_rest_of_the_round_budget);
  RUN_TEST(test_cum477_no_transport_no_resend);
  RUN_TEST(test_cum477_tagged_low_memory_reaches_the_single_shot_head);
  UNITY_END();
  return 0;
}
