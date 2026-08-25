// test_ws_write - CUM-182 regression guard for the reliable whole-frame WS write
// driver. The bug: a large res frame (~370 KB config/login page) that the socket
// accepted only partially was treated as a failure, truncating the frame and
// dropping the tunnel session. drainAll must instead resume at the sent offset
// until the whole buffer is out - and must fail cleanly on a dead socket or a
// blown deadline.
#include <unity.h>

#include "nimbus/cloud/ws_write.h"

using namespace nimbus::cloud;

void setUp() {}
void tearDown() {}

static auto never_expired = []() { return false; };
static auto always_alive = []() { return true; };

// A partial-write sink: accepts at most 1000 bytes per call. The whole buffer
// must still be delivered (no truncation, no false failure).
static void test_partial_writes_complete() {
  const size_t len = 370000;  // ~ a real config page frame
  size_t delivered = 0;
  bool ok = drainAll(
      len, 8192,
      [&](size_t off, size_t want) -> size_t {
        (void)off;
        size_t n = want > 1000 ? 1000 : want;
        delivered += n;
        return n;
      },
      always_alive, never_expired, []() {});
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_UINT(len, delivered);
}

// A would-block sink (returns 0 sometimes) must be retried, not failed.
static void test_would_block_retries_then_completes() {
  const size_t len = 5000;
  size_t delivered = 0;
  int calls = 0;
  bool yielded = false;
  bool ok = drainAll(
      len, 4096,
      [&](size_t off, size_t want) -> size_t {
        (void)off;
        if (++calls % 2 == 1) return 0;  // every other call blocks
        delivered += want;
        return want;
      },
      always_alive, never_expired, [&]() { yielded = true; });
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE(yielded);
  TEST_ASSERT_EQUAL_UINT(len, delivered);
}

// A dead connection mid-write is a hard failure - no spinning to the deadline.
static void test_dead_connection_fails_fast() {
  const size_t len = 100000;
  size_t delivered = 0;
  bool ok = drainAll(
      len, 8192,
      [&](size_t off, size_t want) -> size_t {
        (void)off;
        delivered += want;
        return want;
      },
      [&]() { return delivered < 20000; },  // "dies" after ~20 KB
      never_expired, []() {});
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_TRUE(delivered < len);
}

// A sink that blocks forever must fail once the deadline passes (bounded, no hang).
static void test_deadline_bounds_a_stuck_write() {
  uint32_t clock = 0;
  bool ok = drainAll(
      1000, 512,
      [](size_t, size_t) -> size_t { return 0; },  // always would-block
      always_alive, [&]() { return clock >= 50; }, [&]() { clock += 10; });
  TEST_ASSERT_FALSE(ok);
}

// Zero-length write trivially succeeds.
static void test_zero_length() {
  bool ok = drainAll(
      0, 8192, [](size_t, size_t) -> size_t { return 0; }, always_alive, never_expired, []() {});
  TEST_ASSERT_TRUE(ok);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_partial_writes_complete);
  RUN_TEST(test_would_block_retries_then_completes);
  RUN_TEST(test_dead_connection_fails_fast);
  RUN_TEST(test_deadline_bounds_a_stuck_write);
  RUN_TEST(test_zero_length);
  return UNITY_END();
}
