#include <unity.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "../support/tx_items/gen.h"
#include "nimbus/orch/tx/arena.h"

// test_tx_arena - the RAM side of a chat's transcript (lane RB-T1, PRD section 2): a flat
// block of encoded items behind 32-byte headers. The allocation seam: every operator new
// of this binary is counted and the arena's allocator is a counting hook, so a test
// proves an append takes no heap block (on the device: no internal-heap allocation per
// item; the one block comes from Platform::allocLarge, PSRAM).
namespace tx = nimbus::orch::tx;
using txgen::Rng;

// ---- the allocation seam ---------------------------------------------------------------
static std::atomic<long> g_news{0};
void* operator new(std::size_t n) {
  g_news.fetch_add(1);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) {
  g_news.fetch_add(1);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

struct Seam {
  int allocs = 0;
  int frees = 0;
  size_t bytes = 0;
  bool refuse = false;
  tx::ArenaAllocFn alloc() {
    return [this](size_t n) -> void* {
      if (refuse) return nullptr;
      ++allocs;
      bytes += n;
      return std::malloc(n);
    };
  }
  tx::ArenaFreeFn release() {
    return [this](void* p) {
      ++frees;
      std::free(p);
    };
  }
};

std::string enc(const tx::Item& it) {
  std::string line;
  TEST_ASSERT_TRUE(tx::encodeLine(it, line).ok());
  return line;
}

// A day's worth of shapes: owner message, turn open, call, intent, result, assistant
// text, notes, digests, moves, delivery, turn end; every kind, realistic sizes.
std::vector<tx::Item> conversation(size_t n, uint64_t seed) {
  Rng r(seed);
  std::vector<tx::Item> items;
  uint32_t turn = 1;
  for (uint64_t q = 1; items.size() < n; ++q) {
    const tx::ItemKind k = tx::kAllItemKinds[(q - 1) % tx::kItemKindCount];
    if (k == tx::ItemKind::TurnOpen) ++turn;
    items.push_back(txgen::typicalItem(k, q, turn, r));
  }
  return items;
}

size_t recordsOf(const std::vector<tx::Item>& items) {
  size_t n = 0;
  for (const tx::Item& it : items) n += tx::recordBytes(enc(it).size());
  return n;
}

// The arena holds exactly `items`, in order: header, stored line and decoded item each.
void expectHolds(const tx::TxArena& a, const std::vector<tx::Item>& items) {
  TEST_ASSERT_EQUAL_UINT32(items.size(), a.count());
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string want = enc(items[i]);
    const char* data = nullptr;
    size_t len = 0;
    TEST_ASSERT_TRUE(a.line(i, data, len).ok());
    TEST_ASSERT_TRUE_MESSAGE(std::string(data, len) == want, ("line " + std::to_string(i)).c_str());
    tx::ArenaEntry h;
    TEST_ASSERT_TRUE(a.header(i, h).ok());
    TEST_ASSERT_EQUAL_UINT64(items[i].q, h.q);
    TEST_ASSERT_EQUAL_UINT64(items[i].t, h.t);
    TEST_ASSERT_EQUAL_UINT32(items[i].turn, h.turn);
    TEST_ASSERT_TRUE(items[i].kind == h.kind);
    TEST_ASSERT_EQUAL_UINT32(want.size(), h.lineBytes);
    tx::Item back;
    TEST_ASSERT_TRUE(a.get(i, back).ok());
    TEST_ASSERT_TRUE(txgen::firstDifference(txgen::project(items[i]), back).empty());
    size_t at = 99;
    TEST_ASSERT_TRUE(a.find(items[i].q, at).ok());
    TEST_ASSERT_EQUAL_UINT32(i, at);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

// 1,000 items of every kind in the active chat's arena: each comes back byte for byte,
// in q order, and the block holds nothing but their records.
void test_a_thousand_items_in_one_arena() {
  Seam seam;
  tx::TxArena a;
  TEST_ASSERT_TRUE(a.init(tx::kActiveChatArenaBytes, seam.alloc(), seam.release()).ok());
  const std::vector<tx::Item> items = conversation(1000, 7);
  for (const tx::Item& it : items) TEST_ASSERT_TRUE(a.append(it).ok());
  expectHolds(a, items);
  TEST_ASSERT_EQUAL_UINT32(recordsOf(items), a.usedBytes());
  TEST_ASSERT_EQUAL_UINT32(a.capacity() - a.usedBytes(), a.freeBytes());
  size_t at = 0;
  TEST_ASSERT_TRUE(a.find(1001, at).err == tx::Err::NotFound);
  TEST_ASSERT_TRUE(a.find(0, at).err == tx::Err::NotFound);
  tx::Item none;
  TEST_ASSERT_TRUE(a.get(1000, none).err == tx::Err::NotFound);
  char msg[160];
  std::snprintf(msg, sizeof msg, "1,000 items: %u B of %u B (%.0f B a record on average)", (unsigned)a.usedBytes(),
                (unsigned)a.capacity(), a.usedBytes() / 1000.0);
  TEST_MESSAGE(msg);
}

// The heap rule: appending 1,000 items (encoded or as stored lines), finding, reading
// lines and headers, flagging and dropping take zero operator new; the block is the
// seam's one allocation, given back once.
void test_appends_take_no_heap_block() {
  Seam seam;
  const std::vector<tx::Item> items = conversation(1000, 11);
  std::vector<std::string> lines;
  for (const tx::Item& it : items) lines.push_back(enc(it));
  long during = -1;
  {
    tx::TxArena a, b;
    TEST_ASSERT_TRUE(a.init(tx::kActiveChatArenaBytes, seam.alloc(), seam.release()).ok());
    TEST_ASSERT_TRUE(b.init(tx::kActiveChatArenaBytes, seam.alloc(), seam.release()).ok());
    const long before = g_news.load();
    bool ok = true;
    for (const tx::Item& it : items) ok = ok && a.append(it).ok();
    for (const std::string& l : lines) ok = ok && b.appendLine(l.data(), l.size()).ok();
    for (size_t i = 0; i < items.size(); ++i) {
      size_t at = 0, len = 0;
      const char* data = nullptr;
      tx::ArenaEntry h;
      ok = ok && a.find(items[i].q, at).ok() && a.line(at, data, len).ok() && a.header(at, h).ok();
      ok = ok && a.setFlags(at, tx::kItemNotStored).ok();
    }
    ok = ok && a.dropThrough(500).ok() && b.dropThrough(999).ok();
    a.clear();
    during = g_news.load() - before;
    TEST_ASSERT_TRUE(ok);
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)during, "operator new during appends and reads");
  TEST_ASSERT_EQUAL_INT(2, seam.allocs);
  TEST_ASSERT_EQUAL_UINT32(2 * tx::kActiveChatArenaBytes, seam.bytes);
  TEST_ASSERT_EQUAL_INT(2, seam.frees);
  char msg[200];
  std::snprintf(msg, sizeof msg,
                "heap: operator new during 2,000 appends + 1,000 finds/lines/headers/flags + 2 drops = %ld; "
                "seam allocations = %d (%u B each), frees = %d",
                during, seam.allocs, (unsigned)tx::kActiveChatArenaBytes, seam.frees);
  TEST_MESSAGE(msg);
}

// The restart fill: stored lines appended as lines give the same block as the items.
void test_a_filled_arena_equals_the_appended_one() {
  const std::vector<tx::Item> items = conversation(300, 13);
  tx::TxArena a, b;
  TEST_ASSERT_TRUE(a.init(200 * 1024, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  TEST_ASSERT_TRUE(b.init(200 * 1024, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  for (size_t i = 0; i < items.size(); ++i) {
    TEST_ASSERT_TRUE(a.append(items[i]).ok());
    const std::string l = enc(items[i]);
    const size_t len = i % 2 ? l.size() : l.size() - 1;   // with and without its newline
    TEST_ASSERT_TRUE(b.appendLine(l.data(), len).ok());
  }
  expectHolds(b, items);
  TEST_ASSERT_EQUAL_UINT32(a.usedBytes(), b.usedBytes());
}

// Full: at every size the refused item leaves the arena exactly as it was, and the
// refusal says which ceiling: the arena, or the line itself.
void test_a_full_arena_refuses_and_changes_nothing() {
  Rng r(3);
  for (size_t cap = 64; cap < 4096; cap += 37) {
    tx::TxArena a;
    TEST_ASSERT_TRUE(a.init(cap, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
    std::vector<tx::Item> in;
    for (uint64_t q = 1;; ++q) {
      const tx::Item it = txgen::typicalItem(tx::kAllItemKinds[q % tx::kItemKindCount], q, 1, r);
      const size_t used = a.usedBytes(), count = a.count();
      const tx::Status st = a.append(it);
      if (st.ok()) {
        in.push_back(it);
        continue;
      }
      TEST_ASSERT_TRUE(st.err == tx::Err::TooLarge);
      TEST_ASSERT_EQUAL_STRING("tx::TxArena::append", st.where);
      TEST_ASSERT_TRUE(tx::recordBytes(enc(it).size()) > a.freeBytes());
      TEST_ASSERT_EQUAL_UINT32(used, a.usedBytes());
      TEST_ASSERT_EQUAL_UINT32(count, a.count());
      const std::string l = enc(it);
      TEST_ASSERT_TRUE(a.appendLine(l.data(), l.size()).err == tx::Err::TooLarge);
      break;
    }
    expectHolds(a, in);
  }
  tx::TxArena big;
  // A line of exactly 60,000 B is an item like any other.
  {
    tx::TxArena edge;
    TEST_ASSERT_TRUE(edge.init(tx::kActiveChatArenaBytes, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
    tx::Item max;
    max.kind = tx::ItemKind::Assistant;
    max.q = 1;
    max.x = "a";
    max.x.assign(tx::kMaxLineBytes - (enc(max).size() - 1), 'a');
    TEST_ASSERT_EQUAL_UINT32(tx::kMaxLineBytes, enc(max).size());
    TEST_ASSERT_TRUE(edge.append(max).ok());
    expectHolds(edge, {max});
  }
  TEST_ASSERT_TRUE(big.init(tx::kActiveChatArenaBytes, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  tx::Item huge;
  huge.kind = tx::ItemKind::Assistant;
  huge.q = 1;
  huge.x.assign(tx::kMaxLineBytes, 'x');
  const tx::Status st = big.append(huge);
  TEST_ASSERT_TRUE(st.err == tx::Err::TooLarge);
  TEST_ASSERT_EQUAL_STRING("tx::encodeLine", st.where);
  TEST_ASSERT_EQUAL_UINT32(0, big.count());
  TEST_ASSERT_EQUAL_UINT32(0, big.usedBytes());
}

// Order comes from q: an item not above every q appended since init is refused, also
// after the items above it were dropped; clear starts a new sequence.
void test_q_only_grows() {
  tx::TxArena a;
  TEST_ASSERT_TRUE(a.init(8192, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  Rng r(1);
  TEST_ASSERT_TRUE(a.append(txgen::typicalItem(tx::ItemKind::User, 10, 1, r)).ok());
  TEST_ASSERT_TRUE(a.append(txgen::typicalItem(tx::ItemKind::User, 10, 1, r)).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(a.append(txgen::typicalItem(tx::ItemKind::User, 9, 1, r)).err == tx::Err::InvalidArgument);
  const std::string older = enc(txgen::typicalItem(tx::ItemKind::User, 5, 1, r));
  TEST_ASSERT_TRUE(a.appendLine(older.data(), older.size()).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(a.dropThrough(10).ok());
  TEST_ASSERT_EQUAL_UINT32(0, a.count());
  TEST_ASSERT_EQUAL_UINT64(10, a.lastQ());
  TEST_ASSERT_TRUE(a.append(txgen::typicalItem(tx::ItemKind::User, 7, 1, r)).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(a.append(txgen::typicalItem(tx::ItemKind::User, 11, 1, r)).ok());
  a.clear();
  TEST_ASSERT_TRUE(a.append(txgen::typicalItem(tx::ItemKind::User, 1, 1, r)).ok());
}

// dropThrough at every point: the rest is intact, findable and appendable, and the bytes
// of the dropped records come back.
void test_drop_through_every_point() {
  const std::vector<tx::Item> items = conversation(60, 19);
  for (uint64_t through = 0; through <= 61; ++through) {
    tx::TxArena a;
    TEST_ASSERT_TRUE(a.init(64 * 1024, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
    for (const tx::Item& it : items) TEST_ASSERT_TRUE(a.append(it).ok());
    TEST_ASSERT_TRUE(a.setFlags(a.count() - 1, tx::kItemNotStored).ok());
    TEST_ASSERT_TRUE(a.dropThrough(through).ok());
    const std::vector<tx::Item> rest(items.begin() + (through < items.size() ? through : items.size()), items.end());
    expectHolds(a, rest);
    TEST_ASSERT_EQUAL_UINT32(recordsOf(rest), a.usedBytes());
    if (!rest.empty()) {
      tx::ArenaEntry h;
      TEST_ASSERT_TRUE(a.header(a.count() - 1, h).ok());
      TEST_ASSERT_EQUAL_UINT8(tx::kItemNotStored, h.flags);   // a flag survives the move
    }
    Rng r(through);
    const tx::Item next = txgen::typicalItem(tx::ItemKind::User, 100, 9, r);
    TEST_ASSERT_TRUE(a.append(next).ok());
    std::vector<tx::Item> all = rest;
    all.push_back(next);
    expectHolds(a, all);
  }
}

// Flags live beside the line, never in it.
void test_flags_do_not_touch_the_line() {
  tx::TxArena a;
  TEST_ASSERT_TRUE(a.init(8192, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  const std::vector<tx::Item> items = conversation(12, 2);
  for (const tx::Item& it : items) TEST_ASSERT_TRUE(a.append(it).ok());
  for (size_t i = 0; i < items.size(); i += 2) TEST_ASSERT_TRUE(a.setFlags(i, tx::kItemNotStored).ok());
  expectHolds(a, items);
  for (size_t i = 0; i < items.size(); ++i) {
    tx::ArenaEntry h;
    TEST_ASSERT_TRUE(a.header(i, h).ok());
    TEST_ASSERT_EQUAL_UINT8(i % 2 ? 0 : tx::kItemNotStored, h.flags);
  }
  TEST_ASSERT_TRUE(a.setFlags(items.size(), 1).err == tx::Err::NotFound);
}

// A torn or unreadable line is refused and changes nothing.
void test_append_line_refuses_damage() {
  tx::TxArena a;
  TEST_ASSERT_TRUE(a.init(8192, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
  Rng r(4);
  const std::string l = enc(txgen::typicalItem(tx::ItemKind::Assistant, 3, 1, r));
  for (size_t cut = 0; cut + 1 < l.size(); ++cut) TEST_ASSERT_TRUE(a.appendLine(l.data(), cut).err == tx::Err::Torn);
  std::string flipped = l;
  flipped[10] ^= 4;
  TEST_ASSERT_TRUE(a.appendLine(flipped.data(), flipped.size()).err == tx::Err::Torn);
  TEST_ASSERT_TRUE(a.appendLine(nullptr, 0).err == tx::Err::InvalidArgument);
  TEST_ASSERT_EQUAL_UINT32(0, a.count());
  TEST_ASSERT_EQUAL_UINT32(0, a.usedBytes());
}

// The init contract and a fresh arena.
void test_init_contract() {
  Seam seam;
  tx::TxArena a;
  tx::Item it;
  it.q = 1;
  TEST_ASSERT_TRUE(a.append(it).err == tx::Err::InvalidArgument);   // before init
  TEST_ASSERT_TRUE(a.dropThrough(1).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(a.init(64, nullptr, nullptr).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(a.init(63, seam.alloc(), seam.release()).err == tx::Err::InvalidArgument);
  seam.refuse = true;
  TEST_ASSERT_TRUE(a.init(4096, seam.alloc(), seam.release()).err == tx::Err::Io);
  seam.refuse = false;
  TEST_ASSERT_TRUE(a.init(4096, seam.alloc(), seam.release()).ok());
  TEST_ASSERT_TRUE(a.init(8192, seam.alloc(), seam.release()).ok());   // gives the first block back
  TEST_ASSERT_EQUAL_INT(2, seam.allocs);
  TEST_ASSERT_EQUAL_INT(1, seam.frees);
  TEST_ASSERT_EQUAL_UINT32(8192, a.capacity());
  TEST_ASSERT_EQUAL_UINT32(0, a.count());
  TEST_ASSERT_EQUAL_UINT32(8192, a.freeBytes());
  tx::Item none;
  TEST_ASSERT_TRUE(a.get(0, none).err == tx::Err::NotFound);
  TEST_ASSERT_TRUE(a.append(it).ok());
}

// Three resident chats, least recently used first out; a slot's block is allocated once
// and reused, so chats coming and going never touch the allocator again.
void test_pool_keeps_three_chats_least_recently_used_first_out() {
  Seam seam;
  tx::TxArenaPool pool;
  tx::TxArena* a = nullptr;
  bool fresh = false;
  TEST_ASSERT_TRUE(pool.acquire("1001", a, fresh).err == tx::Err::InvalidArgument);   // before init
  TEST_ASSERT_TRUE(pool.init(4096, seam.alloc(), seam.release()).ok());
  tx::TxArena* chats[4] = {};
  const char* keys[4] = {"1001", "1002", "routine:lesson", "1004"};
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(pool.acquire(keys[i], chats[i], fresh).ok());
    TEST_ASSERT_TRUE(fresh);
    tx::Item it;
    it.q = 1;
    TEST_ASSERT_TRUE(chats[i]->append(it).ok());
  }
  TEST_ASSERT_TRUE(pool.acquire("1001", a, fresh).ok());   // 1001 is now the newest use
  TEST_ASSERT_FALSE(fresh);
  TEST_ASSERT_TRUE(a == chats[0]);
  TEST_ASSERT_EQUAL_UINT32(1, a->count());
  TEST_ASSERT_TRUE(pool.acquire(keys[3], chats[3], fresh).ok());   // evicts 1002
  TEST_ASSERT_TRUE(fresh);
  TEST_ASSERT_TRUE(chats[3] == chats[1]);
  TEST_ASSERT_EQUAL_UINT32(0, chats[3]->count());
  TEST_ASSERT_FALSE(pool.resident("1002"));
  TEST_ASSERT_TRUE(pool.resident("1001") && pool.resident("routine:lesson") && pool.resident("1004"));
  for (int round = 0; round < 10; ++round)
    for (const char* k : keys) TEST_ASSERT_TRUE(pool.acquire(k, a, fresh).ok());
  TEST_ASSERT_EQUAL_INT(3, seam.allocs);
  TEST_ASSERT_TRUE(pool.forget("1004").ok());
  TEST_ASSERT_TRUE(pool.forget("1004").err == tx::Err::NotFound);
  TEST_ASSERT_TRUE(pool.acquire("", a, fresh).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(pool.acquire(nullptr, a, fresh).err == tx::Err::InvalidArgument);
  const std::string longKey(tx::TxArenaPool::kChatKeyMax + 1, 'k');
  TEST_ASSERT_TRUE(pool.acquire(longKey.c_str(), a, fresh).err == tx::Err::InvalidArgument);
  const std::string maxKey(tx::TxArenaPool::kChatKeyMax, 'k');
  TEST_ASSERT_TRUE(pool.acquire(maxKey.c_str(), a, fresh).ok());
  TEST_ASSERT_TRUE(pool.resident(maxKey.c_str()));
  // Re-init at a new size: every chat leaves, each slot takes a block of the new size.
  TEST_ASSERT_TRUE(pool.init(8192, seam.alloc(), seam.release()).ok());
  TEST_ASSERT_FALSE(pool.resident(maxKey.c_str()));
  TEST_ASSERT_TRUE(pool.acquire("1001", a, fresh).ok());
  TEST_ASSERT_TRUE(fresh);
  TEST_ASSERT_EQUAL_UINT32(8192, a->capacity());
  TEST_ASSERT_EQUAL_INT(4, seam.allocs);
  TEST_ASSERT_EQUAL_INT(1, seam.frees);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_thousand_items_in_one_arena);
  RUN_TEST(test_appends_take_no_heap_block);
  RUN_TEST(test_a_filled_arena_equals_the_appended_one);
  RUN_TEST(test_a_full_arena_refuses_and_changes_nothing);
  RUN_TEST(test_q_only_grows);
  RUN_TEST(test_drop_through_every_point);
  RUN_TEST(test_flags_do_not_touch_the_line);
  RUN_TEST(test_append_line_refuses_damage);
  RUN_TEST(test_init_contract);
  RUN_TEST(test_pool_keeps_three_chats_least_recently_used_first_out);
  return UNITY_END();
}
