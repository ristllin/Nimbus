#include <unity.h>

#include <cstdint>
#include <vector>

#include "nimbus/net/late_wait.h"

// The DNS wait's shared state (nimbus/net/late_wait.h): one bounded waiter, one callback
// that may come after the waiter gave up. The state must be freed exactly once and never
// used after it was freed, in every order the two sides can take. The device's first
// version freed the state when the waiter returned, so a late answer wrote into a dead
// stack frame and signalled a freed semaphore (src/sys/net_util.h resolveHostLocked).

using nimbus::net::LateWait;

void setUp() {}
void tearDown() {}

namespace {

// A tracked heap: every cell knows whether it is still allocated; a use or a free of a
// freed cell is counted, never performed.
struct Cell {
  LateWait wait;
  bool live = true;
  bool haveAnswer = false;
  uint32_t answer = 0;
};
struct Heap {
  std::vector<Cell*> all;
  int frees = 0, doubleFrees = 0, usesAfterFree = 0;
  Cell* make() {
    Cell* c = new Cell();
    all.push_back(c);
    return c;
  }
  void release(Cell* c) {
    if (!c->live) { ++doubleFrees; return; }
    c->live = false;
    ++frees;
  }
  void use(Cell* c) { if (!c->live) ++usesAfterFree; }
  ~Heap() { for (Cell* c : all) delete c; }
};

// The callback, as resolveHostLocked's dnsWaitFound runs it (core lock held).
void callback(Heap& h, Cell* c, uint32_t addr) {
  h.use(c);
  if (c->wait.callbackArrives()) {
    c->answer = addr;
    c->haveAnswer = true;   // and signal the waiter
    return;
  }
  h.release(c);   // the waiter left: the state is the callback's
}

// The waiter after its bounded wait (core lock held). Returns the answer it read.
bool waiterLeaves(Heap& h, Cell* c, uint32_t& addr) {
  h.use(c);
  if (!c->wait.waiterLeaves()) return false;   // never touch c again
  const bool ok = c->haveAnswer;
  addr = c->answer;
  h.release(c);
  return ok;
}

}  // namespace

void test_answer_before_the_waiter_leaves() {
  Heap h;
  Cell* c = h.make();
  callback(h, c, 0x0A000001u);
  uint32_t addr = 0;
  TEST_ASSERT_TRUE(waiterLeaves(h, c, addr));
  TEST_ASSERT_EQUAL_HEX32(0x0A000001u, addr);
  TEST_ASSERT_EQUAL_INT(1, h.frees);
  TEST_ASSERT_EQUAL_INT(0, h.doubleFrees);
  TEST_ASSERT_EQUAL_INT(0, h.usesAfterFree);
}

// The failure the device had: the wait times out, the answer comes later.
void test_answer_after_the_waiter_gave_up() {
  Heap h;
  Cell* c = h.make();
  uint32_t addr = 0;
  TEST_ASSERT_FALSE(waiterLeaves(h, c, addr));
  TEST_ASSERT_EQUAL_INT(0, h.frees);   // still the callback's
  callback(h, c, 0x0A000002u);
  TEST_ASSERT_EQUAL_INT(1, h.frees);
  TEST_ASSERT_EQUAL_INT(0, h.doubleFrees);
  TEST_ASSERT_EQUAL_INT(0, h.usesAfterFree);
}

// Every interleaving of many lookups: each cell freed once, nothing used after a free.
void test_every_order_frees_once() {
  Heap h;
  for (int pattern = 0; pattern < 256; ++pattern) {
    Cell* c = h.make();
    uint32_t addr = 0;
    if (pattern & 1) {
      callback(h, c, (uint32_t)pattern);
      TEST_ASSERT_TRUE(waiterLeaves(h, c, addr));
      TEST_ASSERT_EQUAL_UINT32((uint32_t)pattern, addr);
    } else {
      TEST_ASSERT_FALSE(waiterLeaves(h, c, addr));
      callback(h, c, (uint32_t)pattern);
    }
  }
  TEST_ASSERT_EQUAL_INT(256, h.frees);
  TEST_ASSERT_EQUAL_INT(0, h.doubleFrees);
  TEST_ASSERT_EQUAL_INT(0, h.usesAfterFree);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_answer_before_the_waiter_leaves);
  RUN_TEST(test_answer_after_the_waiter_gave_up);
  RUN_TEST(test_every_order_frees_once);
  return UNITY_END();
}
