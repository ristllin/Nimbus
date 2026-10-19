#include <unity.h>

#include <algorithm>

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


// ---- admission for big responses (panic F2) ------------------------------------
using nimbus::cloud::ServeDecision;
using nimbus::cloud::kServeFloorFree;
using nimbus::cloud::kServeSmallMax;

// The grid over body size x turn in flight x internal free. The class rules:
//   - a turn in flight and a body over 16 KB never serve at full speed (relay: busy);
//   - a body up to 16 KB always serves, whatever the memory or the turn (the
//     dashboard's 5 s polls are 20 B to 6.6 KB: no regression for them);
//   - under the floor a big body never serves at full speed either;
//   - the LAN variant only differs where a turn alone would refuse: it trickles.
static void test_serve_decision_grid() {
  const size_t bodies[] = {0, 20, 6605, kServeSmallMax, kServeSmallMax + 1, 65536, 278000, 375598};
  const size_t frees[] = {0, 1180, 9000, kServeFloorFree - 1, kServeFloorFree, 31000, 200000};
  for (size_t b : bodies) {
    for (size_t f : frees) {
      for (int turn = 0; turn < 2; ++turn) {
        const ServeDecision r = nimbus::cloud::tunnelServeDecision(b, turn != 0, f);
        const ServeDecision l = nimbus::cloud::lanServeDecision(b, turn != 0, f);
        if (b <= kServeSmallMax) {
          TEST_ASSERT_EQUAL((int)ServeDecision::Serve, (int)r);
          TEST_ASSERT_EQUAL((int)ServeDecision::Serve, (int)l);
          continue;
        }
        TEST_ASSERT_NOT_EQUAL((int)ServeDecision::Serve, (int)r);
        TEST_ASSERT_NOT_EQUAL((int)ServeDecision::Serve, (int)l);
        if (turn) TEST_ASSERT_EQUAL((int)ServeDecision::Busy, (int)r);
        if (f < kServeFloorFree) {
          TEST_ASSERT_EQUAL((int)ServeDecision::Busy, (int)r);
          TEST_ASSERT_EQUAL((int)ServeDecision::Busy, (int)l);
        } else {
          TEST_ASSERT_EQUAL((int)ServeDecision::Trickle, (int)l);   // a turn alone: trickle
          if (!turn) TEST_ASSERT_EQUAL((int)ServeDecision::Trickle, (int)r);
        }
      }
    }
  }
  TEST_ASSERT_EQUAL_STRING("busy", nimbus::cloud::serveDecisionName(ServeDecision::Busy));
}

// The trickle never stalls: with nothing in flight (a full offer) it always grants a
// segment, for every offer size including a known-length tail shorter than a
// segment; with more than two segments in flight (a short offer) it waits.
static void test_trickle_grant_caps_and_never_stalls() {
  const size_t full = 2 * nimbus::cloud::kTrickleChunk - 8;   // chunked: 2 segments less framing
  TEST_ASSERT_EQUAL((int)nimbus::cloud::kTrickleChunk, (int)nimbus::cloud::trickleGrant(full, full));
  TEST_ASSERT_EQUAL(0, (int)nimbus::cloud::trickleGrant(full - 1, full));
  for (size_t tail = 1; tail <= full; ++tail) {   // a known-length tail: fullOffer = bytes left
    const size_t g = nimbus::cloud::trickleGrant(tail, tail);
    TEST_ASSERT_TRUE(g > 0);
    TEST_ASSERT_TRUE(g <= nimbus::cloud::kTrickleChunk && g <= tail);
  }
  // Model of a chunked response over the server's send buffer (5,744 B; it offers
  // min(2-segment buffer, free space) less the chunk framing): the bytes in flight
  // never exceed three segments, and every acknowledgement lets the next one go.
  const size_t sndBuf = 5744;
  size_t inFlight = 0, sent = 0;
  const size_t body = 375598;
  int stalls = 0;
  while (sent < body) {
    const size_t space = sndBuf - inFlight;
    const size_t offer = std::min(full + 8, space) - 8;
    const size_t g = std::min(nimbus::cloud::trickleGrant(offer, full), body - sent);
    if (g == 0) {
      TEST_ASSERT_TRUE(inFlight > 0);   // only ever waits on in-flight data
      inFlight = inFlight > 1436 ? inFlight - 1436 : 0;   // one acknowledgement
      ++stalls;
      continue;
    }
    sent += g;
    inFlight += g + 8;
    TEST_ASSERT_TRUE(inFlight <= 3 * (nimbus::cloud::kTrickleChunk + 8));
  }
  TEST_ASSERT_TRUE(stalls > 0);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_dials_with_bounce_buffer_fragmentation);
  RUN_TEST(test_dials_on_fragmented_but_adequate_internal);
  RUN_TEST(test_refuses_when_genuinely_starved);
  RUN_TEST(test_largest_floor_clears_the_handshake_head);
  RUN_TEST(test_floor_invariants_hold);
  RUN_TEST(test_serve_decision_grid);
  RUN_TEST(test_trickle_grant_caps_and_never_stalls);
  return UNITY_END();
}
