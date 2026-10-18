// test_utf8_class - the provider-bound UTF-8 class, tested as a class.
//
// On 18 Oct one torn character in a files.search snippet (a byte-offset substr)
// reached every provider body of the turn: Anthropic, OpenAI and Mistral each
// answered 400 in turn, and the dream and two synthesis turns failed. Each layer of
// the fix gets a property over generated inputs, plus a counter-test proving the
// property can see the bug it guards:
//   1. the helpers (validate, repair, boundary-safe window and cuts);
//   2. the tool-result boundary: EVERY registry tool's text is valid UTF-8 and
//      under the string ceiling, whatever the tool returned;
//   3. every provider renderer turns invalid tool text into a valid body;
//   4. serializeChecked refuses an invalid body before the send and names the tool;
//   5. every sub-agent reply writer cuts the 16 KB envelope UTF-8-safe, with a marker.
#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../../lib/harness/src/providers/wire.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/result_envelope.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/transcript.h"
#include "nimbus/orch/wire_limits.h"
#include "nimbus/utf8.h"

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
namespace providers = agent::providers;
namespace orch = nimbus::orch;
namespace wire = agent::providers::wire;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- seeded generators --------------------------------------------------------

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

// One random character of 1-4 bytes (always valid UTF-8).
static void addChar(std::string& out, Rng& r) {
  switch (r.below(5)) {
    case 0: out += (char)('a' + r.below(26)); break;
    case 1: out += "\xC3\xA9"; break;                 // e acute (2 bytes)
    case 2: out += "\xE2\x82\xAC"; break;             // euro sign (3 bytes)
    case 3: out += "\xE2\x89\xA5"; break;             // greater-or-equal (3 bytes)
    default: out += "\xF0\x9F\x98\x80"; break;        // emoji (4 bytes)
  }
}
static std::string denseDoc(Rng& r, size_t chars) {
  std::string d;
  for (size_t i = 0; i < chars; i++) addChar(d, r);
  return d;
}
// Any bytes at all (mostly invalid UTF-8).
static std::string randomBytes(Rng& r, size_t n) {
  std::string b(n, '\0');
  for (char& c : b) c = (char)r.below(256);
  return b;
}

// ---- 1. the helpers -------------------------------------------------------------

static void test_validator_known_vectors() {
  struct V { const char* bytes; size_t n; bool valid; };
  const V vs[] = {
      {"plain ascii", 11, true},
      {"\xC3\xA9", 2, true},
      {"\xE2\x82\xAC", 3, true},
      {"\xF0\x9F\x98\x80", 4, true},
      {"\xF4\x8F\xBF\xBF", 4, true},     // U+10FFFF, the last code point
      {"\x80", 1, false},                // lone continuation
      {"\xC0\xAF", 2, false},            // overlong "/"
      {"\xC1\xBF", 2, false},            // overlong
      {"\xE0\x80\xAF", 3, false},        // overlong 3-byte
      {"\xED\xA0\x80", 3, false},        // UTF-16 surrogate
      {"\xF4\x90\x80\x80", 4, false},    // past U+10FFFF
      {"\xF5\x80\x80\x80", 4, false},    // forbidden lead
      {"\xE2\x82", 2, false},            // torn at the end (the 18 Oct shape)
      {"ab\xE2", 3, false},
  };
  for (const V& v : vs)
    TEST_ASSERT_EQUAL_MESSAGE(v.valid, nimbus::utf8Valid(v.bytes, v.n), v.bytes);
}

static void test_repair_property_any_bytes() {
  Rng r(0xC0FFEE);
  for (int i = 0; i < 3000; i++) {
    std::string s = randomBytes(r, r.below(80));
    const bool wasValid = nimbus::utf8Valid(s);
    const std::string before = s;
    const size_t n = nimbus::utf8Repair(s);
    TEST_ASSERT_TRUE(nimbus::utf8Valid(s));
    TEST_ASSERT_EQUAL(wasValid, n == 0);
    if (wasValid) TEST_ASSERT_TRUE(s == before);   // valid input is never touched
    std::string twice = s;
    TEST_ASSERT_EQUAL(0, (int)nimbus::utf8Repair(twice));   // idempotent
    TEST_ASSERT_TRUE(twice == s);
  }
  // A torn 3-byte character costs ONE replacement character, not three.
  std::string torn = "ab\xE2\x82" "cd";
  TEST_ASSERT_EQUAL(1, (int)nimbus::utf8Repair(torn));
  TEST_ASSERT_EQUAL_STRING("ab\xEF\xBF\xBD" "cd", torn.c_str());
}

// The files.search snippet: a window around a match, at EVERY match offset of a
// dense multi-byte document, is valid UTF-8 at both ends. Counter-test: the old
// byte-offset window (substr(at - 40, 160)) tears characters on the same corpus.
static void test_window_every_offset_both_ends() {
  Rng r(7);
  int oldTorn = 0;
  for (int doc = 0; doc < 6; doc++) {
    const std::string body = denseDoc(r, 300);
    for (size_t at = 0; at <= body.size(); at++) {
      const std::string w = nimbus::utf8Window(body, at, 40, 160);
      TEST_ASSERT_TRUE(nimbus::utf8Valid(w));
      TEST_ASSERT_TRUE(w.size() <= 160);
      TEST_ASSERT_TRUE(body.find(w) != std::string::npos);
      const size_t s0 = at > 40 ? at - 40 : 0;
      if (!nimbus::utf8Valid(body.substr(s0, 160))) oldTorn++;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(oldTorn > 0, "the old byte-offset window must tear on this corpus");
  // The recorded shape: the match sits 1 and 2 bytes into a 3-byte character, so
  // a window starting 40 bytes earlier begins mid-character.
  std::string body(41, 'a');
  body += "\xE2\x82\xAC" "12-month outcomes";
  for (size_t into = 1; into <= 2; into++) {
    const std::string w = nimbus::utf8Window(body, 41 + into + 40, 40, 160);
    TEST_ASSERT_TRUE(nimbus::utf8Valid(w));
    TEST_ASSERT_TRUE(w.find("12-month outcomes") != std::string::npos);
  }
  // A buffer that was itself read short ends mid-character: the window drops it.
  const std::string shortRead = std::string(10, 'b') + "\xE2\x82";
  TEST_ASSERT_TRUE(nimbus::utf8Valid(nimbus::utf8Window(shortRead, 0, 0, 160)));
}

static void test_copy_cap_and_clip_every_cap() {
  Rng r(11);
  const std::string doc = denseDoc(r, 120);
  for (size_t cap = 0; cap <= doc.size() + 2; cap++) {
    char buf[1024];
    const size_t n = nimbus::utf8CopyCap(buf, cap, doc.data(), doc.size());
    if (cap) {
      TEST_ASSERT_TRUE(n < cap);
      TEST_ASSERT_EQUAL(n, strlen(buf));
      TEST_ASSERT_TRUE(nimbus::utf8Valid(buf, n));
    }
    std::string c = doc;
    const bool clipped = nimbus::utf8ClipNoted(c, cap);
    TEST_ASSERT_EQUAL(doc.size() > cap, clipped);
    TEST_ASSERT_TRUE(c.size() <= (cap > doc.size() ? doc.size() : cap));
    TEST_ASSERT_TRUE(nimbus::utf8Valid(c));
    if (clipped && cap > 40) TEST_ASSERT_TRUE(c.find("[cut: kept ") != std::string::npos);
  }
}

// ---- 2. the tool-result boundary --------------------------------------------------

// Adversarial tool outputs: torn, overlong, surrogate, lone continuation, random
// bytes, and texts around the string ceiling with a 4-byte character straddling it.
static std::vector<std::string> adversarialOutputs() {
  std::vector<std::string> v;
  Rng r(99);
  v.push_back("snippet ...\xE2\x82");                    // torn at the end
  v.push_back("\x82\xAC" "12-month outcomes");           // torn at the start (18 Oct)
  v.push_back("overlong \xC0\xAF and surrogate \xED\xA0\x80");
  v.push_back(std::string("\xF4\x90\x80\x80") + "past max");
  for (int i = 0; i < 20; i++) v.push_back(randomBytes(r, 1 + r.below(300)));
  for (size_t sz : {orch::kToolTextMax - 2, orch::kToolTextMax, orch::kToolTextMax + 1,
                    (size_t)65535, (size_t)65536, (size_t)70000, (size_t)200000}) {
    std::string big(sz - 2, 'x');
    big.insert(orch::kToolTextMax - 3 < big.size() ? orch::kToolTextMax - 3 : 0,
               "\xF0\x9F\x98\x80");   // straddles the clip point
    v.push_back(big);
  }
  return v;
}

static std::string textOf(const std::string& rpc) {
  JsonDocument d;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(d, rpc).code());
  JsonVariantConst t = d["result"]["content"][0]["text"];
  TEST_ASSERT_TRUE_MESSAGE(t.is<const char*>(), "tool text went null");
  return std::string(t.as<const char*>());
}

static void test_every_registry_tool_output_is_valid_utf8() {
  const std::vector<std::string> outs = adversarialOutputs();
  orch::ToolRegistry reg;
  // Two tools per output: one that succeeds with it, one that fails with it - the
  // error text crosses the same boundary.
  for (size_t i = 0; i < outs.size(); i++) {
    const std::string o = outs[i];
    reg.add("ok" + std::to_string(i), "x",
            [o](ArduinoJson::JsonObjectConst, const orch::Principal&) { return orch::ToolResult::ok(o); });
    reg.add("bad" + std::to_string(i), "x",
            [o](ArduinoJson::JsonObjectConst, const orch::Principal&) { return orch::ToolResult::fail(o); });
  }
  const orch::Principal who = orch::principalForRole("test", orch::Role::Admin);
  int counterInvalid = 0;
  for (size_t i = 0; i < outs.size(); i++) {
    for (const char* kind : {"ok", "bad"}) {
      const std::string rq = std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                         "\"params\":{\"name\":\"") + kind + std::to_string(i) +
                             "\",\"arguments\":{}}}";
      const std::string text = textOf(reg.handleRpc(rq, who));
      TEST_ASSERT_TRUE_MESSAGE(nimbus::utf8Valid(text), rq.c_str());
      TEST_ASSERT_TRUE(text.size() <= orch::kToolTextMax);
      const bool clipped = outs[i].size() > orch::kToolTextMax;
      TEST_ASSERT_EQUAL(clipped, text.find("\n[cut: kept ") != std::string::npos);
    }
    // Counter-test: the byte-cut boundary this replaced (raw text straight into the
    // block) hands the provider invalid UTF-8 for these outputs.
    if (!nimbus::utf8Valid(outs[i])) counterInvalid++;
  }
  TEST_ASSERT_TRUE(counterInvalid >= 20);
}

// ---- 3. every provider renderer ----------------------------------------------------

// A transcript whose one tool result is invalid UTF-8 (the poisoned files.search
// row), rendered by each provider's step: the body that leaves is valid UTF-8, and
// the repair is logged naming the tool.
static orch::Transcript poisonedTranscript() {
  orch::Transcript tr;
  tr.addUser("find the car seat notes");
  orch::HeadToolCall c;
  c.id = "tu_1";
  c.name = "files_search";
  c.argsJson = "{\"query\":\"car seat\"}";
  tr.addAssistantText("", 0, "");
  tr.addToolCall(c, 0, "");
  orch::HeadToolResult res;
  res.id = "tu_1";
  res.name = "files_search";
  res.output = "notes/diet.md (score 3)\n  ...\x82\xAC" "12-month outcomes**...";
  tr.addToolResult(res, 0);
  return tr;
}

static std::string antFinal() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
         "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\"}";
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

static void test_every_renderer_sends_a_valid_body() {
  struct P {
    const char* host;
    std::string reply;
    orch::HeadStepFn (*make)(const providers::ProviderDeps&, const std::string&,
                             const agent::HeadTools&, orch::TokenUsage*, orch::Transcript&);
  };
  const P ps[] = {
      {"anthropic", antFinal(), providers::antLoopStep},
      {"openai", oaiFinal(), providers::oaiLoopStep},
      {"mistral", misFinal(), providers::misLoopStep},
  };
  for (const P& p : ps) {
    LogCapture::install();
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200, p.reply});
    providers::ProviderDeps pd = d.contract();
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    orch::Transcript tr = poisonedTranscript();
    const std::string instr = "SYS";
    orch::HeadStep st = p.make(pd, instr, rig.ht, nullptr, tr)(true, {}, 60000, "");
    TEST_ASSERT_TRUE_MESSAGE(st.ok, (std::string(p.host) + ": " + st.error).c_str());
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)d.http.seen.size(), p.host);
    TEST_ASSERT_TRUE_MESSAGE(nimbus::utf8Valid(d.http.seen[0].body), p.host);
    // The lead byte was cut off, so two stray continuation bytes: two repairs.
    TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains("utf8: repaired 2 sequence(s) in files_search"), p.host);
  }
}

// ---- 4. serializeChecked ----------------------------------------------------------

static void test_serialize_checked_names_the_tool_per_provider_shape() {
  const std::string bad = "result \xE2\x82";
  {   // Anthropic: tool_use {id, name} answered by tool_result {tool_use_id}
    JsonDocument d;
    JsonObject a = d["messages"].add<JsonObject>();
    JsonObject tu = a["content"].add<JsonObject>();
    tu["type"] = "tool_use"; tu["id"] = "tu_9"; tu["name"] = "files_search";
    JsonObject u = d["messages"].add<JsonObject>();
    JsonObject tr = u["content"].add<JsonObject>();
    tr["type"] = "tool_result"; tr["tool_use_id"] = "tu_9"; tr["content"] = bad;
    std::string out, err;
    TEST_ASSERT_FALSE(wire::serializeChecked(d, out, err));
    TEST_ASSERT_TRUE(out.empty());
    TEST_ASSERT_EQUAL_STRING("request text is not valid UTF-8: tool files_search (content)", err.c_str());
    // Counter-test: the unchecked serializer sends exactly that invalid body.
    TEST_ASSERT_FALSE(nimbus::utf8Valid(wire::serializeBody(d)));
  }
  {   // OpenAI: function_call {call_id, name} answered by function_call_output {call_id}
    JsonDocument d;
    JsonObject fc = d["input"].add<JsonObject>();
    fc["type"] = "function_call"; fc["call_id"] = "call_1"; fc["name"] = "web_search";
    JsonObject fo = d["input"].add<JsonObject>();
    fo["type"] = "function_call_output"; fo["call_id"] = "call_1"; fo["output"] = bad;
    std::string out, err;
    TEST_ASSERT_FALSE(wire::serializeChecked(d, out, err));
    TEST_ASSERT_EQUAL_STRING("request text is not valid UTF-8: tool web_search (output)", err.c_str());
  }
  {   // Mistral: the tool message carries the name itself
    JsonDocument d;
    JsonObject m = d["messages"].add<JsonObject>();
    m["role"] = "tool"; m["tool_call_id"] = "abcdefghi"; m["name"] = "memory_search";
    m["content"] = bad;
    std::string out, err;
    TEST_ASSERT_FALSE(wire::serializeChecked(d, out, err));
    TEST_ASSERT_EQUAL_STRING("request text is not valid UTF-8: tool memory_search (content)",
                             err.c_str());
  }
  {   // not tied to a tool: the request field is named
    JsonDocument d;
    d["instructions"] = bad;
    std::string out, err;
    TEST_ASSERT_FALSE(wire::serializeChecked(d, out, err));
    TEST_ASSERT_EQUAL_STRING("request text is not valid UTF-8: instructions", err.c_str());
  }
}

// The 60,000 B guard (and the 65,535 B ceiling behind it): a string over the guard
// is refused with its size; one over the ceiling (stored as null by ArduinoJson) is
// refused as a dropped field; the guard itself passes.
static void test_serialize_checked_string_guard() {
  for (size_t sz : {(size_t)59999, orch::kJsonStrGuard, orch::kJsonStrGuard + 1, (size_t)65535,
                    (size_t)65536, (size_t)70000}) {
    JsonDocument d;
    d["system"] = std::string(sz, 's');
    std::string out, err;
    const bool ok = wire::serializeChecked(d, out, err);
    TEST_ASSERT_EQUAL(sz <= orch::kJsonStrGuard, ok);
    if (ok) {
      TEST_ASSERT_TRUE(out.size() > sz);
    } else if (sz <= orch::kJsonStringCeiling) {
      TEST_ASSERT_EQUAL_STRING(("request field over 60 KB: system (" + std::to_string(sz) + " B)").c_str(),
                               err.c_str());
    } else {
      TEST_ASSERT_TRUE(err.find("request field over 64 KB") == 0);
    }
  }
}

// putModelStr clips data under the guard with a note; putFieldStr never clips.
static void test_put_helpers() {
  JsonDocument d;
  std::string big(70000, 'q');
  big += "\xE2\x82";
  wire::putModelStr(d["a"], big, "tool x");
  const std::string a = d["a"] | "";
  TEST_ASSERT_TRUE(a.size() <= orch::kJsonStrGuard);
  TEST_ASSERT_TRUE(a.find("[cut: kept ") != std::string::npos);
  TEST_ASSERT_TRUE(nimbus::utf8Valid(a));
  wire::putFieldStr(d["b"], "sys \xE2\x82 end", "the system prompt");
  TEST_ASSERT_EQUAL_STRING("sys \xEF\xBF\xBD end", d["b"] | "");
  std::string out, err;
  TEST_ASSERT_TRUE(wire::serializeChecked(d, out, err));
}

// The final-round notice always fits: a prompt at the guard gives way to it.
static void test_final_round_notice_always_fits() {
  const std::string full(orch::kJsonStrGuard, 'p');
  const std::string r = wire::roundInstructions(full, "rounds");
  TEST_ASSERT_TRUE(r.size() <= orch::kJsonStrGuard);
  TEST_ASSERT_TRUE(r.find("FINAL turn") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("short", wire::roundInstructions("short", "").c_str());
}

// ---- 5. every sub-agent reply writer -------------------------------------------------

// A 20 KB multi-byte reply whose 16,383rd byte falls inside a character.
static std::string longReply() {
  std::string s(16382, 'r');
  while (s.size() < 20000) s += "\xE2\x82\xAC";
  return s;
}
static void assertCutReply(const orch::ResultEnvelope& env, const char* who) {
  const size_t n = strlen(env.reply);
  TEST_ASSERT_TRUE_MESSAGE(n < sizeof(env.reply), who);
  TEST_ASSERT_TRUE_MESSAGE(nimbus::utf8Valid(env.reply, n), who);
  const std::string tail = std::string(env.reply).substr(n - strlen(wire::kReplyCutMarker));
  TEST_ASSERT_EQUAL_STRING_MESSAGE(wire::kReplyCutMarker, tail.c_str(), who);
}
static std::string jsonStr(const std::string& s) {
  JsonDocument d;
  d.set(s);
  std::string out;
  serializeJson(d, out);
  return out;
}

static void test_every_reply_writer_cuts_utf8_safe_with_marker() {
  const std::string text = jsonStr(longReply());
  {   // OpenAI background response poll
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
        "\"output_text\",\"text\":" + text + "}]}]}"});
    providers::ProviderDeps pd = d.contract();
    orch::ResultEnvelope env;
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                      (int)providers::oaiPoll(pd, "api.openai.com", "k", "openai", "openai:r1", env));
    assertCutReply(env, "openai");
  }
  {   // Anthropic managed-agent event poll
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"data\":[{\"type\":\"agent.message\",\"content\":[{\"type\":\"text\",\"text\":" + text +
        "}]},{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
    providers::ProviderDeps pd = d.contract();
    orch::ResultEnvelope env;
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(pd, "anthropic:s1", env));
    assertCutReply(env, "anthropic");
  }
  {   // Mistral synchronous sub: dispatch caches, poll serves
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"outputs\":[{\"type\":\"message.output\",\"content\":" + text + "}]}"});
    providers::ProviderDeps pd = d.contract();
    agent::Directive dir;
    dir.instruction = "go";
    char job[72] = {};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                      (int)providers::mistralDispatch(pd, "mistral-small-latest", dir, job));
    orch::ResultEnvelope env;
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::mistralPoll(pd, job, env));
    assertCutReply(env, "mistral");
  }
  {   // custom endpoint sub
    FakeProviderDeps d;
    d.custBase = "https://p.example";
    d.http.script.push_back({"", "", 200,
        "{\"choices\":[{\"message\":{\"content\":" + text + "}}]}"});
    providers::ProviderDeps pd = d.contract();
    agent::Directive dir;
    dir.instruction = "go";
    char job[72] = {};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::customDispatch(pd, dir, job));
    orch::ResultEnvelope env;
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::customPoll(pd, job, env));
    assertCutReply(env, "custom");
  }
  {   // OpenAI-compatible sub (Z.ai, the Cumulo router)
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200,
        "{\"choices\":[{\"message\":{\"content\":" + text + "}}]}"});
    providers::ProviderDeps pd = d.contract();
    providers::CompatEndpoint ep;
    ep.host = "api.z.ai";
    ep.basePath = "/api/paas/v4";
    ep.key = "k";
    ep.model = "glm-5.3";
    ep.backendTag = "zai";
    agent::Directive dir;
    dir.instruction = "go";
    char job[72] = {};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::openaiCompatDispatch(pd, ep, dir, job));
    orch::ResultEnvelope env;
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::openaiCompatPoll(pd, "zai", job, env));
    assertCutReply(env, "compat");
  }
}

// A reply with a closing note keeps the note whole when the text fills the buffer.
static void test_reply_writer_keeps_the_note() {
  char buf[64];
  wire::ReplyWriter w(buf, sizeof(buf));
  w.append(std::string(100, 'x'));
  w.appendTail("\n[note: 2 files NOT captured]");
  const std::string s = buf;
  TEST_ASSERT_TRUE(s.size() < sizeof(buf));
  TEST_ASSERT_TRUE(s.find("[note: 2 files NOT captured]") != std::string::npos);
  TEST_ASSERT_TRUE(s.find("[cut at 16 KB]") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_validator_known_vectors);
  RUN_TEST(test_repair_property_any_bytes);
  RUN_TEST(test_window_every_offset_both_ends);
  RUN_TEST(test_copy_cap_and_clip_every_cap);
  RUN_TEST(test_every_registry_tool_output_is_valid_utf8);
  RUN_TEST(test_every_renderer_sends_a_valid_body);
  RUN_TEST(test_serialize_checked_names_the_tool_per_provider_shape);
  RUN_TEST(test_serialize_checked_string_guard);
  RUN_TEST(test_put_helpers);
  RUN_TEST(test_final_round_notice_always_fits);
  RUN_TEST(test_every_reply_writer_cuts_utf8_safe_with_marker);
  RUN_TEST(test_reply_writer_keeps_the_note);
  return UNITY_END();
}
