// test_plain_wire_openai - the OpenAI Responses codec of engine 2 (lane E2-R7; router design sections
// 4, 5 item 9 and 8). The request goldens and the parse goldens of the codec live in test_route_goldens
// (responses-openai-direct, responses-openai-routed); this suite holds what a golden does not say by
// itself: the stop table over every synthetic answer, the two ids of a call, a reasoning item stored
// and replayed byte for byte, the rows deciding include, breakpoints and replay, and every request of
// the battery's stored day on both OpenAI rows passing the documented-shape checker.
#include <unity.h>

#include <string>
#include <vector>

#include "../support/scripted_provider/wire_fixture.h"
#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/render.h"
#include "../support/tx_battery/rows.h"
#include "../support/wire_shape/responses_shape.h"
#include "nimbus/harness/plain/route.h"
#include "nimbus/harness/plain/wire.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace caps = nimbus::orch::caps;

void setUp() {}
void tearDown() {}

namespace {

const plain::WireCodec& codec() {
  const plain::WireCodec* w = nullptr;
  const tx::Status st = plain::wireFor(plain::Dialect::OpenAIResponses, w);
  if (!st.ok() || !w) TEST_FAIL_MESSAGE("wireFor(responses) gives no codec");
  return *w;
}

tx::Status parse(const std::string& body, plain::ParsedStep& step, int status = 200) {
  agent::HttpResponse r;
  r.status = status;
  r.body = body;
  return codec().parse(r, step);
}

std::vector<sp::WireFixture> fixtures() {
  std::vector<sp::WireFixture> out;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(sp::loadWireFixtures("test/support/fixtures/openai_wire_synthetic", out, err), err.c_str());
  return out;
}

const sp::WireFixture& fixture(const std::vector<sp::WireFixture>& fs, const char* name) {
  for (const sp::WireFixture& f : fs)
    if (f.file.find(name) != std::string::npos) return f;
  TEST_FAIL_MESSAGE(name);
  return fs.front();
}

bool shapeOk(const agent::HttpRequest& req, std::string& why) {
  wire_shape::Request r;
  r.method = req.method;
  r.path = req.path;
  r.headers = req.headers;
  r.tls = req.tls;
  r.body = agent::materializeBody(req);
  return wire_shape::responsesRequest(r, why);
}

txb::Row openaiRow(bool routed) {
  for (const txb::Row& r : txb::tableRows())
    if (r.slot == caps::Slot::OpenAI && r.routed == routed) return r;
  TEST_FAIL_MESSAGE("no openai row");
  return txb::Row{};
}

// One request of `c` through the head's route and the pre-flight, as the exchange renders it.
struct Routed {
  plain::Route route;
  plain::PreflightState own;
  plain::TurnCtx turn;
  tx::TxArena arena;
  agent::HttpRequest req;
  tx::Status render(const txb::RenderFixture& fx, const txb::Convo& c, uint32_t openTurn) {
    tx::Status st = plain::resolveRoute(fx.head, fx.turn, route);
    if (st.ok()) st = arena.init(tx::kActiveChatArenaBytes, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); });
    for (size_t i = 0; st.ok() && i < c.items.size(); ++i) st = arena.append(c.items[i]);
    if (!st.ok()) return st;
    turn = fx.turn;
    turn.turn = openTurn;
    plain::RenderInput in;
    in.turn = &turn;
    in.head = &fx.head;
    in.system = &fx.system;
    in.tools = &fx.tools;
    in.items = &arena;
    in.maxTokens = 0;   // the exchange's cap for an OpenAI head (no shared cap)
    plain::preflight(route, 0, own, in);
    return codec().render(in, req);
  }
};

// The battery's fixture on an OpenAI head, direct or routed, named as a binding names it.
txb::RenderFixture openaiFixture(bool routed) {
  txb::RenderFixture fx(openaiRow(routed));
  fx.turn.host = routed ? "cumulo" : "openai";
  fx.turn.model = routed ? "openai/gpt-5.6" : "gpt-5.6";
  return fx;
}

}  // namespace

// Every synthetic answer reads to the stop, completeness and call count its expect line names; an error
// or failed body is not an answer (the exchange classifies it before a parse).
void test_the_stop_table_over_every_synthetic_answer() {
  for (const sp::WireFixture& f : fixtures()) {
    plain::ParsedStep step;
    const tx::Status st = parse(f.body, step, f.status);
    if (!f.expected("class").empty()) {
      TEST_ASSERT_FALSE_MESSAGE(st.ok(), f.file.c_str());
      continue;
    }
    TEST_ASSERT_TRUE_MESSAGE(st.ok(), (f.file + ": " + st.where).c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(f.expected("stop").c_str(), plain::stopSlug(step.stop), f.file.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(std::stoi(f.expected("calls")), (int)step.calls.size(), f.file.c_str());
    for (const plain::ToolCall& c : step.calls)
      TEST_ASSERT_EQUAL_MESSAGE(f.expected("complete") == "true", c.complete, f.file.c_str());
    // input_tokens is the whole prompt; promptTokens the uncached part (input less reads and writes).
    TEST_ASSERT_EQUAL_MESSAGE(step.usage.promptTokens + step.usage.cacheReadTokens + step.usage.cacheWriteTokens,
                              step.usage.maxRequestIn, f.file.c_str());
  }
}

// The call's id is its call_id (the output must name it); the item id (fc_...) is kept nowhere.
// The reasoning item is stored with its id, summary and content, before the call it came with.
void test_a_call_keeps_its_call_id_and_the_reasoning_its_own_id() {
  const std::vector<sp::WireFixture> fs = fixtures();
  plain::ParsedStep step;
  TEST_ASSERT_TRUE(parse(fixture(fs, "function_call_two_ids").body, step).ok());
  TEST_ASSERT_EQUAL(1, (int)step.calls.size());
  TEST_ASSERT_EQUAL_STRING("call_syn01", step.calls[0].id.c_str());
  TEST_ASSERT_EQUAL(1, (int)step.meta.size());
  TEST_ASSERT_EQUAL(0, (int)step.meta[0].beforeCall);
  TEST_ASSERT_EQUAL_STRING(
      R"({"type":"reasoning","id":"rs_syn_01","summary":[],"encrypted_content":"gAAAAABsynthetic-encrypted-reasoning-01"})",
      step.meta[0].bytes.c_str());
  for (const plain::MetaPart& m : step.meta) TEST_ASSERT_TRUE(m.bytes.find("fc_") == std::string::npos);
}

// Small answers the fixtures do not hold: a refusal part, arguments that do not parse, a failed status,
// an answer of another API.
void test_refusal_cut_arguments_and_bodies_that_are_not_answers() {
  const std::string head = R"({"object":"response","model":"gpt-5.6","status":)";
  plain::ParsedStep step;
  TEST_ASSERT_TRUE(parse(head + R"("completed","output":[{"type":"message","role":"assistant","content":[{"type":"refusal","refusal":"No."}]},{"type":"function_call","id":"fc_1","call_id":"call_1","name":"x","arguments":"{}"}]})", step).ok());
  TEST_ASSERT_EQUAL_STRING("refusal", plain::stopSlug(step.stop));
  TEST_ASSERT_EQUAL_STRING("No.", step.refusal.c_str());
  TEST_ASSERT_EQUAL(0, (int)step.calls.size());
  TEST_ASSERT_TRUE(parse(head + R"("completed","output":[{"type":"function_call","call_id":"call_1","name":"x","arguments":"{\"a\":"}]})", step).ok());
  TEST_ASSERT_EQUAL_STRING("cut", plain::stopSlug(step.stop));
  TEST_ASSERT_FALSE(step.calls[0].complete);
  TEST_ASSERT_EQUAL_STRING("{}", step.calls[0].argsJson.c_str());
  TEST_ASSERT_TRUE(parse(head + R"("incomplete","incomplete_details":{"reason":"something_new"},"output":[]})", step).ok());
  TEST_ASSERT_EQUAL_STRING("other", plain::stopSlug(step.stop));
  TEST_ASSERT_FALSE(parse(head + R"("failed","output":[]})", step).ok());
  TEST_ASSERT_FALSE(parse(R"({"type":"message","content":[]})", step).ok());
  TEST_ASSERT_FALSE(parse(head + R"("completed","output":[{"type":"function_call","call_id":"","name":"x","arguments":"{}"}]})", step).ok());
}

// A reasoning item read from an answer and stored with its tag comes back in the next request byte for
// byte; the same item tagged for another route does not.
void test_a_stored_reasoning_item_replays_byte_for_byte_to_its_own_route() {
  plain::ParsedStep step;
  TEST_ASSERT_TRUE(parse(fixture(fixtures(), "function_call_two_ids").body, step).ok());
  const std::string model = "gpt-5.6";
  for (bool own : {true, false}) {
    txb::Convo c;
    c.openTurn();
    c.owner("Read the boat note.");
    tx::Item& k = c.add(tx::ItemKind::RoundMeta);
    k.x = step.meta[0].bytes;
    k.wr = plain::replayTag(plain::Dialect::OpenAIResponses, !own, model);
    const std::string id = c.call(step.calls[0].name, step.calls[0].argsJson);
    c.receipt(id, step.calls[0].name, "the boat is the Nereid II");
    c.nextRound();
    Routed r;
    TEST_ASSERT_TRUE(r.render(openaiFixture(false), c, c.turn).ok());
    const std::string body = agent::materializeBody(r.req);
    const bool replayed = body.find(step.meta[0].bytes) != std::string::npos;
    TEST_ASSERT_EQUAL_MESSAGE(own, replayed, own ? "own route: replayed" : "other route: left out");
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(shapeOk(r.req, why), why.c_str());
    TEST_ASSERT_TRUE(body.find("fc_") == std::string::npos);
  }
}

// The rows decide: gpt-5.6 (encrypted replay, breakpoints, the cache key) against gpt-4o-mini (no
// replay, implicit cache): no include and no breakpoint on the second, the cache key on both.
void test_the_row_decides_include_breakpoints_and_the_cache_key() {
  const std::vector<txb::Snapshot> day = txb::dayOfFourTurns(plain::Dialect::OpenAIResponses);
  const txb::Snapshot& s = day[day.size() - 2];
  for (const char* model : {"gpt-5.6", "gpt-4o-mini"}) {
    txb::RenderFixture fx(openaiRow(false));
    fx.turn.model = model;
    txb::Rendered out;
    TEST_ASSERT_TRUE(txb::renderItems(fx, s.c, 0, out, s.openTurn).ok());
    const bool full = std::string(model) == "gpt-5.6";
    TEST_ASSERT_EQUAL_MESSAGE(full, out.body.find("\"include\":[\"reasoning.encrypted_content\"]") != std::string::npos, model);
    TEST_ASSERT_EQUAL_MESSAGE(full, out.body.find("prompt_cache_breakpoint") != std::string::npos, model);
    TEST_ASSERT_TRUE_MESSAGE(out.body.find("\"prompt_cache_key\":\"nb-") != std::string::npos, model);
    TEST_ASSERT_TRUE_MESSAGE(out.body.find("\"store\":false") != std::string::npos, model);
  }
}

// Through the route: direct sends no output cap, routed declares the 25,000-token ceiling, each on its
// own path with a Bearer key.
void test_the_route_gives_the_path_and_the_cap() {
  const std::vector<txb::Snapshot> day = txb::dayOfFourTurns(plain::Dialect::OpenAIResponses);
  for (bool routed : {false, true}) {
    Routed r;
    TEST_ASSERT_TRUE(r.render(openaiFixture(routed), day.front().c, day.front().openTurn).ok());
    const agent::HttpRequest& req = r.req;
    TEST_ASSERT_EQUAL_STRING(routed ? "/router/openai/v1/responses" : "/v1/responses", req.path.c_str());
    TEST_ASSERT_EQUAL(routed, req.body.find("\"max_output_tokens\":25000") != std::string::npos);
    TEST_ASSERT_EQUAL(!routed, req.body.find("max_output_tokens") == std::string::npos);
    TEST_ASSERT_TRUE(req.body.find("\"model\":\"gpt-5.6\"") != std::string::npos);   // the bare id, routed too
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(shapeOk(req, why), why.c_str());
  }
}

// Every request of the battery's stored day, at every stub pointer, on both OpenAI rows, has the
// documented shape (the guard round too).
void test_every_battery_request_has_the_documented_shape() {
  size_t n = 0;
  for (bool routed : {false, true}) {
    txb::RenderFixture fx(openaiRow(routed));
    for (const txb::Snapshot& s : txb::dayOfFourTurns(plain::Dialect::OpenAIResponses))
      for (uint64_t p = 0; p <= s.c.q; ++p)
        for (bool guard : {false, true}) {
          txb::Rendered out;
          TEST_ASSERT_TRUE(txb::renderItems(fx, s.c, p, out, s.openTurn, guard).ok());
          std::string why;
          TEST_ASSERT_TRUE_MESSAGE(shapeOk(out.req, why), (s.name + " pointer " + std::to_string(p) + ": " + why).c_str());
          ++n;
        }
  }
  TEST_ASSERT_TRUE(n > 20);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_stop_table_over_every_synthetic_answer);
  RUN_TEST(test_a_call_keeps_its_call_id_and_the_reasoning_its_own_id);
  RUN_TEST(test_refusal_cut_arguments_and_bodies_that_are_not_answers);
  RUN_TEST(test_a_stored_reasoning_item_replays_byte_for_byte_to_its_own_route);
  RUN_TEST(test_the_row_decides_include_breakpoints_and_the_cache_key);
  RUN_TEST(test_the_route_gives_the_path_and_the_cap);
  RUN_TEST(test_every_battery_request_has_the_documented_shape);
  return UNITY_END();
}
