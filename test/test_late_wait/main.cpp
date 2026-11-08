#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "nimbus/net/dns_lookup.h"
#include "nimbus/net/late_wait.h"

// The DNS wait's shared state (nimbus/net/late_wait.h): one bounded waiter, one callback
// that may come after the waiter gave up. The state must be freed exactly once and never
// used after it was freed, in every order the two sides can take. The device's first
// version freed the state when the waiter returned, so a late answer wrote into a dead
// stack frame and signalled a freed semaphore (src/sys/net_util.h resolveHostLocked).
//
// The first three cases pin the cell's rule. The rest run the device's lookup itself
// (nimbus/net/dns_lookup.h DnsLookup, which resolveHostLocked instantiates with the real
// lwIP calls) against a fake lwIP that delivers the answer at every point it can arrive.

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

// ---- DnsLookup against a fake lwIP --------------------------------------------------

namespace {

// When lwIP's found callback runs, relative to the waiter.
enum class When { InQuery, DuringWait, BeforeLeave, AfterLeave };
const char* whenName(When w) {
  switch (w) {
    case When::InQuery:     return "in the query";
    case When::DuringWait:  return "during the wait";
    case When::BeforeLeave: return "after the wait, before the waiter leaves";
    case When::AfterLeave:  return "after the waiter gave up";
  }
  return "?";
}

// One fake lwIP: a tracked heap (a destroyed state stays allocated until the world ends,
// so a late touch is counted, never performed), tracked semaphores, the core lock, and the
// callbacks lwIP still owes.
struct World {
  nimbus::net::DnsQuery query = nimbus::net::DnsQuery::Pending;
  When when = When::DuringWait;
  bool answers = true;       // the callback carries an address (false: a failed lookup)
  uint32_t addr = 0x0A000001u;
  bool own = false;          // the caller is the TCPIP thread (it holds the core lock)
  bool noMemory = false, noSem = false;

  struct Obj {
    void* p;
    std::function<void()> del;
    bool live;
  };
  std::vector<Obj> objs;
  std::vector<bool> sems;   // index = handle - 1; true while allocated
  struct Owed {
    void (*fn)(const char*, const uint32_t*, void*);
    void* arg;
  };
  std::vector<Owed> owed;
  bool lockHeld = false;
  int destroys = 0, doubleDestroys = 0, semFrees = 0, semDoubleFrees = 0, deadSignals = 0;
  int callbacksIntoFreed = 0, lockErrors = 0, waits = 0;

  ~World() {
    for (Obj& o : objs) o.del();
  }
  bool live(void* p) const {
    for (const Obj& o : objs)
      if (o.p == p) return o.live;
    return false;
  }
  size_t liveObjs() const {
    size_t n = 0;
    for (const Obj& o : objs) n += o.live ? 1 : 0;
    return n;
  }
  size_t liveSems() const {
    size_t n = 0;
    for (bool b : sems) n += b ? 1 : 0;
    return n;
  }
  // The TCPIP thread runs one owed callback (the oldest, or the newest), holding the core
  // lock: no waiter holds it at any point this fake delivers.
  void deliverOne(bool newest = false) {
    if (owed.empty()) return;
    const Owed o = newest ? owed.back() : owed.front();
    if (newest) owed.pop_back();
    else owed.erase(owed.begin());
    if (!live(o.arg)) {   // the waiter freed what lwIP still pointed at
      ++callbacksIntoFreed;
      return;
    }
    if (lockHeld) ++lockErrors;
    lockHeld = true;
    o.fn("host", answers ? &addr : nullptr, o.arg);
    lockHeld = false;
  }
  void deliverAll() {
    while (!owed.empty()) deliverOne();
  }
};

World* W = nullptr;

struct FakeLwip {
  using Addr = uint32_t;
  using Sem = int;   // 1-based handle into World::sems; 0 is no semaphore
  template <class T>
  static T* make() {
    if (W->noMemory) return nullptr;
    T* t = new T();
    W->objs.push_back({t, [t] { delete t; }, true});
    return t;
  }
  template <class T>
  static void destroy(T* t) {
    for (World::Obj& o : W->objs) {
      if (o.p != t) continue;
      if (!o.live) { ++W->doubleDestroys; return; }
      o.live = false;
      ++W->destroys;
      return;
    }
    ++W->doubleDestroys;   // never made here
  }
  static bool semNew(Sem& s) {
    if (W->noSem) return false;
    W->sems.push_back(true);
    s = (int)W->sems.size();
    return true;
  }
  static void semFree(Sem& s) {
    if (s <= 0 || !W->sems[s - 1]) { ++W->semDoubleFrees; return; }
    W->sems[s - 1] = false;
    ++W->semFrees;
  }
  static void semSignal(Sem& s) {
    if (s <= 0 || !W->sems[s - 1]) ++W->deadSignals;
  }
  static void semWait(Sem&, uint32_t) {
    ++W->waits;
    if (W->lockHeld) ++W->lockErrors;   // waiting with the core lock held starves the TCPIP thread
    if (W->when == When::DuringWait) W->deliverOne(/*newest=*/true);   // this lookup's answer
  }
  static bool onTcpipThread() { return W->own; }
  // dns_gethostbyname under the core lock (taken here unless the caller holds it).
  static nimbus::net::DnsQuery query(bool own, const char*, Addr& cached,
                                     void (*fn)(const char*, const Addr*, void*), void* arg) {
    if (own != W->lockHeld) ++W->lockErrors;
    W->lockHeld = true;
    if (W->query == nimbus::net::DnsQuery::Cached) cached = W->addr;
    if (W->query == nimbus::net::DnsQuery::Pending) {
      W->owed.push_back({fn, arg});
      if (W->when == When::InQuery) {   // lwIP answered inside the call, the lock held
        const World::Owed o = W->owed.back();
        W->owed.pop_back();
        o.fn("host", W->answers ? &W->addr : nullptr, o.arg);
      }
    }
    if (!own) W->lockHeld = false;
    return W->query;
  }
  static bool leave(bool own, nimbus::net::LateWait& cell) {
    if (W->when == When::BeforeLeave) W->deliverOne(/*newest=*/true);   // the TCPIP thread got the lock first
    if (own != W->lockHeld) ++W->lockErrors;
    W->lockHeld = true;
    const bool answered = cell.waiterLeaves();
    if (!own) W->lockHeld = false;
    return answered;
  }
};

using Lookup = nimbus::net::DnsLookup<FakeLwip>;

// One lookup in `w`, then the TCPIP thread runs whatever it still owes. Returns the result.
bool runLookup(World& w, uint32_t& out) {
  W = &w;
  w.lockHeld = w.own;
  out = 0;
  const bool ok = Lookup::resolve("api.mistral.ai", out, 10000);
  if (w.lockHeld != w.own) ++w.lockErrors;   // the lock is as the caller had it
  w.lockHeld = false;                          // the TCPIP thread moves on
  w.deliverAll();
  W = nullptr;
  return ok;
}

void assertClean(const World& w, const char* why) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, w.callbacksIntoFreed, why);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, w.deadSignals, why);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, w.doubleDestroys, why);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, w.semDoubleFrees, why);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, w.lockErrors, why);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, w.liveObjs(), why);   // freed, by one side or the other
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, w.liveSems(), why);
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)w.objs.size(), w.destroys, why);
}

}  // namespace

// Every way a lookup can go: a cached answer, a refused query, and an answer (or a
// failure) arriving in the query, during the wait, between the wait and the waiter
// leaving, or after the waiter gave up; from an ordinary task and from the TCPIP thread
// itself. Each state and semaphore freed exactly once, nothing touched after it was freed,
// the wait never holds the core lock, and the answer read only when it came in time.
void test_lookup_every_order_frees_once_and_answers_right() {
  using nimbus::net::DnsQuery;
  {
    World w;
    w.query = DnsQuery::Cached;
    uint32_t out = 0;
    TEST_ASSERT_TRUE(runLookup(w, out));
    TEST_ASSERT_EQUAL_HEX32(0x0A000001u, out);
    TEST_ASSERT_EQUAL_INT(0, w.waits);
    assertClean(w, "cached");
  }
  {
    World w;
    w.query = DnsQuery::Failed;
    uint32_t out = 0;
    TEST_ASSERT_FALSE(runLookup(w, out));
    assertClean(w, "refused");
  }
  for (const bool own : {false, true}) {
    for (const When when : {When::InQuery, When::DuringWait, When::BeforeLeave, When::AfterLeave}) {
      // The TCPIP thread never waits, and nothing else runs on it until it returns.
      if (own && (when == When::DuringWait || when == When::BeforeLeave)) continue;
      for (const bool answers : {true, false}) {
        World w;
        w.query = DnsQuery::Pending;
        w.when = when;
        w.answers = answers;
        w.own = own;
        uint32_t out = 0;
        const bool ok = runLookup(w, out);
        char why[128];
        snprintf(why, sizeof why, "%s, %s, %s", own ? "TCPIP thread" : "a task", whenName(when),
                 answers ? "an address" : "a failure");
        const bool inTime = when != When::AfterLeave;
        TEST_ASSERT_EQUAL_MESSAGE(inTime && answers, ok, why);
        if (ok) TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x0A000001u, out, why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(own ? 0 : 1, w.waits, why);
        assertClean(w, why);
      }
    }
  }
}

// The Lumi-era shape, step by step: the wait times out, the waiter leaves, and the state
// must still be there for the answer lwIP owes; the answer then frees it.
void test_lookup_late_answer_after_the_waiter_gave_up() {
  World w;
  w.query = nimbus::net::DnsQuery::Pending;
  w.when = When::AfterLeave;
  W = &w;
  uint32_t out = 0;
  TEST_ASSERT_FALSE(Lookup::resolve("api.telegram.org", out, 10000));
  TEST_ASSERT_EQUAL_size_t(1, w.owed.size());   // lwIP still owes the answer
  TEST_ASSERT_EQUAL_size_t(1, w.liveObjs());    // and the state it will write is still there
  TEST_ASSERT_EQUAL_size_t(1, w.liveSems());
  w.deliverAll();
  W = nullptr;
  assertClean(w, "late answer");
}

// Out of memory before the query: nothing goes out, nothing leaks.
void test_lookup_out_of_memory() {
  {
    World w;
    w.noMemory = true;
    uint32_t out = 0;
    TEST_ASSERT_FALSE(runLookup(w, out));
    TEST_ASSERT_EQUAL_size_t(0, w.objs.size());
    assertClean(w, "no memory");
  }
  {
    World w;
    w.noSem = true;
    uint32_t out = 0;
    TEST_ASSERT_FALSE(runLookup(w, out));
    TEST_ASSERT_EQUAL_INT(1, w.destroys);
    TEST_ASSERT_EQUAL_INT(0, w.waits);
    TEST_ASSERT_EQUAL_size_t(0, w.owed.size());
    TEST_ASSERT_EQUAL_INT(0, w.callbacksIntoFreed);
    TEST_ASSERT_EQUAL_INT(0, w.doubleDestroys);
  }
}

// Many lookups whose late answers pile up and arrive in a scrambled order, as lwIP's
// retries across three servers can deliver them.
void test_lookup_many_late_answers_in_any_order() {
  World w;
  w.query = nimbus::net::DnsQuery::Pending;
  W = &w;
  uint32_t seed = 12345;
  int answered = 0;
  for (int i = 0; i < 300; ++i) {
    seed = seed * 1103515245u + 12345u;
    w.when = (seed >> 16) % 3 == 0 ? When::DuringWait : When::AfterLeave;
    uint32_t out = 0;
    if (Lookup::resolve("api.mistral.ai", out, 10000)) ++answered;
    if ((seed >> 20) % 5 == 0 && w.owed.size() > 1) {   // the TCPIP thread catches up
      std::swap(w.owed.front(), w.owed.back());
      w.deliverOne();
    }
  }
  w.deliverAll();
  W = nullptr;
  TEST_ASSERT_TRUE(answered > 0);
  assertClean(w, "300 lookups");
  TEST_ASSERT_EQUAL_INT(300, w.destroys);
  TEST_ASSERT_EQUAL_INT(300, w.semFrees);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_answer_before_the_waiter_leaves);
  RUN_TEST(test_answer_after_the_waiter_gave_up);
  RUN_TEST(test_every_order_frees_once);
  RUN_TEST(test_lookup_every_order_frees_once_and_answers_right);
  RUN_TEST(test_lookup_late_answer_after_the_waiter_gave_up);
  RUN_TEST(test_lookup_out_of_memory);
  RUN_TEST(test_lookup_many_late_answers_in_any_order);
  return UNITY_END();
}
