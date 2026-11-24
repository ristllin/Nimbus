#pragma once
// parse_tests.h - the parse half of the Anthropic wire (lane RB-W1b), included by main.cpp after
// its rig. Answers replayed from recordings of the live API (test/support/fixtures/
// anthropic_wire_2026-11, claude-haiku-5-5, ids scrubbed) and from the documented shapes the API
// would not give on request (a refusal, interleaved thinking, an empty content array: files named
// synthetic_*). The class rules:
//   - every recorded shape reads into the fields the loop consumes (text, calls, meta, stop, usage
//     with the cache split, the served model);
//   - a body cut by the transport at ANY byte, a malformed or an error body fills nothing;
//   - a call whose arguments may be cut is never complete; a refused answer runs nothing;
//   - signed thinking is kept byte for byte where it sat, and only when its k line fits;
//   - parse, store, render: the next request carries the block as served;
//   - a refused allocation at every site answers a status with nothing filled (ruling 30).

namespace parse_tests {

const char* const kDir = "test/support/fixtures/anthropic_wire_2026-11/";

std::string fixture(const std::string& name) {
  std::string s;
  FILE* f = std::fopen((std::string(kDir) + name).c_str(), "rb");
  if (!f) {
    TEST_FAIL_MESSAGE(("missing fixture " + name).c_str());
    return s;
  }
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

const plain::WireCodec& wire() {
  const plain::WireCodec* w = nullptr;
  TXB_OK(plain::wireFor(plain::Dialect::AnthropicMessages, w));   // the complete wire
  TEST_ASSERT_NOT_NULL(w);
  return *w;
}

tx::Status parseBody(const std::string& body, plain::ParsedStep& out, int status = 200) {
  agent::HttpResponse r;
  r.status = status;
  r.body = body;
  return wire().parse(r, out);
}

bool empty(const plain::ParsedStep& s) {
  return s.text.empty() && s.calls.empty() && s.meta.empty() && s.refusal.empty() && s.stop == plain::StopReason::Other &&
         s.usage.responses == 0 && s.usage.promptTokens == 0 && s.usage.servedModel.empty();
}

// ---- 1. every recorded shape ----------------------------------------------------------------------
void test_parse_reads_every_recorded_answer_shape() {
  plain::ParsedStep s;
  // Text after signed thinking, end_turn (a warm turn: most of the prompt read from the cache).
  const std::string text = fixture("text_end_turn.json");
  TXB_OK(parseBody(text, s));
  TEST_ASSERT_EQUAL_STRING("You should bring the blue folder.", s.text.c_str());
  TEST_ASSERT_TRUE(s.stop == plain::StopReason::EndTurn);
  TEST_ASSERT_EQUAL(0, (int)s.calls.size());
  TEST_ASSERT_EQUAL(1, (int)s.meta.size());
  TEST_ASSERT_EQUAL(0, (int)s.meta[0].beforeCall);
  TEST_ASSERT_EQUAL(4, (int)s.usage.promptTokens);
  TEST_ASSERT_EQUAL(8145, (int)s.usage.cacheReadTokens);
  TEST_ASSERT_EQUAL(81, (int)s.usage.cacheWriteTokens);
  TEST_ASSERT_EQUAL(0, (int)s.usage.cacheWrite1hTokens);
  TEST_ASSERT_EQUAL(89, (int)s.usage.completionTokens);
  TEST_ASSERT_EQUAL(1, (int)s.usage.responses);
  TEST_ASSERT_EQUAL(4 + 8145 + 81, (int)s.usage.maxRequestIn);
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", s.usage.servedModel.c_str());
  // Two calls in one answer (parallel), no text, the cold request that wrote the cache.
  TXB_OK(parseBody(fixture("tool_use_two.json"), s));
  TEST_ASSERT_TRUE(s.stop == plain::StopReason::ToolUse);
  TEST_ASSERT_EQUAL(2, (int)s.calls.size());
  TEST_ASSERT_TRUE(s.text.empty() && s.meta.empty());
  TEST_ASSERT_EQUAL_STRING("read_file", s.calls[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING(R"({"path":"notes/alpha.md"})", s.calls[0].argsJson.c_str());
  TEST_ASSERT_EQUAL_STRING(R"({"path":"notes/beta.md"})", s.calls[1].argsJson.c_str());
  TEST_ASSERT_TRUE(s.calls[0].complete && s.calls[1].complete);
  TEST_ASSERT_TRUE(s.calls[0].id != s.calls[1].id && s.calls[0].id.rfind("toolu_", 0) == 0);
  TEST_ASSERT_EQUAL(7867, (int)s.usage.cacheWriteTokens);
  TEST_ASSERT_EQUAL(0, (int)s.usage.cacheReadTokens);
  // One call.
  TXB_OK(parseBody(fixture("tool_use_one.json"), s));
  TEST_ASSERT_EQUAL(1, (int)s.calls.size());
  TEST_ASSERT_EQUAL_STRING("calc", s.calls[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING(R"({"expression":"17 * 23"})", s.calls[0].argsJson.c_str());
  // Signed thinking before a call: kept before call 0, byte for byte as served.
  const std::string tc = fixture("thinking_then_call.json");
  TXB_OK(parseBody(tc, s));
  TEST_ASSERT_TRUE(s.stop == plain::StopReason::ToolUse);
  TEST_ASSERT_EQUAL(1, (int)s.calls.size());
  TEST_ASSERT_EQUAL(1, (int)s.meta.size());
  TEST_ASSERT_EQUAL(0, (int)s.meta[0].beforeCall);
  TEST_ASSERT_TRUE(tc.find(s.meta[0].bytes) != std::string::npos);
  TEST_ASSERT_TRUE(s.meta[0].bytes.rfind("{\"type\":\"thinking\"", 0) == 0);
  TEST_ASSERT_TRUE(s.meta[0].bytes.find("\"signature\":\"") != std::string::npos);
  // Thinking after the round's results, then the text (interleaved across requests).
  TXB_OK(parseBody(fixture("thinking_then_text.json"), s));
  TEST_ASSERT_EQUAL(1, (int)s.meta.size());
  TEST_ASSERT_TRUE(s.text.find("Thursday") != std::string::npos);
  // max_tokens with a cut call: the API closed the input object, the content is missing.
  TXB_OK(parseBody(fixture("max_tokens_cut_call.json"), s));
  TEST_ASSERT_TRUE(s.stop == plain::StopReason::MaxTokens);
  TEST_ASSERT_EQUAL(1, (int)s.calls.size());
  TEST_ASSERT_EQUAL_STRING(R"({"path":"notes/poem.md"})", s.calls[0].argsJson.c_str());
  TEST_ASSERT_FALSE_MESSAGE(s.calls[0].complete, "a call of a cut answer must never run");
  // The thinking-binding controls' answer: input_transformations is read past.
  TXB_OK(parseBody(fixture("input_transformations_dropped.json"), s));
  TEST_ASSERT_TRUE(s.stop == plain::StopReason::ToolUse);
}

// ---- 2. the documented shapes the API does not give on request ----------------------------------
void test_parse_reads_a_refusal_interleaved_thinking_and_an_empty_answer() {
  plain::ParsedStep s;
  // A refusal: the text kept, every call dropped, the reason from stop_details.
  TXB_OK(parseBody(fixture("synthetic_refusal.json"), s));
  TEST_ASSERT_TRUE(s.stop == plain::StopReason::Refusal);
  TEST_ASSERT_EQUAL(0, (int)s.calls.size());
  TEST_ASSERT_EQUAL_STRING("I can't help with that part.", s.text.c_str());
  TEST_ASSERT_EQUAL_STRING("The request falls under a declined category.", s.refusal.c_str());
  // Thinking between calls of one answer: each before the call it preceded, the last after them.
  TXB_OK(parseBody(fixture("synthetic_interleaved.json"), s));
  TEST_ASSERT_EQUAL(2, (int)s.calls.size());
  TEST_ASSERT_EQUAL(3, (int)s.meta.size());
  TEST_ASSERT_EQUAL(0, (int)s.meta[0].beforeCall);
  TEST_ASSERT_EQUAL(1, (int)s.meta[1].beforeCall);
  TEST_ASSERT_EQUAL(2, (int)s.meta[2].beforeCall);
  TEST_ASSERT_TRUE(s.meta[2].bytes.find("redacted_thinking") != std::string::npos);
  TEST_ASSERT_EQUAL(1200, (int)s.usage.cacheWrite1hTokens);
  // An empty content array: a step with nothing in it, the loop's "empty" case.
  TXB_OK(parseBody(fixture("synthetic_empty_content.json"), s));
  TEST_ASSERT_TRUE(s.stop == plain::StopReason::EndTurn);
  TEST_ASSERT_TRUE(s.text.empty() && s.calls.empty() && s.meta.empty());
  // Text blocks join in order, escapes decode (every JSON escape, a surrogate pair), counts saturate.
  TXB_OK(parseBody(R"({"type":"message","content":[{"type":"text","text":"a\/b\\c\"d\b\f\n\r\t"},)"
                   R"({"type":"text","text":" \u00e9\ud83d\ude00"}],"stop_reason":"end_turn",)"
                   R"("usage":{"input_tokens":99999999999,"cache_read_input_tokens":4294967295,"output_tokens":null}})",
                   s));
  TEST_ASSERT_EQUAL_STRING("a/b\\c\"d\b\f\n\r\t \xC3\xA9\xF0\x9F\x98\x80", s.text.c_str());
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.usage.promptTokens);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.usage.cacheReadTokens);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.usage.maxRequestIn);
  TEST_ASSERT_EQUAL(0, (int)s.usage.completionTokens);
  // Other stops: the context window is full (lane E2-R4a), a stop sequence ends the turn, pause and unknown are Other.
  const std::string base = R"({"type":"message","role":"assistant","model":"m","content":[{"type":"text","text":"t"},)"
                           R"({"type":"tool_use","id":"toolu_1","name":"read_file","input":{"path":"a"}}],"stop_reason":)";
  const char* stops[][2] = {{"\"model_context_window_exceeded\"", "context"}, {"\"stop_sequence\"", "end"},
                            {"\"pause_turn\"", "other"}, {"\"a_new_reason\"", "other"}, {"null", "other"},
                            {"\"tool_use\"", "tool"}, {"\"end_turn\"", "end"}};
  for (const auto& st : stops) {
    TXB_OK(parseBody(base + st[0] + R"(,"usage":{"input_tokens":1,"output_tokens":1}})", s));
    TEST_ASSERT_EQUAL_STRING(st[1], plain::stopSlug(s.stop));
    TEST_ASSERT_EQUAL(1, (int)s.calls.size());
    TEST_ASSERT_EQUAL_MESSAGE(std::string(st[1]) == "tool", s.calls[0].complete, st[0]);   // only a stop to call runs
  }
}

// ---- 3. nothing half-read is acted on -------------------------------------------------------------
void test_a_cut_malformed_or_error_body_fills_nothing() {
  plain::ParsedStep s;
  // Cut by the transport at every byte: never an answer.
  for (const char* name : {"tool_use_two.json", "thinking_then_call.json", "text_end_turn.json"}) {
    const std::string body = fixture(name);
    for (size_t n = 0; n < body.size(); ++n) {
      s.text = "stale";
      const tx::Status st = parseBody(body.substr(0, n), s);
      TEST_ASSERT_FALSE_MESSAGE(st.ok(), ("a body cut at " + std::to_string(n) + " parsed: " + name).c_str());
      TEST_ASSERT_TRUE(empty(s));
    }
    TXB_OK(parseBody(body + "\n", s));   // trailing white space is still the whole body
  }
  // The API's error body (recorded), as a 404 and as if it came with a 2xx.
  const std::string err = fixture("error_not_found.json");
  TEST_ASSERT_FALSE(parseBody(err, s, 404).ok());
  TEST_ASSERT_FALSE(parseBody(err, s, 200).ok());
  TEST_ASSERT_TRUE(empty(s));
  // A good body with a status outside 2xx.
  TEST_ASSERT_FALSE(parseBody(fixture("text_end_turn.json"), s, 500).ok());
  // Malformed bodies: each must fail and fill nothing.
  const std::string ok = R"({"type":"message","content":[{"type":"text","text":"hi"}],"stop_reason":"end_turn","usage":{"input_tokens":1}})";
  TXB_OK(parseBody(ok, s));   // the counter-case: the well-formed one reads
  const std::vector<std::string> bad = {
      "",
      "{}",
      "[]",
      R"({"type":"message","stop_reason":"end_turn"})",                                    // no content
      R"({"type":"error","content":[],"stop_reason":"end_turn"})",                         // an error type
      R"({"content":[],"stop_reason":"end_turn"})",                                       // no type
      R"({"type":"message","content":[],"content":[],"stop_reason":"end_turn"})",          // content twice
      R"({"type":"message","type":"message","content":[]})",                               // type twice
      R"({"type":"message","content":{},"stop_reason":"end_turn"})",                       // content not an array
      R"({"type":"message","content":["text"],"stop_reason":"end_turn"})",                 // a block not an object
      R"({"type":"message","content":[{"type":"text","text":1}]})",                        // text not a string
      R"({"type":"message","content":[{"type":"text","text":"a","text":"b"}]})",           // a field twice
      R"({"type":"message","content":[{"type":"tool_use","name":"read_file","input":{}}]})",            // no id
      R"({"type":"message","content":[{"type":"tool_use","id":"toolu_1","input":{}}]})",                // no name
      R"({"type":"message","content":[{"type":"tool_use","id":"","name":"x","input":{}}]})",            // blank id
      R"({"type":"message","content":[],"usage":{"input_tokens":-1}})",                                 // negative count
      R"({"type":"message","content":[],"usage":{"input_tokens":1.5}})",                                // fraction
      R"({"type":"message","content":[],"usage":{"input_tokens":"1"}})",                                // a string count
      R"({"type":"message","content":[{"type":"text","text":"a)" "\x01" R"("}]})",                      // raw control byte
      R"({"type":"message","content":[{"type":"text","text":"\ud800"}]})",                                 // lone surrogate
      R"({"type":"message","content":[{"type":"text","text":"\udc00x"}]})",                                // lone low half
      R"({"type":"message","content":[{"type":"text","text":"\ud800\u0041"}]})",                          // high half, no low
      R"({"type":"message","content":[],"x":{"a" 1}})",                                                     // a key without its colon
      R"({"type":"message","content":[{"type":"text","text":"\q"}]})",                                     // bad escape
      R"({"type":"message","content":[{"type":"text","text":"\u12"}]})",                                  // short escape
      R"({"type":"message","content":[{"type":"text","text":"a"}]} x)",                                    // trailing bytes
      R"({"type":"message","content":[{"type":"text","text":"a"},]})",                                     // trailing comma
      R"({"type":"message","content":[{"type":"text","text":"a"}],})",
      R"({"type":"message","content":[],"usage":{"input_tokens":01}})",                                    // leading zero
      R"({"type":"message","content":[],"x":tru})",
      R"({type:"message","content":[]})",
      std::string(R"({"type":"message","content":[{"type":"text","text":")") + "\xC3\x28" + R"("}]})",    // bad UTF-8
  };
  for (size_t i = 0; i < bad.size(); ++i) {
    s.text = "stale";
    const tx::Status st = parseBody(bad[i], s);
    TEST_ASSERT_FALSE_MESSAGE(st.ok(), ("malformed body " + std::to_string(i) + " parsed").c_str());
    TEST_ASSERT_EQUAL(tx::Err::InvalidArgument, st.err);
    TEST_ASSERT_TRUE(empty(s));
  }
  // Nesting: 64 levels read, 65 do not.
  auto nested = [](int depth) {
    std::string v = "1";
    for (int i = 2; i < depth; ++i) v = "[" + v + "]";
    return R"({"type":"message","content":[],"x":)" + v + "}";
  };
  TXB_OK(parseBody(nested(64), s));
  TEST_ASSERT_FALSE(parseBody(nested(66), s).ok());
}

// ---- 4. calls ---------------------------------------------------------------------------------------
void test_a_call_is_complete_only_with_an_object_input_and_never_in_a_cut() {
  plain::ParsedStep s;
  auto one = [&s](const std::string& input, const char* stop) {
    TXB_OK(parseBody(std::string(R"({"type":"message","content":[{"type":"tool_use","id":"toolu_9","name":"note","input":)") +
                         input + R"(}],"stop_reason":")" + stop + R"("})",
                     s));
    TEST_ASSERT_EQUAL(1, (int)s.calls.size());
  };
  one(R"({"text":"a \"quoted\" é line\n","n":[1,2.5e3,{"deep":null}]})", "tool_use");
  TEST_ASSERT_TRUE(s.calls[0].complete);
  TEST_ASSERT_EQUAL_STRING(R"({"text":"a \"quoted\" é line\n","n":[1,2.5e3,{"deep":null}]})", s.calls[0].argsJson.c_str());
  for (const char* in : {"[]", "\"text\"", "null", "3"}) {   // not an object: answered, never run
    one(in, "tool_use");
    TEST_ASSERT_FALSE(s.calls[0].complete);
    TEST_ASSERT_EQUAL_STRING("{}", s.calls[0].argsJson.c_str());
  }
  one(R"({"path":"a"})", "max_tokens");
  TEST_ASSERT_FALSE(s.calls[0].complete);
  // A refused answer runs nothing, whatever it held.
  TXB_OK(parseBody(R"({"type":"message","content":[{"type":"tool_use","id":"toolu_9","name":"note","input":{}}],)"
                   R"("stop_reason":"refusal","stop_details":{"type":"refusal","category":"cyber","explanation":null}})",
                   s));
  TEST_ASSERT_EQUAL(0, (int)s.calls.size());
  TEST_ASSERT_EQUAL_STRING("cyber", s.refusal.c_str());
  TXB_OK(parseBody(R"({"type":"message","content":[],"stop_reason":"refusal","stop_details":null})", s));
  TEST_ASSERT_EQUAL_STRING("refusal", s.refusal.c_str());
  // A JSON string over 65,535 B (ArduinoJson's limit) still reads whole.
  const std::string big(70000, 'w');
  one("{\"text\":\"" + big + "\"}", "tool_use");
  TEST_ASSERT_TRUE(s.calls[0].complete);
  TEST_ASSERT_EQUAL(70000 + 11, (int)s.calls[0].argsJson.size());
}

// ---- 5. signed thinking -----------------------------------------------------------------------------
std::string thinkingBlock(size_t sigBytes, const std::string& text = "") {
  std::string sig;
  for (size_t i = 0; i < sigBytes; ++i) sig += "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i % 64];
  return R"({"type":"thinking","thinking":")" + text + R"(","signature":")" + sig + "\"}";
}
bool lineFits(const std::string& bytes) {
  tx::Item k;
  k.kind = tx::ItemKind::RoundMeta;
  k.q = 999999999999ULL;
  k.turn = UINT32_MAX;
  k.t = 9999999999ULL;
  k.rd = 255;
  k.x = bytes;
  std::string line;
  return tx::encodeLine(k, line).ok();
}

void test_signed_thinking_is_kept_byte_for_byte_only_when_its_line_fits() {
  plain::ParsedStep s;
  // Unsigned or wrongly typed blocks are not replayable: not kept; the answer still reads.
  TXB_OK(parseBody(R"({"type":"message","content":[{"type":"thinking","thinking":"x","signature":""},)"
                   R"({"type":"thinking","thinking":"x"},{"type":"thinking","thinking":null,"signature":"s"},)"
                   R"({"type":"redacted_thinking","data":""},{"type":"redacted_thinking"},{"type":"text","text":"ok"}],)"
                   R"("stop_reason":"end_turn"})",
                   s));
  TEST_ASSERT_EQUAL(0, (int)s.meta.size());
  TEST_ASSERT_EQUAL_STRING("ok", s.text.c_str());
  // Kept exactly as served, white space and escapes included (the signature binds the bytes).
  const std::string spaced = "{ \"type\" : \"thinking\", \"thinking\": \"a\\u00e9\\n\", \"signature\" : \"EqQB\" }";
  TXB_OK(parseBody(R"({"type":"message","content":[)" + spaced + R"(],"stop_reason":"end_turn"})", s));
  TEST_ASSERT_EQUAL(1, (int)s.meta.size());
  TEST_ASSERT_EQUAL_STRING(spaced.c_str(), s.meta[0].bytes.c_str());
  // The line limit, swept around the boundary: kept exactly when its k line fits.
  size_t lo = 59000, hi = 61000;
  while (hi - lo > 1) {   // the largest signature that still fits
    const size_t mid = (lo + hi) / 2;
    (lineFits(thinkingBlock(mid)) ? lo : hi) = mid;
  }
  for (size_t n = lo - 3; n <= lo + 3; ++n) {
    TXB_OK(parseBody(R"({"type":"message","content":[)" + thinkingBlock(n) +
                         R"(,{"type":"tool_use","id":"toolu_1","name":"read_file","input":{}}],"stop_reason":"tool_use"})",
                     s));
    TEST_ASSERT_EQUAL_MESSAGE(n <= lo ? 1 : 0, (int)s.meta.size(), std::to_string(n).c_str());
    TEST_ASSERT_EQUAL(1, (int)s.calls.size());   // the call is kept either way
  }
  // Two real blocks at effort max: 23,217 thinking tokens gave a 57,600 B signature (kept, and the
  // next request carrying it was accepted live); 64,000 tokens gave 157 KB, past the line and past
  // ArduinoJson's 65,535 B string limit: that answer still reads, its block is not kept.
  TXB_OK(parseBody(fixture("thinking_near_the_line_call.json"), s));
  TEST_ASSERT_EQUAL(1, (int)s.meta.size());
  TEST_ASSERT_EQUAL(1, (int)s.calls.size());
  TEST_ASSERT_TRUE(s.meta[0].bytes.size() > 57000);
  std::string scratch;
  plain::body::BodyWriter bw(scratch);
  bw.raw(s.meta[0].bytes);   // the render's writer takes it whole (every string under the guard)
  TXB_OK(bw.status());
  TXB_OK(parseBody(fixture("thinking_over_the_line_cut.json"), s));
  TEST_ASSERT_TRUE(s.stop == plain::StopReason::MaxTokens);
  TEST_ASSERT_EQUAL(0, (int)s.meta.size());
  TEST_ASSERT_EQUAL(64000, (int)s.usage.completionTokens);
  // Escapes count as the line stores them: a thinking text of quotes needs more room.
  TXB_OK(parseBody(R"({"type":"message","content":[)" + thinkingBlock(100, std::string(30000, '\'')) + R"(],"stop_reason":"end_turn"})", s));
  TEST_ASSERT_EQUAL(1, (int)s.meta.size());
  std::string quotes;
  for (int i = 0; i < 15000; ++i) quotes += "\\\"";
  TXB_OK(parseBody(R"({"type":"message","content":[)" + thinkingBlock(100, quotes) + R"(],"stop_reason":"end_turn"})", s));
  TEST_ASSERT_EQUAL(lineFits(thinkingBlock(100, quotes)) ? 1 : 0, (int)s.meta.size());
}

// Parse, store as the engine does, render: the next request carries the block as served and
// passes the checker; with a rung applied it goes without it (sendOpenTurnThinking).
void test_parsed_thinking_rides_the_next_request_of_its_turn_as_served() {
  plain::ParsedStep s;
  const std::string body = fixture("thinking_then_call.json");
  TXB_OK(parseBody(body, s));
  Rig rig;
  txb::Convo c;
  c.ownerTurn("earlier", "earlier answer");
  c.openTurn();
  c.owner("which file mentions Thursday?");
  for (const plain::MetaPart& m : s.meta) c.add(tx::ItemKind::RoundMeta).x = m.bytes;
  for (const plain::ToolCall& call : s.calls) {
    tx::Item& it = c.add(tx::ItemKind::Call);
    it.id = call.id;
    it.n = call.name;
    it.args = call.argsJson;
  }
  for (const plain::ToolCall& call : s.calls) c.receipt(call.id, call.name, "line one");
  c.nextRound();
  Out out;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "parsed thinking replayed");
  TEST_ASSERT_TRUE(out.req.body.find(s.meta[0].bytes) != std::string::npos);
  TEST_ASSERT_TRUE(out.req.body.find(s.calls[0].argsJson) != std::string::npos);
}

// Thinking is kept only while every block before it comes back byte for byte (the engine stores
// a round as [thinking before the first call][one text][call, thinking]...): the first block that
// would come back otherwise ends the run, and no later thinking of the answer is kept.
void test_thinking_is_kept_only_while_the_round_replays_byte_for_byte() {
  plain::ParsedStep s;
  const std::string t1 = R"({"type":"thinking","thinking":"","signature":"sig1"})";
  const std::string t2 = R"({"type":"thinking","thinking":"","signature":"sig2"})";
  const std::string r3 = R"({"type":"redacted_thinking","data":"dat3"})";
  auto call = [](int n) {
    return R"({"type":"tool_use","id":"toolu_)" + std::to_string(n) + R"(","name":"read_file","input":{"path":"a"}})";
  };
  auto text = [](const std::string& t) { return R"({"type":"text","text":")" + t + R"("})"; };
  struct Case {
    std::vector<std::string> blocks;
    size_t kept;
  };
  const std::string tooBig = thinkingBlock(61000);
  const std::string unsignedT = R"({"type":"thinking","thinking":"x","signature":""})";
  const std::string crossed = R"({"type":"thinking","thinking":"x","signature":"","data":"d"})";
  const std::string crossedR = R"({"type":"redacted_thinking","data":"","signature":"s"})";
  const Case cases[] = {
      {{t1, text("a"), call(1), t2, call(2), r3}, 3},           // the engine's own order: all kept
      {{t1, call(1), t2, call(2)}, 2},
      {{text("a"), t1, call(1)}, 0},                             // thinking after text: stored before it
      {{t1, text("a"), text("b"), t2, call(1)}, 1},              // two text blocks come back as one
      {{t1, call(1), text("late"), t2, call(2)}, 1},             // text after a call comes back before it
      {{t1, tooBig, t2, call(1)}, 1},                            // a block not kept ends the run
      {{t1, unsignedT, t2, call(1)}, 1},
      {{crossed, t2}, 0},                                        // a signature belongs to thinking,
      {{crossedR, t2}, 0},                                       // data to redacted thinking
      {{t1, R"({"type":"server_tool_use","id":"srv_1","name":"x","input":{}})", t2, call(1)}, 1},
      {{t1, text(std::string(12289, 'x')), call(1), t2, call(2)}, 1},   // text the engine stores as its stub
      {{R"({"type":"thinking","thinking":null,"signature":"s"})", t2, call(1)}, 0},   // a thinking text that is not one
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    std::string body = R"({"type":"message","role":"assistant","content":[)";
    for (size_t k = 0; k < cases[i].blocks.size(); ++k) body += (k ? "," : "") + cases[i].blocks[k];
    body += R"(],"stop_reason":"tool_use"})";
    TXB_OK(parseBody(body, s));
    TEST_ASSERT_EQUAL_MESSAGE((int)cases[i].kept, (int)s.meta.size(), ("case " + std::to_string(i)).c_str());
  }
}

// A call runs only when its answer stops to call and its input comes back as written: an object,
// nested no deeper than the render takes, each key once. Any other call is answered, never run.
void test_a_call_runs_only_as_written_in_an_answer_that_stops_to_call() {
  plain::ParsedStep s;
  auto one = [&s](const std::string& input) {
    TXB_OK(parseBody(R"({"type":"message","content":[{"type":"tool_use","id":"toolu_1","name":"note","input":)" + input +
                         R"(}],"stop_reason":"tool_use"})",
                     s));
    TEST_ASSERT_EQUAL(1, (int)s.calls.size());
    return s.calls[0].complete;
  };
  auto nest = [](int levels) {   // an input object `levels` deep in all
    std::string v = "1";
    for (int i = 1; i < levels; ++i) v = "[" + v + "]";
    return "{\"a\":" + v + "}";
  };
  TEST_ASSERT_TRUE(one(nest(32)));
  TEST_ASSERT_FALSE(one(nest(33)));
  TEST_ASSERT_TRUE(one(R"({"path":"a","more":{"path":"b"}})"));   // a key again inside a nested object
  TEST_ASSERT_FALSE(one(R"({"path":"a","path":"b"})"));
  // usage given as null, a count given twice, a role that is not the assistant's
  TXB_OK(parseBody(R"({"type":"message","content":[],"stop_reason":"end_turn","usage":null})", s));
  TEST_ASSERT_EQUAL(0, (int)s.usage.promptTokens);
  for (const char* bad : {R"({"type":"message","content":[],"usage":{"input_tokens":1,"input_tokens":2}})",
                          R"({"type":"message","content":[],"usage":{"cache_creation":{"ephemeral_1h_input_tokens":1,"ephemeral_1h_input_tokens":2}}})",
                          R"({"type":"message","role":"user","content":[]})", R"({"type":"message","role":"assistant","role":"assistant","content":[]})"})
    TEST_ASSERT_FALSE_MESSAGE(parseBody(bad, s).ok(), bad);
  // Deep values anywhere are read without recursion: 64 levels read, more refuse the body.
  std::string deep = "1";
  for (int i = 0; i < 60; ++i) deep = "{\"k\":[" + deep + "]}";
  TEST_ASSERT_FALSE(parseBody(R"({"type":"message","content":[],"x":)" + deep + "}", s).ok());
}

// ---- 5b. usage reaches the e item ----------------------------------------------------------------
// Through the engine on this wire: two requests of one turn, each answer's usage with its cache
// split, summed into the turn's e item (the guard and the spend figures read it there).
void test_usage_with_the_cache_split_reaches_the_e_item() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  sp::Step one = sp::call("read_file", R"({"path":"notes/a.md"})");
  one.usage.input = 7;
  one.usage.output = 40;
  one.usage.cacheRead = 8000;
  one.usage.cacheWrite = 300;
  sp::Step two = sp::say("Alpha.");
  two.usage.input = 3;
  two.usage.output = 12;
  two.usage.cacheRead = 8300;
  two.usage.cacheWrite = 90;
  b.model.push(one);
  b.model.push(two);
  plain::TurnOutcome out;
  TXB_OK(b.owner("read a", out));
  TEST_ASSERT_EQUAL_STRING("Alpha.", out.text.c_str());
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  const tx::Item* e = nullptr;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::TurnEnd) e = &it;
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL(10, (int)e->end.promptTokens);
  TEST_ASSERT_EQUAL(52, (int)e->end.outputTokens);
  TEST_ASSERT_EQUAL(16300, (int)e->end.cacheReadTokens);
  TEST_ASSERT_EQUAL(390, (int)e->end.cacheWriteTokens);
}

// ---- 5c. a photo's file id across turns ------------------------------------------------------------
// A photo kept by file id rides later turns by that id only while the provider still holds it;
// past the idle limit or near the provider's expiry, or once the provider refused it, the same
// render sends the bytes inline, or the stub line when no bytes are at hand. Never a request
// naming an id the provider would refuse, never a lost photo without a line.
void test_a_file_id_rides_later_turns_only_while_the_provider_holds_it() {
  namespace ip = agent::providers::imageparts;
  ip::dropFileRefs();
  const std::string chat = "1001", ref = "pab12cd34", bytes = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(80, 'j');
  uint32_t now = 1000;
  ip::keepFileId(chat, ref, "file_011CNha8iCJcU1wXNR6q4V8w", now);
  bool haveBytes = true;
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.items.back().photos = {ref};
  c.owner("look", {ref});
  auto render = [&](Out& out) {
    tx::TxArena arena;
    TXB_OK(arena.init(1 << 20, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }));
    for (const tx::Item& it : c.items) TXB_OK(arena.append(it));
    plain::TurnCtx turn;
    const Opts opts;   // outlives `in`: in.ladder points into it
    plain::RenderInput in = rig.input(c, arena, opts, turn, 0);
    in.photo = [&](const std::string& r, plain::PhotoBytes& p) {   // the binding RB-P1 writes
      p.fileId = ip::heldFileId(chat, r, now);
      if (haveBytes) {
        p.bytes = (const uint8_t*)bytes.data();
        p.len = bytes.size();
        p.mime = "image/jpeg";
      }
      return p.fileId.empty() && !haveBytes ? tx::Status::fail(tx::Err::NotFound, "no photo") : tx::Status::success();
    };
    TXB_OK(wire().render(in, out.req));
    out.body = agent::materializeBody(out.req);
    sp::parseRequest(sp::Dialect::Anthropic, out.body, out.view);
  };
  Out out;
  render(out);
  EXPECT_API_OK(out, "held id");
  TEST_ASSERT_TRUE(out.body.find("\"file_id\":\"file_011CNha8iCJcU1wXNR6q4V8w\"") != std::string::npos);
  // Used within the idle limit it stays; idle past it, it is never named again: bytes go inline.
  now += ip::kKeptUploadIdleMs - 1000;
  render(out);
  TEST_ASSERT_TRUE(out.body.find("file_011") != std::string::npos);
  now += ip::kKeptUploadIdleMs + 1;
  render(out);
  EXPECT_API_OK(out, "idle id");
  TEST_ASSERT_TRUE(out.body.find("file_011") == std::string::npos);
  TEST_ASSERT_TRUE(out.body.find("\"media_type\":\"image/jpeg\",\"data\":\"") != std::string::npos);
  // Near the provider's expiry, however recently used: not named.
  ip::keepFileId(chat, ref, "file_022", now);
  for (uint32_t step = 0; step < 6; ++step) {
    now += 20u * 60u * 1000u;   // used every 20 minutes: named up to 100 minutes old (2 h less 20 min)
    render(out);
    const bool named = out.body.find("file_022") != std::string::npos;
    TEST_ASSERT_EQUAL_MESSAGE(step < 5, named, std::to_string(step).c_str());
  }
  // Another chat never sees it; the provider's refusal forgets it; with no bytes, the stub line.
  ip::keepFileId(chat, ref, "file_033", now);
  TEST_ASSERT_TRUE(ip::heldFileId("2002", ref, now).empty());
  TEST_ASSERT_FALSE(ip::forgetRefusedFileId(chat, 400, R"({"type":"error","error":{"message":"max_tokens too large"}})"));
  ip::keepFileId("2002", "pother001", "file_other", now);   // another chat's photo stays
  TEST_ASSERT_TRUE(ip::forgetRefusedFileId(chat, 404, R"({"type":"error","error":{"type":"not_found_error","message":"File not found."}})"));
  TEST_ASSERT_EQUAL_STRING("file_other", ip::heldFileId("2002", "pother001", now).c_str());
  TEST_ASSERT_TRUE(ip::heldFileId(chat, ref, now).empty());
  ip::keepFileId(chat, ref, "file_033", now);
  TEST_ASSERT_TRUE(ip::forgetRefusedFileId("2002", 404, R"({"type":"error","error":{"message":"File not found: file_033"}})"));
  TEST_ASSERT_EQUAL_STRING("file_other", ip::heldFileId("2002", "pother001", now).c_str());   // only the one named
  haveBytes = false;
  render(out);
  EXPECT_API_OK(out, "forgotten id, no bytes");
  TEST_ASSERT_TRUE(out.body.find("file_033") == std::string::npos);
  TEST_ASSERT_EQUAL(0, (int)txb::detail::countImages(out.view));
  TEST_ASSERT_TRUE(txb::detail::textIn(out.view, "user", "[photo pab12cd34 "));
  TEST_ASSERT_FALSE(ip::forgetRefusedFileId(chat, 404, "file not found"));   // nothing left to forget
  ip::dropFileRefs();
}

// ---- 6. a refused allocation (ruling 30) -------------------------------------------------------------
void test_a_refused_allocation_in_parse_answers_a_status_and_fills_nothing() {
  const std::string body = fixture("thinking_then_call.json");
  plain::ParsedStep good;
  TXB_OK(parseBody(body, good));
  agent::HttpResponse r;
  r.status = 200;
  r.body = body;
  int refused = 0;
  for (long k = 0;; ++k) {
    plain::ParsedStep s;
    s.text.reserve(64);
    gAllocs = 0;
    gFailAt = k;
    gHit = false;
    gArmed = true;
    const tx::Status st = wire().parse(r, s);
    gArmed = false;
    if (!gHit) {
      TXB_OK(st);
      break;
    }
    ++refused;
    TEST_ASSERT_FALSE(st.ok());
    TEST_ASSERT_EQUAL(tx::Err::TooLarge, st.err);
    TEST_ASSERT_TRUE(empty(s));
  }
  TEST_ASSERT_TRUE(refused > 5);
}

void run() {
  RUN_TEST(test_parse_reads_every_recorded_answer_shape);
  RUN_TEST(test_parse_reads_a_refusal_interleaved_thinking_and_an_empty_answer);
  RUN_TEST(test_a_cut_malformed_or_error_body_fills_nothing);
  RUN_TEST(test_a_call_is_complete_only_with_an_object_input_and_never_in_a_cut);
  RUN_TEST(test_signed_thinking_is_kept_byte_for_byte_only_when_its_line_fits);
  RUN_TEST(test_parsed_thinking_rides_the_next_request_of_its_turn_as_served);
  RUN_TEST(test_thinking_is_kept_only_while_the_round_replays_byte_for_byte);
  RUN_TEST(test_a_call_runs_only_as_written_in_an_answer_that_stops_to_call);
  RUN_TEST(test_usage_with_the_cache_split_reaches_the_e_item);
  RUN_TEST(test_a_file_id_rides_later_turns_only_while_the_provider_holds_it);
  RUN_TEST(test_a_refused_allocation_in_parse_answers_a_status_and_fills_nothing);
}

}  // namespace parse_tests
