#include <unity.h>

#include <cstdio>
#include <regex>
#include <set>
#include <string>

#include "nimbus/orch/command.h"
#include "nimbus/orch/msg_batch.h"

using nimbus::orch::Command;
using nimbus::orch::parseCommand;

void setUp() {}
void tearDown() {}

static Command p(const char* s) { return parseCommand(s); }

// Bare command: verb parsed, no args.
static void test_bare_command() {
  Command c = p("/loops");
  TEST_ASSERT_TRUE(c.isCommand);
  TEST_ASSERT_EQUAL_STRING("loops", c.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("", c.args.c_str());
}

// Leading space is trimmed first: " /update" is still a command.
static void test_leading_space_still_command() {
  Command c = p("  /update");
  TEST_ASSERT_TRUE(c.isCommand);
  TEST_ASSERT_EQUAL_STRING("update", c.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("", c.args.c_str());
}

// THE bug: "/updates" must NOT parse as the "update" verb (loose prefix match).
static void test_updates_is_not_update() {
  Command c = p("/updates");
  TEST_ASSERT_TRUE(c.isCommand);
  TEST_ASSERT_EQUAL_STRING("updates", c.verb.c_str());   // distinct verb
}

// "/update now" carries args (the dispatcher requires empty args for /update).
static void test_update_with_args() {
  Command c = p("/update now please");
  TEST_ASSERT_EQUAL_STRING("update", c.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("now please", c.args.c_str());
}

// Telegram group form "/loops@MyBot" resolves to the plain verb.
static void test_botname_suffix_stripped() {
  Command c = p("/loops@Example_bot");
  TEST_ASSERT_EQUAL_STRING("loops", c.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("", c.args.c_str());
  Command c2 = p("/update@SomeBot force");
  TEST_ASSERT_EQUAL_STRING("update", c2.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("force", c2.args.c_str());
}

// Verb is lowercased so "/Update" == "/update".
static void test_case_insensitive_verb() {
  TEST_ASSERT_EQUAL_STRING("update", p("/Update").verb.c_str());
  TEST_ASSERT_EQUAL_STRING("loop", p("/LOOP approve x").verb.c_str());
}

// /loop keeps its full "action id" remainder in args.
static void test_loop_args_preserved() {
  Command c = p("/loop approve lp3f2a");
  TEST_ASSERT_EQUAL_STRING("loop", c.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("approve lp3f2a", c.args.c_str());
}

// A slash mid-sentence is NOT a command (must start the trimmed message).
static void test_midsentence_slash_not_command() {
  Command c = p("please run /update for me");
  TEST_ASSERT_FALSE(c.isCommand);
}

// Degenerate inputs never crash and never look like a command.
static void test_degenerate_inputs() {
  TEST_ASSERT_FALSE(p("").isCommand);
  TEST_ASSERT_FALSE(p("/").isCommand);      // just a slash
  TEST_ASSERT_FALSE(p("   ").isCommand);
  TEST_ASSERT_FALSE(p("hello").isCommand);
  Command c = p("/h");                      // shortest real command
  TEST_ASSERT_TRUE(c.isCommand);
  TEST_ASSERT_EQUAL_STRING("h", c.verb.c_str());
}

// /clear: bare confirms nothing, "/clear yes" carries the confirm arg (CUM-28).
static void test_clear_verb_and_confirm() {
  Command c = p("/clear");
  TEST_ASSERT_TRUE(c.isCommand);
  TEST_ASSERT_EQUAL_STRING("clear", c.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("", c.args.c_str());
  Command y = p("/clear yes");
  TEST_ASSERT_EQUAL_STRING("clear", y.verb.c_str());
  TEST_ASSERT_EQUAL_STRING("yes", y.args.c_str());
}


// ---- the lifted verb table (nimbus::orch::kCommandVerbs) ---------------------------

static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static std::set<std::string> tableVerbs() {
  std::set<std::string> out;
  for (const auto& v : nimbus::orch::kCommandVerbs) out.insert(v.verb);
  return out;
}

// The table is well formed: lowercase verbs (parseCommand lowercases), no duplicates,
// and only help/start are open to members.
static void test_verb_table_is_well_formed() {
  std::set<std::string> seen;
  for (const auto& v : nimbus::orch::kCommandVerbs) {
    const std::string verb = v.verb;
    TEST_ASSERT_FALSE_MESSAGE(verb.empty(), "empty verb");
    for (char ch : verb) TEST_ASSERT_TRUE_MESSAGE(ch >= 'a' && ch <= 'z', v.verb);
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(verb).second, v.verb);
    TEST_ASSERT_EQUAL_MESSAGE(verb != "help" && verb != "start", v.ownerOnly, v.verb);
    TEST_ASSERT_EQUAL(v.ownerOnly, nimbus::orch::isOwnerVerb(verb));
    TEST_ASSERT_TRUE(nimbus::orch::findCommandVerb(verb) == &v);
  }
  TEST_ASSERT_NULL(nimbus::orch::findCommandVerb("updates"));   // exact, never a prefix
  TEST_ASSERT_FALSE(nimbus::orch::isOwnerVerb("zzz"));
}

// The device dispatcher (agent::orchestrator::handleMessage) handles exactly the table:
// a verb added to the table without a handler, or a handler for a verb missing from the
// table, fails here; and the owner gate reads the table, not an inline list.
static void test_dispatcher_matches_the_table() {
  const std::string src = readFile("src/agent/orchestrator.cpp");
  const size_t fn = src.find("void handleMessage(const String& text, const String& fromName,");
  TEST_ASSERT_TRUE(fn != std::string::npos);
  const size_t end = src.find("if (!g_engine) return;", fn);
  TEST_ASSERT_TRUE(end != std::string::npos);
  const std::string block = src.substr(fn, end - fn);
  std::set<std::string> handled;
  const std::regex verbEq(R"re(\bv == "([a-z]+)")re");
  for (auto it = std::sregex_iterator(block.begin(), block.end(), verbEq);
       it != std::sregex_iterator(); ++it)
    handled.insert((*it)[1]);
  TEST_ASSERT_TRUE_MESSAGE(handled == tableVerbs(), "dispatcher verbs != kCommandVerbs");
  TEST_ASSERT_TRUE(block.find("const bool ownerCmd = nimbus::orch::isOwnerVerb(v);") !=
                   std::string::npos);
}

// Every verb is documented in the Telegram owner commands table, so a new command
// cannot ship undocumented (the docs pack carries this page to the assistant too).
static void test_every_verb_is_documented() {
  const std::string doc = readFile("docs/tools-and-commands.md");
  const size_t sec = doc.find("### Telegram owner commands");
  TEST_ASSERT_TRUE(sec != std::string::npos);
  const std::string body = doc.substr(sec, doc.find("\n## ", sec) - sec);
  for (const auto& v : nimbus::orch::kCommandVerbs) {
    const std::string needle = std::string("`/") + v.verb;
    TEST_ASSERT_TRUE_MESSAGE(body.find(needle) != std::string::npos, v.verb);
  }
}

// A command the batcher sealed as its own turn renders as exactly what was sent, so it
// still parses to the same verb and args when the turn reaches handleMessage.
static void test_sealed_render_parses_as_the_command() {
  for (const auto& v : nimbus::orch::kCommandVerbs) {
    nimbus::orch::MessageBatcher b;
    b.add("1", "u", "first");
    const std::string cmd = std::string("/") + v.verb + " approve x1";
    TEST_ASSERT_TRUE(nimbus::orch::BatchAdd::CommandBoundary == b.add("1", "u", cmd));
    const Command c = parseCommand(b.render(1).c_str());
    TEST_ASSERT_TRUE(c.isCommand);
    TEST_ASSERT_EQUAL_STRING(v.verb, c.verb.c_str());
    TEST_ASSERT_EQUAL_STRING("approve x1", c.args.c_str());
    TEST_ASSERT_TRUE(nimbus::orch::isCommandText(cmd));
  }
  TEST_ASSERT_FALSE(nimbus::orch::isCommandText("Message 1 of 2 from u:\n/skill approve x"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_bare_command);
  RUN_TEST(test_leading_space_still_command);
  RUN_TEST(test_updates_is_not_update);
  RUN_TEST(test_update_with_args);
  RUN_TEST(test_botname_suffix_stripped);
  RUN_TEST(test_case_insensitive_verb);
  RUN_TEST(test_loop_args_preserved);
  RUN_TEST(test_midsentence_slash_not_command);
  RUN_TEST(test_degenerate_inputs);
  RUN_TEST(test_clear_verb_and_confirm);
  RUN_TEST(test_verb_table_is_well_formed);
  RUN_TEST(test_dispatcher_matches_the_table);
  RUN_TEST(test_every_verb_is_documented);
  RUN_TEST(test_sealed_render_parses_as_the_command);
  return UNITY_END();
}
