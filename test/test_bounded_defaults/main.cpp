#include <unity.h>

#include <fstream>
#include <sstream>
#include <string>

#include "nimbus/net/tg_dup_guard.h"
#include "nimbus/orch/caps.h"

// Bounded work by default (lane DEF, owner requirement 21 Oct 2026): the fan-out caps
// and lane D's inbound storm breakers are on out of the box, with no setting that turns
// them off. Pins the numbers the owner's docs state (docs/spend-ceiling.md, "Bounded
// work") and proves the breakers act on a fresh record with nothing configured.

void setUp() {}
void tearDown() {}

namespace {
std::string slurp(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
}  // namespace

static void test_fan_out_caps_are_the_bounded_defaults() {
  TEST_ASSERT_EQUAL(8, nimbus::orch::kMaxPendingSpawns);   // was 12 before the defaults
  TEST_ASSERT_EQUAL(6, nimbus::orch::kAgentMaxJobs);
  TEST_ASSERT_EQUAL(4, nimbus::orch::kMaxActiveInflight);
  TEST_ASSERT_TRUE(nimbus::orch::kMaxPendingSpawns <= 12);
}

static void test_storm_breakers_are_on_by_default() {
  using namespace nimbus::net;
  TEST_ASSERT_EQUAL_UINT32(60, kInboundTurnsPerHour);
  TEST_ASSERT_EQUAL_UINT32(12, kOwnerReserveTurns);
  TEST_ASSERT_EQUAL_UINT32(120, kRepeatWindowS);
  // A fresh record (the first power-on), nothing configured: the ceiling holds.
  TgGuardStore rtc{};
  TgGuard g(rtc);
  g.boot(BootKind::PowerOn, 0x1234u);
  const uint32_t t0 = 1792400000u;
  for (uint32_t i = 0; i < kInboundTurnsPerHour; ++i) {
    TEST_ASSERT_TRUE(g.ceilingAllows(t0 + i, true));
    g.noteTurn(t0 + i);
  }
  TEST_ASSERT_FALSE(g.ceilingAllows(t0 + 100, true));    // the owner's 61st turn this hour
  TEST_ASSERT_FALSE(g.ceilingAllows(t0 + 100, false));   // and every other chat's
  // ... and the duplicate breaker: a message answered once is a replay when served again.
  DupKey k;
  k.chat = dupHash(std::string("77"));
  k.messageId = 501;
  k.date = t0;
  k.content = contentKeyText("hello");
  TEST_ASSERT_TRUE(g.check(k) == DupVerdict::New);
  g.noteTurned(k, t0 + 5);
  TEST_ASSERT_TRUE(g.check(k) == DupVerdict::Replay);
}

// The device's receive path always runs through the breakers: the one receiver is
// built with the guard, and it consults both breakers with no setting in between.
static void test_the_device_receive_path_always_consults_them() {
  const std::string tg = slurp("src/agent/telegram.cpp");
  TEST_ASSERT_TRUE(tg.find("TgReceiver g_receiver(g_cursor, g_batch, g_guard)") != std::string::npos);
  const std::string rx = slurp("lib/core/include/nimbus/net/tg_receive.h");
  TEST_ASSERT_TRUE(rx.find("guard_.check(key)") != std::string::npos);
  TEST_ASSERT_TRUE(rx.find("guard_.ceilingAllows(now, owner)") != std::string::npos);
  TEST_ASSERT_TRUE(rx.find("store::") == std::string::npos);   // no setting can switch them off
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fan_out_caps_are_the_bounded_defaults);
  RUN_TEST(test_storm_breakers_are_on_by_default);
  RUN_TEST(test_the_device_receive_path_always_consults_them);
  return UNITY_END();
}
