// test_relay_presence - CUM-182 regression guard for hello-ack-gated, identity-
// bound presence. The load-bearing invariant: the device reports "online" ONLY
// after a relay hello-ack (Welcome) bound to ITS device id. A TLS connect or WS
// upgrade, on its own, must NEVER make presence true - that is what let a device
// latch a false "connected" against a stale/wrong endpoint.
#include <unity.h>

#include "nimbus/cloud/relay_presence.h"

using namespace nimbus::cloud;

void setUp() {}
void tearDown() {}

// The core guard: presence stays false through TLS + upgrade + hello, and only
// flips true on a matching hello-ack. If someone ever wires online to the socket
// instead of the ack, THIS fails.
static void test_online_requires_hello_ack() {
  RelayPresence p;
  TEST_ASSERT_FALSE(p.online());
  p.onTlsConnected();
  TEST_ASSERT_FALSE_MESSAGE(p.online(), "TLS connect alone must not be online");
  p.onUpgraded();
  TEST_ASSERT_FALSE_MESSAGE(p.online(), "WS upgrade alone must not be online");
  p.onHelloSent();
  TEST_ASSERT_FALSE_MESSAGE(p.online(), "hello sent (no ack yet) must not be online");
  TEST_ASSERT_TRUE(p.onHelloAck("dev_me", "dev_me"));
  TEST_ASSERT_TRUE_MESSAGE(p.online(), "matching hello-ack must go online");
}

// A Welcome that echoes a DIFFERENT device id (wrong endpoint / spoof) must be
// refused: presence stays false.
static void test_mismatched_ack_refused() {
  RelayPresence p;
  p.onTlsConnected();
  p.onUpgraded();
  p.onHelloSent();
  TEST_ASSERT_FALSE(p.onHelloAck("dev_other", "dev_me"));
  TEST_ASSERT_FALSE_MESSAGE(p.online(), "mismatched hello-ack must NOT be online");
}

// A legacy relay that does not echo the id still registers us: an empty/absent
// echo is accepted (backward compatible).
static void test_legacy_ack_accepted() {
  RelayPresence p;
  p.onTlsConnected();
  p.onUpgraded();
  p.onHelloSent();
  TEST_ASSERT_TRUE(p.onHelloAck("", "dev_me"));
  TEST_ASSERT_TRUE(p.online());

  RelayPresence p2;
  p2.onTlsConnected();
  p2.onUpgraded();
  p2.onHelloSent();
  TEST_ASSERT_TRUE(p2.onHelloAck(nullptr, "dev_me"));
  TEST_ASSERT_TRUE(p2.online());
}

// An ack that arrives out of order (before hello was sent) is ignored - presence
// cannot be short-circuited by a stray Welcome.
static void test_out_of_order_ack_ignored() {
  RelayPresence p;
  p.onTlsConnected();
  p.onUpgraded();
  // no onHelloSent()
  TEST_ASSERT_FALSE(p.onHelloAck("dev_me", "dev_me"));
  TEST_ASSERT_FALSE(p.online());
}

// Closing ends presence; a fresh reset starts not-online.
static void test_close_and_reset() {
  RelayPresence p;
  p.onTlsConnected();
  p.onUpgraded();
  p.onHelloSent();
  p.onHelloAck("dev_me", "dev_me");
  TEST_ASSERT_TRUE(p.online());
  p.onClosed();
  TEST_ASSERT_FALSE(p.online());
  p.reset();
  TEST_ASSERT_FALSE(p.online());
}

// The pure decision function, exercised directly.
static void test_evaluate_hello_ack() {
  TEST_ASSERT_EQUAL_INT((int)AckResult::Accept, (int)evaluateHelloAck("dev_me", "dev_me"));
  TEST_ASSERT_EQUAL_INT((int)AckResult::Accept, (int)evaluateHelloAck("", "dev_me"));
  TEST_ASSERT_EQUAL_INT((int)AckResult::Accept, (int)evaluateHelloAck(nullptr, "dev_me"));
  TEST_ASSERT_EQUAL_INT((int)AckResult::RejectMismatch,
                        (int)evaluateHelloAck("dev_other", "dev_me"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_online_requires_hello_ack);
  RUN_TEST(test_mismatched_ack_refused);
  RUN_TEST(test_legacy_ack_accepted);
  RUN_TEST(test_out_of_order_ack_ignored);
  RUN_TEST(test_close_and_reset);
  RUN_TEST(test_evaluate_hello_ack);
  return UNITY_END();
}
