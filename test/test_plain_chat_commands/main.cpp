// test_plain_chat_commands - where each owner command goes on an engine-2 device (lane E2-CHAT,
// nimbus/harness/plain/chat_commands.h). None reaches the model: every verb of the device's one
// command table (nimbus::orch::kCommandVerbs) has a route, and a verb added to the table with no
// route here fails the walk.
#include <unity.h>

#include <string>

#include "../support/tx_battery/mem_files.h"
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


// ---- /clear's generation file (lane E2-FIX2B, finding P7) ---------------------------------------
namespace {

// The card store with a read that answers nothing (a bad read, a refused allocation) and a listing
// that may fail as well.
struct FlakyFiles : txb::MemFiles {
  bool refuseReads = false, refuseList = false;
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override {
    got = 0;
    if (refuseReads) return false;
    return txb::MemFiles::readAt(path, offset, buf, want, got);
  }
  bool list(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    if (refuseList) return false;
    return txb::MemFiles::list(dir, names);
  }
};

}  // namespace

// Before: the number was rewritten in place and any failed read was generation 0, so a cut or one
// bad read reopened the oldest conversation and the next /clear reused a number. Now: written whole
// and renamed; a read that cannot tell is Unknown, never 0.
static void test_the_generation_survives_a_cut_and_a_bad_read_is_never_zero() {
  FlakyFiles f;
  uint32_t g = 7;
  TEST_ASSERT_EQUAL((int)plain::GenRead::Absent, (int)plain::readGeneration(f, "web", g));   // never cleared
  TEST_ASSERT_EQUAL(0u, g);
  TEST_ASSERT_TRUE(plain::writeGeneration(f, "web", 3));
  TEST_ASSERT_EQUAL((int)plain::GenRead::Value, (int)plain::readGeneration(f, "web", g));
  TEST_ASSERT_EQUAL(3u, g);
  // A cut inside the next write: the temp is torn, the number stays 3.
  f.files["/mem/tx/web.gen.tmp"] = "";
  TEST_ASSERT_EQUAL((int)plain::GenRead::Value, (int)plain::readGeneration(f, "web", g));
  TEST_ASSERT_EQUAL(3u, g);
  // A cut between the temp's write and its rename (the target already removed): the temp is whole.
  f.files.erase("/mem/tx/web.gen");
  f.files["/mem/tx/web.gen.tmp"] = "4\n";
  TEST_ASSERT_EQUAL((int)plain::GenRead::Value, (int)plain::readGeneration(f, "web", g));
  TEST_ASSERT_EQUAL(4u, g);
  // A read that answers nothing is Unknown (the file is listed), never 0.
  f.refuseReads = true;
  TEST_ASSERT_EQUAL((int)plain::GenRead::Unknown, (int)plain::readGeneration(f, "web", g));
  f.refuseList = true;
  TEST_ASSERT_EQUAL((int)plain::GenRead::Unknown, (int)plain::readGeneration(f, "voice", g));   // no proof of absence
  f.refuseReads = f.refuseList = false;
  // An empty file (an older build's cut in place) is Unknown, not 0.
  f.files["/mem/tx/tg.gen"] = "";
  TEST_ASSERT_EQUAL((int)plain::GenRead::Unknown, (int)plain::readGeneration(f, "tg", g));
}

// The neighbor: an older build's file ("1", no line end) still reads, and a write refused at the
// rename leaves a whole temp that the next read takes.
static void test_an_older_generation_file_reads_and_a_refused_rename_keeps_the_new_number() {
  FlakyFiles f;
  f.files["/mem/tx/web.gen"] = "1";
  uint32_t g = 0;
  TEST_ASSERT_EQUAL((int)plain::GenRead::Value, (int)plain::readGeneration(f, "web", g));
  TEST_ASSERT_EQUAL(1u, g);
  f.refuseRename = true;
  TEST_ASSERT_FALSE(plain::writeGeneration(f, "web", 2));
  f.refuseRename = false;
  TEST_ASSERT_EQUAL((int)plain::GenRead::Value, (int)plain::readGeneration(f, "web", g));
  TEST_ASSERT_EQUAL(2u, g);
  f.files["/mem/tx/x.gen"] = "12abc";
  TEST_ASSERT_EQUAL((int)plain::GenRead::Unknown, (int)plain::readGeneration(f, "x", g));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_verb_of_the_table_has_its_route);
  RUN_TEST(test_arguments_and_spellings);
  RUN_TEST(test_the_generation_survives_a_cut_and_a_bad_read_is_never_zero);
  RUN_TEST(test_an_older_generation_file_reads_and_a_refused_rename_keeps_the_new_number);
  return UNITY_END();
}
