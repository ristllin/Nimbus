// Lane E2-P-REL: the way back (nimbus/ota/way_back.h). An engine-2 device (v5.9.x) installs a
// release of the earlier engine (v5.2.x) only when the owner asks twice (/update back, then
// /update back yes) after a fresh check of the stable manifest; never a same or newer release,
// never on a stale or failed check.
#include <unity.h>

#include <cstring>
#include <string>

#include "nimbus/ota/way_back.h"

using namespace nimbus::ota;

static BackAsk settled(const char* latest, bool confirm) {
  BackAsk a;
  a.confirm = confirm;
  a.checkedOnStable = true;
  a.state = "up-to-date";
  a.running = "v5.9.0";
  a.latest = latest;
  return a;
}

void test_the_owner_walks_back_to_an_engine1_release() {
  BackAsk a = settled("v5.1.2", false);   // the stale verdict of an earlier check
  a.checkedOnStable = false;
  a.state = "up-to-date";
  TEST_ASSERT_EQUAL_INT(int(BackStep::StartCheck), int(backStep(a)));   // first ask: switch and check
  a.checkedOnStable = true;
  a.state = "checking";
  TEST_ASSERT_EQUAL_INT(int(BackStep::Waiting), int(backStep(a)));
  a = settled("v5.2.1", false);
  TEST_ASSERT_EQUAL_INT(int(BackStep::Offer), int(backStep(a)));        // older: offered, not installed
  a.confirm = true;
  TEST_ASSERT_EQUAL_INT(int(BackStep::Install), int(backStep(a)));      // confirmed: forced install
}

void test_never_a_same_newer_or_unreadable_release() {
  for (const char* latest : {"v5.9.0", "v5.9.0-rc2", "v5.9.1", "v6.0.0", "", "garbage"}) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(int(BackStep::NothingOlder), int(backStep(settled(latest, true))), latest);
    TEST_ASSERT_EQUAL_INT_MESSAGE(int(BackStep::NothingOlder), int(backStep(settled(latest, false))), latest);
  }
}

void test_no_install_without_a_fresh_settled_check() {
  for (const char* st : {"idle", "checking", "error", "downloading"}) {
    BackAsk a = settled("v5.2.1", true);
    a.state = st;
    TEST_ASSERT_TRUE_MESSAGE(backStep(a) != BackStep::Install, st);
  }
  BackAsk stale = settled("v5.2.1", true);
  stale.checkedOnStable = false;   // the verdict may be the candidate manifest's
  TEST_ASSERT_EQUAL_INT(int(BackStep::StartCheck), int(backStep(stale)));
  BackAsk busy = settled("v5.2.1", true);
  busy.installing = true;
  TEST_ASSERT_EQUAL_INT(int(BackStep::Busy), int(backStep(busy)));
  BackAsk none = settled("v5.2.1", true);
  none.state = "unsupported";
  TEST_ASSERT_EQUAL_INT(int(BackStep::Unsupported), int(backStep(none)));
}

void test_every_line_is_plain_and_names_the_versions() {
  for (int s = int(BackStep::Busy); s <= int(BackStep::NothingOlder); ++s) {
    const std::string line = backLine(BackStep(s), "v5.2.1", "v5.9.0");
    TEST_ASSERT_FALSE(line.empty());
    TEST_ASSERT_TRUE(line.find('"') == std::string::npos);
    TEST_ASSERT_TRUE(line.find("\xE2\x80\x94") == std::string::npos);   // no em dash
  }
  TEST_ASSERT_TRUE(backLine(BackStep::Offer, "v5.2.1", "v5.9.0").find("v5.2.1") != std::string::npos);
  TEST_ASSERT_TRUE(backLine(BackStep::Offer, "v5.2.1", "v5.9.0").find("/update back yes") != std::string::npos);
  TEST_ASSERT_TRUE(backLine(BackStep::Install, "v5.2.1", "v5.9.0").find("stay") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_owner_walks_back_to_an_engine1_release);
  RUN_TEST(test_never_a_same_newer_or_unreadable_release);
  RUN_TEST(test_no_install_without_a_fresh_settled_check);
  RUN_TEST(test_every_line_is_plain_and_names_the_versions);
  return UNITY_END();
}
