// test_provider_class - the provider wire and fabric rules, each tested as a CLASS
// over every provider, round kind and model family, with a counter-test proving
// the property can see the bug it guards (AGENTS.md section 3):
//   1. tool_choice is never forced on Anthropic, on any path, for any model id;
//   2. the output-cap table: a cut answer is never run or accepted, on every
//      adapter, and the one retry always sends a DIFFERENT body;
//   3. step errors are typed from the providers' own recorded answers;
//   4. the fabric acts on the kind: same-host retry, switch, park, notice;
//   5. the one time policy: every request's deadline comes from one table;
//   6. a 2xx that did not arrive whole is a transport failure, never an answer;
//   7. Mistral's error text comes from every body shape it answers with;
//   8. sub-agent provenance is counted from each provider's own run record.
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/result_envelope.h"
#include "nimbus/orch/transcript.h"

using harness_test::Exchange;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::reqBody;
namespace providers = agent::providers;
namespace orch = nimbus::orch;
using orch::StepErr;

void setUp() {
  LogCapture::install();
  providers::clearHostParks();
  providers::setTestFirstAttemptCap(0);
}
void tearDown() { agent::hlog::setSink(nullptr); }

// Every Anthropic model id the device can be pointed at, plus unknowns. Models
// are parameters: a new id joins this list, never a literal inside a test.
static const char* const kClaudeIds[] = {
    "claude-haiku-5-5", "claude-sonnet-5-5", "claude-opus-5-5", "claude-fable-5-1",
    "claude-mythos-5-1", "claude-opus-5",    "claude-sonnet-5", "claude-fable-5",
    "claude-opus-4-8",  "claude-sonnet-4-6", "claude-haiku-4-5", "claude-3-5-haiku-latest",
    "model-anthropic",  "",
};

static std::string antFinal() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
         "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\"}";
}
static std::string antTool() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
         "\"input\":{\"q\":\"x\"}}],\"stop_reason\":\"tool_use\"}";
}
static std::string antCut() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_c\",\"name\":\"orch_turn\","
         "\"input\":{}}],\"stop_reason\":\"max_tokens\"}";
}
static std::string oaiFinal() {
  return "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
         "\"call_id\":\"c\",\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}]}";
}
static std::string misFinal() {
  return "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\","
         "\"tool_calls\":[{\"id\":\"abcdefghi\",\"function\":{\"name\":\"orch_turn\","
         "\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}}]}}]}";
}

// ---- 1. tool_choice is never forced -----------------------------------------------

// The predicate every rendered Anthropic request must pass. Returns "" when the
// body is clean, else what is wrong.
static std::string antRequestProblem(const std::string& body, const std::string& model) {
  JsonDocument d;
  if (deserializeJson(d, body, DeserializationOption::NestingLimit(32))) return "not JSON";   // the schema is 11 deep
  const char* tc = d["tool_choice"]["type"] | "";
  if (!strcmp(tc, "tool") || !strcmp(tc, "any")) return "forced tool_choice";
  if (!strcmp(d["thinking"]["type"] | "", "disabled")) return "thinking disabled";
  if (!d["thinking"]["budget_tokens"].isNull()) return "budget_tokens";
  if (!d["temperature"].isNull() || !d["top_p"].isNull() || !d["top_k"].isNull()) return "sampling param";
  JsonArrayConst msgs = d["messages"].as<JsonArrayConst>();
  if (msgs.size() == 0 || strcmp(msgs[msgs.size() - 1]["role"] | "", "user")) return "ends on an assistant turn (prefill)";
  int marks = 0;
  for (size_t p = body.find("cache_control"); p != std::string::npos; p = body.find("cache_control", p + 1)) marks++;
  if (marks > 4) return "more than 4 cache_control";
  const bool effort = !d["output_config"]["effort"].isNull();
  if (effort != providers::anthropicTakesEffort(model)) return "effort on a family that refuses it (or missing)";
  if ((d["max_tokens"] | 0) <= 3000) return "max_tokens at the old 3000 cap";
  return "";
}

static void test_tool_choice_never_forced_any_model_any_path() {
  for (const char* id : kClaudeIds) {
    const std::string model = id;
    // (a) every loop round kind: a tool round, and the final round for each cap
    for (const char* cap : {"", "rounds", "deadline", "heap", "bytes", "stalled"}) {
      FakeProviderDeps d;
      d.http.script.push_back({"", "", 200, antFinal()});
      providers::ProviderDeps pd = d.contract();
      pd.orchModel = [model](const char*) { return model; };
      FakeProviderDeps::ToolRig rig;
      d.fillTools(rig);
      orch::Transcript tr;
      tr.addUser("hi");
      if (!strcmp(cap, "stalled")) tr.addAssistantText("prose answer", 0, "");
      const std::string instr = "SYS";
      const bool allow = !*cap;
      providers::antLoopStep(pd, instr, rig.ht, nullptr, tr)(allow, {}, 600000, cap);
      const std::string why = antRequestProblem(reqBody(d, 0), model);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", why.c_str(), (model + " loop cap=" + cap).c_str());
    }
    // (b) the single shot and (c) the compaction fold
    for (const char* instr : {"SYS", orch::ORCH_COMPACT_PROMPT}) {
      FakeProviderDeps d;
      d.http.script.push_back({"", "", 200, antFinal()});
      providers::ProviderDeps pd = d.contract();
      pd.orchModel = [model](const char*) { return model; };
      std::string conv, out, err;
      providers::orchTurnAnthropic(pd, conv, instr, "U", out, err, nullptr, nullptr);
      const std::string why = antRequestProblem(reqBody(d, 0), model);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", why.c_str(), (model + " single-shot/fold").c_str());
    }
  }
  // (d) a fabric switch INTO Anthropic on a forced final round
  FakeProviderDeps d;
  d.http.script.push_back({"api.openai.com", "", 200,
      "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c1\","
      "\"name\":\"memory_search\",\"arguments\":\"{}\"}]}"});
  d.http.script.push_back({"api.openai.com", "", 503, "{\"error\":{\"message\":\"overloaded\"}}"});
  d.http.script.push_back({"api.openai.com", "", 503, "{\"error\":{\"message\":\"overloaded\"}}"});
  d.http.script.push_back({"api.anthropic.com", "", 200, antFinal()});
  providers::ProviderDeps pd = d.contract();
  pd.orchModel = [](const char* h) { return std::string(!strcmp(h, "anthropic") ? "claude-sonnet-5-5" : "gpt-5.6"); };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  rig.ht.cfg.maxRounds = 1;   // round 1 is the forced final round
  std::string out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::runFabricLoop(pd, {"openai", "anthropic"}, "S", "U", out, err,
                                                    rig.ht, nullptr, nullptr), err.c_str());
  TEST_ASSERT_EQUAL_STRING("", antRequestProblem(reqBody(d, 3), "claude-sonnet-5-5").c_str());
  // Counter-test: the pre-fix forced final round fails the predicate.
  const std::string forced = "{\"model\":\"claude-sonnet-5-5\",\"max_tokens\":4096,\"tool_choice\":"
                             "{\"type\":\"tool\",\"name\":\"orch_turn\"},\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
  TEST_ASSERT_EQUAL_STRING("forced tool_choice", antRequestProblem(forced, "claude-sonnet-5-5").c_str());
}

// ---- 2. the output-cap table ----------------------------------------------------------

// A retry body differs from the first attempt's. The counter-test feeds it two
// identical bodies.
static bool retryDiffers(const std::string& a, const std::string& b) { return !a.empty() && a != b; }

struct CutCase {
  const char* host;
  const char* model;
  std::string cutReply;
  orch::HeadStepFn (*make)(const providers::ProviderDeps&, const std::string&, const agent::HeadTools&,
                           orch::TokenUsage*, orch::Transcript&);
};

static void runCutCase(const CutCase& c, bool allowTools) {
  const providers::RoundKind kind = allowTools ? providers::RoundKind::ToolRound : providers::RoundKind::FinalRound;
  const providers::OutputCap cap = providers::outputCapFor(c.host, c.model, kind, 60000);
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, c.cutReply});
  if (cap.hasRetry()) d.http.script.push_back({"", "", 200, c.cutReply});
  providers::ProviderDeps pd = d.contract();
  const std::string model = c.model;
  pd.orchModel = [model](const char*) { return model; };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  orch::Transcript tr;
  tr.addUser("write the long report");
  const std::string instr = "SYS";
  const orch::HeadStep st = c.make(pd, instr, rig.ht, nullptr, tr)(allowTools, {}, 60000, allowTools ? "" : "rounds");
  const std::string what = std::string(c.host) + "/" + c.model + (allowTools ? " tool" : " final");
  // The gate fired: not finished, nothing to dispatch, typed Cut.
  TEST_ASSERT_FALSE_MESSAGE(st.ok, what.c_str());
  TEST_ASSERT_FALSE_MESSAGE(st.finished, what.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)st.toolCalls.size(), what.c_str());
  TEST_ASSERT_EQUAL_MESSAGE((int)StepErr::Cut, (int)st.kind, what.c_str());
  TEST_ASSERT_FALSE_MESSAGE(st.cut.empty(), what.c_str());
  // The retry, when the table has one, sent a different body; never a second
  // identical one.
  TEST_ASSERT_EQUAL_MESSAGE(cap.hasRetry() ? 2 : 1, (int)d.http.seen.size(), what.c_str());
  if (cap.hasRetry()) TEST_ASSERT_TRUE_MESSAGE(retryDiffers(reqBody(d, 0), reqBody(d, 1)), what.c_str());
}

static void test_cap_table_gate_and_retry_every_adapter_kind_family() {
  const std::string misCut =
      "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"content\":\"\",\"tool_calls\":"
      "[{\"id\":\"abcdefghi\",\"function\":{\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"cut\"}}]}}]}";
  const std::string oaiCut =
      "{\"id\":\"r\",\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
      "\"output\":[{\"type\":\"function_call\",\"call_id\":\"c\",\"name\":\"orch_turn\",\"arguments\":\"{\\\"re\"}]}";
  std::vector<CutCase> cases;
  for (const char* id : kClaudeIds) cases.push_back({"anthropic", id, antCut(), providers::antLoopStep});
  for (const char* id : {"mistral-small-latest", "mistral-large-latest", "mistral-medium-3-5", "x"})
    cases.push_back({"mistral", id, misCut, providers::misLoopStep});
  for (const char* id : {"gpt-5.6-luna", "gpt-5.6", "gpt-4o"})
    cases.push_back({"openai", id, oaiCut, providers::oaiLoopStep});
  for (const CutCase& c : cases)
    for (bool allow : {true, false}) runCutCase(c, allow);
  // Counter-test: an identical retry fails the property.
  TEST_ASSERT_FALSE(retryDiffers("{\"max_tokens\":8192}", "{\"max_tokens\":8192}"));
  // Mistral also refuses a call whose arguments are cut even when finish_reason
  // says "tool_calls" (a cut argument string must never be dispatched).
  CutCase argsCut{"mistral", "mistral-small-latest",
                  "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"tool_calls\":[{\"id\":"
                  "\"abcdefghi\",\"function\":{\"name\":\"memory_search\",\"arguments\":\"{\\\"q\\\":\\\"tr\"}}]}}]}",
                  providers::misLoopStep};
  runCutCase(argsCut, true);
}

// The table itself: every (adapter, round kind) row, every family. Caps that are
// sent fit a thinking model and the round's time; a capped kind always has a
// retry that differs; the effort field rides only families that accept it.
static void test_cap_table_rows() {
  for (int k = 0; k < providers::kRoundKindCount; k++) {
    const providers::RoundKind kind = (providers::RoundKind)k;
    for (const char* id : kClaudeIds) {
      const providers::OutputCap c = providers::outputCapFor("anthropic", id, kind, 120000);
      TEST_ASSERT_TRUE(c.maxTokens > 3000);          // the old cap cut thinking models
      TEST_ASSERT_TRUE(c.maxTokens * 5 <= 65535);   // one string can never pass the ceiling
      TEST_ASSERT_EQUAL(providers::anthropicTakesEffort(id), *c.effort != 0);
      // Time: at the family's slowest rate the cap finishes inside the round.
      const uint32_t rate = providers::slowestTokensPerSecond("anthropic", id);
      TEST_ASSERT_TRUE((uint64_t)c.maxTokens * 1000 / rate <= 120000);
      if (c.hasRetry()) TEST_ASSERT_TRUE(c.retryTokens > c.maxTokens || strcmp(c.retryEffort, c.effort));
    }
    const providers::OutputCap m = providers::outputCapFor("mistral", "mistral-small-latest", kind, 120000);
    TEST_ASSERT_TRUE(m.maxTokens > 0 && m.hasRetry() && m.retryTokens > m.maxTokens);
    const providers::OutputCap o = providers::outputCapFor("openai", "gpt-5.6", kind, 120000);
    TEST_ASSERT_EQUAL(0, (int)o.maxTokens);   // reasoning would starve the answer
  }
  // Haiku 4.5 answers 400 to the effort field (recorded 18 Oct); current families take it.
  TEST_ASSERT_FALSE(providers::anthropicTakesEffort("claude-haiku-4-5"));
  TEST_ASSERT_TRUE(providers::anthropicTakesEffort("claude-haiku-5-5"));
  TEST_ASSERT_TRUE(providers::anthropicTakesEffort("claude-opus-4-5"));
  TEST_ASSERT_FALSE(providers::anthropicTakesEffort("claude-sonnet-4-5-20250929"));
  TEST_ASSERT_TRUE(providers::anthropicTakesEffort("anthropic/claude-sonnet-5-5"));
  // The bench seam: the first attempt's cap is overridden, the retry keeps headroom.
  providers::setTestFirstAttemptCap(256);
  const providers::OutputCap t = providers::outputCapFor("anthropic", "claude-haiku-5-5", providers::RoundKind::ToolRound);
  TEST_ASSERT_EQUAL(256, (int)t.maxTokens);
  TEST_ASSERT_TRUE(t.hasRetry() && t.retryTokens > 256);
  providers::setTestFirstAttemptCap(0);
}

// ---- 3. step errors typed from recorded answers ---------------------------------------

struct Recorded {
  const char* host;   // anthropic | openai | mistral
  int status;
  const char* body;
  StepErr want;
};
// Bodies recorded live on 18 Oct (lane K evidence kl3-*.json; the unknown-model
// bodies match lane N's errors.jsonl) and on Lumi (billing, 13-14 Oct).
static const Recorded kRecorded[] = {
    {"anthropic", 400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"The request body is not valid JSON: invalid unicode code point at line 1 column 127\"}}", StepErr::BadRequest},
    {"anthropic", 401, "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}", StepErr::Auth},
    {"anthropic", 404, "{\"type\":\"error\",\"error\":{\"type\":\"not_found_error\",\"message\":\"model: claude-haiku-0-0\"}}", StepErr::Rejected},
    {"anthropic", 400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"tool_choice: type \\\"tool\\\" and \\\"any\\\" are not supported for this model.\"}}", StepErr::Rejected},
    {"anthropic", 400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"This model does not support the effort parameter.\"}}", StepErr::Rejected},
    {"anthropic", 400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"Your credit balance is too low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits.\"}}", StepErr::Billing},
    {"anthropic", 429, "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"This request would exceed the rate limit of 50 requests per minute.\"}}", StepErr::RateLimited},
    {"anthropic", 529, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}", StepErr::ServerError},
    {"openai", 400, "{\"error\":{\"message\":\"Invalid body: encountered a unicode decode error when parsing this JSON value. Please check the value to ensure it is valid unicode.\",\"type\":\"invalid_request_error\",\"param\":null,\"code\":\"invalid_json\"}}", StepErr::BadRequest},
    {"openai", 401, "{\"error\":{\"message\":\"Incorrect API key provided.\",\"type\":\"invalid_request_error\",\"code\":\"invalid_api_key\"}}", StepErr::Auth},
    {"openai", 404, "{\"error\":{\"message\":\"The model `gpt-0.0-nope` does not exist or you do not have access to it.\",\"type\":\"invalid_request_error\",\"code\":\"model_not_found\"}}", StepErr::Rejected},
    {"openai", 429, "{\"error\":{\"message\":\"You have no credits remaining. Add credits to continue using the API.\",\"type\":\"insufficient_quota\",\"code\":\"insufficient_quota\"}}", StepErr::Billing},
    {"openai", 429, "{\"error\":{\"message\":\"Rate limit reached for gpt-5.6 on requests per min (RPM).\",\"type\":\"requests\",\"code\":\"rate_limit_exceeded\"}}", StepErr::RateLimited},
    {"openai", 500, "{\"error\":{\"message\":\"The server had an error while processing your request.\"}}", StepErr::ServerError},
    {"mistral", 400, "{\"detail\":\"There was an error parsing the body\"}", StepErr::BadRequest},
    {"mistral", 401, "{\"detail\":\"Invalid API Key\"}", StepErr::Auth},
    {"mistral", 400, "{\"object\":\"error\",\"message\":\"Invalid model: mistral-nope-latest\",\"type\":\"invalid_model\",\"param\":null,\"code\":\"1500\"}", StepErr::Rejected},
    {"mistral", 422, "{\"object\":\"error\",\"message\":{\"detail\":[{\"type\":\"enum\",\"msg\":\"Input should be 'auto', 'none', 'any' or 'required'\"}]},\"type\":\"invalid_request_error\"}", StepErr::Rejected},
    {"mistral", 402, "{\"detail\":\"Payment required\"}", StepErr::Billing},
    {"mistral", 429, "{\"message\":\"Requests rate limit exceeded\"}", StepErr::RateLimited},
};

static orch::HeadStep stepFor(const char* host, int status, const char* body) {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", status, body});
  providers::ProviderDeps pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  orch::Transcript tr;
  tr.addUser("hi");
  const std::string instr = "SYS";
  auto make = !strcmp(host, "anthropic") ? providers::antLoopStep
            : !strcmp(host, "openai")    ? providers::oaiLoopStep
                                         : providers::misLoopStep;
  return make(pd, instr, rig.ht, nullptr, tr)(true, {}, 60000, "");
}

static void test_step_errors_typed_from_recorded_answers() {
  for (const Recorded& r : kRecorded) {
    const orch::HeadStep st = stepFor(r.host, r.status, r.body);
    const std::string what = std::string(r.host) + " " + std::to_string(r.status) + " " + st.error;
    TEST_ASSERT_FALSE_MESSAGE(st.ok, what.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(orch::stepErrSlug(r.want), orch::stepErrSlug(st.kind), what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(r.status, st.httpStatus, what.c_str());
    // The text classifier (the legacy ladder, the engine's reply) agrees with
    // the adapter on the same error line.
    TEST_ASSERT_EQUAL_STRING_MESSAGE(orch::stepErrSlug(r.want), orch::stepErrSlug(orch::classifyStepError(st.error)),
                                     what.c_str());
  }
  // Provider text can never forge a second log line: control bytes become spaces.
  for (const char* host : {"anthropic", "openai", "mistral"}) {
    const orch::HeadStep st = stepFor(host, 400,
        "{\"message\":\"bad\\nfabric: forged line\",\"error\":{\"message\":\"bad\\nfabric: forged line\"}}");
    TEST_ASSERT_TRUE_MESSAGE(st.error.find('\n') == std::string::npos, host);
    TEST_ASSERT_TRUE_MESSAGE(st.error.find("bad fabric: forged line") != std::string::npos, host);
  }
  // Every kind has a frozen slug and a fabric rule.
  for (int k = 0; k < orch::kStepErrCount; k++) {
    TEST_ASSERT_NOT_NULL(orch::stepErrSlug((StepErr)k));
    TEST_ASSERT_TRUE(strlen(orch::stepErrSlug((StepErr)k)) > 0);
  }
  // Device-side lines.
  TEST_ASSERT_EQUAL((int)StepErr::LowMemory, (int)orch::classifyStepError("low memory: the device could not make the request"));
  TEST_ASSERT_EQUAL((int)StepErr::Timeout, (int)orch::classifyStepError("timeout: the provider did not answer in time"));
  TEST_ASSERT_EQUAL((int)StepErr::Transport, (int)orch::classifyStepError("network"));
  TEST_ASSERT_EQUAL((int)StepErr::BadRequest, (int)orch::classifyStepError("request text is not valid UTF-8: tool x (content)"));
  TEST_ASSERT_EQUAL((int)StepErr::Cut, (int)orch::classifyStepError("cut at the output limit (8192 tokens)"));
  // The old substring classifier read these as a rate limit / a server error.
  TEST_ASSERT_EQUAL((int)StepErr::Transport, (int)orch::classifyStepError("could not generate a temperature reading"));
  TEST_ASSERT_EQUAL((int)StepErr::Rejected, (int)orch::classifyStepError("messages HTTP 400: max_tokens 1500 too large"));
  // Every billing token is read as billing on a 4xx line.
  for (const char* t : orch::kBillingTokens)
    TEST_ASSERT_TRUE_MESSAGE(orch::isBillingRefusal(429, std::string("x ") + t), t);
}

// ---- 4. the fabric acts on the kind ---------------------------------------------------

struct FabricRow {
  int status;
  const char* body;
  int sameHostAttempts;   // requests to the first host
  bool switches;
  bool parks;
};

static void test_fabric_rule_table_request_counts() {
  const FabricRow rows[] = {
      {0, "", 2, true, false},                                                          // transport: retry, switch
      {503, "{\"error\":{\"message\":\"down\"}}", 2, true, false},                      // server error
      {429, "{\"error\":{\"message\":\"per minute\"}}", 2, true, false},                // rate limited
      {400, "{\"error\":{\"message\":\"The request body is not valid JSON: invalid unicode code point\"}}", 1, false, false},   // content 400: no switch, no memo
      {404, "{\"error\":{\"message\":\"model: claude-haiku-0-0\"}}", 1, true, false},   // unknown model: no retry, no memo
      {401, "{\"error\":{\"message\":\"invalid x-api-key\"}}", 1, true, true},          // auth: park
      {400, "{\"error\":{\"message\":\"Your credit balance is too low to access the Anthropic API.\"}}", 1, true, true},  // billing: park
  };
  for (const FabricRow& row : rows) {
    providers::clearHostParks();
    FakeProviderDeps d;
    for (int i = 0; i < row.sameHostAttempts; i++)
      d.http.script.push_back({"api.anthropic.com", "", row.status, row.body});
    if (row.switches) d.http.script.push_back({"api.openai.com", "", 200, oaiFinal()});
    providers::ProviderDeps pd = d.contract();
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    int notifies = 0;
    std::string out, err;
    const bool ok = providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr,
                                             [&](const std::string&, const std::string&) { notifies++; });
    const std::string what = std::to_string(row.status) + " " + row.body;
    TEST_ASSERT_EQUAL_MESSAGE(row.switches, ok, what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(row.sameHostAttempts + (row.switches ? 1 : 0), (int)d.http.seen.size(), what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(row.switches ? 1 : 0, notifies, what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(row.parks, providers::hostParked(pd, "anthropic"), what.c_str());
  }
}

// A billing refusal parks the host: the next turn starts on the next host with
// no request sent to the parked one; a key change un-parks it; a success there
// ends the episode.
static void test_billing_park_skips_later_turns_until_the_key_changes() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "", 400,
      "{\"error\":{\"message\":\"Your credit balance is too low to access the Anthropic API.\"}}"});
  d.http.script.push_back({"api.openai.com", "", 200, oaiFinal()});
  providers::ProviderDeps pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string out, err;
  TEST_ASSERT_TRUE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr, nullptr));
  TEST_ASSERT_EQUAL((int)StepErr::Billing, (int)providers::lastFabricSwitch().kind);
  TEST_ASSERT_EQUAL(400, providers::lastFabricSwitch().httpStatus);
  // Turn 2: straight to openai, not one request to anthropic; the switch record
  // says it was a parked start (no new notice).
  d.http.script.push_back({"api.openai.com", "", 200, oaiFinal()});
  int notifies = 0;
  TEST_ASSERT_TRUE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr,
                                            [&](const std::string&, const std::string&) { notifies++; }));
  TEST_ASSERT_EQUAL(3, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("api.openai.com", d.http.seen[2].host.c_str());
  TEST_ASSERT_EQUAL(1, notifies);
  TEST_ASSERT_TRUE(providers::lastFabricSwitch().parkedStart);
  // One owner notice per episode, shared with the embedding seam.
  TEST_ASSERT_TRUE(orch::billingNoticeDue("anthropic"));
  TEST_ASSERT_FALSE(orch::billingNoticeDue("anthropic"));
  // The key changes: un-parked, and the next turn starts on anthropic again.
  d.keys["anthropic"] = "sk-new-ant";
  TEST_ASSERT_FALSE(providers::hostParked(pd, "anthropic"));
  TEST_ASSERT_TRUE(orch::billingNoticeDue("anthropic"));   // a new episode
}

// The probe after kParkProbeMs: a failure of ANY kind re-arms the park (a 503 on
// the probe used to leave the host un-parked for the rest of the boot); a success
// ends it.
static void test_park_probe_failure_rearms_success_ends() {
  FakeProviderDeps d;
  providers::ProviderDeps pd = d.contract();
  providers::parkHost(pd, "anthropic", StepErr::Billing, 400);
  TEST_ASSERT_TRUE(providers::hostParked(pd, "anthropic"));
  d.now += providers::kParkProbeMs;
  // The probe turn: anthropic answers 503 twice (one same-host retry), openai serves.
  d.http.script.push_back({"api.anthropic.com", "", 503, "{\"error\":{\"message\":\"down\"}}"});
  d.http.script.push_back({"api.anthropic.com", "", 503, "{\"error\":{\"message\":\"down\"}}"});
  d.http.script.push_back({"api.openai.com", "", 200, oaiFinal()});
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht,
                                                    nullptr, nullptr), err.c_str());
  TEST_ASSERT_EQUAL(3, (int)d.http.seen.size());   // the probe went out (and its retry)
  TEST_ASSERT_TRUE_MESSAGE(providers::hostParked(pd, "anthropic"), "a failed probe must re-arm the park");
  // The next turn starts past it again: no request to anthropic.
  d.http.script.push_back({"api.openai.com", "", 200, oaiFinal()});
  TEST_ASSERT_TRUE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr, nullptr));
  TEST_ASSERT_EQUAL_STRING("api.openai.com", d.http.seen.back().host.c_str());
  // A successful probe ends the episode.
  d.now += providers::kParkProbeMs;
  d.http.script.push_back({"api.anthropic.com", "", 200, antFinal()});
  TEST_ASSERT_TRUE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr, nullptr));
  TEST_ASSERT_FALSE(providers::hostParked(pd, "anthropic"));
}

// CUM-404 v2 counter-test: a status-0 write failure with NO alloc-failure evidence
// on a board whose largest block sits at 5,200 B (a fragmented but healthy Freenove)
// is a network failure that still fails over - never a fake low-memory.
static void test_cum404_v2_status0_without_evidence_still_fails_over() {
  FakeProviderDeps d;
  d.largest = 5200;
  Exchange unsent;
  unsent.expectHost = "api.anthropic.com";
  unsent.status = 0;
  unsent.err = "request body truncated (0/71508 bytes written)";
  d.http.script.push_back(unsent);
  d.http.script.push_back(unsent);   // the one resend of a request that never left
  d.http.script.push_back({"api.openai.com", "", 200, oaiFinal()});
  providers::ProviderDeps pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  int notifies = 0;
  std::string out, err;
  TEST_ASSERT_TRUE_MESSAGE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr,
                                                    [&](const std::string&, const std::string&) { notifies++; }),
                           err.c_str());
  TEST_ASSERT_EQUAL(1, notifies);
  TEST_ASSERT_EQUAL((int)StepErr::Transport, (int)providers::lastFabricSwitch().kind);
}

// A timeout (the wait used the whole round) switches at once, no same-host retry;
// a fast "no response" keeps its one retry.
static void test_timeout_switches_without_retry_fast_cut_retries() {
  {
    FakeProviderDeps d;
    d.http.script.push_back(harness_test::transportCut(120000, "no response"));
    d.http.script.push_back({"api.openai.com", "", 200, oaiFinal()});
    providers::ProviderDeps pd = d.contract();
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    std::string out, err;
    TEST_ASSERT_TRUE(providers::runFabricLoop(pd, {"mistral", "openai"}, "S", "U", out, err, rig.ht, nullptr, nullptr));
    TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
    TEST_ASSERT_EQUAL((int)StepErr::Timeout, (int)providers::lastFabricSwitch().kind);
    TEST_ASSERT_EQUAL_STRING("mistral did not answer in time",
                             orch::stepErrCause(StepErr::Timeout, "mistral", 0).c_str());
  }
  {
    FakeProviderDeps d;
    d.http.script.push_back(harness_test::transportCut(50, "no response"));
    d.http.script.push_back(harness_test::transportCut(50, "no response"));
    d.http.script.push_back({"api.openai.com", "", 200, oaiFinal()});
    providers::ProviderDeps pd = d.contract();
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    std::string out, err;
    TEST_ASSERT_TRUE(providers::runFabricLoop(pd, {"mistral", "openai"}, "S", "U", out, err, rig.ht, nullptr, nullptr));
    TEST_ASSERT_EQUAL(3, (int)d.http.seen.size());   // the fast cut kept its one retry
  }
}

// ---- 5. the one time policy -----------------------------------------------------------

static void test_every_request_deadline_comes_from_the_policy() {
  for (uint32_t budget : {0u, 5000u, 60000u, 119999u, 120000u, 600000u, UINT32_MAX}) {
    const uint32_t want = budget >= providers::kHeadRoundCeilingMs ? providers::kHeadRoundCeilingMs
                        : budget < providers::kMinRoundMs ? providers::kMinRoundMs : budget;
    for (const char* host : {"anthropic", "openai", "mistral"}) {
      FakeProviderDeps d;
      d.http.script.push_back({"", "", 200, !strcmp(host, "anthropic") ? antFinal()
                                           : !strcmp(host, "openai")    ? oaiFinal() : misFinal()});
      providers::ProviderDeps pd = d.contract();
      FakeProviderDeps::ToolRig rig;
      d.fillTools(rig);
      orch::Transcript tr;
      tr.addUser("hi");
      const std::string instr = "S";
      auto make = !strcmp(host, "anthropic") ? providers::antLoopStep
                : !strcmp(host, "openai")    ? providers::oaiLoopStep : providers::misLoopStep;
      make(pd, instr, rig.ht, nullptr, tr)(true, {}, budget, "");
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, d.http.seen[0].timeoutMs, host);
    }
  }
  // The single shots get the head ceiling (Mistral used to get 60 s).
  for (const char* host : {"anthropic", "openai", "mistral"}) {
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 0, ""});
    providers::ProviderDeps pd = d.contract();
    std::string conv, out, err;
    if (!strcmp(host, "anthropic")) providers::orchTurnAnthropic(pd, conv, "S", "U", out, err, nullptr, nullptr);
    else if (!strcmp(host, "openai")) providers::orchTurnOpenAI(pd, conv, "S", "U", out, err, nullptr, nullptr);
    else providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(providers::kHeadRoundCeilingMs, d.http.seen[0].timeoutMs, host);
  }
  // The engine's stuck-turn reaper (loop deadline + 120 s) must outlast the
  // largest single request ceiling.
  static_assert(120000u >= providers::kHeadRoundCeilingMs, "reaper slack below the head ceiling");
  static_assert(120000u >= providers::kSyncSubCeilingMs, "reaper slack below the sync-sub ceiling");
}

// ---- 6. a 2xx that did not arrive whole -----------------------------------------------

static void test_partial_200_is_a_transport_failure_every_provider() {
  struct P { const char* host; const char* partial; };
  const P ps[] = {
      {"anthropic", "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"do"},
      {"openai", "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\""},
      {"mistral", "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"tool_calls\":["},
  };
  for (const P& p : ps) {
    const orch::HeadStep st = stepFor(p.host, 200, p.partial);
    TEST_ASSERT_FALSE_MESSAGE(st.ok, p.host);   // never an empty "stalled" answer
    TEST_ASSERT_FALSE_MESSAGE(st.finished, p.host);
    TEST_ASSERT_TRUE_MESSAGE(st.kind == StepErr::Transport || st.kind == StepErr::Timeout, p.host);
  }
  // Counter-test: an error status with an unparseable body keeps its code.
  const orch::HeadStep st = stepFor("anthropic", 502, "<html>Bad gateway</html>");
  TEST_ASSERT_EQUAL(502, st.httpStatus);
  TEST_ASSERT_EQUAL((int)StepErr::ServerError, (int)st.kind);
}

// ---- 7. Mistral error text from every body shape, both paths --------------------------

static void test_mistral_reason_text_every_shape_both_paths() {
  struct Shape { int status; const char* body; const char* want; };
  const Shape shapes[] = {
      {400, "{\"detail\":\"There was an error parsing the body\"}", "There was an error parsing the body"},
      {400, "{\"object\":\"error\",\"message\":\"Invalid model: x\",\"type\":\"invalid_model\"}", "Invalid model: x"},
      {422, "{\"object\":\"error\",\"message\":{\"detail\":[{\"msg\":\"Input should be 'auto'\"}]}}", "Input should be 'auto'"},
      {422, "{\"detail\":[{\"msg\":\"Field required\"}]}", "Field required"},
      {400, "{\"error\":{\"message\":\"proxy says no\"}}", "proxy says no"},
  };
  for (const Shape& sh : shapes) {
    const orch::HeadStep st = stepFor("mistral", sh.status, sh.body);
    TEST_ASSERT_TRUE_MESSAGE(st.error.find(sh.want) != std::string::npos, (std::string("loop: ") + st.error).c_str());
    FakeProviderDeps d;
    d.toolLoop = false;
    d.http.script.push_back({"", "", sh.status, sh.body});
    providers::ProviderDeps pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_TRUE_MESSAGE(err.find(sh.want) != std::string::npos, (std::string("single: ") + err).c_str());
  }
}

// ---- 8. sub-agent provenance from the providers' own records --------------------------

static void test_sub_provenance_counts_per_provider() {
  {   // Mistral: tool.execution entries
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"outputs\":[{\"type\":\"tool.execution\",\"name\":\"web_search\"},"
        "{\"type\":\"tool.execution\",\"name\":\"code_interpreter\"},"
        "{\"type\":\"message.output\",\"content\":\"done\"}]}"});
    providers::ProviderDeps pd = d.contract();
    agent::Directive dir;
    dir.instruction = "go";
    char job[72] = {};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::mistralDispatch(pd, "mistral-small-latest", dir, job));
    orch::ResultEnvelope env;
    providers::mistralPoll(pd, job, env);
    TEST_ASSERT_EQUAL(2, env.toolRuns);
    TEST_ASSERT_EQUAL(1, env.webRuns);
  }
  {   // OpenAI: output items ending in _call
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"status\":\"completed\",\"output\":[{\"type\":\"web_search_call\"},{\"type\":\"web_search_call\"},"
        "{\"type\":\"mcp_call\"},{\"type\":\"reasoning\"},{\"type\":\"message\",\"content\":[{\"type\":\"output_text\","
        "\"text\":\"done\"}]}]}"});
    providers::ProviderDeps pd = d.contract();
    orch::ResultEnvelope env;
    providers::oaiPoll(pd, "api.openai.com", "k", "openai", "openai:r1", env);
    TEST_ASSERT_EQUAL(3, env.toolRuns);
    TEST_ASSERT_EQUAL(2, env.webRuns);
  }
  {   // Anthropic: agent.*tool_use events (and the poll asks for them)
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"data\":[{\"type\":\"agent.tool_use\",\"name\":\"web_search\"},{\"type\":\"agent.tool_use\",\"name\":\"bash\"},"
        "{\"type\":\"agent.mcp_tool_use\",\"name\":\"notion_search\"},{\"type\":\"agent.message\",\"content\":"
        "[{\"type\":\"text\",\"text\":\"done\"}]},{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
    providers::ProviderDeps pd = d.contract();
    orch::ResultEnvelope env;
    providers::antPoll(pd, "anthropic:s1", env);
    TEST_ASSERT_EQUAL(3, env.toolRuns);
    TEST_ASSERT_EQUAL(1, env.webRuns);
    TEST_ASSERT_TRUE(d.http.seen[0].path.find("types[]=agent.tool_use") != std::string::npos);
    TEST_ASSERT_TRUE(d.http.seen[0].path.find("types[]=agent.mcp_tool_use") != std::string::npos);
    TEST_ASSERT_TRUE(d.http.seen[0].path.find("types[]=agent.custom_tool_use") != std::string::npos);
  }
}

// The bench seam: a scripted provider answer replaces the network for one API
// host (nothing is sent), so the device's billing path runs on demand.
static void test_test_status_override_answers_locally() {
  FakeProviderDeps d;   // no script: any real request would fail loudly
  providers::setTestStatusOverride("api.anthropic.com", 400,
      "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"Your credit balance is too low to access the Anthropic API.\"}}");
  const orch::HeadStep st = stepFor("anthropic", 200, "unused");
  providers::clearTestStatusOverride();
  TEST_ASSERT_EQUAL((int)StepErr::Billing, (int)st.kind);
  TEST_ASSERT_EQUAL(400, st.httpStatus);
  TEST_ASSERT_TRUE(LogCapture::contains("TEST override answered api.anthropic.com/v1/messages with HTTP 400"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_tool_choice_never_forced_any_model_any_path);
  RUN_TEST(test_cap_table_gate_and_retry_every_adapter_kind_family);
  RUN_TEST(test_cap_table_rows);
  RUN_TEST(test_step_errors_typed_from_recorded_answers);
  RUN_TEST(test_fabric_rule_table_request_counts);
  RUN_TEST(test_billing_park_skips_later_turns_until_the_key_changes);
  RUN_TEST(test_park_probe_failure_rearms_success_ends);
  RUN_TEST(test_cum404_v2_status0_without_evidence_still_fails_over);
  RUN_TEST(test_timeout_switches_without_retry_fast_cut_retries);
  RUN_TEST(test_every_request_deadline_comes_from_the_policy);
  RUN_TEST(test_partial_200_is_a_transport_failure_every_provider);
  RUN_TEST(test_mistral_reason_text_every_shape_both_paths);
  RUN_TEST(test_sub_provenance_counts_per_provider);
  RUN_TEST(test_test_status_override_answers_locally);
  return UNITY_END();
}
