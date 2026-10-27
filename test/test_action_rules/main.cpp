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
#include "nimbus/orch/device_actions.h"
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
// public-mode stranger User or Guest), the turn's unattended flag (a routine or wakeup,
// the sub-agent results turn and the device-staged system turn all run on the owner's
// chat with the flag set), and the LAN endpoint's own principal (web_memory.cpp /mcp).
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
    {"files.stat", kOpen},
    {"image.generate", kTurn},
    {"image.look", kTurn},
    // docs, routines, replies, skills, people
    {"docs.list", kOpen},
    {"docs.read", kOpen},
    {"docs.search", kOpen},
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
                                      "prefs.edit.change",    "reply.telegram.other_chat", "files.send.other_chat"};
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
  installActionRules(reg, [&unattended] { return unattended; });
  int calls = 0;
  for (const std::string& n : names) {
    const Expect* e = expectFor(n);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, n.c_str());
    for (int c = 0; c < kClasses; c++) {
      const ClassCaller cc = classCaller(c);
      unattended = cc.unattended;
      ran = 0;
      ArduinoJson::JsonDocument args;
      args.to<ArduinoJson::JsonObject>();
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
  for (const char* n : {"memory.config", "models.list", "device.selftest", "prefs.edit", "reply.telegram", "files.send"})
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
  };
  for (const Case& k : cases)
    for (int c = 0; c < kClasses; c++) {
      const ClassCaller cc = classCaller(c);
      unattended = cc.unattended;
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
    TEST_ASSERT_EQUAL_MESSAGE(open || turn, actionAllowed(rows[i].key, unknown), rows[i].key);
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
  TEST_ASSERT_EQUAL(int(Role::Admin), int(roleWithoutRow(true, true)));
  TEST_ASSERT_EQUAL(int(Role::User), int(roleWithoutRow(true, false)));
  TEST_ASSERT_EQUAL(int(Role::Unknown), int(roleWithoutRow(false, false)));
  TEST_ASSERT_EQUAL(int(Role::Unknown), int(roleWithoutRow(false, true)));   // never an owner off the allowlist
  const ActionCaller fresh = actionCaller(principalForRole("5005", roleWithoutRow(true, true)), false, "5005");
  TEST_ASSERT_TRUE(actionAllowed("device.orch_model", fresh));
  const ActionCaller other = actionCaller(principalForRole("6006", roleWithoutRow(true, false)), false, "6006");
  TEST_ASSERT_FALSE(actionAllowed("device.orch_model", other));
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
  return UNITY_END();
}
