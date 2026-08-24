#include <unity.h>

#include "nimbus/cloud/relay_heap.h"

void setUp() {}
void tearDown() {}

// The field regression (CUM-167): solide-drivers v0.6.1's 5 KB internal DMA bounce
// buffer fragmented internal SRAM so the largest free block dipped to ~5-6 KB while
// ~26 KB stayed free. Under the OLD 8000 largest floor the relay refused to dial; it
// must dial now, because its real internal demand is only the 4 KB handshake head.
static void test_dials_with_bounce_buffer_fragmentation() {
  TEST_ASSERT_TRUE(nimbus::cloud::relayCanDial(/*free=*/26000, /*largest=*/6000));
  TEST_ASSERT_TRUE(nimbus::cloud::relayCanDial(26000, 5000));   // exactly at the floor
}

// Genuine starvation still refuses: too little total free, or no block big enough for
// the handshake head.
static void test_refuses_when_genuinely_starved() {
  TEST_ASSERT_FALSE(nimbus::cloud::relayCanDial(/*free=*/10000, /*largest=*/9000));  // free too low
  TEST_ASSERT_FALSE(nimbus::cloud::relayCanDial(26000, 4096));  // largest below the 4 KB handshake need
  TEST_ASSERT_FALSE(nimbus::cloud::relayCanDial(26000, 0));
}

// The floor must stay above the relay's real internal demand (the 4096-byte WS
// handshake head) so lowering it can never let the relay dial into a block too small
// to complete the upgrade.
static void test_largest_floor_clears_the_handshake_head() {
  TEST_ASSERT_TRUE(nimbus::cloud::kRelayHeapFloorLargest > 4096);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_dials_with_bounce_buffer_fragmentation);
  RUN_TEST(test_refuses_when_genuinely_starved);
  RUN_TEST(test_largest_floor_clears_the_handshake_head);
  return UNITY_END();
}
