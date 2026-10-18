// reply_markup: the class "text that is not the owner's content reaches a
// channel", tested as properties over generated replies (formatting T1, T2) plus
// the two leak tails measured live (17-18 Oct, scrubbed).
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../support/turn_shape_fixtures.h"
#include "nimbus/orch/reply_markup.h"
#include "nimbus/orch/turn.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

const uint32_t kSeed = 0xC0FFEE;
const int kCases = 2000;

// The namespace prefix Claude's tool-call format writes before its tags. Built
// from two pieces so this source never carries the prefixed tag text verbatim.
const std::string kNs = std::string("an") + "tml:";

std::string open(const std::string& name, const std::string& attrs = "", bool ns = false) {
  return "<" + (ns ? kNs : std::string()) + name + attrs + ">";
}
std::string close(const std::string& name, bool ns = false) {
  return "</" + (ns ? kNs : std::string()) + name + ">";
}
std::string param(const std::string& field, bool ns = false) {
  return open("parameter", " name=\"" + field + "\"", ns);
}

// Owner content, including legitimate angle brackets and code that quotes the
// deny-list (it must survive byte-exact).
std::vector<std::string> contentCorpus() {
  return {
      "Here is the plan for today.",
      "Caf\xC3\xA9 opens at nine; na\xC3\xAFve estimate says 20 minutes.",
      "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E is fine too \xF0\x9F\x98\x80",
      "**Bold heading** then text.",
      "Use `memory.search` first.",
      "```\nexample " + open("think") + "kept" + close("think") + " and " + close("parameter") +
          "\n```",
      "Inline `" + open("invoke", " name=\"x\"") + "` stays.",
      "[the docs](https://docs.example.com/a)",
      "# Heading",
      "a < b and b > c",
      "x<y>z",
      "List<String> items",
      "<provider>/<model>",
      "go -> there",
      "I <3 this",
      "&amp; stays",
      "Capabilities > Connectors",
      "the HTML <output> element renders results",
      "Spawn on <provider> with <MMDDHHMM> as the tag.",
      "Line one.\nLine two.",
  };
}

// Injected spans that are never the owner's content.
std::vector<std::string> leakCorpus() {
  return {
      open("think") + "step one, then two" + close("think"),
      open("thinking") + "\nplanning the answer\n" + close("thinking"),
      open("THINK") + "shouted reasoning" + close("THINK"),
      open("reasoning", " effort=\"low\"") + "brief" + close("reasoning"),
      open("function_calls") + open("invoke", " name=\"memory_search\"") + param("q") + "x" +
          close("parameter") + close("invoke") + close("function_calls"),
      open("invoke", " name=\"orch_turn\"", true) + param("reply", true) + "hi" +
          close("parameter", true) + close("invoke", true),
      close("parameter"),
      param("memory"),
      open("tool_call") + "memory.write" + open("arg_key") + "content" + close("arg_key") +
          open("arg_value") + "v" + close("arg_value") + close("tool_call"),
      "<|im_end|>",
      open("orch_turn"),
      close("orch_turn"),
      open("think"),   // unclosed: only the tag goes
  };
}

// Every code span of `s` in order (the same rules as the cleaner).
std::vector<std::string> codePieces(const std::vector<std::string>& pieces) {
  std::vector<std::string> out;
  for (const auto& p : pieces)
    if (p.find('`') != std::string::npos) out.push_back(p);
  return out;
}

std::string squash(const std::string& s) {
  std::string out;
  bool sp = false;
  for (char c : s) {
    const bool ws = c == ' ' || c == '\n' || c == '\t' || c == '\r';
    if (ws) {
      sp = true;
      continue;
    }
    if (sp && !out.empty()) out += ' ';
    sp = false;
    out += c;
  }
  return out;
}

struct Generated {
  std::string text;       // content with leaks injected
  std::string content;    // the same content, no leaks
  std::vector<std::string> pieces;
};

Generated generate(std::mt19937& rng, bool withLeaks) {
  const auto content = contentCorpus();
  const auto leaks = leakCorpus();
  Generated g;
  const int n = 1 + (int)(rng() % 6);
  for (int i = 0; i < n; i++) {
    const std::string& c = content[rng() % content.size()];
    const std::string sep = (rng() % 3 == 0) ? "\n\n" : " ";
    if (withLeaks && rng() % 2 == 0) g.text += leaks[rng() % leaks.size()];
    g.text += c + sep;
    g.content += c + sep;
    g.pieces.push_back(c);
  }
  if (withLeaks && rng() % 2 == 0) g.text += leaks[rng() % leaks.size()];
  if (withLeaks && rng() % 7 == 0) {   // a whole-text wrapper
    g.text = open("reply") + g.text + close("reply");
  }
  return g;
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    size_t need = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!need || i + need > s.size()) return false;
    for (size_t k = 1; k < need; k++)
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    i += need;
  }
  return true;
}

}  // namespace

// ---- cleaner properties (formatting T1: P1-P7, over 2000 generated replies) ----

static void test_p1_no_leak_survives_and_p2_idempotent_and_p7_never_grows() {
  std::mt19937 rng(kSeed);
  for (int i = 0; i < kCases; i++) {
    const Generated g = generate(rng, true);
    const std::string c = cleanReplyMarkup(g.text);
    std::string why;
    char msg[96];
    snprintf(msg, sizeof msg, "seed %u case %d", (unsigned)kSeed, i);
    if (hasStrayMarkup(c, &why)) TEST_FAIL_MESSAGE((std::string(msg) + ": " + why).c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.c_str(), cleanReplyMarkup(c).c_str(), msg);
    TEST_ASSERT_TRUE_MESSAGE(c.size() <= g.text.size(), msg);
    TEST_ASSERT_TRUE_MESSAGE(validUtf8(c), msg);
  }
}

static void test_p3_text_without_a_tag_is_returned_unchanged() {
  std::mt19937 rng(kSeed + 1);
  for (int i = 0; i < kCases; i++) {
    std::string s = generate(rng, false).text;
    std::string plain;
    for (char ch : s) if (ch != '<') plain += ch;
    TEST_ASSERT_EQUAL_STRING(plain.c_str(), cleanReplyMarkup(plain).c_str());
  }
}

static void test_p4_content_is_preserved_and_p5_code_is_byte_exact() {
  std::mt19937 rng(kSeed + 2);
  for (int i = 0; i < kCases; i++) {
    const Generated g = generate(rng, true);
    const std::string c = cleanReplyMarkup(g.text);
    char msg[96];
    snprintf(msg, sizeof msg, "seed %u case %d", (unsigned)kSeed + 2, i);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(squash(cleanReplyMarkup(g.content)).c_str(),
                                     squash(c).c_str(), msg);
    for (const auto& code : codePieces(g.pieces))
      TEST_ASSERT_TRUE_MESSAGE(contains(c, code), msg);
  }
}

// P6, the counter-test: words in angle brackets that are not on the deny-list
// are content, and a cleaner that strips them is over-reaching.
static void test_p6_unknown_word_tags_survive_verbatim() {
  for (const char* s : {"Spawn on <provider> as <name>.", "Tag <MMDDHHMM> please.",
                        "A List<String> and x<y>z.", "a < b > c", "I <3 it", "<b>bold</b> stays",
                        "the HTML <output> element renders results"}) {
    TEST_ASSERT_EQUAL_STRING(s, cleanReplyMarkup(s).c_str());
    TEST_ASSERT_FALSE_MESSAGE(hasStrayMarkup(s), s);
  }
}

// Counter-test: an unclosed reasoning tag loses only the tag - the answer that
// follows is never eaten (fail visible, never fail silent).
static void test_p9_unclosed_think_keeps_the_text() {
  const std::string s = open("think") + "The answer is 42.";
  TEST_ASSERT_EQUAL_STRING("The answer is 42.", cleanReplyMarkup(s).c_str());
  TEST_ASSERT_EQUAL_STRING("The answer is 42.", stripReasoningPreamble(s).c_str());
}

static void test_real_tool_markup_shapes_are_removed() {
  // GLM-style tool-call text, a <|token|>, a namespaced block, an orphan close.
  const std::string glm = "Saved." + open("tool_call") + "memory.write" + open("arg_key") +
                          "content" + close("arg_key") + open("arg_value") + "x" +
                          close("arg_value") + close("tool_call");
  TEST_ASSERT_EQUAL_STRING("Saved.", cleanReplyMarkup(glm).c_str());
  TEST_ASSERT_EQUAL_STRING("Done.", cleanReplyMarkup("Done.<|im_end|>").c_str());
  TEST_ASSERT_EQUAL_STRING("Hi.", cleanReplyMarkup("Hi." + close("parameter")).c_str());
  const std::string ns = "Ok." + open("function_calls", "", true) + "x" + close("function_calls", true);
  TEST_ASSERT_EQUAL_STRING("Ok.", cleanReplyMarkup(ns).c_str());
  // A whole-text wrapper, and a reply that only ends with the wrapper's close.
  TEST_ASSERT_EQUAL_STRING("Hello.", cleanReplyMarkup(open("reply") + "Hello." + close("reply")).c_str());
  TEST_ASSERT_EQUAL_STRING("Hello.", cleanReplyMarkup("Hello.\n" + close("reply")).c_str());
}

// ---- formatting F3: the shape read ---------------------------------------------

static void test_p8_reasoning_never_turns_a_turn_into_prose() {
  const std::string turn = R"({"reply":"ok","mem_write":[{"content":"x","importance":0.5}]})";
  for (const std::string& pre :
       {open("think") + "plan" + close("think") + "\n", open("thinking") + "a\nb" + close("thinking"),
        open("reasoning") + "r" + close("reasoning") + "  " + open("think") + "t" + close("think")}) {
    Turn t;
    ParseError e;
    TEST_ASSERT_EQUAL(int(ReplyShape::Turn), int(parseHeadReply(pre + turn, t, e)));
    TEST_ASSERT_EQUAL_STRING("ok", t.reply.c_str());
    TEST_ASSERT_EQUAL(1u, t.mem_write.size());
  }
  // A whole-reply wrapper around the JSON is a turn too.
  Turn t;
  ParseError e;
  TEST_ASSERT_EQUAL(int(ReplyShape::Turn),
                    int(parseHeadReply(open("orch_turn") + turn + close("orch_turn"), t, e)));
  // ...and a reasoning block in front of prose leaves only the prose.
  TEST_ASSERT_EQUAL(int(ReplyShape::Prose),
                    int(parseHeadReply(open("think") + "x" + close("think") + "Hello there.", t, e)));
  TEST_ASSERT_EQUAL_STRING("Hello there.", t.reply.c_str());
}

static void test_unwrap_only_the_whole_text() {
  TEST_ASSERT_EQUAL_STRING("inner", unwrapWholeReply(open("answer") + "inner" + close("answer")).c_str());
  const std::string mid = "Use " + open("output") + "x" + close("output") + " here.";
  TEST_ASSERT_EQUAL_STRING(mid.c_str(), unwrapWholeReply(mid).c_str());
  TEST_ASSERT_EQUAL_STRING("plain", unwrapWholeReply("plain").c_str());
}

// ---- formatting F1: leak repair in parseTurn (formatting T2) --------------------

namespace {

struct GenTurn {
  std::string reply, memory, ask;
  bool hasMemory = false, hasAsk = false;
  std::string device, memWrite, memQuery, sessionOps, scratchpad;   // JSON text or ""
};

std::string pick(std::mt19937& rng, const std::vector<std::string>& v) { return v[rng() % v.size()]; }

GenTurn genTurn(std::mt19937& rng) {
  const std::vector<std::string> words = {
      "Here is the plan.", "Caf\xC3\xA9 at nine.", "a < b for sure.", "Use `code` here.",
      "\xF0\x9F\x98\x80 done.", "Spawn on <provider>.", "Line one.\nLine two.", "Plain text."};
  GenTurn g;
  const int n = 1 + (int)(rng() % 4);
  for (int i = 0; i < n; i++) g.reply += (i ? " " : "") + pick(rng, words);
  if (rng() % 2) { g.hasMemory = true; g.memory = pick(rng, words); }
  if (rng() % 2) { g.hasAsk = true; g.ask = rng() % 4 ? pick(rng, words) : std::string(); }
  if (rng() % 2) g.device = R"([{"type":"lights","value":"off"}])";
  if (rng() % 2) g.memWrite = R"([{"content":")" + pick(rng, {"Prefers metric", "Name is Sam"}) +
                              R"(","importance":0.7,"permanent":false,"ttl":"months"}])";
  if (rng() % 2) g.memQuery = R"(["caravan prices"])";
  if (rng() % 2)
    g.sessionOps = R"([{"op":"spawn","task":"Find three sources.","provider":"anthropic","model":null,"skill":null,"name":"src","project":null,"attach":null}])";
  if (rng() % 3 == 0) g.scratchpad = R"({"active":"drafting","short":["a","b"],"mid":null,"long":null})";
  return g;
}

// The clean serialization: every present field at the top level.
std::string cleanJson(const GenTurn& g) {
  ArduinoJson::JsonDocument d;
  d["reply"] = g.reply;
  if (g.hasMemory) d["memory"] = g.memory;
  if (g.hasAsk) d["ask"] = g.ask;
  auto raw = [&](const char* k, const std::string& v) {
    if (v.empty()) return;
    ArduinoJson::JsonDocument x;
    deserializeJson(x, v);
    d[k] = x;
  };
  raw("device", g.device);
  raw("mem_write", g.memWrite);
  raw("mem_query", g.memQuery);
  raw("session_ops", g.sessionOps);
  raw("scratchpad", g.scratchpad);
  std::string out;
  serializeJson(d, out);
  return out;
}

struct F {
  const char* name;
  std::string value;
  bool present;
};

std::vector<F> fieldOrder(const GenTurn& g) {
  return {{"reply", g.reply, true},          {"memory", g.memory, g.hasMemory},
          {"ask", g.ask, g.hasAsk},          {"device", g.device, !g.device.empty()},
          {"mem_write", g.memWrite, !g.memWrite.empty()}, {"mem_query", g.memQuery, !g.memQuery.empty()},
          {"session_ops", g.sessionOps, !g.sessionOps.empty()},
          {"scratchpad", g.scratchpad, !g.scratchpad.empty()}};
}

// The tail the decoder writes after the cut field: a close tag, then one
// parameter block per moved field (closed three ways; only the last may stay
// open), then sometimes a closing invoke.
std::string leakTail(const std::vector<F>& order, size_t cut, const std::vector<size_t>& moved,
                     std::mt19937& rng) {
  const bool ns = rng() % 3 == 0;
  const std::string nl = rng() % 4 == 0 ? "\r\n" : "\n";
  std::string tail = nl + (rng() % 2 ? close(order[cut].name, ns) : close("parameter", ns));
  for (size_t k = 0; k < moved.size(); k++) {
    const F& f = order[moved[k]];
    tail += nl + param(f.name, ns) + f.value;
    const int variant = (int)(rng() % 3);
    if (variant == 0 || (variant == 2 && k + 1 != moved.size())) tail += close("parameter", ns);
    else if (variant == 1) tail += close(f.name, ns);
  }
  // A real leak tail carries tool-call structure (blocks, or the closing invoke);
  // a lone close tag is the reply seam's job (see the counter-test).
  if (moved.empty() || rng() % 2) tail += nl + close("invoke", ns);
  return tail;
}

// The leaked serialization: every field after the cut field moves into the cut
// field's string as parameter blocks (the decoder fault's shape).
std::string leakedJson(const GenTurn& g, std::mt19937& rng) {
  const std::vector<F> order = fieldOrder(g);
  std::vector<size_t> cuts = {0};   // the cut field: reply, or memory/ask when present
  if (g.hasMemory) cuts.push_back(1);
  if (g.hasAsk && !g.ask.empty()) cuts.push_back(2);
  const size_t cut = cuts[rng() % cuts.size()];
  std::vector<size_t> moved;
  for (size_t i = cut + 1; i < order.size(); i++)
    if (order[i].present) moved.push_back(i);
  const std::string tail = leakTail(order, cut, moved, rng);
  ArduinoJson::JsonDocument d;
  for (size_t i = 0; i <= cut; i++) {
    if (!order[i].present) continue;
    d[order[i].name] = i == cut ? order[i].value + tail : order[i].value;
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

void assertSameTurn(const Turn& a, const Turn& b, const char* msg) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE(a.reply.c_str(), b.reply.c_str(), msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(a.memory.c_str(), b.memory.c_str(), msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(a.ask.c_str(), b.ask.c_str(), msg);
  TEST_ASSERT_EQUAL_MESSAGE(a.device.size(), b.device.size(), msg);
  for (size_t i = 0; i < a.device.size(); i++)
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a.device[i].json.c_str(), b.device[i].json.c_str(), msg);
  TEST_ASSERT_EQUAL_MESSAGE(a.mem_write.size(), b.mem_write.size(), msg);
  for (size_t i = 0; i < a.mem_write.size(); i++) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a.mem_write[i].content.c_str(), b.mem_write[i].content.c_str(), msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a.mem_write[i].ttl.c_str(), b.mem_write[i].ttl.c_str(), msg);
  }
  TEST_ASSERT_EQUAL_MESSAGE(a.mem_query.size(), b.mem_query.size(), msg);
  TEST_ASSERT_EQUAL_MESSAGE(a.session_ops.size(), b.session_ops.size(), msg);
  for (size_t i = 0; i < a.session_ops.size(); i++) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a.session_ops[i].op.c_str(), b.session_ops[i].op.c_str(), msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a.session_ops[i].task.c_str(), b.session_ops[i].task.c_str(), msg);
  }
  TEST_ASSERT_EQUAL_MESSAGE(a.scratchpad.present, b.scratchpad.present, msg);
  TEST_ASSERT_EQUAL_MESSAGE(a.scratchpad.shortItems.size(), b.scratchpad.shortItems.size(), msg);
}

}  // namespace

// The core class test: a leaked turn parses equal to the clean one, field by
// field, and no delivered string carries markup or a field name tag.
static void test_leaked_turn_parses_equal_to_the_clean_turn() {
  std::mt19937 rng(kSeed + 3);
  for (int i = 0; i < kCases; i++) {
    const GenTurn g = genTurn(rng);
    const std::string clean = cleanJson(g);
    const std::string leaked = leakedJson(g, rng);
    char msg[64];
    snprintf(msg, sizeof msg, "seed %u case %d", (unsigned)kSeed + 3, i);
    Turn a, b;
    ParseError ea, eb;
    TEST_ASSERT_TRUE_MESSAGE(parseTurn(clean, a, ea), msg);
    TEST_ASSERT_TRUE_MESSAGE(parseTurn(leaked, b, eb), msg);
    assertSameTurn(a, b, msg);
    TEST_ASSERT_TRUE_MESSAGE(b.repair.repaired, msg);
    TEST_ASSERT_FALSE_MESSAGE(a.repair.repaired, msg);
    TEST_ASSERT_FALSE_MESSAGE(hasStrayMarkup(b.reply), msg);
    TEST_ASSERT_FALSE_MESSAGE(hasStrayMarkup(b.ask), msg);
    TEST_ASSERT_FALSE_MESSAGE(hasStrayMarkup(b.memory), msg);
  }
  char line[80];
  snprintf(line, sizeof line, "property: %d leaked turns parsed equal to their clean turn", kCases);
  TEST_MESSAGE(line);
}

// The two leak tails measured live (scrubbed): every field the tail carried is
// back where it belongs, the reply is clean, and the containers are flagged as
// recovered (the taint gate reads containerMask).
static void test_live_leak_tails_are_repaired() {
  {
    const turn_shapes::Leak l = turn_shapes::leak5ef();
    Turn t;
    ParseError e;
    TEST_ASSERT_TRUE(parseTurn(l.json, t, e));
    TEST_ASSERT_EQUAL_STRING(l.reply.c_str(), t.reply.c_str());
    TEST_ASSERT_EQUAL_STRING(l.ask.c_str(), t.ask.c_str());
    TEST_ASSERT_EQUAL_STRING(l.memory.c_str(), t.memory.c_str());
    TEST_ASSERT_TRUE(t.repair.repaired);
    TEST_ASSERT_TRUE(t.repair.containerMask & (1u << (unsigned)TurnField::Device));
    TEST_ASSERT_FALSE(hasStrayMarkup(t.reply));
  }
  {
    const turn_shapes::Leak l = turn_shapes::leak613();
    Turn t;
    ParseError e;
    TEST_ASSERT_TRUE(parseTurn(l.json, t, e));
    TEST_ASSERT_EQUAL_STRING(l.reply.c_str(), t.reply.c_str());
    TEST_ASSERT_EQUAL_STRING(l.memory.c_str(), t.memory.c_str());
    TEST_ASSERT_EQUAL_STRING("", t.ask.c_str());
    TEST_ASSERT_EQUAL(1u, t.mem_write.size());
    TEST_ASSERT_TRUE(t.scratchpad.present);
    TEST_ASSERT_EQUAL(2u, t.scratchpad.shortItems.size());
    const unsigned mw = 1u << (unsigned)TurnField::MemWrite, sp = 1u << (unsigned)TurnField::Scratchpad;
    TEST_ASSERT_TRUE((t.repair.containerMask & mw) && (t.repair.containerMask & sp));
    TEST_ASSERT_FALSE(hasStrayMarkup(t.reply));
  }
}

// Counter-tests (the loosened read must not reach prose or code).
static void test_quoted_markers_in_prose_or_code_are_not_repaired() {
  const std::string prose = "The format closes with " + close("parameter") +
                            " and then the model goes on in prose.";
  for (const std::string& reply :
       {prose, "Example:\n```\n" + close("reply") + "\n" + param("memory") + "x\n```\nThat is all.",
        "Inline `" + close("parameter") + "` mention."}) {
    ArduinoJson::JsonDocument d;
    d["reply"] = reply;
    std::string j;
    serializeJson(d, j);
    Turn t;
    ParseError e;
    TEST_ASSERT_TRUE(parseTurn(j, t, e));
    TEST_ASSERT_EQUAL_STRING(reply.c_str(), t.reply.c_str());
    TEST_ASSERT_FALSE(t.repair.repaired);
  }
}

// Counter-test: prose that merely ENDS on a closing tag keeps every word at the
// parse; the reply seam then drops the stray tag before delivery.
static void test_a_lone_trailing_close_tag_is_not_a_leak() {
  for (const std::string& reply : {"The format closes with " + close("parameter"),
                                   "All done.\n" + close("reply")}) {
    ArduinoJson::JsonDocument d;
    d["reply"] = reply;
    std::string j;
    serializeJson(d, j);
    Turn t;
    ParseError e;
    TEST_ASSERT_TRUE(parseTurn(j, t, e));
    TEST_ASSERT_EQUAL_STRING(reply.c_str(), t.reply.c_str());
    TEST_ASSERT_FALSE(t.repair.repaired);
    TEST_ASSERT_FALSE(hasStrayMarkup(cleanReplyMarkup(t.reply)));
  }
}

static void test_a_present_field_is_never_overwritten_and_bad_json_is_dropped() {
  ArduinoJson::JsonDocument d;
  d["reply"] = "Done." + close("reply") + param("memory") + "leaked note" + close("parameter") +
               param("mem_write") + "not json" + close("parameter");
  d["memory"] = "the real note";
  std::string j;
  serializeJson(d, j);
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL_STRING("Done.", t.reply.c_str());
  TEST_ASSERT_EQUAL_STRING("the real note", t.memory.c_str());   // the document wins
  TEST_ASSERT_EQUAL(0u, t.mem_write.size());                      // unparseable: dropped
  TEST_ASSERT_EQUAL(1, (int)t.repair.droppedBlocks);
}

// The field list the repair knows is the wire schema's: a field added there
// without a row fails here.
static void test_every_schema_field_is_known_to_the_repair() {
  for (const char* f : {"reply", "memory", "ask", "device", "mem_write", "mem_query",
                        "session_ops", "scratchpad"}) {
    TurnField tf;
    TEST_ASSERT_TRUE_MESSAGE(turnFieldFromName(f, strlen(f), tf), f);
    TEST_ASSERT_EQUAL_STRING(f, turnFieldName(tf));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_p1_no_leak_survives_and_p2_idempotent_and_p7_never_grows);
  RUN_TEST(test_p3_text_without_a_tag_is_returned_unchanged);
  RUN_TEST(test_p4_content_is_preserved_and_p5_code_is_byte_exact);
  RUN_TEST(test_p6_unknown_word_tags_survive_verbatim);
  RUN_TEST(test_p9_unclosed_think_keeps_the_text);
  RUN_TEST(test_real_tool_markup_shapes_are_removed);
  RUN_TEST(test_p8_reasoning_never_turns_a_turn_into_prose);
  RUN_TEST(test_unwrap_only_the_whole_text);
  RUN_TEST(test_leaked_turn_parses_equal_to_the_clean_turn);
  RUN_TEST(test_live_leak_tails_are_repaired);
  RUN_TEST(test_quoted_markers_in_prose_or_code_are_not_repaired);
  RUN_TEST(test_a_lone_trailing_close_tag_is_not_a_leak);
  RUN_TEST(test_a_present_field_is_never_overwritten_and_bad_json_is_dropped);
  RUN_TEST(test_every_schema_field_is_known_to_the_repair);
  return UNITY_END();
}
