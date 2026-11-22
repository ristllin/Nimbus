// test_plain_chat_commands - where each owner command goes on an engine-2 device (lane E2-CHAT,
// nimbus/harness/plain/chat_commands.h). None reaches the model: every verb of the device's one
// command table (nimbus::orch::kCommandVerbs) has a route, and a verb added to the table with no
// route here fails the walk.
#include <unity.h>

#include <string>

#include "nimbus/harness/plain/chat_commands.h"

namespace plain = agent::plain;
using R = plain::ChatCommandRoute;

namespace {

R routeOf(const std::string& text) { return plain::chatCommandRoute(nimbus::orch::parseCommand(text)); }

struct Expect {
  const char* verb;
  R route;
};
// Every verb of the table and its route (a bare command).
constexpr Expect kExpected[] = {
    {"help", R::Shared},   {"start", R::Shared},   {"update", R::Shared},    {"loops", R::Shared},
    {"loop", R::Shared},   {"compact", R::Compact}, {"skill", R::NotYet},    {"fetch", R::Shared},
    {"remind", R::Shared}, {"clear", R::Clear},     {"mcp", R::NotYet},      {"play", R::Shared},
    {"prefs", R::NotYet},  {"usage", R::Shared},    {"budget", R::BudgetView}, {"model", R::Shared},
    {"ota", R::Shared},
};

}  // namespace

void setUp() {}
void tearDown() {}

static void test_every_verb_of_the_table_has_its_route() {
  TEST_ASSERT_EQUAL(nimbus::orch::kCommandVerbCount, sizeof(kExpected) / sizeof(kExpected[0]));
  for (const nimbus::orch::CommandVerb& v : nimbus::orch::kCommandVerbs) {
    const Expect* e = nullptr;
    for (const Expect& x : kExpected)
      if (std::string(x.verb) == v.verb) e = &x;
    TEST_ASSERT_NOT_NULL_MESSAGE(e, v.verb);   // a new verb needs its engine-2 route
    const R r = routeOf(std::string("/") + v.verb);
    TEST_ASSERT_TRUE_MESSAGE(r != R::Unknown, v.verb);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(plain::chatCommandRouteSlug(e->route), plain::chatCommandRouteSlug(r), v.verb);
  }
}

static void test_arguments_and_spellings() {
  TEST_ASSERT_TRUE(routeOf("/budget anthropic 30") == R::Shared);   // a limit change: lane BUD's handler
  TEST_ASSERT_TRUE(routeOf("/budget confirm") == R::Shared);
  TEST_ASSERT_TRUE(routeOf("  /budget  ") == R::BudgetView);
  TEST_ASSERT_TRUE(routeOf("/clear yes") == R::Clear);
  TEST_ASSERT_TRUE(routeOf("/Compact@NimbusBot") == R::Compact);
  TEST_ASSERT_TRUE(routeOf("/stop") == R::Unknown);     // not a command on either engine: "try /help"
  TEST_ASSERT_TRUE(routeOf("/updates") == R::Unknown);  // exact verbs only
  TEST_ASSERT_FALSE(nimbus::orch::parseCommand("clear the table").isCommand);   // plain text is a turn
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_verb_of_the_table_has_its_route);
  RUN_TEST(test_arguments_and_spellings);
  return UNITY_END();
}
