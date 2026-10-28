// Who may run each action the head can take (CUM-813): the class table.
//
// One expected row per action, one column per caller class. The table in
// lib/core/src/orch_action_rules.cpp must match it cell for cell, and a new action
// without a rule fails here three ways: a tool the firmware registers with no row,
// a device action or config knob the turn schema offers with no row, a knob the
// validator can set with no strippable row. The dispatch seam (ToolRegistry with
// installActionRules) is driven for every registered tool and every caller class,
// so the table is proven where it is enforced, not only where it is written.
#include <unity.h>

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/action_rules.h"
#include "nimbus/orch/connector_grant.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/device_actions.h"
#include "nimbus/orch/result_store.h"
#include "nimbus/orch/session_tools.h"
#include "nimbus/orch/orch_schema.h"   // ORCH_SCHEMA_BODY: the device actions and knobs the head is offered
#include "nimbus/orch/tool_registry.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

// ---- caller classes ------------------------------------------------------------

enum Cls { OwnerTurn = 0, Member, Guest, Routine, Synthesis, SystemTurn, McpUser, Serial, kClasses };
const char* const kClassNames[kClasses] = {"owner turn", "member", "guest", "routine",
                                           "synthesis", "system turn", "/mcp User", "serial"};

struct ClassCaller {
  Principal who;
  bool unattended;
};

// Each class as the device builds it: the turn's chat principal (roleOfChat: the owner's
// Telegram chat, web, voice and serial are Admin; a member is User; a guest Guest, a
// public-mode stranger User or Guest), the turn's unattended flag (a routine or wakeup
// and the device-staged system turn run on the owner's chat with the flag set; the
// results summary runs on the chat that started the sub-agents - modelled here for the
// owner's, while a member's runs as that member, test_harness_turn
// test_cum830_a_turn_reads_only_its_own_chats_results), and the LAN endpoint's own
// principal (web_memory.cpp /mcp).
ClassCaller classCaller(int c) {
  switch (c) {
    case OwnerTurn:  return {principalForRole("1001", Role::Admin), false};
    case Member:     return {principalForRole("2002", Role::User), false};
    case Guest:      return {principalForRole("3003", Role::Guest), false};
    case Routine:
    case Synthesis:
    case SystemTurn: return {principalForRole("1001", Role::Admin), true};
    case McpUser: {
      Principal p = principalForRole(kMcpNs, Role::User);
      p.ns = kMcpNs;
      return {p, false};
    }
    case Serial:
    default:         return {principalForRole("serial", Role::Admin), false};
  }
}

// The chat each class's turn runs for (the owner's unattended turns run on the owner's
// Telegram chat; the serial console on "serial"); a member's and a guest's chats come
// from their principals, and the LAN has none.
const char* turnChatOf(int c) {
  switch (c) {
    case Member:  return "2002";
    case Guest:   return "3003";
    case McpUser: return "";
    case Serial:  return "serial";
    default:      return "1001";
  }
}

ActionCaller callerOf(int c) {
  const ClassCaller cc = classCaller(c);
  return actionCaller(cc.who, cc.unattended);
}

// ---- the expected class table --------------------------------------------------
// Columns: owner turn, member, guest, routine, synthesis, system turn, /mcp User,
// serial. 'A' allowed, '-' refused.
//   open            AAAAAAAA   every caller; the handler draws the data line
//   turn            AAAAAA-A   every turn caller; never the LAN endpoint
//   owner           A--AAA-A   the owner's principal, attended or not
//   owner-attended  A------A   the owner writing now
struct Expect {
  const char* key;
  const char* cells;
};

const char kOpen[] = "AAAAAAAA";
const char kTurn[] = "AAAAAA-A";
const char kOwner[] = "A--AAA-A";
const char kOwnerNow[] = "A------A";

const Expect kExpect[] = {
    // device[] actions and mid-turn device.control
    {"device.led", kOwner},
    {"device.lights", kOwner},
    {"device.tts", kOwner},
    {"device.reboot", kOwnerNow},
    {"device.orch_model", kOwnerNow},
    {"device.config", kOwner},
    {"device.config.ledBrightness", kOwner},
    {"device.config.priority", kOwnerNow},
    {"device.config.posture", kOwner},
    {"device.config.profile", kOwner},
    {"device.config.theme", kOwner},
    {"device.config.attnHoldMs", kOwner},
    {"device.config.ttsVoice", kOwnerNow},
    {"device.config.ttsProv", kOwnerNow},
    {"device.config.ttsOn", kOwnerNow},
    {"device.config.sttProv", kOwnerNow},
    {"device.config.sfxLvlN", kOwner},
    {"device.config.sfxLvlO", kOwner},
    {"device.config.sfxVol", kOwner},
    {"device.config.sfxTheme", kOwner},
    {"device.config.sleepOvr", kOwnerNow},
    {"device.config.brightOvr", kOwnerNow},
    {"device.config.devName", kOwnerNow},
    // turn fields
    {"turn.scratchpad", kOwner},
    {"turn.ask", kOwner},
    {"turn.spawn", kTurn},
    // memory
    {"memory.write", kOpen},
    {"memory.search", kOpen},
    {"memory.update", kOpen},
    {"memory.pin", kOpen},
    {"memory.episodic", kOpen},
    {"memory.archive", kOpen},
    {"memory.config", kOpen},
    {"memory.config.update", kOwnerNow},
    {"memory.scratchpad", kOwner},
    // standing rules
    {"prefs.edit", kOwner},
    {"prefs.edit.change", kOwnerNow},
    // results, sub-agents
    {"results.get", kOpen},
    {"results.list", kOpen},
    {"session.spawn", kTurn},
    {"session.tell", kOwnerNow},
    {"session.poll", kOpen},
    {"session.list", kOpen},
    {"session.terminate", kOwnerNow},
    // web, models
    {"web.search", kTurn},
    {"models.list", kOpen},
    {"models.list.refresh", kOwner},
    // files
    {"artifact.save", kOpen},
    {"files.append", kOpen},
    {"files.fetch", kOpen},
    {"files.list", kOpen},
    {"files.pdf", kOpen},
    {"files.read", kOpen},
    {"files.search", kOpen},
    {"files.send", kTurn},
    {"files.send.other_chat", kOwnerNow},
    {"files.share", kOpen},
    {"files.share.owner", kOwnerNow},
    {"files.stat", kOpen},
    {"image.generate", kTurn},
    {"image.look", kTurn},
    {"image.note", kTurn},
    // docs, routines, replies, skills, people
    {"docs.list", kOpen},
    {"docs.read", kOpen},
    {"docs.search", kOpen},
    {"food.portions", kOpen},
    {"loop.create", kOwnerNow},
    {"loop.cancel", kOwnerNow},
    {"loop.list", kOwner},
    {"wakeup.set", kOwner},
    {"reply.telegram", kTurn},
    {"reply.telegram.other_chat", kOwnerNow},
    {"reply.speak", kOwner},
    {"skill.get", kOpen},
    {"skill.list", kOpen},
    {"skill.save", kOwnerNow},
    {"skill.delete", kOwnerNow},
    {"system.health", kOpen},
    {"tenant.list", kOwnerNow},
    {"tenant.set_role", kOwnerNow},
    {"tenant.set_quota", kOwnerNow},
    // device
    {"device.status", kOwner},
    {"device.log", kOwner},
    {"device.selftest", kOpen},
    {"device.selftest.audible", kOwnerNow},
    {"device.control", kOwner},
    {"ota.status", kOpen},
    // media
    {"media.list", kOpen},
    {"media.play", kOwner},
    {"media.pause", kOwner},
    {"media.stop", kOwner},
};

const Expect* expectFor(const std::string& key) {
  for (const Expect& e : kExpect)
    if (key == e.key) return &e;
  return nullptr;
}

// Keys a tool call narrows to by its arguments (not registry names themselves).
bool argumentKey(const std::string& key) {
  static const char* const kKeys[] = {"memory.config.update", "models.list.refresh", "device.selftest.audible",
                                      "prefs.edit.change",    "reply.telegram.other_chat", "files.send.other_chat",
                                      "files.share.owner"};
  for (const char* k : kKeys)
    if (key == k) return true;
  return false;
}

bool deviceOrTurnKey(const std::string& key) {
  return key.rfind("device.config", 0) == 0 || key.rfind("turn.", 0) == 0 || key == "device.led" ||
         key == "device.lights" || key == "device.tts" || key == "device.reboot" || key == "device.orch_model";
}

// ---- source scans (the test_tool_taint pattern) ----------------------------------

std::string readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

void walk(const std::string& dir, std::vector<std::string>& out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = dir + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) walk(path, out);
    else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".cpp") == 0) out.push_back(path);
  }
  closedir(d);
}

bool toolNameShape(const std::string& s) {
  const size_t dot = s.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size()) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || c == '_' || c == '.')) return false;
  return true;
}

// Every string literal right after ".add(" that looks like a tool name, in lib and src.
std::set<std::string> registeredNames() {
  std::vector<std::string> files;
  walk("lib", files);
  walk("src", files);
  const std::set<std::string> notTools = {"reasoning.encrypted_content"};   // a JSON array value
  std::set<std::string> names;
  for (const std::string& f : files) {
    const std::string src = readFile(f);
    for (size_t p = src.find(".add("); p != std::string::npos; p = src.find(".add(", p + 1)) {
      size_t q = p + 5;
      while (q < src.size() && (src[q] == ' ' || src[q] == '\n' || src[q] == '\r' || src[q] == '\t')) q++;
      if (q >= src.size() || src[q] != '"') continue;
      const size_t end = src.find('"', q + 1);
      if (end == std::string::npos) continue;
      const std::string lit = src.substr(q + 1, end - q - 1);
      if (toolNameShape(lit) && !notTools.count(lit)) names.insert(lit);
    }
  }
  return names;
}

bool identChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// The first identifier on one line of an enum body, comments dropped ("" for none).
std::string firstIdent(const std::string& line) {
  const std::string code = line.substr(0, line.find("//"));
  size_t b = 0;
  while (b < code.size() && !identChar(code[b])) b++;
  size_t e = b;
  while (e < code.size() && identChar(code[e])) e++;
  return code.substr(b, e - b);
}

// The identifiers between "enum class ActionKind" and its closing brace.
std::vector<std::string> actionKindNames() {
  const std::string h = readFile("lib/core/include/nimbus/orch/device_actions.h");
  std::vector<std::string> out;
  const size_t b = h.find("enum class ActionKind");
  if (b == std::string::npos) return out;
  const size_t open = h.find('{', b), close = h.find("};", b);
  const std::string body = h.substr(open + 1, close - open - 1);
  for (size_t p = 0; p < body.size();) {
    size_t eol = body.find('\n', p);
    if (eol == std::string::npos) eol = body.size();
    const std::string id = firstIdent(body.substr(p, eol - p));
    if (!id.empty()) out.push_back(id);
    p = eol + 1;
  }
  return out;
}

// Every "bool has<Knob>" field of ValidatedAction.
std::vector<std::string> hasFlags() {
  const std::string h = readFile("lib/core/include/nimbus/orch/device_actions.h");
  std::vector<std::string> out;
  const size_t b = h.find("struct ValidatedAction");
  const size_t e = h.find("};", b);
  for (size_t p = h.find("bool", b); p != std::string::npos && p < e; p = h.find("bool", p + 4)) {
    size_t q = p + 4;
    while (q < e && h[q] == ' ') q++;
    if (h.compare(q, 3, "has") != 0) continue;
    std::string id;
    while (q < e && ((h[q] >= 'A' && h[q] <= 'Z') || (h[q] >= 'a' && h[q] <= 'z'))) id.push_back(h[q++]);
    out.push_back(id);
  }
  return out;
}

ArduinoJson::JsonDocument schema() {
  ArduinoJson::JsonDocument d;
  deserializeJson(d, ORCH_SCHEMA_BODY, ArduinoJson::DeserializationOption::NestingLimit(16));
  return d;
}

}  // namespace

// ---- the table, cell by cell ----------------------------------------------------

static void test_class_table_matches_every_cell() {
  int cells = 0;
  for (const Expect& e : kExpect) {
    TEST_ASSERT_EQUAL_MESSAGE(kClasses, (int)std::strlen(e.cells), e.key);
    for (int c = 0; c < kClasses; c++) {
      const bool want = e.cells[c] == 'A';
      const std::string msg = std::string(e.key) + " for " + kClassNames[c];
      TEST_ASSERT_EQUAL_MESSAGE(want, actionAllowed(e.key, callerOf(c)), msg.c_str());
      // A refusal always says why; an allowed action has none.
      TEST_ASSERT_EQUAL_MESSAGE(want, actionRefusal(e.key, callerOf(c)).empty(), msg.c_str());
      cells++;
    }
  }
  TEST_MESSAGE(("property: " + std::to_string(cells) + " cells (" + std::to_string(sizeof(kExpect) / sizeof(kExpect[0])) +
                " actions x " + std::to_string(kClasses) + " caller classes) match the rule table")
                   .c_str());
}

// The table and the expectation list are the same set: a new row needs a deliberate
// expected row here, and an expected row needs a rule.
static void test_every_row_has_an_expected_row_and_back() {
  size_t n = 0;
  const ActionRuleRow* rows = actionRuleTable(n);
  std::set<std::string> seen;
  for (size_t i = 0; i < n; i++) {
    TEST_ASSERT_NOT_NULL_MESSAGE(expectFor(rows[i].key), (std::string("no expected class row for ") + rows[i].key).c_str());
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(rows[i].key).second, rows[i].key);   // no duplicates
    TEST_ASSERT_TRUE_MESSAGE(rows[i].what && rows[i].what[0], rows[i].key);   // the refusal names it
  }
  for (const Expect& e : kExpect)
    TEST_ASSERT_NOT_NULL_MESSAGE(actionRuleRow(e.key), (std::string("no rule for ") + e.key).c_str());
}

// ---- completeness: a new action without a rule fails ----------------------------

static void test_every_registered_tool_has_a_rule() {
  const std::set<std::string> names = registeredNames();
  TEST_ASSERT_TRUE_MESSAGE(names.size() >= 40, "the source scan found too few tool registrations");
  TEST_ASSERT_TRUE(names.count("device.control") && names.count("memory.write") && names.count("media.play"));
  for (const std::string& n : names)
    TEST_ASSERT_NOT_NULL_MESSAGE(actionRuleRow(n), ("no access rule for registered tool " + n).c_str());
  // No stale rows: every tool-name row names a tool the firmware still registers.
  size_t count = 0;
  const ActionRuleRow* rows = actionRuleTable(count);
  for (size_t i = 0; i < count; i++) {
    const std::string k = rows[i].key;
    if (deviceOrTurnKey(k) || argumentKey(k)) continue;
    TEST_ASSERT_TRUE_MESSAGE(names.count(k) == 1, ("rule for a tool nothing registers: " + k).c_str());
  }
  TEST_MESSAGE(("property: " + std::to_string(names.size()) + " registered tools, each with an access rule").c_str());
}

static void test_every_schema_device_action_and_knob_has_a_rule() {
  ArduinoJson::JsonDocument d = schema();
  ArduinoJson::JsonArrayConst branches = d["properties"]["device"]["items"]["anyOf"].as<ArduinoJson::JsonArrayConst>();
  TEST_ASSERT_TRUE_MESSAGE(branches.size() >= 6, "the schema walk found too few device actions");
  int knobs = 0;
  for (ArduinoJson::JsonObjectConst b : branches) {
    const std::string type = b["properties"]["type"]["const"] | "";
    TEST_ASSERT_FALSE(type.empty());
    const std::string key = "device." + type;
    TEST_ASSERT_NOT_NULL_MESSAGE(actionRuleRow(key), ("no rule for device action " + key).c_str());
    if (type != "config") continue;
    for (ArduinoJson::JsonPairConst kv : b["properties"].as<ArduinoJson::JsonObjectConst>()) {
      const std::string knob = kv.key().c_str();
      if (knob == "type") continue;
      TEST_ASSERT_NOT_NULL_MESSAGE(actionRuleRow(configKnobKey(knob.c_str())), ("no rule for knob " + knob).c_str());
      bool strippable = false;
      size_t n = 0;
      const ConfigKnob* t = configKnobTable(n);
      for (size_t i = 0; i < n; i++) strippable = strippable || knob == t[i].knob;
      TEST_ASSERT_TRUE_MESSAGE(strippable, ("knob with no has-flag row (cannot be stripped): " + knob).c_str());
      knobs++;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(knobs >= 17, "the schema walk found too few config knobs");
}

// Every ActionKind the validator can produce maps to a key with a rule.
static void test_every_action_kind_has_a_rule() {
  const std::vector<std::string> kinds = actionKindNames();
  TEST_ASSERT_TRUE_MESSAGE(kinds.size() >= 7, "the enum scan found too few action kinds");
  for (size_t i = 0; i < kinds.size(); i++) {
    const ActionKind k = (ActionKind)i;
    if (k == ActionKind::Unknown) continue;
    const std::string key = deviceActionKey(k);
    TEST_ASSERT_FALSE_MESSAGE(key.empty(), ("no device action key for ActionKind::" + kinds[i]).c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(actionRuleRow(key), ("no rule for ActionKind::" + kinds[i]).c_str());
  }
}

// Every knob flag the validator can set is in the strippable knob table, and each row
// maps to the flag its knob really sets (validated round trip).
static void test_every_validator_knob_is_strippable() {
  const std::map<std::string, std::string> sample = {
      {"ledBrightness", "10"}, {"priority", "\"openai\""}, {"posture", "\"dark\""},  {"profile", "\"balanced\""},
      {"theme", "\"teal\""},   {"attnHoldMs", "60000"},    {"ttsVoice", "\"x\""},    {"ttsProv", "\"openai\""},
      {"ttsOn", "true"},       {"sttProv", "\"openai\""},  {"sfxLvlN", "1"},         {"sfxLvlO", "1"},
      {"sfxVol", "10"},        {"sfxTheme", "\"pulse\""},  {"sleepOvr", "true"},     {"brightOvr", "true"},
      {"devName", "\"desk\""}};
  size_t n = 0;
  const ConfigKnob* t = configKnobTable(n);
  for (const std::string& flag : hasFlags()) {
    if (flag == "hasBrightness" || flag == "hasOrchModel") continue;   // led's own field; orch_model's marker
    bool found = false;
    for (size_t i = 0; i < n; i++) {
      std::string want = std::string("has") + t[i].knob;
      want[3] = (char)(want[3] - 'a' + 'A');
      found = found || flag == want;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, ("validator knob flag with no rule row: " + flag).c_str());
  }
  for (size_t i = 0; i < n; i++) {
    const auto it = sample.find(t[i].knob);
    TEST_ASSERT_TRUE_MESSAGE(it != sample.end(), (std::string("no sample value for knob ") + t[i].knob).c_str());
    const std::string json = std::string("{\"type\":\"config\",\"") + t[i].knob + "\":" + it->second + "}";
    ArduinoJson::JsonDocument d;
    deserializeJson(d, json);
    const ValidatedAction va = validateAction(d.as<ArduinoJson::JsonVariantConst>());
    TEST_ASSERT_TRUE_MESSAGE(va.*(t[i].has), t[i].knob);
    for (size_t j = 0; j < n; j++)
      if (j != i) TEST_ASSERT_FALSE_MESSAGE(va.*(t[j].has), t[i].knob);   // exactly its own flag
  }
}

// ---- the dispatch seam: every registered tool x every caller class ---------------

static void test_dispatch_seam_enforces_the_table_for_every_tool() {
  const std::set<std::string> names = registeredNames();
  ToolRegistry reg;
  int ran = 0;
  for (const std::string& n : names)
    reg.add(n, "stub", [&ran](ArduinoJson::JsonObjectConst, const Principal&) {
      ran++;
      return ToolResult::ok("ran");
    });
  bool unattended = false;
  std::string turnChat;
  installActionRules(reg, [&unattended] { return unattended; }, [&turnChat] { return turnChat; });
  int calls = 0;
  for (const std::string& n : names) {
    TEST_ASSERT_NOT_NULL_MESSAGE(expectFor(n), n.c_str());
    for (int c = 0; c < kClasses; c++) {
      const ClassCaller cc = classCaller(c);
      unattended = cc.unattended;
      turnChat = turnChatOf(c);
      ran = 0;
      ArduinoJson::JsonDocument args;
      args.to<ArduinoJson::JsonObject>();
      // A call with no arguments is what the tool does by default (files.share with no
      // "share" shares): the expected row is the key that call narrows to.
      const std::string key = toolActionKey(n, args.as<ArduinoJson::JsonObjectConst>(),
                                            actionCaller(cc.who, cc.unattended, turnChat));
      const Expect* e = expectFor(key);
      TEST_ASSERT_NOT_NULL_MESSAGE(e, key.c_str());
      const ToolResult r = reg.dispatch(n, args.as<ArduinoJson::JsonObjectConst>(), cc.who);
      const bool want = e->cells[c] == 'A';
      const std::string msg = n + " for " + kClassNames[c] + ": " + r.error;
      TEST_ASSERT_EQUAL_MESSAGE(want, r.success, msg.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(want ? 1 : 0, ran, msg.c_str());   // a refused caller never reaches the handler
      calls++;
    }
  }
  TEST_MESSAGE(("property: " + std::to_string(calls) + " dispatches (every registered tool x every caller class)").c_str());
}

// The argument-narrowed keys, through the same seam.
static void test_dispatch_seam_narrows_by_argument() {
  ToolRegistry reg;
  for (const char* n :
       {"memory.config", "models.list", "device.selftest", "prefs.edit", "reply.telegram", "files.send", "files.share"})
    reg.add(n, "stub", [](ArduinoJson::JsonObjectConst, const Principal&) { return ToolResult::ok("ran"); });
  bool unattended = false;
  std::string turnChat;   // the chat the turn in flight runs for (the owner's own chat)
  installActionRules(reg, [&unattended] { return unattended; }, [&turnChat] { return turnChat; });
  struct Case {
    const char* tool;
    const char* args;
    const char* cells;
  };
  const Case cases[] = {
      {"memory.config", "{\"action\":\"view\"}", kOpen},
      {"memory.config", "{\"action\":\"update\",\"max_vectors\":1}", kOwnerNow},
      {"models.list", "{}", kOpen},
      {"models.list", "{\"refresh\":true}", kOwner},
      {"device.selftest", "{}", kOpen},
      {"device.selftest", "{\"audible\":true}", kOwnerNow},
      {"prefs.edit", "{\"action\":\"list\"}", kOwner},
      {"prefs.edit", "{\"action\":\"add\",\"text\":\"x\"}", kOwnerNow},
      {"prefs.edit", "{\"action\":\"remove\",\"id\":\"p1\"}", kOwnerNow},
      {"reply.telegram", "{\"text\":\"hi\"}", kTurn},
      {"reply.telegram", "{\"text\":\"hi\",\"chat_id\":\"9999\"}", kOwnerNow},
      {"files.send", "{\"project\":\"p\",\"name\":\"n\"}", kTurn},
      {"files.send", "{\"project\":\"p\",\"name\":\"n\",\"chat_id\":\"9999\"}", kOwnerNow},
      // sharing: the owner shares any file only when writing now; a member its own;
      // stopping a share stays open to everyone who may share
      {"files.share", "{\"project\":\"p\",\"name\":\"n\"}", "AAA---AA"},
      {"files.share", "{\"project\":\"p\",\"name\":\"n\",\"share\":true}", "AAA---AA"},
      {"files.share", "{\"project\":\"p\",\"name\":\"n\",\"share\":false}", kOpen},
  };
  for (const Case& k : cases)
    for (int c = 0; c < kClasses; c++) {
      const ClassCaller cc = classCaller(c);
      unattended = cc.unattended;
      turnChat = turnChatOf(c);
      ArduinoJson::JsonDocument d;
      deserializeJson(d, k.args);
      const ToolResult r = reg.dispatch(k.tool, d.as<ArduinoJson::JsonObjectConst>(), cc.who);
      const std::string msg = std::string(k.tool) + " " + k.args + " for " + kClassNames[c];
      TEST_ASSERT_EQUAL_MESSAGE(k.cells[c] == 'A', r.success, msg.c_str());
    }
  // A member may still reply into its OWN chat by naming it, and an owner routine into
  // the chat it runs for; naming any other chat is the owner writing now.
  ArduinoJson::JsonDocument own;
  deserializeJson(own, "{\"text\":\"hi\",\"chat_id\":\"2002\"}");
  unattended = false;
  TEST_ASSERT_TRUE(reg.dispatch("reply.telegram", own.as<ArduinoJson::JsonObjectConst>(), classCaller(Member).who).success);
  ArduinoJson::JsonDocument mine;
  deserializeJson(mine, "{\"text\":\"hi\",\"chat_id\":\"1001\"}");
  unattended = true;
  turnChat = "1001";
  TEST_ASSERT_TRUE(reg.dispatch("reply.telegram", mine.as<ArduinoJson::JsonObjectConst>(), classCaller(Routine).who).success);
  turnChat = "web";
  TEST_ASSERT_FALSE(reg.dispatch("reply.telegram", mine.as<ArduinoJson::JsonObjectConst>(), classCaller(Routine).who).success);
  TEST_ASSERT_FALSE(reg.dispatch("files.send", mine.as<ArduinoJson::JsonObjectConst>(), classCaller(Routine).who).success);
  // The LAN endpoint never borrows the turn's chat.
  unattended = false;
  turnChat = "1001";
  TEST_ASSERT_FALSE(reg.dispatch("reply.telegram", mine.as<ArduinoJson::JsonObjectConst>(), classCaller(McpUser).who).success);
}

// ---- fail closed, the plain words, the advertisement ------------------------------

static void test_unknown_and_connected_server_tools_fail_closed() {
  for (const char* k : {"brand.new_tool", "", "mcp.notion.create_page", "mcp.linear.search"})
    for (int c = 0; c < kClasses; c++)
      TEST_ASSERT_EQUAL_MESSAGE(c == OwnerTurn || c == Serial, actionAllowed(k, callerOf(c)), k);
  TEST_ASSERT_EQUAL(int(ActionRule::OwnerAttended), int(actionRuleFor("mcp.notion.create_page")));
  // An unapproved chat (role Unknown) runs nothing but the open actions, and a routine
  // firing into a member's chat is still that member.
  ActionCaller unknown = actionCaller(principalForRole("4004", Role::Unknown), false);
  ActionCaller memberRoutine = actionCaller(principalForRole("2002", Role::User), true);
  size_t n = 0;
  const ActionRuleRow* rows = actionRuleTable(n);
  for (size_t i = 0; i < n; i++) {
    const bool open = rows[i].rule == ActionRule::Open, turn = rows[i].rule == ActionRule::TurnOnly;
    TEST_ASSERT_EQUAL_MESSAGE(open, actionAllowed(rows[i].key, unknown), rows[i].key);   // a revoked chat: open only
    TEST_ASSERT_EQUAL_MESSAGE(open || turn, actionAllowed(rows[i].key, memberRoutine), rows[i].key);
  }
}

static void test_refusals_are_plain_words() {
  size_t n = 0;
  const ActionRuleRow* rows = actionRuleTable(n);
  for (size_t i = 0; i < n; i++)
    for (int c = 0; c < kClasses; c++) {
      const std::string why = actionRefusal(rows[i].key, callerOf(c));
      if (why.empty()) continue;
      // Says who may: the owner, or (a turn-only action refused to the LAN) a conversation.
      const char* who = rows[i].rule == ActionRule::TurnOnly ? "conversation" : "owner";
      TEST_ASSERT_TRUE_MESSAGE(why.find(who) != std::string::npos, why.c_str());
      TEST_ASSERT_TRUE_MESSAGE(why.find(rows[i].what) != std::string::npos, why.c_str());   // says what
      TEST_ASSERT_TRUE_MESSAGE(why.find("\xE2\x80\x94") == std::string::npos, why.c_str());   // no em dash
    }
  TEST_ASSERT_EQUAL_STRING("only the device's owner can change the main model",
                           actionRefusal("device.orch_model", callerOf(Member)).c_str());
  TEST_ASSERT_TRUE(actionRefusal("device.orch_model", callerOf(Routine)).find("in a message they write") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(actionRefusal("device.control", callerOf(McpUser)).find("LAN") != std::string::npos);
}

// A member's turn is not offered what it cannot call; the owner's list is unchanged.
static void test_member_is_not_advertised_refused_tools() {
  ToolRegistry reg;
  for (const char* n : {"memory.write", "device.control", "media.play", "reply.speak", "mcp.notion.search",
                        "session.terminate", "reply.telegram", "prefs.edit"})
    reg.add(n, "stub", [](ArduinoJson::JsonObjectConst, const Principal&) { return ToolResult::ok("ran"); });
  const size_t all = reg.toolSpecsFor(classCaller(OwnerTurn).who).size();
  TEST_ASSERT_EQUAL(8, (int)reg.toolSpecsFor(classCaller(Member).who).size());   // no resolver: unchanged
  installActionRules(reg, nullptr);
  TEST_ASSERT_EQUAL(all, reg.toolSpecsFor(classCaller(OwnerTurn).who).size());
  std::set<std::string> member;
  for (const auto& s : reg.toolSpecsFor(classCaller(Member).who)) member.insert(s.name);
  TEST_ASSERT_EQUAL(2, (int)member.size());
  TEST_ASSERT_TRUE(member.count("memory.write") && member.count("reply.telegram"));
}

// A chat with no tenant row resolves by the Telegram owner rule (the one the owner-only
// commands use): the freshly onboarded owner may change the device before the table
// first holds a row, another allow-listed chat is a member, anyone else nobody.
static void test_a_chat_without_a_row_follows_the_owner_rule() {
  // No Admin row yet (a freshly onboarded device): the Telegram owner rule decides.
  TEST_ASSERT_EQUAL(int(Role::Admin), int(roleWithoutRow(true, true, false)));
  TEST_ASSERT_EQUAL(int(Role::User), int(roleWithoutRow(true, false, false)));
  TEST_ASSERT_EQUAL(int(Role::Unknown), int(roleWithoutRow(false, false, false)));
  TEST_ASSERT_EQUAL(int(Role::Unknown), int(roleWithoutRow(false, true, false)));   // never an owner off the allowlist
  // Once the table holds an Admin row, position on the allowlist never makes anyone an
  // Admin: the owner who removed their own chat does not hand the device to the next
  // chat in line (the review's scenario).
  TEST_ASSERT_EQUAL(int(Role::User), int(roleWithoutRow(true, true, true)));
  TEST_ASSERT_EQUAL(int(Role::Unknown), int(roleWithoutRow(false, true, true)));
  const ActionCaller fresh = actionCaller(principalForRole("5005", roleWithoutRow(true, true, false)), false, "5005");
  TEST_ASSERT_TRUE(actionAllowed("device.orch_model", fresh));
  const ActionCaller next = actionCaller(principalForRole("6006", roleWithoutRow(true, true, true)), false, "6006");
  TEST_ASSERT_FALSE(actionAllowed("device.orch_model", next));
  // The owner-only commands read the same role: a demoted or revoked chat loses them.
  TEST_ASSERT_TRUE(mayRunOwnerCommands(Role::Admin));
  for (Role r : {Role::User, Role::Guest, Role::Unknown}) TEST_ASSERT_FALSE(mayRunOwnerCommands(r));
}

// The command gate on the device reads the tenant role, never the Telegram owner list
// alone (a demoted or revoked chat kept /model, /budget, /update and /mcp).
static void test_owner_commands_follow_the_tenant_role() {
  const std::string src = readFile("src/agent/orchestrator.cpp");
  const size_t at = src.find("const bool ownerCmd = nimbus::orch::isOwnerVerb(v);");
  TEST_ASSERT_TRUE(at != std::string::npos);
  const std::string gate = src.substr(at, 700);
  TEST_ASSERT_TRUE_MESSAGE(gate.find("mayRunOwnerCommands(roleOfChat(chatId))") != std::string::npos, gate.c_str());
  TEST_ASSERT_TRUE_MESSAGE(gate.find("telegram::isOwner(chatId)") == std::string::npos, gate.c_str());
}

// ---- CUM-828: whose connectors ride a request, per caller class --------------------

namespace {

ConnectorInfo conn(const char* name, const char* prov, const char* kind, const char* url = "",
                   const char* cid = "") {
  ConnectorInfo c;
  c.name = name;
  c.type = name;
  c.prov = prov;
  c.kind = kind;
  c.url = url;
  c.connectorId = cid;
  c.enabled = true;
  return c;
}

// One built-in and two of the owner's own connectors per provider.
std::vector<ConnectorInfo> ownerConnectors() {
  return {conn("code_interpreter", "openai", "builtin"), conn("github", "openai", "mcp", "https://gh.example/mcp"),
          conn("gmail", "openai", "connector", "", "connector_gmail"), conn("web_search", "mistral", "builtin"),
          conn("notion", "mistral", "connector", "", "notion"), conn("linear", "anthropic", "mcp", "https://ln.example/mcp")};
}

struct Attached {
  int openaiPrivate = 0, openaiBuiltin = 0, mistralPrivate = 0, mistralBuiltin = 0, anthropicServers = 0;
};

// Attach every provider's connectors exactly as a provider request does (the one
// path every head and sub-agent request takes), under the calling task's grant.
Attached attachAll() {
  const BearerFn bearer = [](const ConnectorInfo&) { return std::string("tok"); };
  Attached a;
  ArduinoJson::JsonDocument oa, mi, an;
  attachOpenAIWire(oa, ownerConnectors(), bearer);
  for (ArduinoJson::JsonObjectConst t : oa["tools"].as<ArduinoJson::JsonArrayConst>())
    (std::string(t["type"] | "") == "mcp" ? a.openaiPrivate : a.openaiBuiltin)++;
  attachMistralWire(mi, ownerConnectors());
  for (ArduinoJson::JsonObjectConst t : mi["tools"].as<ArduinoJson::JsonArrayConst>())
    (std::string(t["type"] | "") == "connector" ? a.mistralPrivate : a.mistralBuiltin)++;
  attachAnthropicWire(an, ownerConnectors(), bearer);
  a.anthropicServers = (int)an["mcp_servers"].as<ArduinoJson::JsonArrayConst>().size();
  return a;
}

}  // namespace

// A request for the owner (attended or not, any owner channel) carries the owner's
// connectors; a member's, a guest's and anything the LAN endpoint could start carry the
// provider's built-ins only. Without the grant in the attach builders every class gets
// the owner's connectors and this fails.
static void test_connector_grant_by_caller_class() {
  const char* const want = "A--AAA-A";   // owner connectors ride
  for (int c = 0; c < kClasses; c++) {
    const ClassCaller cc = classCaller(c);
    const ConnectorGrant g = connectorGrantFor(cc.who);
    const bool owner = want[c] == 'A';
    TEST_ASSERT_EQUAL_MESSAGE(owner, g == ConnectorGrant::Owner, kClassNames[c]);
    const ConnectorGrantScope scope(g);
    const Attached a = attachAll();
    TEST_ASSERT_EQUAL_MESSAGE(owner ? 2 : 0, a.openaiPrivate, kClassNames[c]);
    TEST_ASSERT_EQUAL_MESSAGE(owner ? 1 : 0, a.mistralPrivate, kClassNames[c]);
    TEST_ASSERT_EQUAL_MESSAGE(owner ? 1 : 0, a.anthropicServers, kClassNames[c]);
    TEST_ASSERT_EQUAL_MESSAGE(1, a.openaiBuiltin, kClassNames[c]);    // the built-ins always ride
    TEST_ASSERT_EQUAL_MESSAGE(1, a.mistralBuiltin, kClassNames[c]);
  }
  // No scope (the device's own checks) is the owner's; scopes nest and restore.
  TEST_ASSERT_EQUAL(int(ConnectorGrant::Owner), int(currentConnectorGrant()));
  {
    const ConnectorGrantScope outer(ConnectorGrant::BuiltinsOnly);
    { const ConnectorGrantScope inner(ConnectorGrant::Owner); }
    TEST_ASSERT_EQUAL(int(ConnectorGrant::BuiltinsOnly), int(currentConnectorGrant()));
  }
  TEST_ASSERT_EQUAL(int(ConnectorGrant::Owner), int(currentConnectorGrant()));
  // An unapproved chat is never the owner.
  TEST_ASSERT_EQUAL(int(ConnectorGrant::BuiltinsOnly),
                    int(connectorGrantFor(principalForRole("4004", Role::Unknown))));
}

// ---- CUM-830: whose sub-agents a caller sees, per caller class ---------------------

namespace {

std::vector<SessionInfo> threeSessions() {
  SessionInfo o, m, g;
  o.id = "job0001"; o.chat = "1001"; o.byOwner = true;  o.state = "running"; o.title = "owner research";
  m.id = "job0002"; m.chat = "2002"; m.byOwner = false; m.state = "running"; m.title = "member research";
  g.id = "job0003"; g.chat = "3003"; g.byOwner = false; g.state = "running"; g.title = "guest research";
  return {o, m, g};
}

// The ids a class sees: the owner every one, a member or a guest its own, the LAN none.
std::string expectedIds(int c) {
  if (c == Member) return "job0002";
  if (c == Guest) return "job0003";
  if (c == McpUser) return "";
  return "job0001,job0002,job0003";
}

std::string ids(const std::vector<SessionInfo>& v) {
  std::string out;
  for (const SessionInfo& s : v) out += (out.empty() ? "" : ",") + s.id;
  return out;
}

}  // namespace

static void test_session_visibility_by_caller_class() {
  for (int c = 0; c < kClasses; c++) {
    const ClassCaller cc = classCaller(c);
    // The turn's digest is built for the turn's own chat (the owner's chat for an
    // unattended turn): the caller as the device builds it there.
    const std::string chat = c == McpUser ? std::string() : (c == Member ? "2002" : (c == Guest ? "3003" : "1001"));
    const ActionCaller caller = actionCaller(cc.who, cc.unattended, chat);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expectedIds(c).c_str(), ids(visibleSessions(threeSessions(), caller)).c_str(),
                                     kClassNames[c]);
  }
}

// The session tools read the same rule: a listing shows only what the caller may see,
// and an id it may not see answers like one that does not exist, never reaching the
// handler (no probing for the owner's sub-agents).
static void test_session_tools_hide_other_chats_sub_agents() {
  int polled = 0;
  SessionHandlers h;
  h.list = [] { return threeSessions(); };
  h.poll = [&polled](const std::string&, std::string&) {
    polled++;
    return std::string("a reply");
  };
  ToolRegistry reg;
  registerSessionTools(reg, h);
  for (int c = 0; c < kClasses; c++) {
    const ClassCaller cc = classCaller(c);
    ArduinoJson::JsonDocument none;
    none.to<ArduinoJson::JsonObject>();
    const ToolResult list = reg.dispatch("session.list", none.as<ArduinoJson::JsonObjectConst>(), cc.who);
    const std::string want = expectedIds(c);
    for (const char* id : {"job0001", "job0002", "job0003"})
      TEST_ASSERT_EQUAL_MESSAGE(want.find(id) != std::string::npos, list.output.find(id) != std::string::npos,
                                (std::string(kClassNames[c]) + " list " + id).c_str());
    polled = 0;
    ArduinoJson::JsonDocument a;
    a["id"] = "job0001";
    const ToolResult poll = reg.dispatch("session.poll", a.as<ArduinoJson::JsonObjectConst>(), cc.who);
    const bool sees = want.find("job0001") != std::string::npos;
    TEST_ASSERT_EQUAL_MESSAGE(sees, poll.success, kClassNames[c]);
    TEST_ASSERT_EQUAL_MESSAGE(sees ? 1 : 0, polled, kClassNames[c]);
    if (!sees) TEST_ASSERT_EQUAL_STRING("no running session 'job0001'", poll.error.c_str());
  }
}

// results.get / results.list: the owner reads every chat's result, a member or a guest
// only its own chat's, the LAN endpoint only its own namespace's.
static void test_results_ring_scoped_by_caller_class() {
  ResultStore rs;
  const std::string o = rs.put("sub", "owner research", "owner findings", 1, "job0001", "chat:1001");
  const std::string m = rs.put("sub", "member research", "member findings", 2, "job0002", "chat:2002");
  const std::string g = rs.put("sub", "guest research", "guest findings", 3, "job0003", "chat:3003");
  for (int c = 0; c < kClasses; c++) {
    const ClassCaller cc = classCaller(c);
    // A member's principal namespace is its chat's ("chat:<id>"); the class callers above
    // already carry it. The result tags a class may read:
    const std::string want = expectedIds(c);
    const std::pair<std::string, const char*> rows[] = {{o, "job0001"}, {m, "job0002"}, {g, "job0003"}};
    for (const auto& r : rows) {
      std::string out;
      size_t total = 0;
      const bool got = rs.get(r.first, 0, 4096, out, total, cc.who);
      TEST_ASSERT_EQUAL_MESSAGE(want.find(r.second) != std::string::npos, got,
                                (std::string(kClassNames[c]) + " " + r.second).c_str());
    }
  }
}

// Every product that runs the job engine tells it who a chat is: with no principalFor a
// sub-agent counts as the owner's (host rigs only), so a product that forgets it would
// hand the owner's connectors to a member's sub-agent.
static void test_every_product_wires_the_spawn_principal() {
  const std::string device = readFile("src/agent/orchestrator.cpp");
  const size_t jd = device.find("static JobEngine::Deps buildJobDeps()");
  TEST_ASSERT_TRUE(jd != std::string::npos);
  const size_t end = device.find("\n}\n", jd);
  TEST_ASSERT_TRUE_MESSAGE(device.substr(jd, end - jd).find("d.principalFor = ") != std::string::npos,
                           "the device's job deps do not set principalFor");
  const std::string hosted = readFile("nimbusd/src/rig.h");
  const size_t w = hosted.find("void wireJobDeps(agent::JobEngine::Deps& jd)");
  TEST_ASSERT_TRUE(w != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(hosted.find("jd.principalFor = ", w) != std::string::npos &&
                               hosted.find("jd.principalFor = ", w) < hosted.find("\n  }\n", w),
                           "the hosted instance's job deps do not set principalFor");
}

// files.send naming no chat: from a Telegram chat it goes back to that chat; from the
// device's own channels (web chat, voice, serial) it would go to the owner's chat, so it
// counts as another chat (the owner writing now); a member or a guest is never sent
// anywhere it did not ask for.
static void test_files_send_without_a_chat_resolves_before_the_check() {
  ArduinoJson::JsonDocument d;
  deserializeJson(d, "{\"project\":\"p\",\"name\":\"n\"}");
  const ArduinoJson::JsonObjectConst a = d.as<ArduinoJson::JsonObjectConst>();
  const Principal owner = principalForRole("1001", Role::Admin);
  for (const char* chat : {"web", "voice", "serial", ""}) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE("files.send.other_chat",
                                     toolActionKey("files.send", a, actionCaller(owner, true, chat)).c_str(), chat);
    TEST_ASSERT_FALSE_MESSAGE(actionAllowed("files.send.other_chat", actionCaller(owner, true, chat)), chat);
    TEST_ASSERT_TRUE_MESSAGE(actionAllowed("files.send.other_chat", actionCaller(owner, false, chat)), chat);
  }
  TEST_ASSERT_EQUAL_STRING("files.send", toolActionKey("files.send", a, actionCaller(owner, true, "1001")).c_str());
  // Where it goes.
  TEST_ASSERT_EQUAL_STRING("1001", filesSendTarget("1001", true, true, "1001").c_str());
  TEST_ASSERT_EQUAL_STRING("2002", filesSendTarget("2002", true, false, "1001").c_str());   // a member's own chat
  TEST_ASSERT_EQUAL_STRING("1001", filesSendTarget("web", false, true, "1001").c_str());     // the owner's chat
  TEST_ASSERT_EQUAL_STRING("", filesSendTarget("5005", false, false, "1001").c_str());      // public-mode guest: refused
  TEST_ASSERT_EQUAL_STRING("", filesSendTarget("web", false, false, "1001").c_str());
}

// The device seams of the rules above, pinned where host tests cannot reach: the role of
// a chat with no row knows whether the table already has an Admin; files.send naming no
// chat resolves its target through filesSendTarget and the OWNER's chat (the owners
// list); the post-update system turn runs on the owner's chat, not merely the first
// allow-listed one.
static void test_device_wiring_of_the_owner_chat_rules() {
  const std::string orch = readFile("src/agent/orchestrator.cpp");
  const size_t role = orch.find("nimbus::orch::Role roleOfChat(const String& chatId) {");
  TEST_ASSERT_TRUE(role != std::string::npos);
  const std::string body = orch.substr(role, orch.find("\n}\n", role) - role);
  TEST_ASSERT_TRUE_MESSAGE(body.find("tableHasAdmin = g_tenants.adminCount() > 0;") != std::string::npos, body.c_str());
  TEST_ASSERT_TRUE_MESSAGE(body.find("roleWithoutRow(isChatAllowed(chatId), agent::telegram::isOwner(chatId), tableHasAdmin)") !=
                               std::string::npos,
                           body.c_str());
  const size_t sys = orch.find("static void drainSystemTurn() {");
  TEST_ASSERT_TRUE(sys != std::string::npos);
  const std::string sysBody = orch.substr(sys, orch.find("\n}\n", sys) - sys);
  TEST_ASSERT_TRUE_MESSAGE(sysBody.find("String chat = agent::telegram::ownerChat();") != std::string::npos, sysBody.c_str());
  TEST_ASSERT_TRUE(sysBody.find("firstAllowedChat()") == std::string::npos);
  const std::string files = readFile("src/agent/files_subsystem.cpp");
  const size_t send = files.find("reg.add(\"files.send\"");
  TEST_ASSERT_TRUE(send != std::string::npos);
  const std::string sendBody = files.substr(send, files.find("reg.add(", send + 10) - send);
  TEST_ASSERT_TRUE_MESSAGE(sendBody.find("nimbus::orch::filesSendTarget(") != std::string::npos, "files.send resolves no target");
  TEST_ASSERT_TRUE(sendBody.find("telegram::ownerChat()") != std::string::npos);
  TEST_ASSERT_TRUE(sendBody.find("firstAllowedChat()") == std::string::npos);
}

// The turn's [ACTIVE SESSIONS] digest is built from the sessions its chat may see.
static void test_the_device_digest_shows_only_visible_sessions() {
  const std::string src = readFile("src/agent/orchestrator.cpp");
  const size_t at = src.find("in.sessions   =");
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, "the digest's sessions line moved: re-pin this test");
  const std::string line = src.substr(at, 200);
  TEST_ASSERT_TRUE_MESSAGE(line.find("visibleSessions(") != std::string::npos, line.c_str());
}

// The device registry installs the rule table where the registry is set up, before any
// tool can be called (in both modes): without it every caller would be allowed.
static void test_the_device_registry_installs_the_rule_table() {
  const std::string src = readFile("src/agent/memory_subsystem.cpp");
  const size_t policy = src.find("g_reg.setPolicyResolver(");
  const size_t install = src.find("nimbus::orch::installActionRules(g_reg,");
  TEST_ASSERT_TRUE_MESSAGE(policy != std::string::npos, "the registry setup moved: re-pin this test");
  TEST_ASSERT_TRUE_MESSAGE(install != std::string::npos, "the device registry does not install the rule table");
  TEST_ASSERT_TRUE(install > policy);
  // Same function, nothing returns between the policy resolver and the install.
  const std::string between = src.substr(policy, install - policy);
  TEST_ASSERT_TRUE_MESSAGE(between.find("return;") == std::string::npos, "an early return skips the install");
  TEST_ASSERT_TRUE_MESSAGE(between.find("\n}\n") == std::string::npos, "the install left the setup function");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_class_table_matches_every_cell);
  RUN_TEST(test_every_row_has_an_expected_row_and_back);
  RUN_TEST(test_every_registered_tool_has_a_rule);
  RUN_TEST(test_every_schema_device_action_and_knob_has_a_rule);
  RUN_TEST(test_every_action_kind_has_a_rule);
  RUN_TEST(test_every_validator_knob_is_strippable);
  RUN_TEST(test_dispatch_seam_enforces_the_table_for_every_tool);
  RUN_TEST(test_dispatch_seam_narrows_by_argument);
  RUN_TEST(test_unknown_and_connected_server_tools_fail_closed);
  RUN_TEST(test_refusals_are_plain_words);
  RUN_TEST(test_member_is_not_advertised_refused_tools);
  RUN_TEST(test_a_chat_without_a_row_follows_the_owner_rule);
  RUN_TEST(test_owner_commands_follow_the_tenant_role);
  RUN_TEST(test_files_send_without_a_chat_resolves_before_the_check);
  RUN_TEST(test_the_device_registry_installs_the_rule_table);
  RUN_TEST(test_the_device_digest_shows_only_visible_sessions);
  RUN_TEST(test_device_wiring_of_the_owner_chat_rules);
  RUN_TEST(test_connector_grant_by_caller_class);
  RUN_TEST(test_session_visibility_by_caller_class);
  RUN_TEST(test_session_tools_hide_other_chats_sub_agents);
  RUN_TEST(test_results_ring_scoped_by_caller_class);
  RUN_TEST(test_every_product_wires_the_spawn_principal);
  return UNITY_END();
}
