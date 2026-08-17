#include <unity.h>

#include "nimbus/orch/command.h"

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
  return UNITY_END();
}
