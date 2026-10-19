// The turn-contract shape class (subagents T1, parsing T1/T2, formatting F2):
// every field the head fills either reaches the device in its contract shape or
// is reported - never zero items with no trace - and a cut reply never executes,
// never shows raw wire JSON, and salvages exactly the prefix it holds.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/turn_shape_fixtures.h"
#include "nimbus/orch/caps.h"
#include "nimbus/orch/orch_schema.h"
#include "nimbus/orch/turn.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

const uint32_t kSeed = 0x5EED0;

std::string ser(const ArduinoJson::JsonDocument& d) {
  std::string s;
  serializeJson(d, s);
  return s;
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

// One canonical, valid value per container field (two items where a list).
std::string canonical(TurnField f) {
  switch (f) {
    case TurnField::Device:     return R"([{"type":"lights","value":"off"},{"type":"tts","text":"hi"}])";
    case TurnField::MemWrite:   return R"([{"content":"Prefers metric","importance":0.6},{"content":"Name is Sam"}])";
    case TurnField::MemQuery:   return R"(["caravan prices","rail status"])";
    case TurnField::SessionOps: return R"([{"op":"spawn","task":"Find three sources."},{"op":"spawn","task":"Check the forecast."}])";
    case TurnField::Scratchpad: return R"({"active":"drafting","short":["a","b"]})";
    case TurnField::Spawn:      return R"([{"task":"Research caravans."},{"task":"Price check."}])";
    case TurnField::Await:      return R"(["job0001","job0002"])";
    default:                    return "";
  }
}

// How many items the parsed Turn carries for field f.
size_t itemCount(const Turn& t, TurnField f) {
  switch (f) {
    case TurnField::Device:     return t.device.size();
    case TurnField::MemWrite:   return t.mem_write.size();
    case TurnField::MemQuery:   return t.mem_query.size();
    case TurnField::SessionOps: return t.session_ops.size();
    case TurnField::Scratchpad: return t.scratchpad.present ? 1 : 0;
    case TurnField::Spawn:      return t.spawn.size();
    case TurnField::Await:      return t.await_.size();
    default:                    return 0;
  }
}

Turn parseWith(const char* key, ArduinoJson::JsonVariantConst value, bool* ok = nullptr) {
  ArduinoJson::JsonDocument d;
  d["reply"] = "r";
  d[key] = value;
  Turn t;
  ParseError e;
  const bool good = parseTurn(ser(d), t, e);
  if (ok) *ok = good;
  return t;
}

bool notesName(const Turn& t, const std::string& field) {
  for (const auto& n : t.parseNotes)
    if (n.find(field) != std::string::npos) return true;
  return false;
}

}  // namespace

// ---- the field table is the wire schema's ----------------------------------------

static void test_the_field_table_matches_the_wire_schema() {
  ArduinoJson::JsonDocument schema;
  TEST_ASSERT_FALSE(deserializeJson(schema, ORCH_SCHEMA_BODY,
                                    ArduinoJson::DeserializationOption::NestingLimit(16)));
  std::set<std::string> props;
  for (ArduinoJson::JsonPairConst kv : schema["properties"].as<ArduinoJson::JsonObjectConst>()) {
    const std::string name = kv.key().c_str();
    props.insert(name);
    TurnField f;
    TEST_ASSERT_TRUE_MESSAGE(turnFieldFromName(name.data(), name.size(), f), name.c_str());
    // The kind the table declares is the kind the schema declares.
    ArduinoJson::JsonVariantConst type = kv.value()["type"];
    const std::string t = type.is<const char*>() ? type.as<const char*>() : type[0].as<const char*>();
    const FieldKind k = turnFieldKind(f);
    if (t == "string") {
      TEST_ASSERT_EQUAL_MESSAGE(int(FieldKind::String), int(k), name.c_str());
    } else if (t == "object") {
      TEST_ASSERT_EQUAL_MESSAGE(int(FieldKind::Object), int(k), name.c_str());
    } else {
      TEST_ASSERT_EQUAL_STRING_MESSAGE("array", t.c_str(), name.c_str());
      ArduinoJson::JsonVariantConst items = kv.value()["items"];
      const bool strings = items["type"].is<const char*>() &&
                           std::string(items["type"].as<const char*>()) == "string";
      TEST_ASSERT_EQUAL_MESSAGE(int(strings ? FieldKind::ArrayOfStrings : FieldKind::ArrayOfObjects),
                                int(k), name.c_str());
    }
  }
  // ...and every schema property has a row (the retired arrays are extra).
  for (size_t i = 0; i < (size_t)TurnField::Count; i++) {
    const std::string n = turnFieldName((TurnField)i);
    if (n == "spawn" || n == "await") continue;
    TEST_ASSERT_TRUE_MESSAGE(props.count(n) == 1, n.c_str());
  }
}

// ---- every container shape is repaired or reported (L-T1) ---------------------------

static void test_every_container_shape_is_repaired_or_reported() {
  for (size_t i = (size_t)TurnField::Device; i < (size_t)TurnField::Count; i++) {
    const TurnField f = (TurnField)i;
    const char* key = turnFieldName(f);
    const FieldKind kind = turnFieldKind(f);
    ArduinoJson::JsonDocument canon;
    TEST_ASSERT_FALSE(deserializeJson(canon, canonical(f)));
    const size_t want = itemCount(parseWith(key, canon.as<ArduinoJson::JsonVariantConst>()), f);
    TEST_ASSERT_TRUE_MESSAGE(want >= 1, key);

    // Repairable: the value as a JSON string.
    {
      ArduinoJson::JsonDocument v;
      v.set(canonical(f));
      const Turn t = parseWith(key, v.as<ArduinoJson::JsonVariantConst>());
      TEST_ASSERT_EQUAL_MESSAGE(want, itemCount(t, f), key);
      TEST_ASSERT_TRUE_MESSAGE(notesName(t, key), key);
      TEST_ASSERT_EQUAL_MESSAGE(1, t.coercedFields, key);
    }
    // Repairable: one bare object where a list of objects belongs (and its JSON string).
    if (kind == FieldKind::ArrayOfObjects) {
      ArduinoJson::JsonVariantConst first = canon[0];
      const Turn t = parseWith(key, first);
      TEST_ASSERT_EQUAL_MESSAGE(1u, itemCount(t, f), key);
      TEST_ASSERT_TRUE_MESSAGE(notesName(t, key), key);
      ArduinoJson::JsonDocument v;
      v.set(ser([&] { ArduinoJson::JsonDocument x; x.set(first); return x; }()));
      const Turn t2 = parseWith(key, v.as<ArduinoJson::JsonVariantConst>());
      TEST_ASSERT_EQUAL_MESSAGE(1u, itemCount(t2, f), key);
    }
    // Unrepairable: wrong scalar types and text that is not JSON. The forbidden
    // outcome - zero items and no note - must never happen.
    for (const char* bad : {"42", "true", "\"none of these\"", "\"[not json\""}) {
      ArduinoJson::JsonDocument v;
      TEST_ASSERT_FALSE(deserializeJson(v, bad));
      const Turn t = parseWith(key, v.as<ArduinoJson::JsonVariantConst>());
      if (kind == FieldKind::ArrayOfStrings && std::string(bad) == "\"none of these\"") {
        TEST_ASSERT_EQUAL_MESSAGE(1u, itemCount(t, f), key);   // one query string: a list of one
        continue;
      }
      TEST_ASSERT_EQUAL_MESSAGE(0u, itemCount(t, f), key);
      TEST_ASSERT_TRUE_MESSAGE(notesName(t, key), (std::string(key) + " " + bad).c_str());
      if (f == TurnField::SessionOps || f == TurnField::Spawn)
        TEST_ASSERT_TRUE_MESSAGE(!t.spawnDrops.empty(), (std::string(key) + " " + bad).c_str());
    }
    // Counter-tests: a true empty value or null raises no note (no false alarm).
    for (const char* empty : {"null", "[]", "\"\""}) {
      ArduinoJson::JsonDocument v;
      TEST_ASSERT_FALSE(deserializeJson(v, empty));
      if (kind == FieldKind::Object && std::string(empty) == "[]") continue;
      const Turn t = parseWith(key, v.as<ArduinoJson::JsonVariantConst>());
      TEST_ASSERT_EQUAL_MESSAGE(0u, t.parseNotes.size(), (std::string(key) + " " + empty).c_str());
      TEST_ASSERT_EQUAL_MESSAGE(0u, t.spawnDrops.size(), key);
    }
  }
}

// The list hidden inside an item's op (13 Oct shape), a missing op, an op
// synonym, and the brief under another name all come through.
static void test_session_ops_item_shapes_are_repaired() {
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(
      R"({"reply":"r","session_ops":[{"op":"[{\"op\":\"spawn\",\"task\":\"a\"},{\"op\":\"spawn\",\"task\":\"b\"}]"}]})",
      t, e));
  TEST_ASSERT_EQUAL(2u, t.session_ops.size());
  TEST_ASSERT_EQUAL_STRING("b", t.session_ops[1].task.c_str());
  TEST_ASSERT_TRUE(parseTurn(
      R"({"reply":"r","session_ops":[{"task":"no op"},{"op":"Launch","prompt":"aliased"},"{\"op\":\"spawn\",\"task\":\"s\"}"]})",
      t, e));
  TEST_ASSERT_EQUAL(3u, t.session_ops.size());
  for (const auto& op : t.session_ops) TEST_ASSERT_EQUAL_STRING("spawn", op.op.c_str());
  TEST_ASSERT_EQUAL_STRING("aliased", t.session_ops[1].task.c_str());
  TEST_ASSERT_TRUE(t.spawnDrops.empty());
}

// Seven spawns keep six; the seventh is reported with the cap reason.
static void test_seven_spawns_keep_six_and_report_one() {
  std::string ops;
  for (int i = 0; i < 7; i++) ops += std::string(i ? "," : "") + R"({"op":"spawn","task":"t)" + std::to_string(i) + "\"}";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn("{\"reply\":\"r\",\"session_ops\":[" + ops + "]}", t, e));
  TEST_ASSERT_EQUAL(kAgentMaxJobs, (int)t.session_ops.size());
  TEST_ASSERT_EQUAL(1u, t.spawnDrops.size());
  TEST_ASSERT_EQUAL_STRING("t6", t.spawnDrops[0].task.c_str());
  TEST_ASSERT_TRUE(t.spawnDrops[0].reason.find("next wave") != std::string::npos);
}

// Conservation law over generated item lists: every spawn intent ends in exactly
// one of {a kept spawn op, a reported drop}. Nothing disappears.
static void test_every_spawn_intent_is_kept_or_reported() {
  std::mt19937 rng(kSeed);
  const std::vector<std::string> items = {
      R"({"op":"spawn","task":"real work"})",       // intent, kept
      R"({"op":"spawn"})",                          // intent, no task -> drop
      R"({"op":"spawn","task":"   "})",             // intent, blank task -> drop
      R"({"op":"start","task":"synonym"})",         // intent, kept
      R"({"task":"no op given"})",                  // intent, kept
      R"({"op":"frobnicate","task":"unknown op"})", // intent, drop
      R"({"op":"spawn","instructions":"alias"})",   // intent, kept
      R"("not an object")",                          // intent (non-object), drop
      R"(17)",                                       // intent (non-object), drop
      R"({"op":"terminate","id":"job0001"})",       // not a spawn
      R"({"op":"list"})",                            // not a spawn
      R"({"noOp":1})",                               // not a spawn (no task)
  };
  const std::vector<int> intent = {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0};
  int allIntents = 0, allKept = 0, allDrops = 0;
  for (int c = 0; c < 2000; c++) {
    const int n = 1 + (int)(rng() % 9);
    std::string list;
    int intents = 0;
    for (int i = 0; i < n; i++) {
      const size_t k = rng() % items.size();
      list += std::string(i ? "," : "") + items[k];
      intents += intent[k];
    }
    Turn t;
    ParseError e;
    TEST_ASSERT_TRUE(parseTurn("{\"reply\":\"r\",\"session_ops\":[" + list + "]}", t, e));
    int kept = 0;
    for (const auto& op : t.session_ops) kept += op.op == "spawn";
    char msg[64];
    snprintf(msg, sizeof msg, "case %d", c);
    TEST_ASSERT_EQUAL_MESSAGE(intents, kept + (int)t.spawnDrops.size(), msg);
    for (const auto& op : t.session_ops)
      if (op.op == "spawn") TEST_ASSERT_FALSE_MESSAGE(op.task.empty() && op.message.empty(), msg);
    allIntents += intents;
    allKept += kept;
    allDrops += (int)t.spawnDrops.size();
  }
  char line[128];
  snprintf(line, sizeof line, "property: 2000 lists, %d spawn intents = %d kept + %d reported", allIntents, allKept,
           allDrops);
  TEST_MESSAGE(line);
}

// Counter-test for the loosened read: a well-formed turn is untouched - the
// normalizer reports no change, no note, and the document is byte-identical.
static void test_a_valid_turn_is_byte_identical_after_normalization() {
  std::mt19937 rng(kSeed + 1);
  for (int c = 0; c < 500; c++) {
    ArduinoJson::JsonDocument d;
    d["reply"] = c % 2 ? "Plain reply." : "Uses <provider> and `code`.";
    d["memory"] = "note";
    d["ask"] = "";
    for (size_t i = (size_t)TurnField::Device; i <= (size_t)TurnField::Scratchpad; i++) {
      if (rng() % 2) continue;
      const TurnField f = (TurnField)i;
      ArduinoJson::JsonDocument v;
      deserializeJson(v, canonical(f));
      d[turnFieldName(f)] = v;
    }
    const std::string before = ser(d);
    Turn report;
    TEST_ASSERT_FALSE(normalizeTurnDoc(d.as<ArduinoJson::JsonObject>(), report));
    TEST_ASSERT_EQUAL_STRING(before.c_str(), ser(d).c_str());
    TEST_ASSERT_EQUAL(0u, report.parseNotes.size());
    TEST_ASSERT_EQUAL(0u, report.spawnDrops.size());
  }
}

// ---- the pseudo-tool shapes (subagents F2) ------------------------------------------

static void test_spawn_items_sent_through_the_pseudo_tool_are_read() {
  for (const auto& s : turn_shapes::pseudoToolShapes()) {
    Turn t;
    TEST_ASSERT_EQUAL_MESSAGE(s.spawns, spawnsFromToolArgs(s.args, t), s.label);
    TEST_ASSERT_EQUAL_MESSAGE((size_t)s.spawns, t.session_ops.size(), s.label);
    // A call that carried nothing usable is a reported attempt, never silence.
    if (s.spawns == 0) TEST_ASSERT_FALSE_MESSAGE(t.spawnDrops.empty(), s.label);
  }
  Turn t;
  TEST_ASSERT_EQUAL(0, spawnsFromToolArgs(R"({"op":"terminate","id":"job0007"})", t));
  TEST_ASSERT_EQUAL(0, spawnsFromToolArgs("{not json", t));
  TEST_ASSERT_FALSE(t.spawnDrops.empty());
}

// ---- cuts at every byte (parsing T1) -----------------------------------------------

namespace {

std::string jsonEscape(const std::string& s) {
  ArduinoJson::JsonDocument d;
  d.set(s);
  std::string out;
  serializeJson(d, out);
  return out;   // quoted
}

struct CutCase {
  std::string text;        // the full reply as the provider sent it
  std::string reply;       // decoded reply
  size_t replyClose = 0;   // index of the reply string's closing quote in text
};

CutCase genCut(std::mt19937& rng) {
  const std::vector<std::string> pieces = {
      "Hello there.", " Caf\xC3\xA9 au lait.", " \xF0\x9F\x98\x80 done!", " A \"quoted\" word.",
      " back\\slash.", "\nNew line.", " Tab\there.", " \xE6\x97\xA5\xE6\x9C\xAC."};
  std::string reply;
  const int n = 2 + (int)(rng() % 5);
  for (int i = 0; i < n; i++) reply += pieces[rng() % pieces.size()];
  std::string enc = jsonEscape(reply);
  // Sometimes the escaped form the providers also send (é, surrogate pairs).
  if (rng() % 3 == 0) {
    std::string alt;
    for (size_t i = 0; i < enc.size(); i++) {
      if ((unsigned char)enc[i] == 0xC3 && i + 1 < enc.size() && (unsigned char)enc[i + 1] == 0xA9) {
        alt += "\\u00e9";
        i++;
      } else if ((unsigned char)enc[i] == 0xF0 && i + 3 < enc.size()) {
        alt += "\\ud83d\\ude00";
        i += 3;
      } else {
        alt += enc[i];
      }
    }
    enc = alt;
  }
  const bool askFirst = rng() % 4 == 0;
  std::string obj = "{";
  if (askFirst) obj += "\"ask\":\"Which one?\",";
  const size_t replyAt = obj.size() + 8;   // after "reply":
  obj += "\"reply\":" + enc;
  CutCase c;
  c.reply = reply;
  c.replyClose = replyAt + enc.size() - 1;
  obj += ",\"memory\":\"m\",\"device\":[{\"type\":\"lights\",\"value\":\"off\"}],"
         "\"session_ops\":[{\"op\":\"spawn\",\"task\":\"t\"}],\"scratchpad\":null}";
  if (rng() % 3 == 0) {
    c.text = "```json\n" + obj + "\n```";
    c.replyClose += 8;
  } else {
    c.text = obj;
  }
  return c;
}

}  // namespace

static void test_a_cut_reply_never_executes_and_salvages_its_prefix() {
  std::mt19937 rng(kSeed + 2);
  int checked = 0;
  for (int c = 0; c < 60; c++) {
    const CutCase cc = genCut(rng);
    {
      Turn full;
      ParseError e;
      TEST_ASSERT_EQUAL(int(ReplyShape::Turn), int(parseHeadReply(cc.text, full, e)));
      TEST_ASSERT_EQUAL_STRING(cc.reply.c_str(), full.reply.c_str());
    }
    for (size_t L = 0; L < cc.text.size(); L++) {
      const std::string prefix = cc.text.substr(0, L);
      char msg[64];
      snprintf(msg, sizeof msg, "case %d cut %u", c, (unsigned)L);
      Turn t;
      ParseError e;
      const ReplyShape shape = parseHeadReply(prefix, t, e);
      TEST_ASSERT_TRUE_MESSAGE(shape == ReplyShape::Unusable, msg);   // never runs, never shown raw
      Salvage s;
      salvageTurnText(prefix, s);
      TEST_ASSERT_TRUE_MESSAGE(validUtf8(s.reply), msg);
      TEST_ASSERT_TRUE_MESSAGE(cc.reply.compare(0, s.reply.size(), s.reply) == 0, msg);
      TEST_ASSERT_EQUAL_MESSAGE(L > cc.replyClose, s.replyComplete, msg);
      if (s.replyComplete) TEST_ASSERT_EQUAL_STRING_MESSAGE(cc.reply.c_str(), s.reply.c_str(), msg);
      // No nested field ever rides the salvage.
      TEST_ASSERT_TRUE_MESSAGE(s.reply.find("session_ops") == std::string::npos, msg);
      checked++;
    }
  }
  TEST_ASSERT_TRUE(checked > 5000);
  char line[96];
  snprintf(line, sizeof line, "property: 60 replies, %d cut prefixes, none executed", checked);
  TEST_MESSAGE(line);
}

static void test_salvage_decodes_escapes_and_lone_surrogates() {
  Salvage s;
  TEST_ASSERT_TRUE(salvageTurnText(R"({"reply":"café 😀 \"q\" a\\b\nz"})", s));
  TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9 \xF0\x9F\x98\x80 \"q\" a\\b\nz", s.reply.c_str());
  TEST_ASSERT_TRUE(s.replyComplete);
  TEST_ASSERT_TRUE(salvageTurnText(R"({"reply":"x\udc00y"})", s));
  TEST_ASSERT_EQUAL_STRING("x\xEF\xBF\xBDy", s.reply.c_str());   // U+FFFD
  // Only the top-level reply: a nested "reply" key never answers.
  TEST_ASSERT_FALSE(salvageTurnText(R"({"device":[{"reply":"nested"}]})", s));
  TEST_ASSERT_TRUE(salvageTurnText(R"({"device":[{"reply":"nested"}],"ask":"A?"})", s));
  TEST_ASSERT_EQUAL_STRING("", s.reply.c_str());
  TEST_ASSERT_EQUAL_STRING("A?", s.ask.c_str());
  // A cut string backs up to its last sentence end.
  TEST_ASSERT_TRUE(salvageTurnText(R"({"reply":"First part is here and long enough to count. Second wo)", s));
  TEST_ASSERT_EQUAL_STRING("First part is here and long enough to count.", s.reply.c_str());
  TEST_ASSERT_FALSE(s.replyComplete);
}

static void test_cut_errors_are_recognized() {
  for (const char* e : {"truncated at max_tokens inside a tool call", "cut at the output limit (12000 tokens)",
                        "cut: max_output_tokens", "finish_reason=length",
                        "response too large for the device"})
    TEST_ASSERT_TRUE_MESSAGE(isCutError(e), e);
  for (const char* e : {"messages HTTP 500", "network", "", "schema parse",
                        "timeout: the provider did not answer in time", "messages HTTP 400: bad",
                        // a refused max_tokens VALUE is a bad request, not a cut answer
                        "messages HTTP 400: max_tokens: 100000 > 64000, the maximum allowed"})
    TEST_ASSERT_FALSE_MESSAGE(isCutError(e), e);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_field_table_matches_the_wire_schema);
  RUN_TEST(test_every_container_shape_is_repaired_or_reported);
  RUN_TEST(test_session_ops_item_shapes_are_repaired);
  RUN_TEST(test_seven_spawns_keep_six_and_report_one);
  RUN_TEST(test_every_spawn_intent_is_kept_or_reported);
  RUN_TEST(test_a_valid_turn_is_byte_identical_after_normalization);
  RUN_TEST(test_spawn_items_sent_through_the_pseudo_tool_are_read);
  RUN_TEST(test_a_cut_reply_never_executes_and_salvages_its_prefix);
  RUN_TEST(test_salvage_decodes_escapes_and_lone_surrogates);
  RUN_TEST(test_cut_errors_are_recognized);
  return UNITY_END();
}
