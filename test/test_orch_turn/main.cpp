#include <unity.h>

#include <string>

#include "nimbus/orch/turn.h"
#include "nimbus/orch/caps.h"

using namespace nimbus::orch;
using Code = ParseError::Code;

void setUp() {}
void tearDown() {}

// A minimal contract-valid turn: all six top-level keys present and typed, empty
// arrays where nothing is requested. Reused as a base for negative cases.
static std::string validFull() {
  return R"({
    "reply":"Working on it.",
    "memory":"user prefers metric units",
    "ask":"Which repo?",
    "device":[{"led":{"mode":"pulse","r":10,"g":20,"b":30}}],
    "spawn":[{"provider":"OpenAI","model":"gpt-x","skill":"web",
              "task":"research caravans","category":"research","note":"On it."}],
    "await":["job0003"]
  })";
}

// ---- happy path -------------------------------------------------------------

static void test_full_valid_turn_roundtrips() {
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(validFull(), t, e));
  TEST_ASSERT_TRUE(e.ok());

  TEST_ASSERT_EQUAL_STRING("Working on it.", t.reply.c_str());
  TEST_ASSERT_EQUAL_STRING("user prefers metric units", t.memory.c_str());
  TEST_ASSERT_EQUAL_STRING("Which repo?", t.ask.c_str());

  TEST_ASSERT_EQUAL(1u, t.device.size());
  TEST_ASSERT_EQUAL(1u, t.spawn.size());
  TEST_ASSERT_EQUAL(1u, t.await_.size());
  TEST_ASSERT_EQUAL_STRING("job0003", t.await_[0].c_str());

  const Spawn& s = t.spawn[0];
  TEST_ASSERT_EQUAL_STRING("openai", s.provider.c_str());   // lowercased on copy
  TEST_ASSERT_EQUAL_STRING("gpt-x", s.model.c_str());
  TEST_ASSERT_EQUAL_STRING("web", s.skill.c_str());
  TEST_ASSERT_EQUAL_STRING("research caravans", s.task.c_str());
  TEST_ASSERT_EQUAL_STRING("research", s.category.c_str());
  TEST_ASSERT_EQUAL_STRING("On it.", s.note.c_str());
}

// All six top-level keys present but every array empty - a fully valid "no-op" turn.
static void test_empty_arrays_are_valid() {
  const char* j = R"({"reply":"","memory":"","ask":"",
                      "device":[],"spawn":[],"await":[]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_TRUE(e.ok());
  TEST_ASSERT_EQUAL_STRING("", t.reply.c_str());
  TEST_ASSERT_EQUAL(0u, t.device.size());
  TEST_ASSERT_EQUAL(0u, t.spawn.size());
  TEST_ASSERT_EQUAL(0u, t.await_.size());
}

// ---- required-key enforcement (strict at the wire) --------------------------

// Only reply/memory/ask are required; when absent each rejects with MissingField.
static void test_missing_each_required_key_rejects() {
  const char* keys[] = {"reply", "memory", "ask"};
  const char* bodies[] = {
    R"({"memory":"","ask":""})",     // no reply
    R"({"reply":"","ask":""})",      // no memory
    R"({"reply":"","memory":""})",   // no ask
  };
  for (int i = 0; i < 3; i++) {
    Turn t;
    ParseError e;
    TEST_ASSERT_FALSE_MESSAGE(parseTurn(bodies[i], t, e), keys[i]);
    TEST_ASSERT_EQUAL_MESSAGE(int(Code::MissingField), int(e.code), keys[i]);
    TEST_ASSERT_EQUAL_STRING(keys[i], e.detail.c_str());
  }
}

// Arrays are OPTIONAL now (Q3): a turn with ONLY the required strings is valid,
// every array empty. This is the shape a new turn takes when it just replies.
static void test_arrays_optional_reply_only() {
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(R"({"reply":"hi","memory":"","ask":""})", t, e));
  TEST_ASSERT_TRUE(e.ok());
  TEST_ASSERT_EQUAL_STRING("hi", t.reply.c_str());
  TEST_ASSERT_EQUAL(0u, t.device.size());
  TEST_ASSERT_EQUAL(0u, t.spawn.size());
  TEST_ASSERT_EQUAL(0u, t.mem_write.size());
  TEST_ASSERT_EQUAL(0u, t.session_ops.size());
}

// A mistyped top-level field: the required STRINGS stay strict; arrays are tolerant
// (owner field bug 2026-07-16 - a whole-turn failure leaked raw JSON to the user, so
// a present-but-not-array field now reads as empty instead of failing the turn).
static void test_mistyped_top_level_field_policy() {
  Turn t;
  ParseError e;
  // reply must be a string, given an array -> still a hard reject (no usable turn).
  TEST_ASSERT_FALSE(parseTurn(
    R"({"reply":[],"memory":"","ask":"","device":[],"spawn":[],"await":[]})", t, e));
  TEST_ASSERT_EQUAL(int(Code::WrongType), int(e.code));
  // spawn given a string -> TOLERANT: parses, spawn reads as empty, reply survives.
  TEST_ASSERT_TRUE(parseTurn(
    R"({"reply":"hi","memory":"","ask":"","device":[],"spawn":"x","await":[]})", t, e));
  TEST_ASSERT_EQUAL(0u, t.spawn.size());
  TEST_ASSERT_EQUAL_STRING("hi", t.reply.c_str());
}

// ---- spawn item validation --------------------------------------------------

// A spawn item missing a non-task key is TOLERANT: the missing field takes its
// default and the item is KEPT - a whole-turn reject here is what leaked raw JSON
// to the user (the wire schema is advisory on Anthropic, so the parser must not
// throw away a good reply over one slightly-off item).
static void test_spawn_item_missing_key_defaults() {
  // Missing "note" -> defaults "On it.".
  const char* j = R"({"reply":"ok","memory":"","ask":"","device":[],"await":[],
    "spawn":[{"provider":"openai","model":"m","skill":"","task":"go","category":"ops"}]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(1u, t.spawn.size());
  TEST_ASSERT_EQUAL_STRING("On it.", t.spawn[0].note.c_str());
  TEST_ASSERT_EQUAL_STRING("go", t.spawn[0].task.c_str());
}

// A spawn item whose TASK is unusable (non-string) is DROPPED - task is the one
// semantically-required field; the rest of the turn survives.
static void test_spawn_item_bad_task_dropped() {
  const char* j = R"({"reply":"ok","memory":"","ask":"","device":[],"await":[],
    "spawn":[{"provider":"openai","model":"m","skill":"","task":42,"category":"ops","note":"n"}]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(0u, t.spawn.size());
  TEST_ASSERT_EQUAL_STRING("ok", t.reply.c_str());
}

// A well-formed spawn item with an EMPTY task is dropped silently (not an error) -
// matches the device enqueueSpawn `if (!task[0]) return;`.
static void test_spawn_empty_task_dropped_not_error() {
  const char* j = R"({"reply":"","memory":"","ask":"","device":[],"await":[],
    "spawn":[
      {"provider":"","model":"","skill":"","task":"","category":"","note":""},
      {"provider":"","model":"","skill":"","task":"real work","category":"","note":""}
    ]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(1u, t.spawn.size());               // empty-task item dropped
  TEST_ASSERT_EQUAL_STRING("real work", t.spawn[0].task.c_str());
}

// Defaults are applied on copy: category "" -> "research", note "" -> "On it.".
static void test_spawn_defaults_applied() {
  const char* j = R"({"reply":"","memory":"","ask":"","device":[],"await":[],
    "spawn":[{"provider":"","model":"","skill":"","task":"t","category":"","note":""}]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(1u, t.spawn.size());
  TEST_ASSERT_EQUAL_STRING("research", t.spawn[0].category.c_str());
  TEST_ASSERT_EQUAL_STRING("On it.", t.spawn[0].note.c_str());
}

// The produced spawn vector is capped at kAgentMaxJobs (6) accepted items.
static void test_spawn_vector_capped_at_max_jobs() {
  std::string j = R"({"reply":"","memory":"","ask":"","device":[],"await":[],"spawn":[)";
  for (int i = 0; i < 10; i++) {
    if (i) j += ",";
    j += R"({"provider":"","model":"","skill":"","task":"t","category":"","note":""})";
  }
  j += "]}";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL((unsigned)kAgentMaxJobs, t.spawn.size());
}

// A spawn task longer than kSpawnTaskMax (1024) is UTF-8-safe capped: a 3-byte char
// straddling the boundary is dropped whole, never split.
static void test_spawn_task_utf8_capped() {
  // (kSpawnTaskMax-1) ASCII bytes, then a 3-byte char (€ = E2 82 AC) straddling
  // the cap. The cap would split it, so the whole char is dropped -> kept length
  // kSpawnTaskMax-1. Tracks the constant (was hard-coded 419; the cap rose to
  // 1024 in Glass Box A6).
  const size_t cap = (size_t)nimbus::orch::kSpawnTaskMax - 1;
  std::string task(cap, 'a');
  task += "\xE2\x82\xAC";  // one 3-byte UTF-8 char
  task += "tail";
  std::string j = R"({"reply":"","memory":"","ask":"","device":[],"await":[],
    "spawn":[{"provider":"","model":"","skill":"","task":")" + task +
    R"(","category":"","note":""}]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(1u, t.spawn.size());
  TEST_ASSERT_EQUAL(cap, t.spawn[0].task.size());      // multi-byte char dropped whole
}

// ---- await + device ---------------------------------------------------------

// Blank await entries are skipped; non-blank ones kept in order.
static void test_await_blanks_skipped() {
  const char* j = R"({"reply":"","memory":"","ask":"","device":[],"spawn":[],
    "await":["job0001","","job0002"]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(2u, t.await_.size());
  TEST_ASSERT_EQUAL_STRING("job0001", t.await_[0].c_str());
  TEST_ASSERT_EQUAL_STRING("job0002", t.await_[1].c_str());
}

// A non-string await entry is DROPPED (tolerant), never a turn error.
static void test_await_non_string_dropped() {
  const char* j = R"({"reply":"ok","memory":"","ask":"","device":[],"spawn":[],
    "await":[7,"job0009"]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(1u, t.await_.size());
  TEST_ASSERT_EQUAL_STRING("job0009", t.await_[0].c_str());
}

// Device elements are carried as raw JSON slices for the device-action validator.
static void test_device_carried_raw() {
  const char* j = R"({"reply":"","memory":"","ask":"","spawn":[],"await":[],
    "device":[{"led":{"mode":"solid"}},{"config":{"token":"secret"}}]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(2u, t.device.size());
  // Raw JSON preserved (serializer is compact / no spaces).
  TEST_ASSERT_EQUAL_STRING(R"({"led":{"mode":"solid"}})", t.device[0].json.c_str());
  TEST_ASSERT_EQUAL_STRING(R"({"config":{"token":"secret"}})", t.device[1].json.c_str());
}

// A non-object device element is DROPPED (tolerant); valid siblings survive. The
// device-action validator downstream re-checks every kept action anyway.
static void test_device_non_object_dropped() {
  const char* j = R"({"reply":"ok","memory":"","ask":"","spawn":[],"await":[],
    "device":["not-an-object",{"led":{"mode":"solid"}}]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(1u, t.device.size());
  TEST_ASSERT_EQUAL_STRING(R"({"led":{"mode":"solid"}})", t.device[0].json.c_str());
}

// ---- memory cap -------------------------------------------------------------

// An oversized memory string is clamped to kMemModelMax (1200), UTF-8-safe.
static void test_memory_clamped_to_cap() {
  std::string mem(2000, 'm');  // 2000 ASCII bytes
  std::string j = R"({"reply":"","memory":")" + mem +
    R"(","ask":"","device":[],"spawn":[],"await":[]})";
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL((unsigned)kMemModelMax, t.memory.size());
}

// ---- malformed input --------------------------------------------------------

// Garbage / truncated JSON rejects with JsonError and does not crash.
static void test_malformed_json_rejects() {
  Turn t;
  ParseError e;
  TEST_ASSERT_FALSE(parseTurn("this is not json", t, e));
  TEST_ASSERT_EQUAL(int(Code::JsonError), int(e.code));

  TEST_ASSERT_FALSE(parseTurn(R"({"reply":"x")", t, e));  // truncated
  TEST_ASSERT_EQUAL(int(Code::JsonError), int(e.code));

  TEST_ASSERT_FALSE(parseTurn("", t, e));                 // empty
  TEST_ASSERT_EQUAL(int(Code::JsonError), int(e.code));
}

// A JSON array/scalar at top level (valid JSON, not an object) rejects as NotObject.
static void test_non_object_top_level_rejects() {
  Turn t;
  ParseError e;
  TEST_ASSERT_FALSE(parseTurn("[1,2,3]", t, e));
  TEST_ASSERT_EQUAL(int(Code::NotObject), int(e.code));
  TEST_ASSERT_FALSE(parseTurn("42", t, e));
  TEST_ASSERT_EQUAL(int(Code::NotObject), int(e.code));
}

// On any failure the output Turn is left cleared (no half-parsed state leaks out).
static void test_session_ops_v4_fields_parse() {
  // v4.0.0: spawn ops carry skill/name/project/attach; caps enforced; attach
  // bounded at kSpawnAttachMax items.
  nimbus::orch::Turn t;
  nimbus::orch::ParseError e;
  const char* j =
      "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":\"spawn\",\"id\":null,"
      "\"task\":\"research X\",\"provider\":\"openai\",\"model\":null,"
      "\"skill\":\"deep-research\",\"name\":\"moon-facts\",\"project\":\"dr-moon-08061200\","
      "\"attach\":[\"dr-moon-08061200/notes.md\",\"dr-moon-08061200/plan.md\"]}]}";
  TEST_ASSERT_TRUE(nimbus::orch::parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(1, (int)t.session_ops.size());
  const auto& so = t.session_ops[0];
  TEST_ASSERT_EQUAL_STRING("deep-research", so.skill.c_str());
  TEST_ASSERT_EQUAL_STRING("moon-facts", so.name.c_str());
  TEST_ASSERT_EQUAL_STRING("dr-moon-08061200", so.project.c_str());
  TEST_ASSERT_EQUAL(2, (int)so.attach.size());
  TEST_ASSERT_EQUAL_STRING("dr-moon-08061200/notes.md", so.attach[0].c_str());
  // Legacy wire (no new fields) parses with empty defaults - backward compat.
  nimbus::orch::Turn t2; nimbus::orch::ParseError e2;
  const char* j2 = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":\"spawn\","
                   "\"id\":null,\"task\":\"y\",\"provider\":null,\"model\":null}]}";
  TEST_ASSERT_TRUE(nimbus::orch::parseTurn(j2, t2, e2));
  TEST_ASSERT_EQUAL_STRING("", t2.session_ops[0].skill.c_str());
  TEST_ASSERT_EQUAL(0, (int)t2.session_ops[0].attach.size());
}

static void test_failed_parse_clears_output() {
  Turn t;
  ParseError e;
  TEST_ASSERT_TRUE(parseTurn(validFull(), t, e));
  TEST_ASSERT_EQUAL(1u, t.spawn.size());
  // Now feed garbage into the SAME struct - it must be wiped.
  TEST_ASSERT_FALSE(parseTurn("nope", t, e));
  TEST_ASSERT_EQUAL(0u, t.spawn.size());
  TEST_ASSERT_EQUAL(0u, t.device.size());
  TEST_ASSERT_EQUAL_STRING("", t.reply.c_str());
}

// ---- live-integration fields (mem_write / mem_query / session_ops) ----------

static void test_mem_write_parsed_and_clamped() {
  const char* j = R"({"reply":"","memory":"","ask":"","mem_write":[
    {"content":"owner likes teal","importance":0.9,"permanent":true},
    {"content":"clamp me","importance":5.0}]})";
  Turn t; ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(2u, t.mem_write.size());
  TEST_ASSERT_EQUAL_STRING("owner likes teal", t.mem_write[0].content.c_str());
  TEST_ASSERT_TRUE(t.mem_write[0].permanent);
  TEST_ASSERT_EQUAL_FLOAT(0.9, t.mem_write[0].importance);
  TEST_ASSERT_EQUAL_FLOAT(1.0, t.mem_write[1].importance);   // clamped from 5.0
  TEST_ASSERT_FALSE(t.mem_write[1].permanent);               // default
}

// TOLERANT: a malformed / empty-content mem_write item is dropped, not a turn error.
static void test_mem_write_tolerant_drops_bad() {
  const char* j = R"({"reply":"","memory":"","ask":"","mem_write":[
    {"content":""}, {"noContent":1}, "not-an-object", {"content":"keep"}]})";
  Turn t; ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));   // still a valid turn
  TEST_ASSERT_EQUAL(1u, t.mem_write.size());
  TEST_ASSERT_EQUAL_STRING("keep", t.mem_write[0].content.c_str());
}

static void test_mem_query_parsed() {
  const char* j = R"({"reply":"","memory":"","ask":"","mem_query":["  what is my tz  ","",42]})";
  Turn t; ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));           // 42 dropped tolerantly, not an error
  TEST_ASSERT_EQUAL(1u, t.mem_query.size());
  TEST_ASSERT_EQUAL_STRING("what is my tz", t.mem_query[0].c_str());  // trimmed
}

static void test_session_ops_parsed() {
  const char* j = R"({"reply":"","memory":"","ask":"","session_ops":[
    {"op":"spawn","task":"research pricing","provider":"OpenAI"},
    {"op":"tell","id":"job0002","message":"focus on Q3"},
    {"op":"terminate","id":"job0003"},
    {"op":"list"}]})";
  Turn t; ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(4u, t.session_ops.size());
  TEST_ASSERT_EQUAL_STRING("spawn", t.session_ops[0].op.c_str());
  TEST_ASSERT_EQUAL_STRING("research pricing", t.session_ops[0].task.c_str());
  TEST_ASSERT_EQUAL_STRING("openai", t.session_ops[0].provider.c_str());  // lowercased
  TEST_ASSERT_EQUAL_STRING("focus on Q3", t.session_ops[1].message.c_str());
  TEST_ASSERT_EQUAL_STRING("job0003", t.session_ops[2].id.c_str());
}

// Unknown op or non-object dropped; the turn stays valid.
static void test_session_ops_unknown_op_dropped() {
  const char* j = R"({"reply":"","memory":"","ask":"","session_ops":[
    {"op":"bogus","id":"x"}, {"noOp":1}, {"op":"list"}]})";
  Turn t; ParseError e;
  TEST_ASSERT_TRUE(parseTurn(j, t, e));
  TEST_ASSERT_EQUAL(1u, t.session_ops.size());
  TEST_ASSERT_EQUAL_STRING("list", t.session_ops[0].op.c_str());
}

// Backward compatibility: an OLD full 6-field turn (with spawn/await/device) still
// parses and applies exactly as before - the new fields simply stay empty.
static void test_backward_compat_old_six_field_turn() {
  Turn t; ParseError e;
  TEST_ASSERT_TRUE(parseTurn(validFull(), t, e));
  TEST_ASSERT_TRUE(e.ok());
  TEST_ASSERT_EQUAL(1u, t.spawn.size());       // old spawn[] still parsed
  TEST_ASSERT_EQUAL(0u, t.mem_write.size());   // new fields empty
  TEST_ASSERT_EQUAL(0u, t.session_ops.size());
}

static void test_scratchpad_field_parses_and_tiers_replace() {
  // v4.1.0: the inline scratchpad update. A non-null tier is flagged for replace;
  // null active leaves it unchanged; the whole field null => not present.
  nimbus::orch::Turn t;
  nimbus::orch::ParseError e;
  const char* j =
      "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\",\"scratchpad\":{"
      "\"active\":\"drafting the report\",\"short\":[\"step 1\",\" step 2 \",\"\"],"
      "\"mid\":null,\"long\":[\"ship v4.1\"]}}";
  TEST_ASSERT_TRUE(nimbus::orch::parseTurn(j, t, e));
  TEST_ASSERT_TRUE(t.scratchpad.present);
  TEST_ASSERT_TRUE(t.scratchpad.hasActive);
  TEST_ASSERT_EQUAL_STRING("drafting the report", t.scratchpad.active.c_str());
  TEST_ASSERT_TRUE(t.scratchpad.hasShort);
  TEST_ASSERT_EQUAL(2, (int)t.scratchpad.shortItems.size());   // blank dropped, trimmed
  TEST_ASSERT_EQUAL_STRING("step 2", t.scratchpad.shortItems[1].c_str());
  TEST_ASSERT_FALSE(t.scratchpad.hasMid);                      // null => untouched
  TEST_ASSERT_TRUE(t.scratchpad.hasLong);
  TEST_ASSERT_EQUAL(1, (int)t.scratchpad.longItems.size());
}

static void test_scratchpad_null_is_no_change() {
  nimbus::orch::Turn t;
  nimbus::orch::ParseError e;
  const char* j = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\",\"scratchpad\":null}";
  TEST_ASSERT_TRUE(nimbus::orch::parseTurn(j, t, e));
  TEST_ASSERT_FALSE(t.scratchpad.present);   // null => apply skipped
}

static void test_scratchpad_absent_is_backward_compatible() {
  nimbus::orch::Turn t;
  nimbus::orch::ParseError e;
  const char* j = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  TEST_ASSERT_TRUE(nimbus::orch::parseTurn(j, t, e));   // old turns still valid
  TEST_ASSERT_FALSE(t.scratchpad.present);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_full_valid_turn_roundtrips);
  RUN_TEST(test_empty_arrays_are_valid);
  RUN_TEST(test_arrays_optional_reply_only);
  RUN_TEST(test_mem_write_parsed_and_clamped);
  RUN_TEST(test_mem_write_tolerant_drops_bad);
  RUN_TEST(test_mem_query_parsed);
  RUN_TEST(test_session_ops_parsed);
  RUN_TEST(test_session_ops_unknown_op_dropped);
  RUN_TEST(test_backward_compat_old_six_field_turn);
  RUN_TEST(test_missing_each_required_key_rejects);
  RUN_TEST(test_mistyped_top_level_field_policy);
  RUN_TEST(test_spawn_item_missing_key_defaults);
  RUN_TEST(test_spawn_item_bad_task_dropped);
  RUN_TEST(test_spawn_empty_task_dropped_not_error);
  RUN_TEST(test_spawn_defaults_applied);
  RUN_TEST(test_spawn_vector_capped_at_max_jobs);
  RUN_TEST(test_spawn_task_utf8_capped);
  RUN_TEST(test_await_blanks_skipped);
  RUN_TEST(test_await_non_string_dropped);
  RUN_TEST(test_device_carried_raw);
  RUN_TEST(test_device_non_object_dropped);
  RUN_TEST(test_memory_clamped_to_cap);
  RUN_TEST(test_malformed_json_rejects);
  RUN_TEST(test_non_object_top_level_rejects);
  RUN_TEST(test_session_ops_v4_fields_parse);
  RUN_TEST(test_scratchpad_field_parses_and_tiers_replace);
  RUN_TEST(test_scratchpad_null_is_no_change);
  RUN_TEST(test_scratchpad_absent_is_backward_compatible);
  RUN_TEST(test_failed_parse_clears_output);
  return UNITY_END();
}
