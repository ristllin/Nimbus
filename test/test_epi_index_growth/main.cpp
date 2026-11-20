// Candidate 6, revised (bench crash on 2da4a52c): the conversation index grows in steps of
// kIndexGrowRows, never by doubling, and the background backfill asks for its room with a
// guarded reserve BEFORE it decodes or moves anything. A refused reserve is "no progress this
// step": index, ring, cursors and the next-id hint stay exactly as they were; after
// kBackfillMaxReadErrors refusals in a row the backfill stops (the existing stalled state).
// A guarded top-up keeps kIndexFreeSlots free slots for live appends; its refusal changes
// nothing. The hook here refuses blocks over a size, which on the host only the index's own
// buffer reaches (its rows are 88 B), and counts what it refused so each test proves the
// refusal was really taken.
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <chrono>
#include <vector>

#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/psram_alloc.h"

using namespace nimbus::orch;

namespace {
struct FakeFs : EpiFs {
  std::map<std::string, std::string> files;
  long size(const std::string& p) const override {
    auto it = files.find(p);
    return it == files.end() ? 0 : (long)it->second.size();
  }
  bool failing = false;   // the card refuses writes (rows are held)
  long append(const std::string& p, const std::string& b) override {
    if (failing) return -1;
    std::string& f = files[p];
    const long off = (long)f.size();
    f += b;
    return off;
  }
  std::string readAll(const std::string& p) const override {
    auto it = files.find(p);
    return it == files.end() ? std::string() : it->second;
  }
  mutable long ranges = 0;   // readRange calls (card reads of record blocks)
  std::string readRange(const std::string& p, long off, long len) const override {
    ranges++;
    auto it = files.find(p);
    if (it == files.end() || off < 0 || len < 0 || (size_t)(off + len) > it->second.size()) return std::string();
    return it->second.substr((size_t)off, (size_t)len);
  }
  void forEachName(const std::string& dir, const std::function<void(const char*)>& fn) const override {
    const std::string prefix = dir + "/";
    for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
      if (std::strchr(it->first.c_str() + prefix.size(), '/') == nullptr) fn(it->first.c_str() + prefix.size());
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    forEachName(dir, [&](const char* n) { out.push_back(n); });
    return out;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool exists(const std::string& p) const override { return files.count(p) > 0; }
};

// The hook: refuses any block over g_refuseOver bytes (0 = none), or with g_refuseOnce only the
// first such block; counts refusals and records the largest block asked for.
size_t g_refuseOver = 0;
bool g_refuseOnce = false;
long g_refused = 0;
size_t g_largest = 0;
void* hookAlloc(size_t n) {
  if (n > g_largest) g_largest = n;
  if (g_refuseOver && n > g_refuseOver) {
    g_refused++;
    if (g_refuseOnce) g_refuseOver = 0;
    return nullptr;
  }
  return std::malloc(n ? n : 1);
}
void hookFree(void* p) { std::free(p); }

const char* kDir = "/mem/episodic";
std::string idOf(uint32_t n) {
  char b[16];
  std::snprintf(b, sizeof(b), "m%08x", (unsigned)n);
  return b;
}
EpisodicMessage row(uint32_t n) {   // ids from m00000001; 40 rows a day
  EpisodicMessage m;
  m.id = idOf(n + 1);
  m.sessionId = "1001";
  m.tsHours = (20000 + n / 40) * 24 + (n % 24);
  m.role = n % 2 ? "assistant" : "user";
  m.kind = MsgKind::Message;
  m.text = "row " + std::to_string(n) + " about the garden";
  return m;
}
void writeRows(FakeFs& fs, uint32_t from, uint32_t to) {
  AppendLogEpisodicStore w(fs, kDir, 64);
  for (uint32_t n = from; n < to; n++) w.addMessage(row(n));
}
// Every row id, newest first, through the `before` cursor and cold reads: once each, in order.
void assertEveryRowFound(const AppendLogEpisodicStore& st, uint32_t rows, const char* what) {
  std::vector<std::string> ids;
  MsgQuery q;
  q.limit = 100;
  q.coldScan = true;
  std::string before;
  for (int pages = 0; pages < 2000; pages++) {
    q.before = before;
    EpiQueryInfo info;
    for (const auto& m : st.query(q, &info)) {
      TEST_ASSERT_TRUE_MESSAGE(ids.empty() || m.id < ids.back(), what);
      ids.push_back(m.id);
    }
    if (info.nextBefore.empty() || info.nextBefore == before) break;
    before = info.nextBefore;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)rows, (int)ids.size(), what);
  for (uint32_t n = 0; n < rows; n++) TEST_ASSERT_EQUAL_STRING_MESSAGE(idOf(rows - n).c_str(), ids[n].c_str(), what);
}
// What a refused step must leave untouched.
struct Shot {
  std::vector<std::vector<uint32_t>> index;
  std::pair<size_t, long> cursor;
  std::vector<std::string> ring;
  uint32_t nextId = 0;
  int count = 0;
  bool operator==(const Shot& o) const {
    return index == o.index && cursor == o.cursor && ring == o.ring && nextId == o.nextId && count == o.count;
  }
};
Shot shot(const AppendLogEpisodicStore& st) {
  Shot s;
  st.forEachIndexed([&](uint32_t d, uint32_t off, uint32_t len, uint32_t sfx) { s.index.push_back({d, off, len, sfx}); });
  s.cursor = st.backfillCursor();
  s.ring = st.ringIds();
  s.nextId = st.nextIdHint();
  s.count = st.messageCount();
  return s;
}
size_t capacity(const AppendLogEpisodicStore& st) { return st.indexFreeSlots() + (size_t)st.messageCount(); }
// v5.1.1's doubling or 2,048 rows, whichever is less (S1).
size_t step(size_t size) { return std::min<size_t>(kIndexGrowRows, std::max<size_t>(1, size)); }
size_t target(size_t size) { return std::min<size_t>(kIndexFreeSlots, std::max<size_t>(1, size)); }

// A store whose boot scan indexed `boot` rows; then, with every block over 64 KB refused (the
// index's next step), the backfill runs on the slots it already has until its first refused
// step (the first of the refusals in a row).
void bootAndFill(AppendLogEpisodicStore& st, int boot) {
  st.hydrate(boot, 0);
  st.backfillStep();   // the first step tops the index up (memory still free)
  g_refuseOver = 64 * 1024;
  for (int i = 0; i < 2000 && g_refused == 0 && st.backfillStep(); i++) {
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)g_refused, "the fill ends on one refused step");
}
}  // namespace

void setUp() {
  g_refuseOver = 0;
  g_refuseOnce = false;
  g_refused = 0;
  g_largest = 0;
  setWorkingAllocators(hookAlloc, hookFree);
}
void tearDown() {
  g_refuseOver = 0;
  setWorkingAllocators(nullptr, nullptr);
}

// S1: every growth, backfill or live, from an empty or a 100-row index to 12,000 rows, adds
// at most min(2,048, size) rows (plus the slice's own room at a backfill step): never more
// than v5.1.1's doubling, never more than 2,048 rows. Checked on the index's capacity.
static void test_every_growth_is_at_most_its_size_or_2048_rows() {
  FakeFs fs;
  writeRows(fs, 0, 9000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(100, 0);
  for (int i = 0; i < 2000; i++) {
    const size_t size = (size_t)st.messageCount(), cap = capacity(st);
    const bool more = st.backfillStep();
    if (capacity(st) > cap)
      TEST_ASSERT_TRUE_MESSAGE(capacity(st) <= size + step(size) + (size_t)kBackfillSliceRows,
                               (std::to_string(size) + " -> " + std::to_string(capacity(st))).c_str());
    if (!more) break;
  }
  TEST_ASSERT_TRUE(st.indexComplete());
  for (uint32_t n = 9000; n < 12000; n++) {
    const size_t size = (size_t)st.messageCount(), cap = capacity(st);
    st.addMessage(row(n));
    if (capacity(st) > cap) TEST_ASSERT_TRUE(capacity(st) <= size + step(size));
  }
  assertEveryRowFound(st, 12000, "every row after growth");
  FakeFs empty;   // from nothing: the first live appends ask for a row or two, not 2,048
  AppendLogEpisodicStore e(empty, kDir, 64);
  e.hydrate(0, 0);
  for (uint32_t n = 0; n < 3000; n++) {
    const size_t size = (size_t)e.messageCount(), cap = capacity(e);
    e.addMessage(row(n));
    if (capacity(e) > cap)
      TEST_ASSERT_TRUE_MESSAGE(capacity(e) <= size + step(size),
                               (std::to_string(size) + " -> " + std::to_string(capacity(e))).c_str());
  }
  assertEveryRowFound(e, 3000, "every row from an empty index");
}

// S1/S5: the live append's own step, with its top-up refused once: the append goes, the step is
// at most 2,048 rows (a doubling there would add the index's whole size), one refusal taken.
static void test_the_live_step_under_a_refuse_once_hook() {
  FakeFs fs;
  writeRows(fs, 0, 3000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(0, 0);   // 3,000 rows, no free slot
  TEST_ASSERT_EQUAL_INT(0, (int)st.indexFreeSlots());
  g_refuseOver = 64 * 1024;
  g_refuseOnce = true;
  st.addMessage(row(3000));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)g_refused, "the top-up was refused once");
  TEST_ASSERT_TRUE_MESSAGE(capacity(st) <= 3000 + (size_t)kIndexGrowRows, std::to_string(capacity(st)).c_str());
  assertEveryRowFound(st, 3001, "every row after the step");
}

// R2/S5: a refused backfill reserve changes nothing (index rows, ring content, cursor, next-id
// hint, count), each refused step is exactly one refusal of the backfill's own reserve (the
// top-up is not due, or backed off), refusals short of the limit with a success in between do not stall, and
// kBackfillMaxReadErrors in a row do; every row stays reachable; a restart completes the index.
static void test_refusals_in_a_row_stall_and_a_success_in_between_does_not() {
  FakeFs fs;
  writeRows(fs, 0, 6000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  bootAndFill(st, 100);
  TEST_ASSERT_FALSE(st.indexComplete());
  for (int round = 0; round < 2; round++) {
    for (int i = (round == 0 ? 1 : 0); i < kBackfillMaxReadErrors - 1; i++) {
      const Shot before = shot(st);
      const long refused = g_refused;
      TEST_ASSERT_TRUE_MESSAGE(st.backfillStep(), "a refused step is tried again");
      // The first refused step after the success may also refuse the top-up (which then backs
      // off); every other is exactly one refusal: the backfill's own reserve.
      if (round == 1 && i == 0) TEST_ASSERT_TRUE(g_refused - refused >= 1);
      else TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)(g_refused - refused), "one refusal: the backfill's reserve");
      TEST_ASSERT_TRUE_MESSAGE(shot(st) == before, "a refused step changes nothing");
    }
    if (round == 0) {   // one step with memory in between: the count of refusals in a row restarts
      g_refuseOver = 0;
      const int rows = st.messageCount();
      TEST_ASSERT_TRUE(st.backfillStep());
      TEST_ASSERT_TRUE(st.messageCount() > rows);
      g_refuseOver = 64 * 1024;
      TEST_ASSERT_FALSE(st.backfillStats().stalled);
    }
  }
  TEST_ASSERT_FALSE_MESSAGE(st.backfillStats().stalled, "seven in a row do not stall");
  TEST_ASSERT_FALSE_MESSAGE(st.backfillStep(), "the eighth in a row stops the backfill");
  TEST_ASSERT_TRUE(st.backfillStats().stalled);
  TEST_ASSERT_TRUE(st.stalledForMemory());
  TEST_ASSERT_FALSE(st.stalledOnReadErrors());
  g_refuseOver = 0;
  assertEveryRowFound(st, 6000, "every row reachable while stalled");
  AppendLogEpisodicStore again(fs, kDir, 64);
  again.hydrate(100, 0);
  for (int i = 0; i < 4000 && again.backfillStep(); i++) {}
  TEST_ASSERT_TRUE(again.indexComplete());
  assertEveryRowFound(again, 6000, "every row after the restart");
}

// S3: a refused top-up backs off: while blocks are refused, live appends on the free slots
// already there try the top-up once per kTopUpBackoffRows appends, not once per message.
static void test_a_refused_top_up_backs_off() {
  FakeFs fs;
  writeRows(fs, 0, 3000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(0, 0);
  st.addMessage(row(3000));   // memory free: the step gives 2,048 free slots
  uint32_t n = 3001;
  g_refuseOver = 64 * 1024;
  while (st.indexFreeSlots() > 1) st.addMessage(row(n++));
  const long attempts = (long)(kIndexGrowRows - kIndexFreeSlots) / kTopUpBackoffRows + 2;
  TEST_ASSERT_TRUE_MESSAGE(g_refused > 0, "the top-up was tried");
  TEST_ASSERT_TRUE_MESSAGE(g_refused <= attempts, std::to_string(g_refused).c_str());
  g_refuseOver = 0;
  assertEveryRowFound(st, n, "every live row");
}

// S5: a small heap (every block over 64 KB refused from the start): appends to an empty store
// and to a 300-row store go, as they did in v5.1.1.
static void test_a_small_heap_takes_appends_to_small_stores() {
  g_refuseOver = 64 * 1024;
  FakeFs empty;
  AppendLogEpisodicStore e(empty, kDir, 64);
  e.hydrate(0, 0);
  for (uint32_t n = 0; n < 200; n++) e.addMessage(row(n));
  FakeFs fs;
  {
    g_refuseOver = 0;
    writeRows(fs, 0, 300);
    g_refuseOver = 64 * 1024;
  }
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(0, 0);
  for (uint32_t n = 300; n < 400; n++) st.addMessage(row(n));
  g_refuseOver = 0;
  assertEveryRowFound(e, 200, "the empty store's rows");
  assertEveryRowFound(st, 400, "the 300-row store's rows");
}

// T1: the top-up has hysteresis: from an empty index, 2,048 live appends reallocate the index
// at most 40 times (v5.1.1's doubling did it 12 times), not once per message.
static void test_live_appends_reallocate_a_bounded_number_of_times() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(0, 0);
  int reallocs = 0;
  for (uint32_t n = 0; n < 2048; n++) {
    const size_t cap = capacity(st);
    st.addMessage(row(n));
    reallocs += capacity(st) != cap;
  }
  TEST_ASSERT_TRUE_MESSAGE(reallocs <= 40, std::to_string(reallocs).c_str());
  assertEveryRowFound(st, 2048, "every row");
}

// T2: at a low cap (5,000 rows over a 9,000-row card) the backfill keeps the free-slot target:
// every growth is at most max(cap + target, size + step + a slice), it ends at the cap with
// capped set and free slots left, so the first message after it needs no unguarded step.
static void test_at_the_cap_the_backfill_keeps_free_slots() {
  FakeFs fs;
  writeRows(fs, 0, 9000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.setIndexCap(5000);
  st.hydrate(100, 0);
  for (int i = 0; i < 2000; i++) {
    const size_t size = (size_t)st.messageCount(), cap = capacity(st);
    const bool more = st.backfillStep();
    if (capacity(st) > cap)
      TEST_ASSERT_TRUE_MESSAGE(capacity(st) <= std::max(5000 + target(size), size + step(size) + (size_t)kBackfillSliceRows),
                               (std::to_string(size) + " -> " + std::to_string(capacity(st))).c_str());
    if (!more) break;
  }
  TEST_ASSERT_EQUAL_INT(5000, st.messageCount());
  TEST_ASSERT_TRUE(st.backfillStats().capped);
  TEST_ASSERT_TRUE_MESSAGE(st.indexFreeSlots() > 0, "free slots remain at the cap");
  assertEveryRowFound(st, 9000, "every row at the cap");
}

// T2: the same in the narrow memory band where a block up to the cap is given but a larger one
// (the top-up past the cap) is refused: the backfill stops for memory with free slots kept, or
// caps with them; it never ends with the index full.
static void test_a_refused_reserve_near_the_cap_stalls_with_slots_kept() {
  FakeFs fs;
  writeRows(fs, 0, 9000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.setIndexCap(5000);
  st.hydrate(100, 0);
  g_refuseOver = (5000 + 512) * AppendLogEpisodicStore::indexRowBytes();
  for (int i = 0; i < 2000 && st.backfillStep(); i++) {
  }
  TEST_ASSERT_TRUE_MESSAGE(g_refused > 0, "a reserve was refused");
  TEST_ASSERT_TRUE_MESSAGE(st.indexFreeSlots() > 0, "the index never ends full");
  TEST_ASSERT_TRUE(st.stalledForMemory() || st.backfillStats().capped);
  g_refuseOver = 0;
  assertEveryRowFound(st, 9000, "every row");
}

// T4: the held-row flush, the third growth site, under the same bound.
static void test_the_held_row_flush_grows_by_the_step() {
  FakeFs fs;
  writeRows(fs, 0, 3000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(0, 0);   // 3,000 rows, no free slot
  fs.failing = true;
  for (uint32_t n = 3000; n < 3050; n++) st.addMessage(row(n));
  TEST_ASSERT_EQUAL_INT(50, st.unpersistedCount());
  fs.failing = false;
  while (st.unpersistedCount() > 0) {
    const size_t size = (size_t)st.messageCount() - (size_t)st.unpersistedCount(), cap = capacity(st) - (size_t)st.unpersistedCount();
    TEST_ASSERT_TRUE(st.flushHeld(1) == 1);
    const size_t capAfter = capacity(st) - (size_t)st.unpersistedCount();
    if (capAfter > cap) TEST_ASSERT_TRUE_MESSAGE(capAfter <= size + step(size), std::to_string(capAfter).c_str());
  }
  assertEveryRowFound(st, 3050, "every flushed row");
}

// R2: refusals short of the limit are forgiven once memory returns, and the index completes.
static void test_a_later_step_with_memory_completes_the_index() {
  FakeFs fs;
  writeRows(fs, 0, 4000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  bootAndFill(st, 100);
  const long refused = g_refused;
  for (int i = 0; i < kBackfillMaxReadErrors - 2; i++) st.backfillStep();
  TEST_ASSERT_TRUE(g_refused > refused);
  g_refuseOver = 0;
  for (int i = 0; i < 2000 && st.backfillStep(); i++) {}
  TEST_ASSERT_TRUE(st.indexComplete());
  assertEveryRowFound(st, 4000, "complete after memory returned");
}

// Ruling 90 (RB-Q2b): a call's own card-read cap. The turn-open conversation recall reads the
// RAM ring freely and at most maxReads record blocks off the card; 0 keeps the store's budget.
void test_a_call_can_tighten_its_card_read_budget() {
  FakeFs fs;
  writeRows(fs, 0, 3000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(0, 0);
  MsgQuery q;
  q.sessionId = "1001";
  q.textContains = "garden 17";   // ranked: the whole index is a candidate
  q.limit = 12;
  q.maxReads = 4;
  fs.ranges = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const auto capped = st.query(q);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const long cappedReads = fs.ranges;
  std::printf("RBQ2B-RECALL index of 3000 rows, ring 64, maxReads 4: %ld block reads, %.2f ms (host)\n", cappedReads, ms);
  TEST_ASSERT_TRUE(cappedReads <= 4);
  TEST_ASSERT_FALSE(capped.empty());   // the ring answered what it could
  q.maxReads = 0;   // counter-test: the store's own budget reads further
  fs.ranges = 0;
  st.query(q);
  TEST_ASSERT_TRUE(fs.ranges > cappedReads);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_growth_is_at_most_its_size_or_2048_rows);
  RUN_TEST(test_the_live_step_under_a_refuse_once_hook);
  RUN_TEST(test_refusals_in_a_row_stall_and_a_success_in_between_does_not);
  RUN_TEST(test_a_refused_top_up_backs_off);
  RUN_TEST(test_a_small_heap_takes_appends_to_small_stores);
  RUN_TEST(test_a_later_step_with_memory_completes_the_index);
  RUN_TEST(test_live_appends_reallocate_a_bounded_number_of_times);
  RUN_TEST(test_at_the_cap_the_backfill_keeps_free_slots);
  RUN_TEST(test_a_refused_reserve_near_the_cap_stalls_with_slots_kept);
  RUN_TEST(test_the_held_row_flush_grows_by_the_step);
  RUN_TEST(test_a_call_can_tighten_its_card_read_budget);
  return UNITY_END();
}
