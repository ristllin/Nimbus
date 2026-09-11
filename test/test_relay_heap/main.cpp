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
  TEST_ASSERT_TRUE(nimbus::cloud::relayCanDial(26000, 5000));   // exactly at the largest floor
}

// The CUM-387 change: the total-free floor dropped from 16000 to 8000 because the
// relay's big staging is now all PSRAM-backed (response body + parser staging, inbound
// WS frame + reassembly + payload, request/upload decode, res frame, mbedTLS content).
// A device whose internal SRAM is healthy but fragmented - the field case, low-water
// ~9 KB total free while PSRAM sat ~7.7 MB empty - must now dial instead of refusing
// with "Not enough memory right now." These frees were REFUSED under the old floor.
static void test_dials_on_fragmented_but_adequate_internal() {
  TEST_ASSERT_TRUE(nimbus::cloud::relayCanDial(/*free=*/9000, /*largest=*/6000));   // old floor refused
  TEST_ASSERT_TRUE(nimbus::cloud::relayCanDial(15000, 12000));                      // old floor refused
  TEST_ASSERT_TRUE(nimbus::cloud::relayCanDial(8000, 5000));                        // exactly at both floors
}

// Counter-test for the loosened threshold (AGENTS.md sec 4): the floor was lowered, so
// prove genuine starvation BELOW the new floor still refuses - the check was retuned,
// not deleted. Too little total free, or no block big enough for the handshake head,
// both still refuse.
static void test_refuses_when_genuinely_starved() {
  TEST_ASSERT_FALSE(nimbus::cloud::relayCanDial(/*free=*/7999, /*largest=*/25000));  // free just below floor
  TEST_ASSERT_FALSE(nimbus::cloud::relayCanDial(6000, 6000));                        // free too low
  TEST_ASSERT_FALSE(nimbus::cloud::relayCanDial(26000, 4096));  // largest below the 4 KB handshake need
  TEST_ASSERT_FALSE(nimbus::cloud::relayCanDial(26000, 0));
}

// The floor must stay above the relay's real internal demand (the 4096-byte WS
// handshake head) so lowering it can never let the relay dial into a block too small
// to complete the upgrade.
static void test_largest_floor_clears_the_handshake_head() {
  TEST_ASSERT_TRUE(nimbus::cloud::kRelayHeapFloorLargest > 4096);
}

// Invariants that keep the two-part gate coherent: you can never require a contiguous
// block larger than the total free reserve, and the total-free backstop must itself
// clear the handshake head. Guards against a future edit that lowers the free floor
// below the largest floor (unsatisfiable) or below the real handshake demand.
static void test_floor_invariants_hold() {
  TEST_ASSERT_TRUE(nimbus::cloud::kRelayHeapFloorFree >= nimbus::cloud::kRelayHeapFloorLargest);
  TEST_ASSERT_TRUE(nimbus::cloud::kRelayHeapFloorFree > 4096);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_dials_with_bounce_buffer_fragmentation);
  RUN_TEST(test_dials_on_fragmented_but_adequate_internal);
  RUN_TEST(test_refuses_when_genuinely_starved);
  RUN_TEST(test_largest_floor_clears_the_handshake_head);
  RUN_TEST(test_floor_invariants_hold);
  return UNITY_END();
}
