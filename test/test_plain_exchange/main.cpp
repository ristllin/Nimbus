// One exchange of the plain engine (lane RB-EX, PRD section 7): the checked body writer,
// the output cap, the body budget, the retry policy per error class, cut handling and the
// qualification rule for fallback and ladder targets. Every fault is injected through the
// scripted provider at the transport seam, with a scripted clock, so "inside the wall
// clock" is measured, not assumed.
#include <ArduinoJson.h>
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../../lib/harness/src/providers/image_parts.h"
#include "../../lib/harness/src/providers/plain_body.h"
#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/platform.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/orch/wire_limits.h"
#include "nimbus/utf8.h"

namespace sp = harness_test::scripted;
namespace plain = agent::plain;
namespace body = agent::plain::body;
namespace hq = nimbus::orch::headqual;
using plain::ExchangeErr;

namespace {

// ---- a scripted clock ----------------------------------------------------------------
uint32_t gClock = 0;
agent::Platform gPlatform;

// ---- a minimal Anthropic-shaped wire for the tests --------------------------------------
// It renders the owner text (shortened by the test ladder's rung) through the checked
// writer, and parses the scripted provider's Anthropic answers.
struct LadderState {
  int rung = 0;            // each rung halves the text
  int climbsAsked = 0;
  int refusedAsked = 0;
  int maxRungs = 3;
  std::vector<plain::ClimbAsk> asks;   // what each climb was told
};

struct TestWire : plain::WireCodec {
  std::string text = "Hello there.";
  const LadderState* ladder = nullptr;
  std::string forcedBody;   // non-empty: rendered as is (a wire that breaks the rules)
  int copies = 1;           // the owner text sent as this many messages
  mutable int renders = 0;
  int failFromRender = 0;   // > 0: that render and every later one fails
  plain::Dialect dialect() const override { return plain::Dialect::AnthropicMessages; }

  plain::Status render(const plain::RenderInput& in, agent::HttpRequest& out) const override {
    if (failFromRender && ++renders >= failFromRender) return plain::Status::fail(plain::Err::TooLarge, "test render");
    out.method = "POST";
    out.host = in.head->host;
    out.path = "/v1/messages";
    out.headers.push_back({"x-api-key", in.head->key ? in.head->key() : std::string()});
    if (!forcedBody.empty()) {
      out.body = forcedBody;
      return plain::Status::success();
    }
    std::string t = text;
    for (int r = ladder ? ladder->rung : 0; r > 0; --r) t.resize(t.size() / 2);
    body::BodyWriter w(out.body, &out.splices);
    w.beginObject().key("model").str(in.turn->model);
    if (in.maxTokens) w.key("max_tokens").unum(in.maxTokens);
    if (!in.head->effort.empty()) w.key("output_config").beginObject().key("effort").str(in.head->effort).endObject();
    w.key("messages").beginArray();
    for (int i = 0; i < copies; ++i) w.beginObject().key("role").str("user").key("content").str(t).endObject();
    w.endArray();
    w.endObject();
    return w.status();
  }

  plain::Status parse(const agent::HttpResponse& in, plain::ParsedStep& out) const override {
    JsonDocument doc;
    if (deserializeJson(doc, in.body) || !doc["content"].is<JsonArrayConst>())
      return plain::Status::fail(plain::Err::InvalidArgument, "test parse");
    for (JsonObjectConst b : doc["content"].as<JsonArrayConst>()) {
      const std::string type = b["type"] | "";
      if (type == "text") out.text += b["text"] | "";
      if (type == "tool_use") {
        plain::ToolCall c;
        c.id = b["id"] | "";
        c.name = b["name"] | "";
        serializeJson(b["input"], c.argsJson);
        out.calls.push_back(c);
      }
    }
    const std::string stop = doc["stop_reason"] | "";
    out.stop = stop == "max_tokens" ? plain::StopReason::MaxTokens
               : stop == "tool_use" ? plain::StopReason::ToolUse
               : stop == "refusal"  ? plain::StopReason::Refusal
                                    : plain::StopReason::EndTurn;
    out.usage.promptTokens = doc["usage"]["input_tokens"] | 0;
    out.usage.completionTokens = doc["usage"]["output_tokens"] | 0;
    return plain::Status::success();
  }
};

// One exchange's world: the model, the wire, the turn, the head.
struct Rig {
  sp::ScriptedProvider model{sp::Dialect::Anthropic};
  TestWire wire;
  LadderState ladder;
  plain::TurnCtx turn;
  plain::HeadEndpoint head;
  plain::SystemBlocks system;
  std::vector<plain::ToolSpec> tools;
  plain::RenderInput in;
  plain::ExchangeRequest req;
  plain::ExchangeResult out;

  explicit Rig(const char* host = "anthropic", const char* model_ = "claude-sonnet-5-5") {
    gClock = 1000;
    model.clock = &gClock;
    turn.host = host;
    turn.model = model_;
    head.host = std::string(host) == "anthropic" ? "api.anthropic.com" : std::string(host) == "openai" ? "api.openai.com" : "router.example";
    head.routed = std::string(host) == "cumulo";
    head.effort = "medium";
    head.key = [] { return std::string("test-key"); };
    wire.ladder = &ladder;
    in.turn = &turn;
    in.head = &head;
    in.system = &system;
    in.tools = &tools;
    req.wire = &wire;
    req.render = &in;
    req.timeoutMs = 120000;
    req.deadlineMs = 240000;
    req.bodyCap = 60000;
    req.climb = [this](const plain::ClimbAsk& ask) {
      ladder.asks.push_back(ask);
      if (ask.why == plain::Climb::Refused) ++ladder.refusedAsked;
      else ++ladder.climbsAsked;
      if (ladder.rung >= ladder.maxRungs) return false;
      ++ladder.rung;
      return true;
    };
  }
  plain::Status run() {
    plain::ExchangeDeps deps;
    deps.transport = &model;
    deps.platform = &gPlatform;
    return plain::runExchange(deps, req, out);
  }
  uint32_t sentMaxTokens(size_t i) const { return model.requests()[i].view.maxTokens; }
};

void expectOk(const plain::Status& st) {
  TEST_ASSERT_TRUE_MESSAGE(st.ok(), (std::string(nimbus::orch::tx::errSlug(st.err)) + " at " + st.where).c_str());
}

// An error ends in one plain line: a status line, one line of text, no em dash.
void expectOnePlainLine(const plain::ExchangeResult& r) {
  TEST_ASSERT_TRUE(r.status != plain::StatusLine::None);
  TEST_ASSERT_FALSE(r.errorLine.empty());
  TEST_ASSERT_TRUE_MESSAGE(r.errorLine.find('\n') == std::string::npos, r.errorLine.c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.errorLine.find("\xE2\x80\x94") == std::string::npos, r.errorLine.c_str());
  TEST_ASSERT_TRUE(nimbus::utf8Valid(r.errorLine));
}

sp::Step slow(sp::Step s, uint32_t ms) {
  s.takesMs = ms;
  return s;
}

}  // namespace

void setUp() {
  gClock = 1000;
  gPlatform.nowMs = [] { return gClock; };
  gPlatform.delayMs = [](uint32_t ms) { gClock += ms; };
}
void tearDown() {}

// ======================================================================================
// The checked body writer
// ======================================================================================

static bool parsesAs(const std::string& json, JsonDocument& doc) {
  return !deserializeJson(doc, json, DeserializationOption::NestingLimit(64));
}

// Every byte under 0x20, '"' and '\\' is escaped; a strict parser reads the text back.
static void test_writer_escapes_every_control_byte() {
  std::string s;
  for (int c = 0; c < 0x80; ++c) s += (char)c;
  std::string out;
  body::BodyWriter w(out);
  w.beginObject().key("t").str(s).endObject();
  expectOk(w.status());
  for (unsigned char c : out) TEST_ASSERT_TRUE_MESSAGE(c >= 0x20, "a raw control byte reached the body");
  TEST_ASSERT_TRUE(out.find("\\u0000\\u0001") != std::string::npos);
  JsonDocument doc;
  TEST_ASSERT_TRUE(parsesAs(out, doc));
  TEST_ASSERT_TRUE(doc["t"].as<std::string>() == s);
}

// Invalid UTF-8 in any string value is repaired before it is written.
static void test_writer_repairs_invalid_utf8() {
  const char* bad[] = {"\xC3", "a\xE2\x82", "\xED\xA0\x80 surrogate", "\xF5 high", "\xC0\xAF overlong", "ok \xFF"};
  for (const char* b : bad) {
    std::string out;
    body::BodyWriter w(out);
    w.beginArray().str(b, std::strlen(b), body::Over::Clip).str(std::string(b), body::Over::Refuse).endArray();
    expectOk(w.status());
    TEST_ASSERT_TRUE_MESSAGE(nimbus::utf8Valid(out), b);
    TEST_ASSERT_EQUAL(2, (int)w.stats().repaired);
  }
}

// A string over the guard: Refuse fails the writer, Clip cuts on a character boundary with
// the note inside the guard, after the repair. Swept around the guard with multi-byte text.
static void test_writer_guard_refuses_or_clips_utf8_safely() {
  const size_t g = nimbus::orch::kJsonStrGuard;
  for (size_t extra = 0; extra < 9; ++extra) {
    std::string s(g - 4, 'a');
    for (size_t i = 0; i < extra; ++i) s += "\xE2\x82\xAC";   // the euro sign, three bytes
    if (extra == 8) s += "\xE2\x82";                           // and a torn tail
    std::string refused;
    body::BodyWriter r(refused);
    r.beginArray().str(s).endArray();
    if (s.size() > g) {
      TEST_ASSERT_TRUE(r.status().err == plain::Err::TooLarge);
    } else {
      expectOk(r.status());
    }
    std::string out;
    body::BodyWriter w(out);
    w.beginArray().str(s, body::Over::Clip).endArray();
    expectOk(w.status());
    TEST_ASSERT_TRUE(nimbus::utf8Valid(out));
    JsonDocument doc;
    TEST_ASSERT_TRUE(parsesAs(out, doc));
    const std::string back = doc[0] | "";
    TEST_ASSERT_TRUE_MESSAGE(back.size() <= g, "a clipped string is over the guard");
    TEST_ASSERT_TRUE(w.stats().longest <= g);
    if (s.size() > g) {
      TEST_ASSERT_TRUE(back.find("[cut: kept ") != std::string::npos);
      TEST_ASSERT_EQUAL(1, (int)w.stats().clipped);
    }
  }
}

// Structure misuse is refused and sticky; output of a well-formed sequence parses.
static void test_writer_structure_rules() {
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginObject().str("no key");
    TEST_ASSERT_TRUE(w.status().err == plain::Err::InvalidArgument);
  }
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginArray().key("k");
    TEST_ASSERT_FALSE(w.ok());
  }
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginObject().key("a\"b");
    TEST_ASSERT_FALSE(w.ok());
  }
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginObject().endArray();
    TEST_ASSERT_FALSE(w.ok());
  }
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginObject();
    TEST_ASSERT_FALSE(w.status().ok());   // unfinished
  }
  {
    std::string out;
    body::BodyWriter w(out);
    w.null().null();
    TEST_ASSERT_FALSE(w.ok());   // two top-level values
  }
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginObject().key("n").num(-5).key("u").unum(7).key("b").boolean(true).key("z").null();
    w.key("a").beginArray().str("x").beginObject().endObject().beginArray().endArray().endArray().endObject();
    expectOk(w.status());
    TEST_ASSERT_EQUAL_STRING(R"({"n":-5,"u":7,"b":true,"z":null,"a":["x",{},[]]})", out.c_str());
  }
}

// A stored fragment: compacted, its strings escaped and guarded, refused unless it is one
// balanced object or array.
static void test_writer_raw_fragments() {
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginObject().key("s").raw("{ \"a\" : [1, 2],\n \"b\": \"x\ty\" }").endObject();
    expectOk(w.status());
    TEST_ASSERT_EQUAL_STRING(R"({"s":{"a":[1,2],"b":"x\ty"}})", out.c_str());
  }
  const char* bad[] = {"{", "{\"a\":1}}", "{\"a\":1} {}", "\"str\"", "12", "", "{\"a\":\"open}", "[1]x",
                       "{\"a\":\"\\\n\"}", "{\"a\":\"\\q\"}", "{\"a\":\"\\u12\"}", "{]", "[}", "{abc , : }", "[1,]",
                       "{\"a\" 1}", "[01]", "[-]", "[1.]", "[1e]", "[tru]", "{\"a\":1,}", "[,1]", "{1:2}"};
  for (const char* b : bad) {
    std::string out;
    body::BodyWriter w(out);
    w.beginArray().raw(b);
    TEST_ASSERT_FALSE_MESSAGE(w.ok(), b);
  }
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginArray().raw(" {\"a\" : [true, false, null, -1.5e+3, 0, \"\\u00e9\\n\\/\"], \"b\":{}} ").endArray();
    expectOk(w.status());
    TEST_ASSERT_EQUAL_STRING(R"([{"a":[true,false,null,-1.5e+3,0,"\u00e9\n\/"],"b":{}}])", out.c_str());
  }
  {   // a \uXXXX counts the bytes it decodes to: 9,000 of them are 9,000 bytes, not 63,000
    std::string esc = "{\"t\":\"";
    for (int i = 0; i < 9000; ++i) esc += "\\u0041";
    std::string out;
    body::BodyWriter w(out);
    w.beginArray().raw(esc + "\"}").endArray();
    expectOk(w.status());
    TEST_ASSERT_TRUE(body::detail::stringsWithinGuard(out, nimbus::orch::kJsonStrGuard));
    std::string atGuard = "\"";
    for (size_t i = 0; i < nimbus::orch::kJsonStrGuard; ++i) atGuard += "\\u0041";
    TEST_ASSERT_TRUE(body::detail::stringsWithinGuard(atGuard + "\"", nimbus::orch::kJsonStrGuard));
    TEST_ASSERT_FALSE(body::detail::stringsWithinGuard(atGuard + "a\"", nimbus::orch::kJsonStrGuard));
    TEST_ASSERT_FALSE(body::detail::stringsWithinGuard(atGuard + "\\u00e9\"", nimbus::orch::kJsonStrGuard + 1));
  }
  {
    std::string big = "{\"t\":\"" + std::string(nimbus::orch::kJsonStrGuard + 1, 'x') + "\"}";
    std::string out;
    body::BodyWriter w(out);
    w.beginArray().raw(big);
    TEST_ASSERT_TRUE(w.status().err == plain::Err::TooLarge);
  }
  {
    std::string out;
    body::BodyWriter w(out);
    w.beginArray().raw("{\"t\":\"a\xC3\"}").endArray();
    expectOk(w.status());
    TEST_ASSERT_TRUE(nimbus::utf8Valid(out));
  }
}

// A string in pieces with a photo spliced in: the splice sits at its offset inside the
// string, the guard counts only the text, and the materialized body parses.
static void test_writer_pieces_and_splices() {
  static const uint8_t png[] = {0x89, 'P', 'N', 'G', 1, 2, 3, 4, 5};
  agent::HttpRequest req;
  body::BodyWriter w(req.body, &req.splices);
  w.beginObject().key("data").beginStr().part("pre:").splice(png, sizeof(png)).part(":post").endStr();
  w.key("stamp").beginStr().part("[Tue 17 Nov 2026 13:02] ").part("hi\n").endStr().endObject();
  expectOk(w.status());
  TEST_ASSERT_EQUAL(1, (int)req.splices.size());
  JsonDocument doc;
  TEST_ASSERT_TRUE(parsesAs(agent::materializeBody(req), doc));
  TEST_ASSERT_EQUAL_STRING("pre:iVBORwECAwQF:post", doc["data"] | "");
  TEST_ASSERT_EQUAL_STRING("[Tue 17 Nov 2026 13:02] hi\n", doc["stamp"] | "");
  // invalid UTF-8 in the pieces is repaired, around a splice too
  agent::HttpRequest torn;
  body::BodyWriter t(torn.body, &torn.splices);
  t.beginArray().beginStr().part("a\xC3").splice(png, 3).part("\xE2\x82").endStr().beginStr(body::Over::Clip).part("\xFF").endStr().endArray();
  expectOk(t.status());
  TEST_ASSERT_TRUE(nimbus::utf8Valid(torn.body));
  TEST_ASSERT_EQUAL(3, (int)t.stats().repaired);
  // a splice outside a string, or without a splice list, is refused
  std::string out;
  body::BodyWriter none(out);
  none.beginArray().beginStr().splice(png, sizeof(png));
  TEST_ASSERT_FALSE(none.ok());
  // a spliced string's text over the guard is refused, never clipped
  agent::HttpRequest big;
  body::BodyWriter b(big.body, &big.splices);
  b.beginArray().beginStr(body::Over::Clip).part(std::string(nimbus::orch::kJsonStrGuard, 'x')).splice(png, 3).part("y").endStr();
  TEST_ASSERT_TRUE(b.status().err == plain::Err::TooLarge);
}

// Random structures of random byte strings: the body is always valid UTF-8 JSON with no
// string over the guard, and the same calls give the same bytes.
static void test_writer_random_bodies_parse_and_are_stable() {
  uint32_t seed = 12345;
  auto rnd = [&seed](uint32_t n) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % n;
  };
  for (int iter = 0; iter < 200; ++iter) {
    const uint32_t s0 = seed;
    std::string outs[2];
    for (int pass = 0; pass < 2; ++pass) {
      seed = s0;
      body::BodyWriter w(outs[pass]);
      w.beginObject();
      const uint32_t keys = rnd(6) + 1;
      for (uint32_t k = 0; k < keys; ++k) {
        w.key(k % 2 ? "a" : "b");
        std::string s;
        const uint32_t len = rnd(3) == 0 ? nimbus::orch::kJsonStrGuard - 2 + rnd(8) : rnd(64);
        for (uint32_t i = 0; i < len; ++i) s += (char)rnd(256);
        w.str(s, body::Over::Clip);
      }
      w.endObject();
      expectOk(w.status());
    }
    TEST_ASSERT_TRUE(outs[0] == outs[1]);
    TEST_ASSERT_TRUE(nimbus::utf8Valid(outs[0]));
    JsonDocument doc;
    TEST_ASSERT_TRUE(parsesAs(outs[0], doc));
    for (JsonPairConst kv : doc.as<JsonObjectConst>())
      TEST_ASSERT_TRUE(std::strlen(kv.value().as<const char*>()) <= nimbus::orch::kJsonStrGuard);
  }
}

// ======================================================================================
// One exchange
// ======================================================================================

static void test_inputs_are_checked() {
  plain::ExchangeResult r;
  const plain::Status st = plain::runExchange({}, {}, r);
  TEST_ASSERT_TRUE(st.err == plain::Err::InvalidArgument);
  Rig rig;
  rig.req.deadlineMs = 0;
  TEST_ASSERT_TRUE(rig.run().err == plain::Err::InvalidArgument);
  Rig rig2;
  rig2.req.wire = nullptr;
  TEST_ASSERT_TRUE(rig2.run().err == plain::Err::InvalidArgument);
  TEST_ASSERT_EQUAL(0, (int)rig.model.requests().size() + (int)rig2.model.requests().size());
}

static void test_a_plain_answer_is_one_request() {
  Rig rig;
  rig.model.push(sp::say("It is sunny."));
  expectOk(rig.run());
  TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::None);
  TEST_ASSERT_EQUAL_STRING("It is sunny.", rig.out.step.text.c_str());
  TEST_ASSERT_EQUAL(1, rig.out.requests);
  TEST_ASSERT_EQUAL(0, rig.out.retries);
  TEST_ASSERT_TRUE(rig.out.status == plain::StatusLine::None);
  TEST_ASSERT_TRUE(rig.out.errorLine.empty());
  TEST_ASSERT_EQUAL(rig.model.requests()[0].body.size(), rig.out.requestBytes);
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(rig.model.verify(why), why.c_str());
}

// The output cap is main's load-aware cap for the real body: sent on a capped host, it
// shrinks when the turn has less time left, and an uncapped host sends none.
// A custom host that speaks the Messages API sends an output cap (the API requires max_tokens):
// Anthropic's table for its model; another dialect's custom host keeps its own (none).
static void test_a_custom_messages_host_sends_an_output_cap() {
  Rig custom("custom");
  custom.model.push(sp::say("ok"));
  expectOk(custom.run());
  TEST_ASSERT_TRUE(custom.out.err == ExchangeErr::None);
  TEST_ASSERT_TRUE(custom.sentMaxTokens(0) > 0);
  Rig anthropic;
  anthropic.model.push(sp::say("ok"));
  expectOk(anthropic.run());
  TEST_ASSERT_EQUAL(anthropic.sentMaxTokens(0), custom.sentMaxTokens(0));
  Rig chat("custom");
  chat.turn.dialect = plain::Dialect::ChatCompletions;
  chat.model = sp::ScriptedProvider{sp::Dialect::Chat};
  chat.model.clock = &gClock;
  chat.model.push(sp::say("ok"));
  expectOk(chat.run());
  TEST_ASSERT_EQUAL(0, (int)chat.out.maxTokens);
}

static void test_the_output_cap_fits_the_body_and_the_time() {
  Rig full;
  full.model.push(sp::say("a"));
  expectOk(full.run());
  const uint32_t roomy = full.sentMaxTokens(0);
  TEST_ASSERT_TRUE(roomy > 0);
  TEST_ASSERT_EQUAL(roomy, full.out.maxTokens);
  Rig late;
  late.req.deadlineMs = 20000;
  late.model.push(sp::say("a"));
  expectOk(late.run());
  TEST_ASSERT_TRUE_MESSAGE(late.sentMaxTokens(0) < roomy, "the cap ignored the time left");
  Rig open("openai", "gpt-5.6-luna");
  open.model.push(sp::say("a"));
  expectOk(open.run());
  TEST_ASSERT_EQUAL(0, (int)open.sentMaxTokens(0));
  // a caller's maxTokens is not what is sent
  Rig mine;
  mine.in.maxTokens = 7;
  mine.model.push(sp::say("a"));
  expectOk(mine.run());
  TEST_ASSERT_EQUAL(roomy, mine.sentMaxTokens(0));
}

// Transport, 5xx and 429: the same request again after 2 s then 8 s, at most two retries;
// then one plain line. The retried bodies are byte for byte the first.
static void test_retryable_classes_retry_twice_then_one_line() {
  struct Case {
    sp::Step fault;
    ExchangeErr err;
  };
  const Case cases[] = {{sp::transportCut(), ExchangeErr::Transport}, {sp::httpError(500), ExchangeErr::Server},
                        {sp::httpError(502), ExchangeErr::Server},     {sp::httpError(503), ExchangeErr::Server},
                        {sp::httpError(529), ExchangeErr::Server},     {sp::httpError(429), ExchangeErr::RateLimited}};
  for (const Case& c : cases) {
    for (int fails = 0; fails <= 4; ++fails) {
      Rig rig;
      for (int i = 0; i < fails; ++i) rig.model.push(c.fault);
      if (fails < 3) rig.model.push(sp::say("done"));
      if (fails == 4) rig.model.push(sp::say("never asked"));
      expectOk(rig.run());
      const int sent = fails < 3 ? fails + 1 : 3;
      TEST_ASSERT_EQUAL_MESSAGE((int)(fails == 4 ? 2 : 0), (int)rig.model.remaining(), "more than two retries");
      TEST_ASSERT_EQUAL_MESSAGE(sent, (int)rig.model.requests().size(), plain::exchangeErrSlug(c.err));
      TEST_ASSERT_EQUAL(sent, rig.out.requests);
      TEST_ASSERT_EQUAL(sent - 1, rig.out.retries);
      for (size_t i = 1; i < rig.model.requests().size(); ++i)
        TEST_ASSERT_TRUE_MESSAGE(rig.model.requests()[i].body == rig.model.requests()[0].body, "a retry changed the body");
      const uint32_t waits[] = {0, 2000, 10000, 10000};
      TEST_ASSERT_EQUAL(waits[sent - 1], rig.out.waitedMs);
      if (fails < 3) {
        TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::None);
        TEST_ASSERT_EQUAL_STRING("done", rig.out.step.text.c_str());
      } else {
        TEST_ASSERT_TRUE_MESSAGE(rig.out.err == c.err, plain::exchangeErrSlug(rig.out.err));
        TEST_ASSERT_TRUE(rig.out.step.text.empty() && rig.out.step.calls.empty());
        TEST_ASSERT_TRUE(rig.out.status == plain::StatusLine::ProviderError);
        expectOnePlainLine(rig.out);
      }
      std::string why;
      if (fails < 4) TEST_ASSERT_TRUE_MESSAGE(rig.model.verify(why), why.c_str());
    }
  }
}

// Inside the wall clock: whatever the deadline, the exchange ends by it, every request's
// read window fits the time left at its send, and a retry is taken only when its wait and
// a request still fit.
static void test_every_fault_ends_inside_the_wall_clock() {
  for (uint32_t deadline = 1000; deadline <= 60000; deadline += 1500) {
    for (int kind = 0; kind < 2; ++kind) {
      Rig rig;
      rig.req.deadlineMs = deadline;
      uint32_t sentAt = 0;
      bool windowFits = true;
      rig.model.respondWith([&](const sp::Recorded& r, size_t) {
        // the fault takes the request's whole read window (a hung read), or 300 ms
        const uint32_t left = deadline - (gClock - 1000);
        windowFits = windowFits && r.req.timeoutMs <= left;
        sentAt = gClock;
        return slow(kind ? sp::httpError(503) : sp::transportCut(), kind ? 300 : r.req.timeoutMs);
      });
      expectOk(rig.run());
      (void)sentAt;
      TEST_ASSERT_TRUE_MESSAGE(windowFits, "a read window outlived the turn's wall clock");
      TEST_ASSERT_TRUE_MESSAGE(gClock - 1000 <= deadline, "the exchange outlived its deadline");
      TEST_ASSERT_TRUE(rig.out.err != ExchangeErr::None);
      TEST_ASSERT_TRUE(rig.out.requests >= 1 && rig.out.requests <= 3);
      expectOnePlainLine(rig.out);
    }
  }
}

// A context refusal: the ladder climbs (rung 3 is the caller's) and the request is sent once
// more; a second refusal is one plain line. Every provider wording and 413 is one.
static void test_a_context_refusal_climbs_once_and_retries_once() {
  const char* words[] = {
      "prompt is too long: 210000 tokens > 200000 maximum",
      "This model's maximum context length is 128000 tokens (context_length_exceeded)",
      "Prompt 140000 > 131072 maximum context length",
      "too many total text bytes",
      "prompt exceeds max length",
      "prompt_too_long",
      "Your input exceeds the context window of this model.",
      "input length and max_tokens exceed context limit",
      "too large for model",
  };
  for (const char* w : words) {
    const std::string err = std::string(R"({"type":"error","error":{"type":"invalid_request_error","message":")") + w + "\"}}";
    Rig rig;
    rig.wire.text = std::string(4000, 'x');
    rig.model.push(sp::httpError(400, err));
    rig.model.push(sp::say("ok"));
    expectOk(rig.run());
    TEST_ASSERT_TRUE_MESSAGE(rig.out.err == ExchangeErr::None, w);
    TEST_ASSERT_EQUAL(1, rig.ladder.refusedAsked);
    TEST_ASSERT_EQUAL(0, (int)rig.ladder.asks.back().bodyBytes);   // a refusal knows no size
    TEST_ASSERT_EQUAL(2, (int)rig.model.requests().size());
    TEST_ASSERT_TRUE(rig.model.requests()[1].body.size() < rig.model.requests()[0].body.size());
    TEST_ASSERT_EQUAL(1, rig.out.climbs);
    Rig twice;
    twice.model.push(sp::httpError(400, err));
    twice.model.push(sp::httpError(400, err));
    expectOk(twice.run());
    TEST_ASSERT_TRUE(twice.out.err == ExchangeErr::ContextRefused);
    TEST_ASSERT_EQUAL(2, (int)twice.model.requests().size());
    expectOnePlainLine(twice.out);
  }
  Rig big;
  big.model.push(sp::httpError(413));
  big.req.climb = nullptr;
  expectOk(big.run());
  TEST_ASSERT_TRUE(big.out.err == ExchangeErr::ContextRefused);
  TEST_ASSERT_EQUAL(1, (int)big.model.requests().size());
  // counter-test: a refused output cap is not a context refusal
  TEST_ASSERT_TRUE(plain::classifyAnswer(400, R"({"error":{"message":"max_tokens exceeds the maximum of 64000"}})",
                                         false) == ExchangeErr::Other);
}

// Billing or key refused: park the host, one plain line, never retried.
static void test_a_key_or_billing_refusal_parks_and_is_never_retried() {
  const std::pair<int, std::string> cases[] = {
      {401, ""},
      {402, ""},
      {403, ""},
      {400, R"({"type":"error","error":{"type":"invalid_request_error","message":"Your credit balance is too low to access the Anthropic API."}})"},
      {429, R"({"error":{"message":"You exceeded your current quota","code":"insufficient_quota"}})"},
  };
  for (const auto& c : cases) {
    Rig rig;
    rig.model.push(sp::httpError(c.first, c.second));
    expectOk(rig.run());
    TEST_ASSERT_TRUE_MESSAGE(rig.out.err == ExchangeErr::KeyRefused, plain::exchangeErrSlug(rig.out.err));
    TEST_ASSERT_TRUE(rig.out.parkHost);
    TEST_ASSERT_EQUAL(1, (int)rig.model.requests().size());
    TEST_ASSERT_EQUAL(c.first, rig.out.httpStatus);
    expectOnePlainLine(rig.out);
  }
}

// A refused file reference (an expired or unknown photo id, lane RB-W1b): the caller forgets it
// and the exchange renders and sends once more; anything else stays one request and one line.
static void test_a_refused_file_reference_is_forgotten_and_sent_once_more() {
  namespace ip = agent::providers::imageparts;
  const std::string gone = R"({"type":"error","error":{"type":"not_found_error","message":"File not found: file_0aa"}})";
  ip::dropFileRefs();
  ip::keepFileId("1001", "pab12cd34", "file_0aa", gClock);
  Rig rig;
  int asked = 0;
  rig.req.refusedRef = [&asked](int st, const std::string& body) {
    ++asked;
    return ip::forgetRefusedFileId("1001", st, body);
  };
  rig.model.push(sp::httpError(404, gone));
  rig.model.push(sp::say("I see it."));
  expectOk(rig.run());
  TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::None);
  TEST_ASSERT_EQUAL_STRING("I see it.", rig.out.step.text.c_str());
  TEST_ASSERT_EQUAL(2, (int)rig.model.requests().size());
  TEST_ASSERT_EQUAL(1, asked);
  TEST_ASSERT_TRUE(ip::heldFileId("1001", "pab12cd34", gClock).empty());
  // Once per exchange: a second refusal is the one line.
  ip::keepFileId("1001", "pab12cd34", "file_0aa", gClock);
  Rig twice;
  twice.req.refusedRef = [](int st, const std::string& body) { return ip::forgetRefusedFileId("1001", st, body) || true; };
  twice.model.push(sp::httpError(400, gone));
  twice.model.push(sp::httpError(400, gone));
  twice.model.push(sp::say("never asked"));
  expectOk(twice.run());
  TEST_ASSERT_TRUE(twice.out.err == ExchangeErr::Other);
  TEST_ASSERT_EQUAL(2, (int)twice.model.requests().size());
  expectOnePlainLine(twice.out);
  // A caller that says yes to anything: only a 400 or a 404 is handed to it.
  {
    Rig any;
    int calls = 0;
    any.req.refusedRef = [&calls](int, const std::string&) { return ++calls > 0; };
    any.model.push(sp::httpError(422, gone));
    any.model.push(sp::say("never asked"));
    expectOk(any.run());
    TEST_ASSERT_EQUAL(0, calls);
    TEST_ASSERT_EQUAL(1, (int)any.model.requests().size());
  }
  // The caller's no, no caller, another status, another kind of 400: one request.
  for (int k = 0; k < 4; ++k) {
    Rig one;
    if (k == 0) one.req.refusedRef = [](int, const std::string&) { return false; };
    if (k >= 2) one.req.refusedRef = [](int st, const std::string& body) { return ip::forgetRefusedFileId("1001", st, body); };
    ip::keepFileId("1001", "pab12cd34", "file_0aa", gClock);
    one.model.push(k == 2 ? sp::httpError(422, gone) : k == 3 ? sp::httpError(400) : sp::httpError(404, gone));
    one.model.push(sp::say("never asked"));
    expectOk(one.run());
    TEST_ASSERT_TRUE_MESSAGE(one.out.err == ExchangeErr::Other, std::to_string(k).c_str());
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)one.model.requests().size(), std::to_string(k).c_str());
  }
  ip::dropFileRefs();
}

// Anything else, and a 2xx that does not parse: one request, one plain line.
static void test_other_answers_are_one_plain_line() {
  const sp::Step steps[] = {sp::httpError(400), sp::httpError(404), sp::httpError(422), sp::raw(200, "not json"),
                            sp::raw(200, "{\"content\":")};
  const ExchangeErr want[] = {ExchangeErr::Other, ExchangeErr::Other, ExchangeErr::Other, ExchangeErr::Malformed,
                              ExchangeErr::Malformed};
  for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
    Rig rig;
    rig.model.push(steps[i]);
    expectOk(rig.run());
    TEST_ASSERT_TRUE_MESSAGE(rig.out.err == want[i], plain::exchangeErrSlug(rig.out.err));
    TEST_ASSERT_EQUAL(1, (int)rig.model.requests().size());
    TEST_ASSERT_FALSE(rig.out.parkHost);
    expectOnePlainLine(rig.out);
  }
}

// A cut answer gets one retry at a larger cap (and the lower retry effort); a second cut is
// delivered with the CutAnswer line and none of its calls may be dispatched.
static void test_a_cut_answer_retries_once_then_is_delivered_with_its_line() {
  Rig rig;
  rig.model.push(sp::cut("The first half"));
  rig.model.push(sp::say("The whole answer."));
  expectOk(rig.run());
  TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::None);
  TEST_ASSERT_EQUAL_STRING("The whole answer.", rig.out.step.text.c_str());
  TEST_ASSERT_TRUE(rig.out.cutRetried);
  TEST_ASSERT_TRUE(rig.sentMaxTokens(1) > rig.sentMaxTokens(0));
  TEST_ASSERT_TRUE(rig.model.requests()[0].body.find("\"effort\":\"medium\"") != std::string::npos);
  TEST_ASSERT_TRUE(rig.model.requests()[1].body.find("\"effort\":\"low\"") != std::string::npos);
  TEST_ASSERT_EQUAL(40, (int)rig.out.step.usage.completionTokens);   // both answers are paid

  Rig twice;
  sp::Step c1 = sp::call("write_file", R"({"path":"notes/x.md","text":"half"})", "Saving");
  c1.finish = sp::Finish::MaxTokens;
  twice.model.push(c1);
  twice.model.push(c1);
  expectOk(twice.run());
  TEST_ASSERT_TRUE(twice.out.err == ExchangeErr::Cut);
  TEST_ASSERT_TRUE(twice.out.status == plain::StatusLine::CutAnswer);
  TEST_ASSERT_EQUAL_STRING("Saving", twice.out.step.text.c_str());
  TEST_ASSERT_EQUAL(1, (int)twice.out.step.calls.size());
  for (const plain::ToolCall& c : twice.out.step.calls) TEST_ASSERT_FALSE_MESSAGE(c.complete, "a cut call is dispatchable");
  TEST_ASSERT_EQUAL(2, (int)twice.model.requests().size());
  expectOnePlainLine(twice.out);
}

// A cut call followed by the whole call: only the whole one comes back, so the cut
// arguments never reach a dispatch.
static void test_cut_arguments_are_never_returned_for_dispatch() {
  Rig rig;
  sp::Step cut = sp::call("write_file", R"({"path":"notes/x.md","text":"half of it)");
  cut.finish = sp::Finish::MaxTokens;
  rig.model.push(cut);
  rig.model.push(sp::call("write_file", R"({"path":"notes/x.md","text":"whole"})"));
  expectOk(rig.run());
  TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::None);
  TEST_ASSERT_EQUAL(1, (int)rig.out.step.calls.size());
  TEST_ASSERT_TRUE(rig.out.step.calls[0].complete);
  TEST_ASSERT_TRUE(rig.out.step.calls[0].argsJson.find("whole") != std::string::npos);
  // a host that sends no cap has no retry: the cut is delivered at once, calls withheld
  Rig open("openai", "gpt-5.6-luna");
  open.model.push(cut);
  expectOk(open.run());
  TEST_ASSERT_TRUE(open.out.err == ExchangeErr::Cut);
  TEST_ASSERT_EQUAL(1, (int)open.model.requests().size());
  for (const plain::ToolCall& c : open.out.step.calls) TEST_ASSERT_FALSE(c.complete);
}

// A cut answer whose retry then fails: the cut text is still delivered with its line.
static void test_a_cut_then_a_failed_retry_keeps_the_cut_text() {
  Rig rig;
  rig.model.push(sp::cut("Part one"));
  for (int i = 0; i < 3; ++i) rig.model.push(sp::httpError(503));
  expectOk(rig.run());
  TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::Cut);
  TEST_ASSERT_EQUAL_STRING("Part one", rig.out.step.text.c_str());
  TEST_ASSERT_TRUE(rig.out.status == plain::StatusLine::CutAnswer);
}

// The class property: whatever the fault script, the exchange returns at most the calls of
// one answer, never a call of an answer it retried past, and never more than five
// requests (two same-host retries, one cut retry, one ladder retry, the first).
static void test_no_fault_sequence_returns_a_call_twice_or_loops() {
  const sp::Step faults[] = {sp::transportCut(), sp::httpError(500), sp::httpError(429), sp::httpError(413),
                             sp::cut("cut"), sp::raw(200, "x"), sp::httpError(401)};
  const size_t nf = sizeof(faults) / sizeof(faults[0]);
  for (size_t a = 0; a < nf; ++a)
    for (size_t b = 0; b < nf; ++b)
      for (size_t c = 0; c < nf; ++c) {
        Rig rig;
        rig.model.push(faults[a]).push(faults[b]).push(faults[c]);
        rig.model.respondWith([](const sp::Recorded&, size_t i) {
          return sp::call("ledger_write", "{\"n\":" + std::to_string(i) + "}");
        });
        expectOk(rig.run());
        TEST_ASSERT_TRUE(rig.out.requests <= 5);
        TEST_ASSERT_EQUAL(rig.out.requests, (int)rig.model.requests().size());
        TEST_ASSERT_TRUE(rig.out.step.calls.size() <= 1);
        for (const plain::ToolCall& call : rig.out.step.calls)
          TEST_ASSERT_TRUE_MESSAGE(call.argsJson == "{\"n\":" + std::to_string(rig.out.requests - 1) + "}",
                                   "a call from an earlier answer came back");
        if (rig.out.err != ExchangeErr::None) expectOnePlainLine(rig.out);
      }
}

// The body budget: a body over the head's cap is never sent; the ladder climbs until it fits,
// or the exchange ends with nothing sent.
static void test_a_body_over_the_cap_climbs_and_is_never_sent() {
  Rig rig;
  rig.wire.text = std::string(50000, 'y');
  rig.req.bodyCap = 20000;
  rig.model.push(sp::say("fits"));
  expectOk(rig.run());
  TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::None);
  TEST_ASSERT_EQUAL(2, rig.ladder.climbsAsked);
  // Each climb is told the body it must shrink and the cap (ruling 44): over the cap, shrinking.
  TEST_ASSERT_EQUAL(2, (int)rig.ladder.asks.size());
  for (const plain::ClimbAsk& a : rig.ladder.asks) {
    TEST_ASSERT_EQUAL(20000, (int)a.bodyCap);
    TEST_ASSERT_TRUE(a.bodyBytes > a.bodyCap);
  }
  TEST_ASSERT_TRUE(rig.ladder.asks[1].bodyBytes < rig.ladder.asks[0].bodyBytes);
  TEST_ASSERT_TRUE(rig.model.requests()[0].body.size() <= 20000);
  TEST_ASSERT_EQUAL(1, (int)rig.model.requests().size());

  Rig never;
  never.wire.text = std::string(50000, 'y');
  never.req.bodyCap = 1000;
  expectOk(never.run());
  TEST_ASSERT_TRUE(never.out.err == ExchangeErr::ContextRefused);
  TEST_ASSERT_EQUAL(0, (int)never.model.requests().size());
  TEST_ASSERT_EQUAL(0, never.out.httpStatus);
  expectOnePlainLine(never.out);

  // No cap given: the head's released row holds (claude-sonnet-5-5: 250,000 B), never the 85,000 B
  // floor by accident (ruling 83 item 4). A 100 KB body goes out; one over the row is refused.
  const uint32_t row = nimbus::orch::requestBudgetFor("anthropic", "claude-sonnet-5-5", false).wireBodyBytes;
  TEST_ASSERT_TRUE(row > nimbus::orch::tx::kFloorHeadBodyCap);
  for (int copies : {2, (int)(row / 50000) + 1}) {
    Rig floor;
    floor.wire.text = std::string(50000, 'y');
    floor.wire.copies = copies;
    floor.req.bodyCap = 0;
    floor.req.climb = nullptr;
    floor.model.push(sp::say("ok"));
    expectOk(floor.run());
    const bool over = (uint32_t)copies * 50000 > row;
    TEST_ASSERT_EQUAL(over, floor.out.err == ExchangeErr::ContextRefused);
    TEST_ASSERT_EQUAL(over ? 0 : 1, (int)floor.model.requests().size());
  }
}

// No body leaves with a string over the 60,000 B guard or with invalid UTF-8, whatever the
// wire wrote.
static void test_no_body_string_over_the_guard_is_sent() {
  Rig rig;
  rig.wire.forcedBody = "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"" +
                        std::string(nimbus::orch::kJsonStrGuard + 1, 'q') + "\"}]}";
  rig.req.bodyCap = 200000;
  TEST_ASSERT_TRUE(rig.run().err == plain::Err::InvalidArgument);
  TEST_ASSERT_EQUAL(0, (int)rig.model.requests().size());
  Rig esc;   // escapes count as the text they stand for: 60,002 newlines written as two-byte escapes are over
  std::string s;
  for (size_t i = 0; i < nimbus::orch::kJsonStrGuard / 2 + 1; ++i) s += "\\n";
  esc.wire.forcedBody = "{\"m\":\"" + s + s + "\"}";
  esc.req.bodyCap = 400000;
  TEST_ASSERT_TRUE(esc.run().err == plain::Err::InvalidArgument);
  Rig torn;
  torn.wire.forcedBody = "{\"m\":\"a\xC3\"}";
  TEST_ASSERT_TRUE(torn.run().err == plain::Err::InvalidArgument);
  Rig fine;   // counter-test: a string at the guard goes
  fine.wire.forcedBody = "{\"m\":\"" + std::string(nimbus::orch::kJsonStrGuard, 'q') + "\"}";
  fine.req.bodyCap = 200000;
  fine.model.push(sp::say("ok"));
  expectOk(fine.run());
  TEST_ASSERT_EQUAL(1, (int)fine.model.requests().size());
}

// A routed head follows the router refusal contract.
static void test_a_routed_head_follows_the_refusal_contract() {
  {
    Rig rig("cumulo", "anthropic/claude-sonnet-5-5");
    rig.model.push(sp::httpError(402, R"({"reason":"insufficient_credits_for_call","maxAffordableOutputTokens":1000})"));
    rig.model.push(sp::say("short"));
    expectOk(rig.run());
    TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::None);
    TEST_ASSERT_EQUAL(1000, (int)rig.sentMaxTokens(1));
    TEST_ASSERT_FALSE(rig.model.requests()[0].req.cappedResend);
    TEST_ASSERT_TRUE(rig.model.requests()[1].req.cappedResend);
  }
  for (int st : {401, 402, 403}) {   // no reason: a proxy's answer, never a park of the router
    Rig rig("cumulo", "anthropic/claude-sonnet-5-5");
    rig.model.push(sp::httpError(st, R"({"message":"forbidden"})"));
    expectOk(rig.run());
    TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::Other);
    TEST_ASSERT_FALSE(rig.out.parkHost);
  }
  {
    Rig rig("cumulo", "anthropic/claude-sonnet-5-5");
    rig.model.push(sp::httpError(402, R"({"reason":"insufficient_credits"})"));
    expectOk(rig.run());
    TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::KeyRefused);
    TEST_ASSERT_TRUE(rig.out.parkHost);
    TEST_ASSERT_EQUAL(1, (int)rig.model.requests().size());
  }
  {
    Rig rig("cumulo", "anthropic/claude-sonnet-5-5");
    rig.model.push(sp::httpError(502, R"({"reason":"fallback_exhausted","retryable":true})"));
    expectOk(rig.run());
    TEST_ASSERT_TRUE(rig.out.failOver);
    TEST_ASSERT_EQUAL(1, (int)rig.model.requests().size());
    expectOnePlainLine(rig.out);
  }
  {
    Rig rig("cumulo", "anthropic/claude-sonnet-5-5");
    rig.model.push(sp::httpError(429, R"({"reason":"rate_limited","retryable":true,"retryAfterMs":1500})"));
    rig.model.push(sp::httpError(429, R"({"reason":"rate_limited","retryable":true,"retryAfterMs":1500})"));
    expectOk(rig.run());
    TEST_ASSERT_EQUAL(2, (int)rig.model.requests().size());   // resent once only
    TEST_ASSERT_EQUAL(1500, (int)rig.out.waitedMs);
    TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::RateLimited);
  }
  {
    Rig rig("cumulo", "anthropic/claude-sonnet-5-5");
    rig.model.push(sp::httpError(413, R"({"reason":"request_too_large"})"));
    rig.model.push(sp::say("ok"));
    expectOk(rig.run());
    TEST_ASSERT_EQUAL(1, rig.ladder.refusedAsked);
    TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::None);
  }
  {
    Rig rig("cumulo", "anthropic/claude-sonnet-5-5");
    rig.model.push(sp::httpError(400, R"({"reason":"some_future_reason"})"));
    expectOk(rig.run());
    TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::Other);
    TEST_ASSERT_FALSE(rig.out.parkHost);
  }
}

// ======================================================================================
// Lines and qualification
// ======================================================================================

static void test_every_error_class_has_one_plain_line() {
  const ExchangeErr all[] = {ExchangeErr::Transport, ExchangeErr::Server,    ExchangeErr::RateLimited,
                             ExchangeErr::ContextRefused, ExchangeErr::KeyRefused, ExchangeErr::Cut,
                             ExchangeErr::Malformed, ExchangeErr::Other,     ExchangeErr::Blocked};
  const char* hosts[] = {"anthropic", "openai", "zai", "cumulo", "mistral", "custom", "unknown"};
  for (ExchangeErr e : all)
    for (const char* h : hosts) {
      plain::ExchangeResult r;
      r.err = e;
      r.status = plain::StatusLine::ProviderError;
      r.errorLine = plain::exchangeLine(e, h, 503);
      expectOnePlainLine(r);
    }
  TEST_ASSERT_TRUE(plain::exchangeLine(ExchangeErr::None, "anthropic", 200).empty());
}

static void test_provider_slug_of_a_head() {
  plain::TurnCtx t;
  plain::HeadEndpoint h;
  t.host = "anthropic";
  TEST_ASSERT_EQUAL_STRING("anthropic", plain::providerSlugOf(t, h).c_str());
  t.host = "";
  h.host = "api.openai.com";
  TEST_ASSERT_EQUAL_STRING("openai", plain::providerSlugOf(t, h).c_str());
  h.host = "api.z.ai";
  TEST_ASSERT_EQUAL_STRING("zai", plain::providerSlugOf(t, h).c_str());
  h.host = "router.example";
  h.routed = true;
  TEST_ASSERT_EQUAL_STRING("cumulo", plain::providerSlugOf(t, h).c_str());
  h.routed = false;
  h.host = "192.168.1.9";
  TEST_ASSERT_EQUAL_STRING("custom", plain::providerSlugOf(t, h).c_str());
}

// A fallback answers only a fresh attempt with no effect run and the head parked, and only
// a target the table qualifies; otherwise none.
static void test_a_fallback_the_table_does_not_qualify_is_skipped() {
  const std::vector<plain::HeadTarget> order = {
      {"mistral", "mistral-large-4-0", hq::Wire::MistralChat, false},
      {"anthropic", "claude-sonnet-5-5", hq::Wire::AnthropicMessages, true},
      {"zai", "glm-5.3", hq::Wire::ChatCompat, false},
      {"anthropic", "claude-haiku-5-5", hq::Wire::AnthropicMessages, false},
  };
  plain::FallbackFacts f;
  f.freshAttempt = true;
  f.headParked = true;
  f.order = &order;
  f.line = hq::Line::E1;
  int idx = 9;
  expectOk(plain::pickFallback(f, idx));
  TEST_ASSERT_EQUAL(3, idx);   // mistral held, sonnet parked, glm assumed
  f.line = hq::Line::E2;       // nothing is qualified on engine 2 yet
  expectOk(plain::pickFallback(f, idx));
  TEST_ASSERT_EQUAL(-1, idx);
  f.line = hq::Line::E1;
  f.effectRun = true;
  expectOk(plain::pickFallback(f, idx));
  TEST_ASSERT_EQUAL(-1, idx);
  f.effectRun = false;
  f.freshAttempt = false;
  expectOk(plain::pickFallback(f, idx));
  TEST_ASSERT_EQUAL(-1, idx);
  f.freshAttempt = true;
  f.headParked = false;
  expectOk(plain::pickFallback(f, idx));
  TEST_ASSERT_EQUAL(-1, idx);
  f.order = nullptr;
  TEST_ASSERT_TRUE(plain::pickFallback(f, idx).err == plain::Err::InvalidArgument);

  std::vector<plain::HeadTarget> kept;
  expectOk(plain::qualifiedTargets(order, hq::Line::E1, kept));
  TEST_ASSERT_EQUAL(2, (int)kept.size());
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5-5", kept[0].model.c_str());
  expectOk(plain::qualifiedTargets(order, hq::Line::E2, kept));
  TEST_ASSERT_EQUAL(0, (int)kept.size());
  // an unknown head is never qualified
  TEST_ASSERT_FALSE(plain::targetQualified({"nowhere", "x", hq::Wire::Any, false}, hq::Line::E1));
}

// A body of control bytes the writer escapes six-fold is still inside the guard: the check
// counts what each escape stands for.
static void test_an_escaped_body_passes_the_check() {
  Rig rig;
  rig.wire.text = std::string(9000, '\x01');
  rig.model.push(sp::say("ok"));
  expectOk(rig.run());
  TEST_ASSERT_EQUAL(1, (int)rig.model.requests().size());
}

// A cut retry is sent only with more room than the cut request had (or a lower effort): late
// in a turn the retry's fit is the time left now, and an equal request is never sent.
static void test_a_cut_retry_always_has_more_room() {
  for (uint32_t took = 0; took <= 40000; took += 2500) {
    Rig rig;
    rig.req.deadlineMs = 60000;
    rig.model.push(slow(sp::cut("half"), took));
    rig.model.respondWith([](const sp::Recorded&, size_t) { return sp::say("whole"); });
    expectOk(rig.run());
    if (rig.model.requests().size() == 2) {
      const bool more = rig.sentMaxTokens(1) > rig.sentMaxTokens(0);
      const bool lower = rig.model.requests()[1].body.find("\"effort\":\"low\"") != std::string::npos;
      TEST_ASSERT_TRUE_MESSAGE(more || lower, "a cut retry sent no more room");
    } else {
      TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::Cut);
    }
    TEST_ASSERT_TRUE(gClock - 1000 <= 60000);
  }
}

// On a host whose retry keeps its effort (Z.ai), a late cut whose retry would have no more
// room is delivered at once, never asked again with an equal request.
static void test_a_late_cut_with_no_room_is_not_asked_again() {
  int delivered = 0, retried = 0;
  for (uint32_t took = 0; took <= 44000; took += 2000) {
    Rig rig("zai", "glm-5.3");
    rig.head.effort = "low";   // the effort Z.ai's row sends on both attempts
    rig.req.timeoutMs = 50000;
    rig.req.deadlineMs = 52000;
    rig.model.push(slow(sp::cut("half"), took));
    rig.model.respondWith([](const sp::Recorded&, size_t) { return sp::say("whole"); });
    expectOk(rig.run());
    if (rig.model.requests().size() == 2) {
      ++retried;
      TEST_ASSERT_TRUE_MESSAGE(rig.sentMaxTokens(1) > rig.sentMaxTokens(0), "an equal cut retry was sent");
    } else {
      ++delivered;
      TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::Cut);
    }
  }
  TEST_ASSERT_TRUE(retried > 0);
  TEST_ASSERT_TRUE(delivered > 0);
}

// A render that fails after a cut answer still delivers the cut text with its line.
static void test_a_failed_render_after_a_cut_delivers_the_cut() {
  Rig rig;
  rig.wire.failFromRender = 3;   // the cut request renders twice (measure, fit); the retry fails
  rig.model.push(sp::cut("Part one"));
  expectOk(rig.run());
  TEST_ASSERT_TRUE(rig.out.err == ExchangeErr::Cut);
  TEST_ASSERT_EQUAL_STRING("Part one", rig.out.step.text.c_str());
  Rig first;
  first.wire.failFromRender = 1;
  TEST_ASSERT_TRUE(first.run().err == plain::Err::TooLarge);   // nothing sent: the render's status
  TEST_ASSERT_EQUAL(0, (int)first.model.requests().size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_writer_escapes_every_control_byte);
  RUN_TEST(test_writer_repairs_invalid_utf8);
  RUN_TEST(test_writer_guard_refuses_or_clips_utf8_safely);
  RUN_TEST(test_writer_structure_rules);
  RUN_TEST(test_writer_raw_fragments);
  RUN_TEST(test_writer_pieces_and_splices);
  RUN_TEST(test_writer_random_bodies_parse_and_are_stable);
  RUN_TEST(test_inputs_are_checked);
  RUN_TEST(test_a_plain_answer_is_one_request);
  RUN_TEST(test_the_output_cap_fits_the_body_and_the_time);
  RUN_TEST(test_retryable_classes_retry_twice_then_one_line);
  RUN_TEST(test_every_fault_ends_inside_the_wall_clock);
  RUN_TEST(test_a_context_refusal_climbs_once_and_retries_once);
  RUN_TEST(test_a_key_or_billing_refusal_parks_and_is_never_retried);
  RUN_TEST(test_other_answers_are_one_plain_line);
  RUN_TEST(test_a_custom_messages_host_sends_an_output_cap);
  RUN_TEST(test_a_refused_file_reference_is_forgotten_and_sent_once_more);
  RUN_TEST(test_a_cut_answer_retries_once_then_is_delivered_with_its_line);
  RUN_TEST(test_cut_arguments_are_never_returned_for_dispatch);
  RUN_TEST(test_a_cut_then_a_failed_retry_keeps_the_cut_text);
  RUN_TEST(test_no_fault_sequence_returns_a_call_twice_or_loops);
  RUN_TEST(test_a_body_over_the_cap_climbs_and_is_never_sent);
  RUN_TEST(test_no_body_string_over_the_guard_is_sent);
  RUN_TEST(test_a_routed_head_follows_the_refusal_contract);
  RUN_TEST(test_every_error_class_has_one_plain_line);
  RUN_TEST(test_provider_slug_of_a_head);
  RUN_TEST(test_a_fallback_the_table_does_not_qualify_is_skipped);
  RUN_TEST(test_an_escaped_body_passes_the_check);
  RUN_TEST(test_a_cut_retry_always_has_more_room);
  RUN_TEST(test_a_late_cut_with_no_room_is_not_asked_again);
  RUN_TEST(test_a_failed_render_after_a_cut_delivers_the_cut);
  return UNITY_END();
}
