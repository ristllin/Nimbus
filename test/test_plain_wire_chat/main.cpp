// test_plain_wire_chat - the Chat codec of engine 2 (lane E2-R6; router design section 4, codec C10).
// The battery (test_router_conformance) runs the row properties and the shape checker on every chat
// row and the route goldens pin its bytes; this suite holds what is the codec's own: the registry
// hands it out for Chat and Flat, the stop-value table, arguments as a string or an object, usage as
// the uncached part plus the cache reads, reasoning replayed only to its own kind, text mode, and a
// codec that reads no host.
#include <unity.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../support/tx_battery/render.h"
#include "../support/tx_battery/rows.h"
#include "../support/wire_shape/chat_shape.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace caps = nimbus::orch::caps;

void setUp() {}
void tearDown() {}

namespace {

const plain::WireCodec& chat() {
  const plain::WireCodec* w = nullptr;
  TXB_OK(plain::wireFor(plain::Dialect::ChatCompletions, w));
  return *w;
}

plain::ParsedStep parsed(const std::string& message, const char* finish, const char* usage = nullptr) {
  std::string body = R"({"id":"t","object":"chat.completion","model":"m","choices":[{"index":0,"message":)" + message +
                     R"(,"finish_reason":)" + (finish ? std::string("\"") + finish + "\"" : std::string("null")) + "}]";
  if (usage) body += std::string(",\"usage\":") + usage;
  body += "}";
  agent::HttpResponse resp;
  resp.status = 200;
  resp.body = body;
  plain::ParsedStep step;
  TXB_OK(chat().parse(resp, step));
  return step;
}

const char* kCall = R"({"role":"assistant","content":"","tool_calls":[{"id":"c1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.md\"}"}}]})";

txb::Row rowOf(caps::Slot slot) {
  for (const txb::Row& r : txb::tableRows())
    if (r.slot == slot && !r.routed) return r;
  TEST_FAIL_MESSAGE("no row");
  return {};
}

// One open turn: the owner's ask, a stored reasoning item, the round's text and one call with its result.
txb::Convo reasoningTurn(const std::string& meta) {
  txb::Convo c;
  c.callPrefix = "call";
  c.openTurn();
  c.owner("Which bed gets the most sun?");
  tx::Item& k = c.add(tx::ItemKind::RoundMeta);
  k.x = meta;
  c.say("Reading the note.", false);
  const std::string id = c.call("read_file", R"({"path":"notes/bed2.md"})");
  c.receipt(id, "read_file", "bed 2: full sun");
  c.nextRound();
  return c;
}

std::string renderOn(caps::Slot slot, const txb::Convo& c) {
  txb::RenderFixture fx(rowOf(slot));
  txb::Rendered out;
  TXB_OK(txb::renderItems(fx, c, 0, out, c.turn));
  return out.body;
}

}  // namespace

// Before this lane wireFor answered pending for both: a chat head could not be served.
void test_wire_for_chat_and_flat_give_the_codec() {
  const plain::WireCodec* a = nullptr;
  const plain::WireCodec* b = nullptr;
  TXB_OK(plain::wireFor(plain::Dialect::ChatCompletions, a));
  TXB_OK(plain::wireFor(plain::Dialect::Flat, b));
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_TRUE(a == b);   // Flat is the Chat codec in text mode
}

// Design section 5 item 9: one class per signal; a completed answer that holds calls is ToolUse
// whatever its label; a cut, full or refused answer runs nothing.
void test_the_stop_values_map_by_the_table() {
  const char* text = R"({"role":"assistant","content":"Hi."})";
  TEST_ASSERT_EQUAL_STRING("end", plain::stopSlug(parsed(text, "stop").stop));
  TEST_ASSERT_EQUAL_STRING("tool", plain::stopSlug(parsed(kCall, "tool_calls").stop));
  TEST_ASSERT_EQUAL_STRING("tool", plain::stopSlug(parsed(kCall, "stop").stop));
  TEST_ASSERT_EQUAL_STRING("end", plain::stopSlug(parsed(text, "tool_calls").stop));
  TEST_ASSERT_EQUAL_STRING("cut", plain::stopSlug(parsed(text, "length").stop));
  TEST_ASSERT_EQUAL_STRING("context", plain::stopSlug(parsed(text, "model_length").stop));
  TEST_ASSERT_EQUAL_STRING("context", plain::stopSlug(parsed(text, "model_context_window_exceeded").stop));
  TEST_ASSERT_EQUAL_STRING("refusal", plain::stopSlug(parsed(text, "content_filter").stop));
  TEST_ASSERT_EQUAL_STRING("refusal", plain::stopSlug(parsed(text, "sensitive").stop));
  TEST_ASSERT_EQUAL_STRING("other", plain::stopSlug(parsed(text, "network_error").stop));
  TEST_ASSERT_EQUAL_STRING("other", plain::stopSlug(parsed(text, nullptr).stop));
  const plain::ParsedStep cut = parsed(kCall, "length");
  TEST_ASSERT_EQUAL(1, (int)cut.calls.size());
  TEST_ASSERT_FALSE(cut.calls[0].complete);
  const plain::ParsedStep refused = parsed(kCall, "sensitive");
  TEST_ASSERT_EQUAL(0, (int)refused.calls.size());
  TEST_ASSERT_EQUAL_STRING("sensitive", refused.refusal.c_str());
  const plain::ParsedStep openai = parsed(R"({"role":"assistant","content":null,"refusal":"I can't help with that."})", "stop");
  TEST_ASSERT_EQUAL_STRING("refusal", plain::stopSlug(openai.stop));
  TEST_ASSERT_EQUAL_STRING("I can't help with that.", openai.refusal.c_str());
}

// Arguments as the documented JSON string or as Z.ai's object; a string that is not one object is a
// call that never runs, kept as written (the engine stores its stub).
void test_arguments_as_a_string_or_an_object() {
  const plain::ParsedStep s = parsed(kCall, "tool_calls");
  TEST_ASSERT_TRUE(s.calls[0].complete);
  TEST_ASSERT_EQUAL_STRING(R"({"path":"a.md"})", s.calls[0].argsJson.c_str());
  const plain::ParsedStep o = parsed(
      R"({"role":"assistant","tool_calls":[{"id":"c1","type":"function","function":{"name":"read_file","arguments":{"path":"a.md"}}}]})",
      "tool_calls");
  TEST_ASSERT_TRUE(o.calls[0].complete);
  TEST_ASSERT_EQUAL_STRING(R"({"path":"a.md"})", o.calls[0].argsJson.c_str());
  const plain::ParsedStep cut = parsed(
      R"({"role":"assistant","tool_calls":[{"id":"c1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a."}}]})",
      "tool_calls");
  TEST_ASSERT_FALSE(cut.calls[0].complete);
  TEST_ASSERT_EQUAL_STRING(R"({"path":"a.)", cut.calls[0].argsJson.c_str());
  agent::HttpResponse resp;   // a call with no id cannot be answered: not an answer
  resp.status = 200;
  resp.body = R"({"choices":[{"message":{"role":"assistant","tool_calls":[{"function":{"name":"x","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})";
  plain::ParsedStep step;
  TEST_ASSERT_FALSE(chat().parse(resp, step).ok());
}

// prompt_tokens includes the cached tokens: promptTokens is the uncached part, maxRequestIn the whole.
void test_usage_is_the_uncached_part_plus_the_cache_reads() {
  const plain::ParsedStep s = parsed(R"({"role":"assistant","content":"Hi."})", "stop",
                                     R"({"prompt_tokens":1200,"completion_tokens":40,"total_tokens":1240,"prompt_tokens_details":{"cached_tokens":1024}})");
  TEST_ASSERT_EQUAL(176, (int)s.usage.promptTokens);
  TEST_ASSERT_EQUAL(1024, (int)s.usage.cacheReadTokens);
  TEST_ASSERT_EQUAL(40, (int)s.usage.completionTokens);
  TEST_ASSERT_EQUAL(1200, (int)s.usage.maxRequestIn);
  TEST_ASSERT_EQUAL_STRING("m", s.usage.servedModel.c_str());
}

// The one switch: a Mistral thinking chunk goes back inside Mistral's content list, Z.ai's reasoning
// beside Z.ai's content, and neither reaches the other (nor Anthropic's signed thinking either).
void test_reasoning_replays_only_to_its_own_kind() {
  const std::string chunk = R"({"type":"thinking","thinking":[{"type":"text","text":"Read the note."}]})";
  const std::string zai = R"({"reasoning_content":"Read the note."})";
  const std::string signed_ = R"({"type":"thinking","thinking":"Read the note.","signature":"c2ln"})";
  TEST_ASSERT_TRUE(renderOn(caps::Slot::Mistral, reasoningTurn(chunk)).find(R"("content":[{"type":"thinking")") != std::string::npos);
  TEST_ASSERT_TRUE(renderOn(caps::Slot::Zai, reasoningTurn(zai)).find(R"("reasoning_content":"Read the note.")") != std::string::npos);
  for (const std::string& b : {renderOn(caps::Slot::Mistral, reasoningTurn(zai)), renderOn(caps::Slot::Mistral, reasoningTurn(signed_)),
                               renderOn(caps::Slot::Zai, reasoningTurn(chunk)), renderOn(caps::Slot::Zai, reasoningTurn(signed_))}) {
    TEST_ASSERT_TRUE_MESSAGE(b.find("Read the note.") == std::string::npos, b.c_str());
    TEST_ASSERT_TRUE(b.find("Reading the note.") != std::string::npos);   // the round itself is sent
  }
  // Parsed back: the chunk and the reasoning are each one MetaPart of the stored form.
  const plain::ParsedStep m = parsed(R"({"role":"assistant","content":[)" + chunk + R"(,{"type":"text","text":"Bed 2."}]})", "stop");
  TEST_ASSERT_EQUAL_STRING("Bed 2.", m.text.c_str());
  TEST_ASSERT_EQUAL(1, (int)m.meta.size());
  TEST_ASSERT_EQUAL_STRING(chunk.c_str(), m.meta[0].bytes.c_str());
  const plain::ParsedStep z = parsed(R"({"role":"assistant","content":"Bed 2.","reasoning_content":"Read the note."})", "stop");
  TEST_ASSERT_EQUAL(1, (int)z.meta.size());
  TEST_ASSERT_EQUAL_STRING(zai.c_str(), z.meta[0].bytes.c_str());
}

// Text mode (Flat, or a row with the tools bit off): no tools and no tool_choice; a stored call is
// refused until the planner renders history as text (lane R4b), never sent with the round left out.
void test_text_mode_sends_no_tools() {
  txb::RenderFixture fx(rowOf(caps::Slot::CustomChat));
  txb::Convo c;
  c.openTurn();
  c.owner("Hello.");
  txb::Rendered out;
  TXB_OK(txb::renderItems(fx, c, 0, out, c.turn));
  TEST_ASSERT_TRUE(out.body.find("\"tools\"") == std::string::npos);
  TEST_ASSERT_TRUE(out.body.find("\"tool_choice\"") == std::string::npos);
  wire_shape::Request q;
  q.path = out.req.path;
  for (const auto& h : out.req.headers) q.headers.emplace_back(h.first, h.second);
  q.body = out.body;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(wire_shape::chatRequest(q, wire_shape::ChatFlavor::Custom, why), why.c_str());
  const std::string id = c.call("read_file", "{}");
  c.receipt(id, "read_file", "x");
  const tx::Status st = txb::renderItems(fx, c, 0, out, c.turn);
  TEST_ASSERT_FALSE(st.ok());
  TEST_ASSERT_EQUAL_STRING("plain::chat.render.textModeCalls", st.where);
}

// Design D2: the codec reads the route; it names no provider host, window, cap or price.
void test_the_codec_names_no_host() {
  std::ifstream f("lib/harness/src/providers/plain_wire_chat.cpp");
  TEST_ASSERT_TRUE(f.good());
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string src = ss.str();
  for (const char* host : {"mistral.ai", "z.ai\"", "bigmodel", "openai.com", "anthropic.com", "\"mistral\"", "\"zai\"", "\"custom\""})
    TEST_ASSERT_TRUE_MESSAGE(src.find(host) == std::string::npos, host);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_wire_for_chat_and_flat_give_the_codec);
  RUN_TEST(test_the_stop_values_map_by_the_table);
  RUN_TEST(test_arguments_as_a_string_or_an_object);
  RUN_TEST(test_usage_is_the_uncached_part_plus_the_cache_reads);
  RUN_TEST(test_reasoning_replays_only_to_its_own_kind);
  RUN_TEST(test_text_mode_sends_no_tools);
  RUN_TEST(test_the_codec_names_no_host);
  return UNITY_END();
}
