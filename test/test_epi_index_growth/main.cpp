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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
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
  long append(const std::string& p, const std::string& b) override {
    std::string& f = files[p];
    const long off = (long)f.size();
    f += b;
    return off;
  }
  std::string readAll(const std::string& p) const override {
    auto it = files.find(p);
    return it == files.end() ? std::string() : it->second;
  }
  std::string readRange(const std::string& p, long off, long len) const override {
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

// The hook: refuses any block over g_refuseOver bytes (0 = none), counts refusals, and
// records the largest block asked for.
size_t g_refuseOver = 0;
long g_refused = 0;
size_t g_largest = 0;
void* hookAlloc(size_t n) {
  if (n > g_largest) g_largest = n;
  if (g_refuseOver && n > g_refuseOver) {
    g_refused++;
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
  size_t ring = 0;
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
  s.ring = st.ringRows();
  s.nextId = st.nextIdHint();
  s.count = st.messageCount();
  return s;
}
// A store whose boot scan indexed `boot` rows; then, with every block over 64 KB refused (the
// index's next step), the backfill runs on the slots it already has until the next slice
// would need a new block.
void bootAndFill(AppendLogEpisodicStore& st, int boot) {
  st.hydrate(boot, 0);
  st.backfillStep();   // the first step tops the index up (memory still free)
  g_refuseOver = 64 * 1024;
  while (st.indexFreeSlots() >= (size_t)kBackfillSliceRows && st.backfillStep()) {
  }
}
}  // namespace

void setUp() {
  g_refuseOver = 0;
  g_refused = 0;
  g_largest = 0;
  setWorkingAllocators(hookAlloc, hookFree);
}
void tearDown() {
  g_refuseOver = 0;
  setWorkingAllocators(nullptr, nullptr);
}

// R1: from a boot scan of 100 rows to the cap, no single block the index asks for is larger
// than (its size + kIndexGrowRows) rows: steps, never doubling.
static void test_the_index_grows_in_steps_never_doubling() {
  FakeFs fs;
  writeRows(fs, 0, 9000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(100, 0);
  size_t worst = 0, rowBytes = 0;
  for (int i = 0; i < 2000; i++) {
    const size_t size = (size_t)st.messageCount();
    g_largest = 0;
    const bool more = st.backfillStep();
    if (g_largest > 0 && st.indexRowBytes()) {
      rowBytes = st.indexRowBytes();
      const size_t allowed = (size + (size_t)kIndexGrowRows + (size_t)kIndexFreeSlots) * rowBytes;
      if (g_largest > worst) worst = g_largest;
      TEST_ASSERT_TRUE_MESSAGE(g_largest <= allowed, std::to_string(g_largest).c_str());
    }
    if (!more) break;
  }
  TEST_ASSERT_TRUE(st.indexComplete());
  TEST_ASSERT_TRUE(worst > 0);
  // Live appends past it grow by the same step.
  for (uint32_t n = 9000; n < 12000; n++) {
    const size_t size = (size_t)st.messageCount();
    g_largest = 0;
    st.addMessage(row(n));
    if (g_largest > 1024 * 1024)   // the index's block (the row's own strings are small)
      TEST_ASSERT_TRUE(g_largest <= (size + (size_t)kIndexGrowRows) * rowBytes);
  }
  assertEveryRowFound(st, 12000, "every row after growth");
}

// R2: a backfill step whose reserve is refused changes nothing (index, ring, cursors, the
// next-id hint), is retried, and after kBackfillMaxReadErrors refusals in a row stops in the
// existing stalled state; every row stays reachable meanwhile; with memory back and a restart
// the index completes.
static void test_a_refused_backfill_reserve_changes_nothing_then_stalls() {
  FakeFs fs;
  writeRows(fs, 0, 4000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  bootAndFill(st, 100);
  TEST_ASSERT_FALSE(st.indexComplete());
  for (int i = 0; i < kBackfillMaxReadErrors - 1; i++) {
    const Shot before = shot(st);
    const long refused = g_refused;
    TEST_ASSERT_TRUE_MESSAGE(st.backfillStep(), "a refused step is tried again");
    TEST_ASSERT_TRUE_MESSAGE(g_refused > refused, "the refusal was really taken");
    TEST_ASSERT_TRUE_MESSAGE(shot(st) == before, "a refused step changes nothing");
  }
  TEST_ASSERT_FALSE_MESSAGE(st.backfillStep(), "the limit stops the backfill");
  TEST_ASSERT_TRUE(st.backfillStats().stalled);
  TEST_ASSERT_FALSE(st.stalledOnReadErrors());
  TEST_ASSERT_FALSE(st.indexComplete());
  g_refuseOver = 0;
  assertEveryRowFound(st, 4000, "every row reachable while stalled");
  // Memory back and a restart: the index completes.
  AppendLogEpisodicStore again(fs, kDir, 64);
  again.hydrate(100, 0);
  for (int i = 0; i < 2000 && again.backfillStep(); i++) {}
  TEST_ASSERT_TRUE(again.indexComplete());
  TEST_ASSERT_EQUAL_INT(4000, again.messageCount());
  assertEveryRowFound(again, 4000, "every row after the restart");
}

// R2: a refusal in between is forgiven: the counter is of refusals IN A ROW.
static void test_a_later_step_with_memory_completes_the_index() {
  FakeFs fs;
  writeRows(fs, 0, 4000);
  AppendLogEpisodicStore st(fs, kDir, 64);
  bootAndFill(st, 100);
  for (int i = 0; i < kBackfillMaxReadErrors - 2; i++) st.backfillStep();
  TEST_ASSERT_TRUE(g_refused > 0);
  g_refuseOver = 0;
  for (int i = 0; i < 2000 && st.backfillStep(); i++) {}
  TEST_ASSERT_TRUE(st.indexComplete());
  assertEveryRowFound(st, 4000, "complete after memory returned");
}

// R3: the top-up keeps kIndexFreeSlots free for live appends, also once the backfill is
// done; a refused top-up changes nothing and the live append still goes (a step block).
static void test_the_top_up_keeps_free_slots_and_a_refusal_changes_nothing() {
  FakeFs fs;
  writeRows(fs, 0, 300);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(0, 0);   // complete at boot: the backfill has nothing to do
  st.addMessage(row(300));
  TEST_ASSERT_TRUE_MESSAGE(st.indexFreeSlots() >= (size_t)kIndexFreeSlots, "free slots after a live append");
  // Use the free slots up with the top-up refused: nothing changes but the rows appended.
  g_refuseOver = 64 * 1024;
  uint32_t n = 301;
  while (st.indexFreeSlots() > 1) st.addMessage(row(n++));
  const long refused = g_refused;
  TEST_ASSERT_TRUE_MESSAGE(refused > 0, "the top-up was refused");
  g_refuseOver = 0;
  st.addMessage(row(n++));   // the step block, allowed again
  TEST_ASSERT_TRUE(st.indexFreeSlots() >= (size_t)kIndexFreeSlots);
  assertEveryRowFound(st, n, "every live row");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_index_grows_in_steps_never_doubling);
  RUN_TEST(test_a_refused_backfill_reserve_changes_nothing_then_stalls);
  RUN_TEST(test_a_later_step_with_memory_completes_the_index);
  RUN_TEST(test_the_top_up_keeps_free_slots_and_a_refusal_changes_nothing);
  return UNITY_END();
}
