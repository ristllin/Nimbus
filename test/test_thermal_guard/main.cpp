#include <unity.h>

#include "nimbus/power/thermal_guard.h"

using nimbus::power::ThermalGuard;
using D = nimbus::power::ThermalGuard::Decision;

void setUp() {}
void tearDown() {}

// Defaults: absC 65, riseC 18, hystC 8, maxTrips 3.

static void test_no_decision_unarmed() {
  ThermalGuard g;
  TEST_ASSERT_EQUAL(int(D::None), int(g.onTemp(90.0f)));   // unarmed: never acts
}

static void test_normal_temps_never_trip() {
  ThermalGuard g;
  g.arm(42.0f);   // typical WiFi-up die temp
  for (float t = 42.0f; t < 58.0f; t += 1.0f)               // below abs AND below rise
    TEST_ASSERT_EQUAL(int(D::None), int(g.onTemp(t)));
  TEST_ASSERT_FALSE(g.tripped());
}

static void test_absolute_ceiling_trips() {
  ThermalGuard g;
  g.arm(60.0f);                                             // hot start
  TEST_ASSERT_EQUAL(int(D::Trip), int(g.onTemp(65.0f)));    // abs cap
  TEST_ASSERT_TRUE(g.tripped());
  TEST_ASSERT_EQUAL(1, g.trips());
}

static void test_rise_over_baseline_trips_below_absolute() {
  ThermalGuard g;
  g.arm(35.0f);                                             // cool start
  // 55C is under the 65 abs cap but 20C over baseline -> the enclosure is cooking.
  TEST_ASSERT_EQUAL(int(D::Trip), int(g.onTemp(55.0f)));
}

static void test_resume_needs_hysteresis() {
  ThermalGuard g;
  g.arm(40.0f);
  TEST_ASSERT_EQUAL(int(D::Trip), int(g.onTemp(65.0f)));    // trip at 65
  TEST_ASSERT_EQUAL(int(D::None), int(g.onTemp(63.0f)));    // small dip: stay off
  TEST_ASSERT_EQUAL(int(D::None), int(g.onTemp(58.0f)));    // not cooled enough (65-8=57)
  TEST_ASSERT_EQUAL(int(D::Resume), int(g.onTemp(56.5f)));  // cooled past hysteresis
  TEST_ASSERT_FALSE(g.tripped());
}

static void test_abort_after_max_trips() {
  ThermalGuard g;
  g.arm(40.0f);
  for (int i = 0; i < 3; ++i) {                             // three trip/resume cycles
    TEST_ASSERT_EQUAL(int(D::Trip), int(g.onTemp(66.0f)));
    TEST_ASSERT_EQUAL(int(D::Resume), int(g.onTemp(50.0f)));
  }
  TEST_ASSERT_EQUAL(int(D::Abort), int(g.onTemp(66.0f)));   // 4th trip -> give up
  TEST_ASSERT_TRUE(g.aborted());
  TEST_ASSERT_EQUAL(int(D::None), int(g.onTemp(90.0f)));    // aborted: silent forever
}

static void test_disarm_stops_decisions() {
  ThermalGuard g;
  g.arm(40.0f);
  g.disarm();
  TEST_ASSERT_EQUAL(int(D::None), int(g.onTemp(80.0f)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_no_decision_unarmed);
  RUN_TEST(test_normal_temps_never_trip);
  RUN_TEST(test_absolute_ceiling_trips);
  RUN_TEST(test_rise_over_baseline_trips_below_absolute);
  RUN_TEST(test_resume_needs_hysteresis);
  RUN_TEST(test_abort_after_max_trips);
  RUN_TEST(test_disarm_stops_decisions);
  return UNITY_END();
}
