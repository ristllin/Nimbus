// test_unattended_policy - what a turn nobody asked for may do (lane RB-Q2a, PRD section 6).
//
// The class (DIAGNOSIS RC-13): the post-update turn, a check-in routine, a wakeup and the
// sub-agent report ran as full model turns with every tool, and changed the owner's files.
// These tests hold the policy table to its rules over EVERY class and EVERY tool the
// firmware registers (tool_groups.h, action_rules.h), the end-of-turn fields, the fixed
// update notice, and the engine's notice path (no provider request). The end-to-end proof
// over a real hosted turn with real files is nimbusd/tests/test_unattended_turns.cpp.
#include <unity.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/engine.h"
#include "nimbus/orch/action_rules.h"
#include "nimbus/orch/connector_grant.h"
#include "nimbus/orch/rbac.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/unattended_policy.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

const TurnClass kUnattended[] = {TurnClass::UpdateNotice, TurnClass::Routine,        TurnClass::Reminder,
                                 TurnClass::Wakeup,       TurnClass::SelfWakeup,     TurnClass::SubagentReport,
                                 TurnClass::Dream};

// A call's arguments from JSON text (the document outlives the view for the test).
struct Args {
  ArduinoJson::JsonDocument doc;
  explicit Args(const char* json) { deserializeJson(doc, json); }
  ArduinoJson::JsonObjectConst view() const { return doc.as<ArduinoJson::JsonObjectConst>(); }
};

ToolUse useOf(const char* name, const char* json = "{}") {
  Args a(json);
  return toolUseFor(name, a.view());
}

std::string refusal(TurnClass c, const char* name, const char* json = "{}",
                    const std::vector<std::string>& listed = {}) {
  Args a(json);
  return unattendedRefusal(c, name, a.view(), listed);
}

// Every tool name either table knows.
std::vector<std::string> everyToolName() {
  std::vector<std::string> out;
  size_t n = 0;
  const ToolClassRow* rows = toolClassTable(n);
  for (size_t i = 0; i < n; ++i) out.push_back(rows[i].name);
  const ActionRuleRow* ar = actionRuleTable(n);
  for (size_t i = 0; i < n; ++i) out.push_back(ar[i].key);
  return out;
}

}  // namespace

// ---- the table -------------------------------------------------------------------------

// One row per class, in enum order; a person's turn grants every class of tool (the
// policy has no say there); the update notice calls no model and grants nothing.
static void test_one_row_per_class() {
  size_t n = 0;
  const UnattendedPolicyRow* rows = unattendedPolicyTable(n);
  TEST_ASSERT_EQUAL(kTurnClassCount, (int)n);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_EQUAL_MESSAGE((int)i, (int)rows[i].cls, turnClassSlug(rows[i].cls));
    TEST_ASSERT_EQUAL_PTR(&rows[i], &unattendedPolicy(rows[i].cls));
  }
  for (int u = 0; u < kToolUseCount; ++u) {
    TEST_ASSERT_EQUAL_MESSAGE((int)Grant::Yes, (int)unattendedPolicy(TurnClass::Attended).grant[u], toolUseSlug((ToolUse)u));
    TEST_ASSERT_EQUAL_MESSAGE((int)Grant::No, (int)unattendedPolicy(TurnClass::UpdateNotice).grant[u], toolUseSlug((ToolUse)u));
  }
  TEST_ASSERT_EQUAL((int)ModelCall::None, (int)unattendedPolicy(TurnClass::UpdateNotice).model);
  TEST_ASSERT_EQUAL((int)Output::Fixed, (int)unattendedPolicy(TurnClass::UpdateNotice).output);
  // An out-of-range class grants nothing (fail closed).
  for (int u = 0; u < kToolUseCount; ++u)
    TEST_ASSERT_EQUAL((int)Grant::No, (int)unattendedPolicy((TurnClass)200).grant[u]);
}

// The rules every unattended row keeps: it reads (the update notice calls no model); it
// never changes a setting, and writes a file only where its routine lists the path.
// Memory only for the upkeep. A routine, a reminder, a wakeup, a held note and the upkeep
// send only through their own reply and spend nothing; the sub-agent report (PRD section
// 6) may send a sub-agent's file to its own chat and start the next wave.
static void test_every_unattended_row_keeps_the_rules() {
  for (TurnClass c : kUnattended) {
    const UnattendedPolicyRow& r = unattendedPolicy(c);
    const char* slug = turnClassSlug(c);
    TEST_ASSERT_EQUAL_MESSAGE(c == TurnClass::UpdateNotice ? (int)Grant::No : (int)Grant::Yes,
                              (int)r.grant[(int)ToolUse::Read], slug);
    const int report = c == TurnClass::SubagentReport ? (int)Grant::Yes : (int)Grant::No;
    TEST_ASSERT_EQUAL_MESSAGE(report, (int)r.grant[(int)ToolUse::Send], slug);
    TEST_ASSERT_EQUAL_MESSAGE(report, (int)r.grant[(int)ToolUse::Spend], slug);
    TEST_ASSERT_EQUAL_MESSAGE((int)Grant::No, (int)r.grant[(int)ToolUse::Control], slug);
    TEST_ASSERT_EQUAL_MESSAGE(c == TurnClass::Routine ? (int)Grant::ListedPaths : (int)Grant::No,
                              (int)r.grant[(int)ToolUse::Write], slug);
    TEST_ASSERT_EQUAL_MESSAGE(c == TurnClass::Dream ? (int)Grant::Yes : (int)Grant::No,
                              (int)r.grant[(int)ToolUse::Memory], slug);
    TEST_ASSERT_TRUE_MESSAGE(r.model == (c == TurnClass::UpdateNotice ? ModelCall::None : ModelCall::Sub), slug);
  }
  // A routine, a reminder and a wakeup (the owner's timed asks) may set the light or speak.
  for (TurnClass c : kUnattended) {
    const bool timed = c == TurnClass::Routine || c == TurnClass::Reminder || c == TurnClass::Wakeup;
    TEST_ASSERT_EQUAL_MESSAGE(timed ? (int)Grant::Yes : (int)Grant::No,
                              (int)unattendedPolicy(c).grant[(int)ToolUse::Ambient], turnClassSlug(c));
  }
  // A routine and a wakeup may set a follow-up wakeup; nothing else may schedule.
  for (TurnClass c : kUnattended)
    TEST_ASSERT_EQUAL_MESSAGE(c == TurnClass::Wakeup || c == TurnClass::Routine ? (int)Grant::Yes : (int)Grant::No,
                              (int)unattendedPolicy(c).grant[(int)ToolUse::Schedule], turnClassSlug(c));
}

static void test_fire_flags_map_to_one_class() {
  TEST_ASSERT_EQUAL((int)TurnClass::Routine, (int)fireClass(false, false, false, false));
  TEST_ASSERT_EQUAL((int)TurnClass::Wakeup, (int)fireClass(false, false, false, true));
  TEST_ASSERT_EQUAL((int)TurnClass::Reminder, (int)fireClass(false, false, true, true));
  TEST_ASSERT_EQUAL((int)TurnClass::SelfWakeup, (int)fireClass(false, true, false, true));
  TEST_ASSERT_EQUAL((int)TurnClass::Dream, (int)fireClass(true, false, false, false));
  for (int bits = 0; bits < 16; ++bits)   // never a person's turn, whatever the flags
    TEST_ASSERT_TRUE(fireClass(bits & 1, bits & 2, bits & 4, bits & 8) != TurnClass::Attended);
}

// ---- the tool classes ------------------------------------------------------------------

// Every tool the firmware registers has a class: a tool registered as an effect never
// reads as Read unless an argument narrows it (the standing rules' view), and a pure or
// read tool reads as Read (image.note records a note: Memory). A name nobody declared,
// a connected server's tool among them, is Control (fail closed).
static void test_every_registered_tool_has_a_class() {
  size_t n = 0;
  const ToolClassRow* rows = toolClassTable(n);
  for (size_t i = 0; i < n; ++i) {
    const std::string name = rows[i].name;
    const ToolUse u = useOf(rows[i].name);
    const bool narrowedRead = name == "prefs.edit" || name == "openq.edit" || name == "memory.config";
    if (rows[i].effect == EffectClass::Effect && !narrowedRead)
      TEST_ASSERT_TRUE_MESSAGE(u != ToolUse::Read, rows[i].name);
    else if (name != "image.note")
      TEST_ASSERT_TRUE_MESSAGE(u == ToolUse::Read, rows[i].name);
  }
  TEST_ASSERT_EQUAL((int)ToolUse::Control, (int)useOf("mcp.gmail.send_mail"));
  TEST_ASSERT_EQUAL((int)ToolUse::Control, (int)useOf("no_such_tool"));
  TEST_ASSERT_EQUAL((int)ToolUse::Memory, (int)useOf("image.note"));
}

// The writing and sending tools by name, one per class, on both engines' names.
static void test_the_named_classes() {
  for (const char* w : {"files.append", "files.edit", "files.delete", "files.fetch", "files.pdf", "artifact.save",
                        "write_file", "append_file", "edit_file", "delete_file", "move_file", "fetch_url"})
    TEST_ASSERT_EQUAL_MESSAGE((int)ToolUse::Write, (int)useOf(w), w);
  for (const char* m : {"memory.write", "memory.update", "memory.pin", "memory.scratchpad", "note"})
    TEST_ASSERT_EQUAL_MESSAGE((int)ToolUse::Memory, (int)useOf(m), m);
  for (const char* s : {"reply.telegram", "files.send", "send"})
    TEST_ASSERT_EQUAL_MESSAGE((int)ToolUse::Send, (int)useOf(s), s);
  TEST_ASSERT_EQUAL((int)ToolUse::Send, (int)useOf("reply.telegram", "{\"chat_id\":\"-100999\",\"text\":\"x\"}"));
  for (const char* s : {"session.spawn", "session.tell", "image.generate", "agent_start"})
    TEST_ASSERT_EQUAL_MESSAGE((int)ToolUse::Spend, (int)useOf(s), s);
  for (const char* s : {"loop.create", "loop.cancel", "wakeup.set", "schedule"})
    TEST_ASSERT_EQUAL_MESSAGE((int)ToolUse::Schedule, (int)useOf(s), s);
  for (const char* s : {"media.play", "media.stop", "reply.speak", "speak"}) TEST_ASSERT_EQUAL_MESSAGE((int)ToolUse::Ambient, (int)useOf(s), s);
  for (const char* r : {"files.read", "files.list", "files.search", "memory.search", "web.search", "read_file",
                        "list_files", "search", "more", "device.status"})
    TEST_ASSERT_EQUAL_MESSAGE((int)ToolUse::Read, (int)useOf(r), r);
  // Arguments narrow: the standing rules' view reads, a change is Control.
  TEST_ASSERT_EQUAL((int)ToolUse::Read, (int)useOf("prefs.edit", "{\"action\":\"list\"}"));
  TEST_ASSERT_EQUAL((int)ToolUse::Control, (int)useOf("prefs.edit", "{\"action\":\"add\",\"text\":\"x\"}"));
  TEST_ASSERT_EQUAL((int)ToolUse::Memory, (int)useOf("openq.edit", "{\"action\":\"remove\"}"));
  TEST_ASSERT_EQUAL((int)ToolUse::Control, (int)useOf("memory.config", "{\"action\":\"update\"}"));
  TEST_ASSERT_EQUAL((int)ToolUse::Control, (int)useOf("models.list", "{\"refresh\":true}"));
  TEST_ASSERT_EQUAL((int)ToolUse::Ambient, (int)useOf("device.selftest", "{\"audible\":true}"));
  // device.control by its action: light and speech Ambient, the rest Control.
  TEST_ASSERT_EQUAL((int)ToolUse::Ambient, (int)useOf("device.control", "{\"action\":{\"type\":\"lights\",\"value\":\"off\"}}"));
  TEST_ASSERT_EQUAL((int)ToolUse::Ambient, (int)useOf("device.control", "{\"action\":{\"type\":\"tts\",\"text\":\"hi\"}}"));
  TEST_ASSERT_EQUAL((int)ToolUse::Control, (int)useOf("device.control", "{\"action\":{\"type\":\"reboot\"}}"));
  TEST_ASSERT_EQUAL((int)ToolUse::Control, (int)useOf("device.control", "{}"));
}

// ---- the decision ------------------------------------------------------------------------

// Every unattended class refuses every writing, sending, spending and settings tool with
// a plain line the model reads; a person's turn is never refused by the policy. Reads run.
static void test_every_class_refuses_writes_and_sends_in_plain_words() {
  for (TurnClass c : kUnattended) {
    for (const std::string& name : everyToolName()) {
      Args none("{}");
      const ToolUse u = toolUseFor(name, none.view());
      const std::string why = unattendedRefusal(c, name, none.view());
      const Grant g = unattendedPolicy(c).grant[(int)u];
      const std::string msg = std::string(turnClassSlug(c)) + " " + name;
      if (g == Grant::Yes) {
        TEST_ASSERT_TRUE_MESSAGE(why.empty(), msg.c_str());
        continue;
      }
      TEST_ASSERT_FALSE_MESSAGE(why.empty(), msg.c_str());
      TEST_ASSERT_TRUE_MESSAGE(why.rfind("Not run: ", 0) == 0, msg.c_str());
      TEST_ASSERT_TRUE_MESSAGE(why.find("Nothing was changed.") != std::string::npos, msg.c_str());
      TEST_ASSERT_TRUE_MESSAGE(why.find('\n') == std::string::npos, msg.c_str());
    }
    TEST_ASSERT_TRUE(refusal(TurnClass::Attended, "files.append").empty());
  }
  // The refusal names the class and what it cannot do.
  TEST_ASSERT_EQUAL_STRING("Not run: a routine runs with nobody writing, and changes to files are closed to it. Nothing was changed.",
                           refusal(TurnClass::Routine, "files.append").c_str());
  TEST_ASSERT_TRUE(refusal(TurnClass::Routine, "reply.telegram").find("its reply is what reaches the owner") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(refusal(TurnClass::SelfWakeup, "files.read").empty());
  TEST_ASSERT_TRUE(refusal(TurnClass::Wakeup, "wakeup.set").empty());
  TEST_ASSERT_TRUE(refusal(TurnClass::Routine, "wakeup.set").empty());
  TEST_ASSERT_FALSE(refusal(TurnClass::SelfWakeup, "wakeup.set").empty());
  TEST_ASSERT_TRUE(refusal(TurnClass::SubagentReport, "session.spawn").empty());
  TEST_ASSERT_FALSE(refusal(TurnClass::SubagentReport, "files.append").empty());
  TEST_ASSERT_TRUE(refusal(TurnClass::Dream, "memory.write").empty());
  TEST_ASSERT_FALSE(refusal(TurnClass::Routine, "memory.write").empty());
}

// A routine writes only to the paths its own definition lists; nothing else does, and a
// path that climbs out of a listed folder is never listed.
static void test_writes_only_to_listed_paths() {
  const std::vector<std::string> listed = {"notes/shopping.md", "logs/"};
  TEST_ASSERT_TRUE(refusal(TurnClass::Routine, "files.append", "{\"path\":\"notes/shopping.md\"}", listed).empty());
  TEST_ASSERT_TRUE(refusal(TurnClass::Routine, "append_file", "{\"path\":\"/logs/2026-11.csv\"}", listed).empty());
  TEST_ASSERT_TRUE(
      refusal(TurnClass::Routine, "files.append", "{\"project\":\"logs\",\"name\":\"a.csv\"}", listed).empty());
  TEST_ASSERT_FALSE(refusal(TurnClass::Routine, "files.append", "{\"path\":\"food.csv\"}", listed).empty());
  TEST_ASSERT_FALSE(refusal(TurnClass::Routine, "files.edit", "{\"path\":\"logs/../food.csv\"}", listed).empty());
  TEST_ASSERT_FALSE(refusal(TurnClass::Routine, "files.append", "{\"path\":\"notes/shopping.md.bak\"}", listed).empty());
  TEST_ASSERT_FALSE(refusal(TurnClass::Routine, "files.append", "{}", listed).empty());
  TEST_ASSERT_FALSE(refusal(TurnClass::Routine, "files.append", "{\"path\":\"notes/shopping.md\"}").empty());
  // A listed path opens files only, never memory or a send.
  TEST_ASSERT_FALSE(refusal(TurnClass::Routine, "memory.write", "{\"path\":\"notes/shopping.md\"}", listed).empty());
  for (TurnClass c : kUnattended)
    if (c != TurnClass::Routine)
      TEST_ASSERT_FALSE_MESSAGE(refusal(c, "files.append", "{\"path\":\"notes/shopping.md\"}", listed).empty(),
                                turnClassSlug(c));
  TEST_ASSERT_FALSE(pathListed("", listed));
  TEST_ASSERT_FALSE(pathListed("logs", listed));   // the folder itself is not under it
  TEST_ASSERT_FALSE(pathListed("logs/x", {""}));
}

// The offered list follows the same decision: a routine is offered reads and the light,
// never a write (it lists no paths on the old engine), a send or a spend.
static void test_the_offered_list_follows_the_table() {
  for (TurnClass c : kUnattended)
    for (const std::string& name : everyToolName()) {
      Args none("{}");
      if (name == "device.control" || name == "device_control") continue;   // offered by its action's class (below)
      const bool listedOk = unattendedMayList(c, name);
      TEST_ASSERT_EQUAL_MESSAGE(unattendedRefusal(c, name, none.view()).empty(), listedOk,
                                (std::string(turnClassSlug(c)) + " " + name).c_str());
    }
  TEST_ASSERT_TRUE(unattendedMayList(TurnClass::Routine, "files.append", true));
  TEST_ASSERT_FALSE(unattendedMayList(TurnClass::Routine, "files.append", false));
  TEST_ASSERT_TRUE(unattendedMayList(TurnClass::Attended, "loop.create"));
  // device.control is offered where the light and speech are open; a reboot through it is
  // still refused at the call.
  for (TurnClass c : kUnattended)
    TEST_ASSERT_EQUAL_MESSAGE(unattendedPolicy(c).grant[(int)ToolUse::Ambient] == Grant::Yes,
                              unattendedMayList(c, "device.control"), turnClassSlug(c));
  TEST_ASSERT_FALSE(refusal(TurnClass::Routine, "device.control", "{\"action\":{\"type\":\"reboot\"}}").empty());
}

// ---- the end-of-turn fields ---------------------------------------------------------------

static Turn fullTurn() {
  Turn t;
  t.reply = "hello";
  t.ask = "anything else?";
  t.memory = "running memory";
  t.scratchpad.present = true;
  t.mem_write.push_back(MemWrite{"a fact", 0.5, false, "", false});
  t.mem_write.push_back(MemWrite{"another", 0.5, false, "", false});
  t.mem_query.push_back("what");
  t.await_.push_back("s1");
  t.spawn.push_back(Spawn{});
  SessionOp sp, te, po;
  sp.op = "spawn";
  te.op = "terminate";
  po.op = "poll";
  t.session_ops = {sp, te, po};
  t.device.push_back(DeviceAction{"{\"type\":\"lights\",\"value\":\"off\"}"});
  t.device.push_back(DeviceAction{"{\"type\":\"tts\",\"text\":\"hi\"}"});
  t.device.push_back(DeviceAction{"{\"type\":\"reboot\"}"});
  t.device.push_back(DeviceAction{"not json"});
  return t;
}

// What a class keeps of fullTurn(), written here from PRD section 6, not read from the table.
struct Kept {
  bool memory, light, report, reads;
  explicit Kept(TurnClass c)
      : memory(c == TurnClass::Dream),
        light(c == TurnClass::Routine || c == TurnClass::Reminder || c == TurnClass::Wakeup),
        report(c == TurnClass::SubagentReport),   // may start the next wave
        reads(c != TurnClass::UpdateNotice) {}
  int cut() const { return (memory ? 0 : 4) + (report ? 0 : 1) + (report ? 1 : 2) + (light ? 2 : 4) + (reads ? 0 : 2); }
};

static void checkKept(TurnClass c, const Turn& t, int cut, const std::string& why) {
  const Kept k(c);
  const char* slug = turnClassSlug(c);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("hello", t.reply.c_str(), slug);   // the reply is its output
  TEST_ASSERT_EQUAL_STRING_MESSAGE("anything else?", t.ask.c_str(), slug);
  TEST_ASSERT_EQUAL_MESSAGE(k.reads ? 1 : 0, (int)t.mem_query.size(), slug);   // a lookup reads
  TEST_ASSERT_EQUAL_MESSAGE(k.reads ? 1 : 0, (int)t.await_.size(), slug);
  TEST_ASSERT_EQUAL_MESSAGE(k.memory, !t.memory.empty(), slug);
  TEST_ASSERT_EQUAL_MESSAGE(k.memory, t.scratchpad.present, slug);
  TEST_ASSERT_EQUAL_MESSAGE(k.memory ? 2 : 0, (int)t.mem_write.size(), slug);
  TEST_ASSERT_EQUAL_MESSAGE(k.report ? 1 : 0, (int)t.spawn.size(), slug);
  TEST_ASSERT_EQUAL_MESSAGE((k.reads ? 1 : 0) + (k.report ? 1 : 0), (int)t.session_ops.size(), slug);
  TEST_ASSERT_EQUAL_MESSAGE(k.light ? 2 : 0, (int)t.device.size(), slug);   // the light and the speech
  TEST_ASSERT_EQUAL_MESSAGE(k.cut(), cut, slug);
  TEST_ASSERT_TRUE_MESSAGE(why.find("device: Not run: ") != std::string::npos, slug);
  TEST_ASSERT_EQUAL_MESSAGE(!k.report, why.find("spawn: Not run: ") != std::string::npos, slug);
}

static void test_end_of_turn_fields_follow_the_row() {
  for (TurnClass c : kUnattended) {
    Turn t = fullTurn();
    std::string why;
    const int cut = restrictUnattendedTurn(t, c, why);
    checkKept(c, t, cut, why);
  }
  Turn t = fullTurn();
  std::string why;
  TEST_ASSERT_EQUAL(0, restrictUnattendedTurn(t, TurnClass::Attended, why));
  TEST_ASSERT_TRUE(why.empty());
  TEST_ASSERT_EQUAL(4, (int)t.device.size());
  TEST_ASSERT_EQUAL(2, (int)t.mem_write.size());
}

// ---- the update notice -------------------------------------------------------------------

static void test_update_notice_is_a_fixed_string() {
  TEST_ASSERT_EQUAL_STRING("Updated to v5.2.0 \xE2\x9C\x93", updateNoticeText("v5.2.0", "").c_str());
  TEST_ASSERT_EQUAL_STRING("Updated to v5.2.0 \xE2\x9C\x93\nRoutines can no longer change files.",
                           updateNoticeText("v5.2.0", "  Routines can no longer change files.  \nsecond line").c_str());
  TEST_ASSERT_EQUAL_STRING("Updated to v5.2.0 \xE2\x9C\x93", updateNoticeText("v5.2.0", " \n tail").c_str());
  // At most kUpdateNoteMax bytes of the note, never inside a character.
  std::string longNote(kUpdateNoteMax - 1, 'a');
  longNote += "\xC3\xA9\xC3\xA9";   // a two-byte character across the limit
  const std::string text = updateNoticeText("v1", longNote);
  const std::string note = text.substr(text.find('\n') + 1);
  TEST_ASSERT_TRUE(note.size() <= kUpdateNoteMax);
  TEST_ASSERT_EQUAL(kUpdateNoteMax - 1, note.size());
  // The same inputs always give the same bytes (nothing a model wrote).
  TEST_ASSERT_EQUAL_STRING(updateNoticeText("v9", "x").c_str(), updateNoticeText("v9", "x").c_str());
}

// The engine stores the notice as the chat's assistant row with no provider request at
// all, and a person's next turn still reaches the provider.
static void test_engine_delivers_the_notice_without_a_model_call() {
  using agent::TurnEngine;
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  int requests = 0;
  std::vector<std::string> delivered, rows;
  TurnEngine::Deps d;
  d.cfg = cfg.contract();
  d.platform = plat.contract();
  d.deliver = [&](const std::string&, const std::string& t) { delivered.push_back(t); };
  d.composeInputs = [](const std::string&) { return agent::ComposeInputs{}; };
  d.firstAllowedChat = [] { return std::string("1001"); };
  d.apply.deliver = d.deliver;
  d.apply.captureAssistant = [&](const std::string&, const std::string& t) { rows.push_back(t); };
  for (const char* h : {"openai", "anthropic", "mistral"})
    d.hosts.add(h, [&](std::string&, const std::string&, const std::string&, std::string& out, std::string&,
                       const agent::HeadTools*, TokenUsage*) {
      ++requests;
      out = "{\"reply\":\"hi\",\"memory\":\"\",\"ask\":\"\"}";
      return true;
    });
  TurnEngine eng(std::move(d));
  const std::string text = updateNoticeText("v5.2.0", "Routines can no longer change files.");
  eng.recordNotice("1001", text);
  TEST_ASSERT_EQUAL(0, requests);
  TEST_ASSERT_EQUAL(0, (int)delivered.size());   // the OTA hook sent it; the engine stores its row
  TEST_ASSERT_EQUAL(1, (int)rows.size());
  TEST_ASSERT_EQUAL_STRING(text.c_str(), rows[0].c_str());
  TEST_ASSERT_TRUE(eng.unattendedClass() == TurnClass::Attended);
  eng.handleMessage("hi", "Roy", "1001");   // counter-test: the rig does reach a provider
  TEST_ASSERT_TRUE(requests > 0);
}

// The owner's connectors act with his sign-ins on the provider's side, out of the device's
// reach: a turn nobody is writing carries the built-ins only, on every class; the owner's
// own turn still carries his connectors (counter-test).
static void test_unattended_requests_carry_no_owner_connectors() {
  using agent::TurnEngine;
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  std::vector<ConnectorGrant> grants;
  TurnEngine::Deps d;
  d.cfg = cfg.contract();
  d.platform = plat.contract();
  d.deliver = [](const std::string&, const std::string&) {};
  d.composeInputs = [](const std::string&) { return agent::ComposeInputs{}; };
  d.firstAllowedChat = [] { return std::string("1001"); };
  d.apply.deliver = d.deliver;
  d.apply.principalFor = [](const std::string& chat) { return principalForRole(chat, Role::Admin); };
  for (const char* h : {"openai", "anthropic", "mistral"})
    d.hosts.add(h, [&](std::string&, const std::string&, const std::string&, std::string& out, std::string&,
                       const agent::HeadTools*, TokenUsage*) {
      grants.push_back(currentConnectorGrant());
      out = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
      return true;
    });
  TurnEngine eng(std::move(d));
  eng.handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_FALSE(grants.empty());
  TEST_ASSERT_TRUE_MESSAGE(grants.back() == ConnectorGrant::Owner, "the owner's turn carries his connectors");
  for (int kind = 0; kind < 4; ++kind) {   // routine, wakeup, reminder, self-wakeup
    grants.clear();
    eng.injectScheduledTurn("1001", "check in", "check-in", "lp1", false, kind > 0, kind == 2, kind == 3);
    TEST_ASSERT_FALSE(grants.empty());
    for (ConnectorGrant g : grants) TEST_ASSERT_TRUE_MESSAGE(g == ConnectorGrant::BuiltinsOnly, "an unattended fire");
  }
  TEST_ASSERT_TRUE(currentConnectorGrant() == ConnectorGrant::Owner);   // the scope ends with the turn
}

// The engine cuts an unattended turn's end-of-turn fields before they apply: a routine's
// answer that carries a memory rewrite and a spawn changes neither; the owner's own turn
// with the same answer applies both (counter-test).
static void test_engine_cuts_unattended_end_of_turn_fields() {
  using agent::TurnEngine;
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  std::vector<std::string> memories;
  int spawns = 0;
  TurnEngine::Deps d;
  d.cfg = cfg.contract();
  d.platform = plat.contract();
  d.deliver = [](const std::string&, const std::string&) {};
  d.composeInputs = [](const std::string&) { return agent::ComposeInputs{}; };
  d.firstAllowedChat = [] { return std::string("1001"); };
  d.apply.deliver = d.deliver;
  d.apply.principalFor = [](const std::string& chat) { return principalForRole(chat, Role::Admin); };
  d.apply.setModelMemory = [&](const std::string&, const std::string& v) {
    memories.push_back(v);
    return false;
  };
  d.apply.enqueueSpawn = [&](const Spawn&, const std::string&, bool) {
    ++spawns;
    return true;
  };
  for (const char* h : {"openai", "anthropic", "mistral"})
    d.hosts.add(h, [&](std::string&, const std::string&, const std::string&, std::string& out, std::string&,
                       const agent::HeadTools*, TokenUsage*) {
      out = "{\"reply\":\"ok\",\"memory\":\"invented\",\"ask\":\"\","
            "\"session_ops\":[{\"op\":\"spawn\",\"task\":\"look it up\"}]}";
      return true;
    });
  TurnEngine eng(std::move(d));
  eng.injectScheduledTurn("1001", "check in", "check-in", "lp1");
  TEST_ASSERT_EQUAL(0, (int)memories.size());
  TEST_ASSERT_EQUAL(0, spawns);
  eng.handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)memories.size());
  TEST_ASSERT_EQUAL(1, spawns);
}

// ---- the device seams (source guards: main.cpp and orchestrator.cpp are not host-built) ---

static std::string readSource(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static std::string between(const std::string& s, const char* from, const char* to) {
  const size_t a = s.find(from);
  TEST_ASSERT_TRUE_MESSAGE(a != std::string::npos, from);
  const size_t b = s.find(to, a);
  TEST_ASSERT_TRUE_MESSAGE(b != std::string::npos, to);
  return s.substr(a, b - a);
}

// The post-update handler sends the fixed string and stages its row, and nothing that
// could reach a model; the drain hands the row to the engine. Nothing stages a turn.
static void test_the_device_notice_path_calls_no_model() {
  const std::string main = readSource("src/main.cpp");
  const std::string onValidated = between(main, "case otaupd::EvValidated:", "default: break;");
  TEST_ASSERT_TRUE(onValidated.find("stageUpdateNotice(owner, notice)") != std::string::npos);
  TEST_ASSERT_TRUE(onValidated.find("agent::telegram::send(owner, notice, /*block=*/false)") != std::string::npos);
  TEST_ASSERT_TRUE(onValidated.find("updateNoticeText(") != std::string::npos);
  for (const char* turnApi : {"injectScheduledTurn", "injectBoundedTurn", "stageSystemTurn", "handleMessage",
                              "injectMessage"})
    TEST_ASSERT_TRUE_MESSAGE(onValidated.find(turnApi) == std::string::npos, turnApi);
  const std::string orch = readSource("src/agent/orchestrator.cpp");
  const std::string drain = between(orch, "static void drainUpdateNotice()", "\n}\n");
  TEST_ASSERT_TRUE(drain.find("g_engine->recordNotice(") != std::string::npos);
  for (const char* turnApi : {"injectScheduledTurn", "injectBoundedTurn", "handleMessage", "runTurn"})
    TEST_ASSERT_TRUE_MESSAGE(drain.find(turnApi) == std::string::npos, turnApi);
  TEST_ASSERT_TRUE(orch.find("stageSystemTurn") == std::string::npos);
  // Both registries consult the policy at the access seam, the LAN endpoint excepted.
  const std::string mem = readSource("src/agent/memory_subsystem.cpp");
  TEST_ASSERT_TRUE(mem.find("unattendedRefusal(orchestrator::unattendedClass(), name, args)") != std::string::npos);
  const std::string rig = readSource("nimbusd/src/rig.h");
  TEST_ASSERT_TRUE(rig.find("unattendedRefusal(eng_->unattendedClass(), name, args)") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_one_row_per_class);
  RUN_TEST(test_every_unattended_row_keeps_the_rules);
  RUN_TEST(test_fire_flags_map_to_one_class);
  RUN_TEST(test_every_registered_tool_has_a_class);
  RUN_TEST(test_the_named_classes);
  RUN_TEST(test_every_class_refuses_writes_and_sends_in_plain_words);
  RUN_TEST(test_writes_only_to_listed_paths);
  RUN_TEST(test_the_offered_list_follows_the_table);
  RUN_TEST(test_end_of_turn_fields_follow_the_row);
  RUN_TEST(test_update_notice_is_a_fixed_string);
  RUN_TEST(test_engine_delivers_the_notice_without_a_model_call);
  RUN_TEST(test_unattended_requests_carry_no_owner_connectors);
  RUN_TEST(test_engine_cuts_unattended_end_of_turn_fields);
  RUN_TEST(test_the_device_notice_path_calls_no_model);
  return UNITY_END();
}
