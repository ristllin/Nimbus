// The scripted provider (test/support/scripted_provider), lane RB-B0a.
//
// What it proves: the answers it renders are read correctly by the production parsers of
// every dialect (the old engine's Anthropic, OpenAI Responses and Mistral chat loop
// steps), so a scripted turn exercises the same parsing a live one does; a request is
// read back into one view per dialect; the prefix and pairing properties catch the
// faults they exist for (each has its counter-case); and running out of script is loud.
#include <unity.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../support/fake_provider_deps.h"
#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/transcript.h"

using harness_test::FakeProviderDeps;
namespace sp = harness_test::scripted;
namespace providers = agent::providers;

void setUp() { providers::clearHostParks(); }
void tearDown() {}

namespace {

using Maker = nimbus::orch::HeadStepFn (*)(const providers::ProviderDeps&, const std::string&,
                                           const agent::HeadTools&, nimbus::orch::TokenUsage*,
                                           nimbus::orch::Transcript&);
struct Head {
  const char* name;
  Maker make;
  sp::Dialect dialect;
};
const Head kHeads[] = {
    {"anthropic", providers::antLoopStep, sp::Dialect::Anthropic},
    {"openai", providers::oaiLoopStep, sp::Dialect::Responses},
    {"mistral", providers::misLoopStep, sp::Dialect::Chat},
};

// One loop step of `head` answered by `model`.
struct Run {
  nimbus::orch::HeadStep step;
  nimbus::orch::TokenUsage usage;
};
Run stepOn(const Head& head, sp::ScriptedProvider& model, const char* user = "hi") {
  FakeProviderDeps d;
  auto pd = d.contract();
  pd.http = &model;
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  nimbus::orch::Transcript tr;
  tr.addUser(user);
  Run r;
  r.step = head.make(pd, "SYS", rig.ht, &r.usage, tr)(true, {}, 60000, "");
  return r;
}

bool hasTool(const sp::RequestView& v, const char* name) {
  for (const std::string& t : v.tools)
    if (t == name) return true;
  return false;
}

bool userSaid(const sp::RequestView& v, const char* needle) {
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks)
      if (m.role == "user" && b.kind == sp::BlockKind::Text && b.text.find(needle) != std::string::npos) return true;
  return false;
}

}  // namespace

// A scripted tool call is parsed by every production loop step: name, arguments, usage.
static void test_tool_call_is_parsed_by_every_dialect() {
  for (const Head& h : kHeads) {
    sp::ScriptedProvider model;
    sp::Step s = sp::call("memory_search", "{\"q\":\"boat\"}");
    s.usage.output = 33;
    model.push(s);
    const Run r = stepOn(h, model);
    TEST_ASSERT_TRUE_MESSAGE(r.step.ok, (std::string(h.name) + ": " + r.step.error).c_str());
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.step.toolCalls.size(), h.name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("memory_search", r.step.toolCalls[0].name.c_str(), h.name);
    TEST_ASSERT_TRUE_MESSAGE(r.step.toolCalls[0].argsJson.find("boat") != std::string::npos, h.name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(33, r.usage.completionTokens, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)model.requests().size(), h.name);
    const sp::Recorded& rec = model.last();
    TEST_ASSERT_TRUE_MESSAGE(rec.dialect == h.dialect, h.name);
    TEST_ASSERT_TRUE_MESSAGE(rec.view.parsed, h.name);
    TEST_ASSERT_TRUE_MESSAGE(hasTool(rec.view, "memory_search"), h.name);
    TEST_ASSERT_TRUE_MESSAGE(rec.view.system().find("SYS") != std::string::npos, h.name);
    TEST_ASSERT_TRUE_MESSAGE(userSaid(rec.view, "hi"), h.name);
    TEST_ASSERT_TRUE_MESSAGE(model.allUsed() && model.failures().empty(), h.name);
  }
}

// The Anthropic cache split reaches the usage the loop reports.
static void test_anthropic_cache_split_is_parsed() {
  sp::ScriptedProvider model;
  sp::Step s = sp::call("memory_search", "{}");
  s.usage.cacheRead = 700;
  s.usage.cacheWrite = 50;
  model.push(s);
  const Run r = stepOn(kHeads[0], model);
  TEST_ASSERT_TRUE_MESSAGE(r.step.ok, r.step.error.c_str());
  TEST_ASSERT_EQUAL_UINT32(700, r.usage.cacheReadTokens);
  TEST_ASSERT_EQUAL_UINT32(50, r.usage.cacheWriteTokens);
}

// A cut answer reads as cut on every dialect: never finished, its calls never dispatched.
static void test_cut_answer_reads_as_cut_on_every_dialect() {
  for (const Head& h : kHeads) {
    sp::ScriptedProvider model;
    sp::Step s = sp::cut("I was about to");
    s.calls.push_back({"memory_search", "{\"q\":\"bo", ""});
    model.push(s);
    const Run r = stepOn(h, model);
    TEST_ASSERT_FALSE_MESSAGE(r.step.ok, h.name);
    TEST_ASSERT_FALSE_MESSAGE(r.step.cut.empty(), h.name);
    TEST_ASSERT_FALSE_MESSAGE(r.step.finished, h.name);
  }
}

// An HTTP error reaches the loop as that status, in the dialect's own error shape.
static void test_http_error_reaches_the_loop() {
  for (const Head& h : kHeads) {
    sp::ScriptedProvider model;
    model.push(sp::httpError(400));
    const Run r = stepOn(h, model);
    TEST_ASSERT_FALSE_MESSAGE(r.step.ok, h.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(400, r.step.httpStatus, h.name);
  }
}

// Running out of script is a transport failure and a recorded failure, never an answer.
static void test_exhausted_script_is_loud() {
  sp::ScriptedProvider model;
  agent::HttpRequest req;
  req.host = "api.anthropic.com";
  req.path = "/v1/messages";
  req.body = "{}";
  agent::HttpResponse out;
  std::string err;
  TEST_ASSERT_FALSE(model.exec(req, out, err));
  TEST_ASSERT_EQUAL_INT(0, out.status);
  TEST_ASSERT_TRUE(err.find("script exhausted") != std::string::npos);
  TEST_ASSERT_EQUAL(1, (int)model.failures().size());
  TEST_ASSERT_EQUAL(1, (int)model.requests().size());
}

// A responder answers from the request once the pushed steps run out; a transport cut
// takes its time on the rig clock.
static void test_responder_and_clock() {
  sp::ScriptedProvider model;
  uint32_t now = 0;
  model.clock = &now;
  model.push(sp::transportCut(1500));
  model.respondWith([](const sp::Recorded& rec, size_t i) {
    return sp::say("answer " + std::to_string(i) + " to " + rec.view.model);
  });
  agent::HttpRequest req;
  req.host = "api.anthropic.com";
  req.path = "/v1/messages";
  req.body = "{\"model\":\"claude-haiku-5-5\",\"messages\":[]}";
  agent::HttpResponse out;
  std::string err;
  TEST_ASSERT_FALSE(model.exec(req, out, err));
  TEST_ASSERT_EQUAL_UINT32(1500, now);
  TEST_ASSERT_TRUE(model.exec(req, out, err));
  TEST_ASSERT_EQUAL_INT(200, out.status);
  TEST_ASSERT_TRUE(out.body.find("answer 1 to claude-haiku-5-5") != std::string::npos);
}

// The JSON script format (shared with scripted_provider.py) loads the fixture both arms use.
static void test_json_script_loads() {
  std::string path = __FILE__;
  path = path.substr(0, path.rfind('/')) + "/../support/scripted_provider/example_script.json";
  std::ifstream f(path);
  TEST_ASSERT_TRUE_MESSAGE(f.good(), path.c_str());
  std::stringstream ss;
  ss << f.rdbuf();
  sp::ScriptedProvider model(sp::Dialect::Chat);
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(model.load(ss.str(), err), err.c_str());
  TEST_ASSERT_EQUAL(2, (int)model.remaining());
  // The script names anthropic, so a chat path still gets Anthropic answers.
  const Run r = stepOn(kHeads[0], model);
  TEST_ASSERT_TRUE_MESSAGE(r.step.ok, r.step.error.c_str());
  TEST_ASSERT_EQUAL_STRING("read_file", r.step.toolCalls[0].name.c_str());
  TEST_ASSERT_EQUAL_UINT32(900, r.usage.cacheReadTokens);
  // Malformed scripts are refused with a reason.
  std::vector<sp::Step> steps;
  sp::Dialect d = sp::Dialect::Chat;
  TEST_ASSERT_FALSE(sp::loadScript("{\"steps\":[{\"calls\":[{}]}]}", d, steps, err));
  TEST_ASSERT_TRUE(err.find("no name") != std::string::npos);
  TEST_ASSERT_FALSE(sp::loadScript("{\"dialect\":\"smoke\",\"steps\":[]}", d, steps, err));
  TEST_ASSERT_FALSE(sp::loadScript("[", d, steps, err));
}

// ---- the request view, over every dialect -----------------------------------------

// The same conversation in each dialect's request shape: system, a user turn, a call and
// its result. Every view reads it the same way.
static const char* conversationIn(sp::Dialect d) {
  switch (d) {
    case sp::Dialect::Anthropic:
      return "{\"model\":\"m\",\"max_tokens\":900,\"system\":[{\"type\":\"text\",\"text\":\"S\","
             "\"cache_control\":{\"type\":\"ephemeral\"}}],\"tools\":[{\"name\":\"read_file\"}],"
             "\"tool_choice\":{\"type\":\"auto\"},\"messages\":[{\"role\":\"user\",\"content\":\"hi\"},"
             "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"read_file\","
             "\"input\":{\"path\":\"a/b\"}}]},{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\","
             "\"tool_use_id\":\"c1\",\"content\":\"ok\"}]}]}";
    case sp::Dialect::Responses:
      return "{\"model\":\"m\",\"max_output_tokens\":900,\"instructions\":\"S\",\"tools\":[{\"type\":\"function\","
             "\"name\":\"read_file\"}],\"tool_choice\":\"auto\",\"input\":[{\"role\":\"user\",\"content\":[{\"type\":"
             "\"input_text\",\"text\":\"hi\"}]},{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"read_file\","
             "\"arguments\":\"{\\\"path\\\":\\\"a/b\\\"}\"},{\"type\":\"function_call_output\",\"call_id\":\"c1\","
             "\"output\":\"ok\"}]}";
    case sp::Dialect::Chat:
      return "{\"model\":\"m\",\"max_tokens\":900,\"tools\":[{\"type\":\"function\",\"function\":{\"name\":"
             "\"read_file\"}}],\"tool_choice\":\"auto\",\"messages\":[{\"role\":\"system\",\"content\":\"S\"},"
             "{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
             "\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":"
             "\"{\\\"path\\\":\\\"a/b\\\"}\"}}]},{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"ok\"}]}";
  }
  return "";
}

static void test_request_view_reads_every_dialect_alike() {
  for (sp::Dialect d : {sp::Dialect::Anthropic, sp::Dialect::Responses, sp::Dialect::Chat}) {
    sp::RequestView v;
    TEST_ASSERT_TRUE_MESSAGE(sp::parseRequest(d, conversationIn(d), v), sp::dialectName(d));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("S", v.system().c_str(), sp::dialectName(d));
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)v.tools.size(), sp::dialectName(d));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("auto", v.toolChoice.c_str(), sp::dialectName(d));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(900, v.maxTokens, sp::dialectName(d));
    TEST_ASSERT_TRUE_MESSAGE(userSaid(v, "hi"), sp::dialectName(d));
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(sp::callsPaired(v, why), why.c_str());
    size_t callBlocks = 0, resultBlocks = 0;
    for (const sp::Message& m : v.messages)
      for (const sp::Block& b : m.blocks) {
        if (b.kind == sp::BlockKind::ToolCall) {
          ++callBlocks;
          TEST_ASSERT_TRUE(b.args.find("a/b") != std::string::npos);
        }
        if (b.kind == sp::BlockKind::ToolResult) {
          ++resultBlocks;
          TEST_ASSERT_EQUAL_STRING("ok", b.text.c_str());
        }
      }
    TEST_ASSERT_EQUAL(1, (int)callBlocks);
    TEST_ASSERT_EQUAL(1, (int)resultBlocks);
  }
  sp::RequestView v;
  TEST_ASSERT_TRUE(sp::parseRequest(sp::Dialect::Anthropic, conversationIn(sp::Dialect::Anthropic), v));
  TEST_ASSERT_EQUAL(1, (int)v.cacheMarks);
  TEST_ASSERT_FALSE(sp::parseRequest(sp::Dialect::Chat, "{not json", v));
  TEST_ASSERT_FALSE(v.parsed);
}

// ---- the properties and their counter-cases ---------------------------------------

static sp::Block textB(const char* t) {
  sp::Block b;
  b.kind = sp::BlockKind::Text;
  b.text = t;
  return b;
}
static sp::Block callB(const char* id) {
  sp::Block b;
  b.kind = sp::BlockKind::ToolCall;
  b.id = id;
  b.name = "read_file";
  return b;
}
static sp::Block resultB(const char* id) {
  sp::Block b;
  b.kind = sp::BlockKind::ToolResult;
  b.id = id;
  b.text = "ok";
  return b;
}
static sp::RequestView viewOf(std::vector<sp::Message> ms) {
  sp::RequestView v;
  v.parsed = true;
  v.systemBlocks = {"S"};
  v.tools = {"read_file"};
  v.messages = std::move(ms);
  return v;
}

static void test_prefix_property_and_its_counter_cases() {
  const sp::RequestView a = viewOf({{"user", {textB("hi")}}, {"assistant", {callB("c1")}}, {"user", {resultB("c1")}}});
  sp::RequestView b = a;
  b.messages.push_back({"assistant", {textB("done")}});
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(sp::isPrefix(a, b, why), why.c_str());
  sp::RequestView edited = b;   // an earlier message rewritten: the fault the property exists for
  edited.messages[0].blocks[0].text = "hi (edited)";
  TEST_ASSERT_FALSE(sp::isPrefix(a, edited, why));
  TEST_ASSERT_EQUAL_STRING("message 0 changed", why.c_str());
  sp::RequestView dropped = b;   // a history item dropped
  dropped.messages.erase(dropped.messages.begin() + 1);
  TEST_ASSERT_FALSE(sp::isPrefix(a, dropped, why));
  sp::RequestView sys = b;
  sys.systemBlocks[0] = "S2";
  TEST_ASSERT_FALSE(sp::isPrefix(a, sys, why));
  TEST_ASSERT_EQUAL_STRING("system changed", why.c_str());
  sp::RequestView tools = b;
  tools.tools.push_back("write_file");
  TEST_ASSERT_FALSE(sp::isPrefix(a, tools, why));
  TEST_ASSERT_EQUAL(2u, sp::commonPrefixBytes("abcd", "abXd"));
}

static void test_pairing_property_and_its_counter_cases() {
  std::string why;
  // Two calls in one round, both answered: paired.
  TEST_ASSERT_TRUE(sp::callsPaired(viewOf({{"user", {textB("hi")}},
                                           {"assistant", {textB("looking"), callB("c1"), callB("c2")}},
                                           {"user", {resultB("c1"), resultB("c2")}}}),
                                   why));
  // A call never answered.
  TEST_ASSERT_FALSE(sp::callsPaired(viewOf({{"user", {textB("hi")}}, {"assistant", {callB("c1")}}}), why));
  // A call answered only after the next assistant message.
  TEST_ASSERT_FALSE(sp::callsPaired(
      viewOf({{"assistant", {callB("c1")}}, {"assistant", {textB("x")}}, {"user", {resultB("c1")}}}), why));
  // A result for a call that was never made.
  TEST_ASSERT_FALSE(sp::callsPaired(viewOf({{"user", {resultB("c9")}}}), why));
  TEST_ASSERT_TRUE(why.find("answers no call") != std::string::npos);
  // A call answered twice, and a call id reused.
  TEST_ASSERT_FALSE(
      sp::callsPaired(viewOf({{"assistant", {callB("c1")}}, {"user", {resultB("c1"), resultB("c1")}}}), why));
  TEST_ASSERT_FALSE(sp::callsPaired(viewOf({{"assistant", {callB("c1")}},
                                            {"user", {resultB("c1")}},
                                            {"assistant", {callB("c1")}},
                                            {"user", {resultB("c1")}}}),
                                    why));
}

// A whole scripted round trip over a recorded multi-round conversation: the provider's
// own prefix and pairing checks pass on a well-formed loop and fail on a broken one.
static void test_provider_checks_recorded_rounds() {
  sp::ScriptedProvider model(sp::Dialect::Chat);
  model.push(sp::say("one")).push(sp::say("two"));
  agent::HttpRequest req;
  req.host = "api.mistral.ai";
  req.path = "/v1/chat/completions";
  req.body = "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}]}";
  agent::HttpResponse out;
  std::string err;
  TEST_ASSERT_TRUE(model.exec(req, out, err));
  req.body = "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"},{\"role\":\"assistant\",\"content\":\"one\"}]}";
  TEST_ASSERT_TRUE(model.exec(req, out, err));
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(model.allPrefixStable(why), why.c_str());
  TEST_ASSERT_TRUE_MESSAGE(model.allPaired(why), why.c_str());
  sp::ScriptedProvider broken(sp::Dialect::Chat);
  broken.push(sp::say("one")).push(sp::say("two"));
  req.body = "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}]}";
  TEST_ASSERT_TRUE(broken.exec(req, out, err));
  req.body = "{\"messages\":[{\"role\":\"user\",\"content\":\"b\"}]}";
  TEST_ASSERT_TRUE(broken.exec(req, out, err));
  TEST_ASSERT_FALSE(broken.allPrefixStable(why));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_tool_call_is_parsed_by_every_dialect);
  RUN_TEST(test_anthropic_cache_split_is_parsed);
  RUN_TEST(test_cut_answer_reads_as_cut_on_every_dialect);
  RUN_TEST(test_http_error_reaches_the_loop);
  RUN_TEST(test_exhausted_script_is_loud);
  RUN_TEST(test_responder_and_clock);
  RUN_TEST(test_json_script_loads);
  RUN_TEST(test_request_view_reads_every_dialect_alike);
  RUN_TEST(test_prefix_property_and_its_counter_cases);
  RUN_TEST(test_pairing_property_and_its_counter_cases);
  RUN_TEST(test_provider_checks_recorded_rounds);
  return UNITY_END();
}
