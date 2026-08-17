#include <unity.h>

#include "nimbus/telegram_offset.h"

using nimbus::core::nextTelegramOffset;

void setUp() {}
void tearDown() {}

// After handling update N, the next offset must be N+1 (confirms/deletes N
// server-side). Storing N itself would re-fetch N forever (flooding the decider).
static void test_confirms_with_plus_one() {
  TEST_ASSERT_EQUAL_INT32(101, nextTelegramOffset(0, 100));
  TEST_ASSERT_EQUAL_INT32(1, nextTelegramOffset(0, 0));
}

// A batch advances the offset to the highest handled id + 1.
static void test_advances_across_batch() {
  int32_t off = 0;
  off = nextTelegramOffset(off, 5);
  off = nextTelegramOffset(off, 6);
  off = nextTelegramOffset(off, 7);
  TEST_ASSERT_EQUAL_INT32(8, off);
}

// Never move the offset backwards: an out-of-order / already-confirmed older
// update must not rewind it (that would re-deliver the whole tail).
static void test_never_moves_backwards() {
  TEST_ASSERT_EQUAL_INT32(50, nextTelegramOffset(50, 10));   // older id -> unchanged
  TEST_ASSERT_EQUAL_INT32(50, nextTelegramOffset(50, 49));   // id+1 == offset -> unchanged
  TEST_ASSERT_EQUAL_INT32(51, nextTelegramOffset(50, 50));   // id+1 > offset -> advance
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_confirms_with_plus_one);
  RUN_TEST(test_advances_across_batch);
  RUN_TEST(test_never_moves_backwards);
  return UNITY_END();
}
