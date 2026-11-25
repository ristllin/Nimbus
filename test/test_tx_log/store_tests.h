#pragma once
// test_tx_log, lane RB-T2's own cases (added beside the battery's; main.cpp runs them):
// the store interface and its not-stored state, the restart fill (same bytes, bounded
// read), head.json as a cache (caught up, torn, from another state), a cut at every
// append, and the paths property over every byte.
#include <unity.h>

#include <algorithm>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/tx_battery/items.h"
#include "../support/tx_battery/mem_files.h"
#include "../support/tx_battery/pending.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"

namespace store_tests {

namespace tx = nimbus::orch::tx;

const char* kKey = "1001";

// MemFiles that counts what readers read.
struct CountingFiles : txb::MemFiles {
  size_t reads = 0;
  size_t readBytes = 0;
  std::vector<size_t> offsets;
  size_t maxSegRead = 0;
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override {
    const bool ok = txb::MemFiles::readAt(path, offset, buf, want, got);
    ++reads;
    readBytes += got;
    if (path.find("/seg-") != std::string::npos) {
      offsets.push_back(offset);
      maxSegRead = std::max(maxSegRead, got);
    }
    return ok;
  }
};

// The line crc (IEEE crc32) and tail, to forge lines and head.json files a writer bug or
// another state of the log would leave.
inline uint32_t crc32(const std::string& s) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : s) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}
inline std::string seal(const std::string& body) {
  char hex[9];
  std::snprintf(hex, sizeof hex, "%08x", crc32(body));
  return body + ",\"z\":\"" + hex + "\"}\n";
}
// The body of a sealed line or head.json (everything before its 17-byte tail).
inline std::string unseal(const std::string& line) { return line.substr(0, line.size() - 17); }

void appendItems(tx::TxLog& log, const std::vector<tx::Item>& items) {
  for (const tx::Item& it : items) {
    tx::StoreState st;
    TXB_OK(log.append(it, st));
  }
}

void* alloc(size_t n) { return std::malloc(n); }
void release(void* p) { std::free(p); }

void initArena(tx::TxArena& a, size_t cap = tx::kActiveChatArenaBytes) { TXB_OK(a.init(cap, alloc, release)); }

// Several days of ordinary turns with tools, a compaction (s item) and pointer moves.
txb::Convo days(int turns, uint64_t& thru) {
  txb::Convo c;
  thru = 0;
  for (int i = 0; i < turns; ++i) {
    c.openTurn();
    if (i % 5 == 0) c.move(c.q, "cold");
    c.owner("question " + std::to_string(i) + " " + std::string(200, 'q'));
    const std::string id = c.call("read_file", R"({"path":"notes/a.md"})");
    c.receipt(id, "read_file", txb::payload("R" + std::to_string(i), 900));
    c.say("answer " + std::to_string(i) + " " + std::string(300, 'a'));
    c.close();
    c.advance(600);
    if (i == turns / 2) {
      thru = c.q;
      tx::Item& s = c.add(tx::ItemKind::Digest);
      s.day = "2026-11-17";
      s.x = "Tue 17 Nov: the first half.";
      s.thru = thru;
    }
  }
  return c;
}

std::vector<std::string> arenaLines(const tx::TxArena& a) {
  std::vector<std::string> out;
  for (size_t i = 0; i < a.count(); ++i) {
    const char* d = nullptr;
    size_t n = 0;
    TXB_OK(a.line(i, d, n));
    out.emplace_back(d, n);
  }
  return out;
}

void sameHead(const tx::HeadState& a, const tx::HeadState& b) {
  TEST_ASSERT_EQUAL_UINT64(a.nextQ, b.nextQ);
  TEST_ASSERT_EQUAL_UINT64(a.thru, b.thru);
  TEST_ASSERT_EQUAL_UINT64(a.stub, b.stub);
  TEST_ASSERT_EQUAL(a.seg, b.seg);
  TEST_ASSERT_EQUAL_UINT64(a.end, b.end);
  TEST_ASSERT_EQUAL(a.thruSeg, b.thruSeg);
  TEST_ASSERT_EQUAL_UINT64(a.thruOffset, b.thruOffset);
  TEST_ASSERT_EQUAL_UINT64(a.hw, b.hw);
  TEST_ASSERT_EQUAL_UINT64(a.mHi, b.mHi);
  TEST_ASSERT_EQUAL_STRING(a.profile.c_str(), b.profile.c_str());
  TEST_ASSERT_EQUAL(a.openTurn, b.openTurn);
  TEST_ASSERT_EQUAL(a.attempts, b.attempts);
}

// A restart renders the same bytes: what the arena held after thru before the restart is
// what the fill puts back, line for line.
void test_restart_fill_gives_the_same_arena_bytes() {
  txb::MemFiles files;
  uint64_t thru = 0;
  const txb::Convo c = days(40, thru);
  tx::TxArena live;
  initArena(live);
  {
    tx::TxLog log(files, kKey);
    tx::HeadState head;
    TXB_OK(log.open(head));
    for (const tx::Item& it : c.items) {
      tx::StoreState st;
      TXB_OK(log.store(it, live, st));
      TEST_ASSERT_TRUE(st == tx::StoreState::Stored);
      if (it.q == c.q / 3) {   // a turn close mid-way: head.json written, then stale
        tx::HeadState h;
        TXB_OK(log.open(h));
        TXB_OK(log.writeHead(h));
      }
    }
  }
  TXB_OK(live.dropThrough(thru));
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  TEST_ASSERT_EQUAL_UINT64(thru, head.thru);
  tx::TxArena filled;
  initArena(filled);
  tx::FillReport rep;
  TXB_OK(log.fill(head, filled, rep));
  const std::vector<std::string> a = arenaLines(live), b = arenaLines(filled);
  TEST_ASSERT_EQUAL((int)a.size(), (int)b.size());
  for (size_t i = 0; i < a.size(); ++i) TEST_ASSERT_EQUAL_STRING(a[i].c_str(), b[i].c_str());
  TEST_ASSERT_EQUAL(0, (int)rep.torn);
  TEST_ASSERT_EQUAL(0, (int)rep.skipped);   // the fill starts just past thru's line
  TEST_ASSERT_EQUAL(0, (int)rep.dropped);
}

// The fill is one sequential read from thru's offset: no byte before it, slices of at most
// 8 KB, every offset after the one before.
void test_restart_fill_is_one_bounded_sequential_read() {
  CountingFiles files;
  uint64_t thru = 0;
  const txb::Convo c = days(120, thru);
  tx::HeadState head;
  {
    tx::TxLog log(files, kKey);
    TXB_OK(log.open(head));
    for (const tx::Item& it : c.items) {
      tx::StoreState st;
      TXB_OK(log.append(it, st));
    }
    TXB_OK(log.open(head));
    TXB_OK(log.writeHead(head));
  }
  tx::TxLog log(files, kKey);
  TXB_OK(log.open(head));
  files.offsets.clear();
  files.reads = files.readBytes = 0;
  tx::TxArena arena;
  initArena(arena);
  tx::FillReport rep;
  TXB_OK(log.fill(head, arena, rep));
  std::string seg;
  TXB_OK(tx::segmentPath(kKey, head.seg, seg));
  const size_t tail = files.files[seg].size() - head.thruOffset;
  TEST_ASSERT_TRUE(head.thruOffset > 0);
  TEST_ASSERT_EQUAL((int)tail, (int)rep.bytes);
  TEST_ASSERT_EQUAL((int)tail, (int)files.readBytes);
  TEST_ASSERT_TRUE_MESSAGE(rep.reads <= (tail + tx::kFillSliceBytes - 1) / tx::kFillSliceBytes + 1, "more reads than slices");
  TEST_ASSERT_EQUAL_UINT64(head.thruOffset, files.offsets.front());
  for (size_t i = 1; i < files.offsets.size(); ++i) TEST_ASSERT_TRUE(files.offsets[i] > files.offsets[i - 1]);
  size_t after = 0;
  for (const tx::Item& it : c.items) after += it.q > thru;
  TEST_ASSERT_EQUAL((int)after, (int)arena.count());
}

// The persist point: a refused write keeps the item in RAM flagged not stored, the status
// is ok and the transcript goes on; after a restart only stored items come back.
// Lane E2-BFIX1: the refused item is held (flagged) and written before the next one, in q order.
void test_store_keeps_a_refused_item_in_ram_as_not_stored() {
  txb::MemFiles files;
  txb::Convo c;
  for (int i = 0; i < 4; ++i) c.ownerTurn("q" + std::to_string(i), "a" + std::to_string(i));
  tx::TxArena arena;
  initArena(arena);
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  const size_t refused = 5;
  files.refuseAppend = (long)refused;
  for (size_t i = 0; i < c.items.size(); ++i) {
    tx::StoreState st = tx::StoreState::Stored;
    TXB_OK(log.store(c.items[i], arena, st));
    TEST_ASSERT_TRUE((i == refused) == (st == tx::StoreState::NotStored));
    tx::ArenaEntry e;
    TXB_OK(arena.header(i, e));
    TEST_ASSERT_EQUAL((i == refused) ? tx::kItemNotStored : 0, e.flags & tx::kItemNotStored);
  }
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)arena.count());
  tx::ArenaEntry was;
  TXB_OK(arena.header(refused, was));
  TEST_ASSERT_EQUAL(0, was.flags & tx::kItemNotStored);   // written with the next item
  TEST_ASSERT_EQUAL(0u, log.heldCount());
  TEST_ASSERT_EQUAL(0u, log.lostCount());
  files.clearFaults();
  tx::TxLog again(files, kKey);
  TXB_OK(again.open(head));
  tx::TxArena filled;
  initArena(filled);
  TXB_OK(again.fill(head, filled));
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)filled.count());
  size_t idx = 0;
  TXB_OK(filled.find(c.items[refused].q, idx));
  // An arena refusal writes nothing anywhere.
  tx::Item back = c.items.front();
  tx::StoreState st;
  const size_t before = files.appends;
  TEST_ASSERT_TRUE(log.store(back, arena, st).err == tx::Err::InvalidArgument);
  TEST_ASSERT_EQUAL((int)before, (int)files.appends);
}

// No card: open answers Io (never NotImplemented), every append is NotStored.
void test_no_storage_opens_closed_and_never_stores() {
  txb::NullFiles none;
  tx::TxLog log(none, kKey);
  tx::HeadState head;
  TEST_ASSERT_TRUE(log.open(head).err == tx::Err::Io);
  TEST_ASSERT_FALSE(log.isOpen());
  tx::Item it;
  it.q = 1;
  it.x = "hello";
  tx::StoreState st = tx::StoreState::Stored;
  TEST_ASSERT_TRUE(log.append(it, st).err == tx::Err::NotStored);
  TEST_ASSERT_TRUE(st == tx::StoreState::NotStored);
  TEST_ASSERT_TRUE(log.writeHead(head).err == tx::Err::InvalidArgument);
}

// head.json is caught up on every kind of later item, and agrees with a rebuild.
void test_stale_head_is_caught_up_to_the_rebuild() {
  txb::MemFiles files;
  uint64_t thru = 0;
  txb::Convo c = days(10, thru);
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  for (const tx::Item& it : c.items) {
    tx::StoreState st;
    TXB_OK(log.append(it, st));
  }
  TXB_OK(log.open(head));
  TXB_OK(log.writeHead(head));
  // Later: a new compaction, a move, an open turn with a retry.
  tx::Item& s = c.add(tx::ItemKind::Digest);
  s.x = "more";
  s.thru = c.q - 3;
  const uint64_t newThru = s.thru;
  c.move(c.q - 1, "warm");
  c.openTurn();
  c.items.back().attempt = 2;
  c.owner("late");
  for (size_t i = c.items.size() - 4; i < c.items.size(); ++i) {
    tx::StoreState st;
    TXB_OK(log.append(c.items[i], st));
  }
  tx::TxLog reader(files, kKey);
  tx::HeadState caught, rebuilt;
  TXB_OK(reader.open(caught));
  TXB_OK(reader.rebuildHead(rebuilt));
  sameHead(rebuilt, caught);
  TEST_ASSERT_EQUAL(c.turn, caught.openTurn);
  TEST_ASSERT_EQUAL(2, caught.attempts);
  TEST_ASSERT_EQUAL_UINT64(newThru, caught.thru);
}

// A head.json that is torn, flipped, or from another state of the log is not trusted.
void test_untrusted_head_is_rebuilt() {
  txb::MemFiles files;
  uint64_t thru = 0;
  const txb::Convo c = days(12, thru);
  tx::HeadState good;
  {
    tx::TxLog log(files, kKey);
    TXB_OK(log.open(good));
    for (const tx::Item& it : c.items) {
      tx::StoreState st;
      TXB_OK(log.append(it, st));
    }
    TXB_OK(log.open(good));
    TXB_OK(log.writeHead(good));
  }
  std::string hp;
  TXB_OK(tx::headPath(kKey, hp));
  const std::string saved = files.files[hp];
  const auto check = [&](const std::string& headText, const char* what) {
    files.files[hp] = headText;
    tx::TxLog log(files, kKey);
    tx::HeadState h;
    TXB_OK(log.open(h));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(good.nextQ, h.nextQ, what);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(good.thruOffset, h.thruOffset, what);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(thru, h.thru, what);
  };
  check(saved.substr(0, saved.size() / 2), "torn");
  for (size_t i = 0; i + 1 < saved.size(); i += 7) {
    std::string flipped = saved;
    flipped[i] ^= 0x04;
    check(flipped, "flipped byte");
  }
  // A crc-valid head.json that claims more than the segment holds (another state of the log).
  files.files[hp] = saved;
  std::string seg;
  TXB_OK(tx::segmentPath(kKey, good.seg, seg));
  const std::string full = files.files[seg];
  files.files[seg] = full.substr(0, full.size() - 1);   // the last line lost its end
  {
    tx::TxLog log(files, kKey);
    tx::HeadState h;
    TXB_OK(log.open(h));
    TEST_ASSERT_EQUAL_UINT64(good.nextQ - 1, h.nextQ);   // the torn last item is gone
  }
  files.files[seg] = full;
  check(saved, "intact");
}

// A cut at every append, clean or torn: the reopened log keeps every item before the cut,
// loses only the one cut, and the next append is a whole line of its own.
void test_cut_at_every_append_loses_only_that_item() {
  uint64_t thru = 0;
  const txb::Convo c = days(6, thru);
  for (int torn = 0; torn < 2; ++torn) {
    for (size_t k = 0; k < c.items.size(); ++k) {
      txb::MemFiles files;
      {
        tx::TxLog log(files, kKey);
        tx::HeadState head;
        TXB_OK(log.open(head));
        files.killAtAppend = (long)(files.appends + k);
        files.tornKill = torn;
        bool cut = false;
        try {
          for (const tx::Item& it : c.items) {
            tx::StoreState st;
            TXB_OK(log.append(it, st));
          }
        } catch (const txb::Killed&) {
          cut = true;
        }
        TEST_ASSERT_TRUE(cut);
      }
      files.clearFaults();
      tx::TxLog log(files, kKey);
      tx::HeadState head;
      TXB_OK(log.open(head));
      TEST_ASSERT_EQUAL_UINT64(c.items[k].q, head.nextQ);
      for (size_t i = k; i < c.items.size(); ++i) {   // the replay after the boot
        tx::StoreState st;
        TXB_OK(log.append(c.items[i], st));
      }
      tx::TxArena arena;
      initArena(arena);
      tx::HeadState from0 = head;
      from0.thru = 0;
      from0.thruSeg = 1;
      from0.thruOffset = 0;
      tx::FillReport rep;
      TXB_OK(log.fill(from0, arena, rep));
      TEST_ASSERT_EQUAL((int)c.items.size(), (int)arena.count());
      TEST_ASSERT_EQUAL(torn, (int)rep.torn);
      for (size_t i = 0; i < c.items.size(); ++i) {
        tx::ArenaEntry e;
        TXB_OK(arena.header(i, e));
        TEST_ASSERT_EQUAL_UINT64(c.items[i].q, e.q);
      }
    }
  }
}

// A compaction moves thru: writeHead locates the line, and the fill loads exactly the rest.
void test_write_head_locates_a_new_thru() {
  txb::MemFiles files;
  uint64_t unused = 0;
  const txb::Convo c = days(20, unused);
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  for (const tx::Item& it : c.items) {
    tx::StoreState st;
    TXB_OK(log.append(it, st));
  }
  TXB_OK(log.open(head));
  const uint64_t newThru = c.items[c.items.size() * 2 / 3].q;
  head.thru = newThru;
  TXB_OK(log.writeHead(head));
  tx::TxLog reader(files, kKey);
  TXB_OK(reader.open(head));
  TEST_ASSERT_EQUAL_UINT64(newThru, head.thru);
  tx::TxArena arena;
  initArena(arena);
  tx::FillReport rep;
  TXB_OK(reader.fill(head, arena, rep));
  TEST_ASSERT_EQUAL(0, (int)rep.skipped);
  tx::ArenaEntry e;
  TXB_OK(arena.header(0, e));
  TEST_ASSERT_EQUAL_UINT64(newThru + 1, e.q);
  // thru and stub never move back.
  head.thru = 1;
  head.stub = 0;
  TXB_OK(reader.writeHead(head));
  tx::TxLog third(files, kKey);
  tx::HeadState h3;
  TXB_OK(third.open(h3));
  TEST_ASSERT_EQUAL_UINT64(newThru, h3.thru);
}

// A full arena gives up its oldest whole turns, never the newest items, and says so.
void test_fill_into_a_small_arena_keeps_the_newest_turns() {
  txb::MemFiles files;
  uint64_t unused = 0;
  const txb::Convo c = days(30, unused);
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  for (const tx::Item& it : c.items) {
    tx::StoreState st;
    TXB_OK(log.append(it, st));
  }
  tx::HeadState from0;
  tx::TxArena arena;
  initArena(arena, 16 * 1024);
  tx::FillReport rep;
  TXB_OK(log.fill(from0, arena, rep));
  TEST_ASSERT_TRUE(rep.dropped > 0);
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)(rep.dropped + arena.count() + rep.skipped));
  tx::ArenaEntry first, last;
  TXB_OK(arena.header(0, first));
  TXB_OK(arena.header(arena.count() - 1, last));
  TEST_ASSERT_EQUAL_UINT64(c.q, last.q);
  TEST_ASSERT_EQUAL_UINT64(rep.droppedThru + 1, first.q);
  TEST_ASSERT_TRUE_MESSAGE(c.items[first.q - 1].kind == tx::ItemKind::TurnOpen, "the arena starts on a turn");
}

// Garbage longer than any line (no end) is dropped and the lines around it are kept.
void test_garbage_without_a_line_end_is_dropped() {
  txb::MemFiles files;
  txb::Convo c;
  for (int i = 0; i < 3; ++i) c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  for (size_t i = 0; i < c.items.size(); ++i) {
    tx::StoreState st;
    TXB_OK(log.append(c.items[i], st));
    if (i == 3) {
      std::string seg;
      TXB_OK(tx::segmentPath(kKey, 1, seg));
      files.files[seg] += std::string(tx::kMaxLineBytes + 5000, 'g') + "\n";
    }
  }
  tx::TxArena arena;
  initArena(arena);
  tx::FillReport rep;
  TXB_OK(log.fill(tx::HeadState{}, arena, rep));
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)arena.count());
  TEST_ASSERT_EQUAL(1, (int)rep.torn);
}

// Every chat key, of any bytes: either refused, or every path is under /mem/tx/<key>/ with
// no folder step of its own.
void test_paths_property_over_every_byte() {
  std::mt19937 rng(17);
  for (int n = 0; n < 4000; ++n) {
    std::string key;
    const int len = (int)(rng() % 52);
    for (int i = 0; i < len; ++i) key += (char)(n < 256 && i == 0 ? n : (int)(rng() % 256));
    std::string dir, p;
    const bool ok = tx::txDir(key, dir).ok();
    TEST_ASSERT_EQUAL(tx::chatKeyOk(key), ok);
    if (!ok) {
      TEST_ASSERT_TRUE(tx::segmentPath(key, 1, p).err == tx::Err::InvalidArgument && p.empty());
      TEST_ASSERT_TRUE(tx::bulkPath(key, 1, p).err == tx::Err::InvalidArgument && p.empty());
      continue;
    }
    TEST_ASSERT_TRUE(key.find('.') == std::string::npos && key.find('/') == std::string::npos);
    TEST_ASSERT_EQUAL_STRING(("/mem/tx/" + key).c_str(), dir.c_str());
    TXB_OK(tx::bulkPath(key, 4294967295u, p));
    TEST_ASSERT_EQUAL_STRING((dir + "/b/4294967295.txb").c_str(), p.c_str());
  }
  std::string p;
  TEST_ASSERT_TRUE(tx::segmentPath(kKey, 0, p).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::segmentPath(kKey, tx::kMaxSegment + 1, p).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::bulkPath(kKey, 0, p).err == tx::Err::InvalidArgument);
  for (const char* m : {"2026-13", "2026-00", "2026/11", "../2026", "2026-1", "2026-111", "abcd-11"})
    TEST_ASSERT_TRUE_MESSAGE(tx::digestArchivePath(kKey, m, p).err == tx::Err::InvalidArgument, m);
  TEST_ASSERT_EQUAL(47, (int)tx::kChatKeyMaxBytes);
  TEST_ASSERT_TRUE(tx::chatKeyOk("-1001234567890"));
  TEST_ASSERT_FALSE(tx::chatKeyOk(std::string(48, '1')));
}

// A bulk handle names a turn and offsets only: anything else is refused, never a path.
void test_bulk_handles_cannot_name_a_path() {
  txb::MemFiles files;
  tx::TxLog log(files, kKey);
  std::string h, out;
  TXB_OK(log.appendBulk(7, "abcdef", h));
  TEST_ASSERT_EQUAL_STRING("b/7.txb@0+6", h.c_str());
  TXB_OK(log.appendBulk(7, "ghi", h));
  TEST_ASSERT_EQUAL_STRING("b/7.txb@6+3", h.c_str());
  const char* bad[] = {"",          "b/../x.txb@0+1", "b/7.txb@0+0", "b/0.txb@0+1", "b/7.txb@x+1",  "/mem/tx/2/b/7.txb@0+1",
                       "b/7.txt@0+1", "b/7.txb@0",    "b/7.txb+1@0", "b/-7.txb@0+1", "b/7.txb@0+1 ", "b/99999999999.txb@0+1"};
  for (const char* b : bad) TEST_ASSERT_TRUE_MESSAGE(log.readBulk(b, 0, 10, out).err == tx::Err::InvalidArgument, b);
  TXB_OK(log.readBulk("b/7.txb@6+3", 0, 100, out));
  TEST_ASSERT_EQUAL_STRING("ghi", out.c_str());
  TXB_OK(log.readBulk("b/7.txb@6+3", 3, 100, out));
  TEST_ASSERT_TRUE(out.empty());
  TEST_ASSERT_TRUE(log.readBulk("b/7.txb@6+30", 0, 10, out).err == tx::Err::NotFound);
  TEST_ASSERT_TRUE(log.readBulk("b/8.txb@0+3", 0, 10, out).err == tx::Err::NotFound);
  // Retention: older turns go, newer stay; a refused bulk append is NotStored.
  TXB_OK(log.appendBulk(9, "x", h));
  size_t removed = 0;
  TXB_OK(log.pruneBulk(9, removed));
  TEST_ASSERT_EQUAL(1, (int)removed);
  TEST_ASSERT_TRUE(files.files.count("/mem/tx/1001/b/9.txb") == 1);
  files.refuseAll = true;
  TEST_ASSERT_TRUE(log.appendBulk(9, "y", h).err == tx::Err::NotStored);
  TEST_ASSERT_TRUE(h.empty());
}

// A refusal that left half a line behind: the next append starts on a line of its own.
void test_partial_refusal_never_glues() {
  struct HalfRefuse : txb::MemFiles {
    long halfAt = -1;
    bool append(const std::string& path, const char* data, size_t len) override {
      if ((long)appends == halfAt) {
        ++appends;
        files[path].append(data, len / 2);
        return false;
      }
      return txb::MemFiles::append(path, data, len);
    }
  } files;
  txb::Convo c;
  for (int i = 0; i < 3; ++i) c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  files.halfAt = 4;
  size_t stored = 0;
  for (const tx::Item& it : c.items) {
    tx::StoreState st;
    const tx::Status s = log.append(it, st);
    TEST_ASSERT_TRUE(s.ok() || s.err == tx::Err::NotStored);
    stored += st == tx::StoreState::Stored;
  }
  TEST_ASSERT_EQUAL((int)c.items.size() - 1, (int)stored);
  tx::TxArena arena;
  initArena(arena);
  tx::FillReport rep;
  TXB_OK(log.fill(tx::HeadState{}, arena, rep));
  TEST_ASSERT_EQUAL((int)stored, (int)arena.count());
  TEST_ASSERT_EQUAL(1, (int)rep.torn);
}

// head.json keeps every field it holds, escapes included; over its limits it is refused.
void test_head_round_trips_its_fields() {
  txb::MemFiles files;
  txb::Convo c;
  c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  for (const tx::Item& it : c.items) {
    tx::StoreState st;
    TXB_OK(log.append(it, st));
  }
  head.profile = "wide \"quoted\"\\ \x01";
  head.pinned = {"rule:a", "note \"b\"", ""};
  head.lastReqAt = 1794920520ULL;
  head.hw = 9000000000ULL;
  head.mHi = 77;
  head.nextQ = c.q + 9;   // not-stored items consumed q's
  TXB_OK(log.writeHead(head));
  tx::TxLog reader(files, kKey);
  tx::HeadState got;
  TXB_OK(reader.open(got));
  TEST_ASSERT_EQUAL_STRING(head.profile.c_str(), got.profile.c_str());
  TEST_ASSERT_EQUAL(3, (int)got.pinned.size());
  TEST_ASSERT_EQUAL_STRING("note \"b\"", got.pinned[1].c_str());
  TEST_ASSERT_EQUAL_UINT64(head.lastReqAt, got.lastReqAt);
  TEST_ASSERT_EQUAL_UINT64(head.hw, got.hw);
  TEST_ASSERT_EQUAL_UINT64(77, got.mHi);
  TEST_ASSERT_EQUAL_UINT64(c.q + 9, got.nextQ);
  tx::HeadState big = got;
  big.pinned.assign(tx::kHeadMaxPinned + 1, "x");
  TEST_ASSERT_TRUE(reader.writeHead(big).err == tx::Err::InvalidArgument);
}

// A torn last append with nothing after it: the fill and open drop it.
void test_a_torn_tail_at_the_end_is_dropped() {
  txb::MemFiles files;
  txb::Convo c;
  for (int i = 0; i < 3; ++i) c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  std::string seg, line;
  TXB_OK(tx::segmentPath(kKey, 1, seg));
  tx::Item more = c.items.back();
  more.q = c.q + 1;
  TXB_OK(tx::encodeLine(more, line));
  files.files[seg] += line.substr(0, line.size() - 1);   // all but its line end
  tx::TxLog again(files, kKey);
  TXB_OK(again.open(head));
  TEST_ASSERT_EQUAL_UINT64(c.q + 1, head.nextQ);
  tx::TxArena arena;
  initArena(arena);
  tx::FillReport rep;
  TXB_OK(again.fill(tx::HeadState{}, arena, rep));
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)arena.count());
  TEST_ASSERT_EQUAL(1, (int)rep.torn);
}

// A crc-valid head.json whose next_q is behind the segment (another state of the log, a
// restored card) is not trusted: next_q comes from the segment, never from the cache.
void test_a_head_behind_the_segment_is_not_trusted() {
  txb::MemFiles files;
  txb::Convo c;
  for (int i = 0; i < 4; ++i) c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  TXB_OK(log.open(head));
  TXB_OK(log.writeHead(head));
  std::string hp;
  TXB_OK(tx::headPath(kKey, hp));
  std::string body = unseal(files.files[hp]);
  const std::string want = "\"next_q\":" + std::to_string(c.q + 1);
  TEST_ASSERT_TRUE(body.find(want) != std::string::npos);
  body.replace(body.find(want), want.size(), "\"next_q\":5");
  files.files[hp] = seal(body);
  tx::TxLog reader(files, kKey);
  tx::HeadState got;
  TXB_OK(reader.open(got));
  TEST_ASSERT_EQUAL_UINT64(c.q + 1, got.nextQ);
}

// A crc-valid line decodeLine cannot read (a writer bug) never reaches the arena.
void test_fill_drops_crc_valid_lines_it_cannot_read() {
  txb::MemFiles files;
  txb::Convo c;
  for (int i = 0; i < 2; ++i) c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  const std::string bad = seal("{\"k\":\"u\",\"q\":" + std::to_string(c.q + 1) + ",\"x\":5");
  tx::LineHeader h;
  TXB_OK(tx::checkLine(bad.data(), bad.size(), h));   // the forged line passes the crc
  tx::StoreState st;
  TXB_OK(log.appendLine(bad.data(), bad.size(), st));
  c.q += 1;
  // crc-valid with no readable header (k not first): appendLine refuses it, so it is
  // written raw, as a writer bug would.
  const std::string headless = seal("{\"q\":" + std::to_string(c.q + 1) + ",\"k\":\"u\"");
  TEST_ASSERT_TRUE(log.appendLine(headless.data(), headless.size(), st).err == tx::Err::InvalidArgument);
  std::string seg;
  TXB_OK(tx::segmentPath(kKey, 1, seg));
  files.files[seg] += headless;
  c.q += 1;
  const size_t before = c.items.size();
  for (int i = 0; i < 2; ++i) c.ownerTurn("q2", "a2");
  appendItems(log, std::vector<tx::Item>(c.items.begin() + (long)before, c.items.end()));
  tx::TxArena arena;
  initArena(arena);
  tx::FillReport rep;
  TXB_OK(log.fill(tx::HeadState{}, arena, rep));
  TEST_ASSERT_EQUAL(2, (int)rep.invalid);
  TEST_ASSERT_EQUAL(0, (int)rep.torn);
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)arena.count());
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::Item it;
    TXB_OK(arena.get(i, it));
  }
}

// open() checks head.json with one 8 KB read of the segment when the last line is short,
// and still trusts it (pinned survives: a rebuild would lose it) when the last line is long.
void test_open_checks_the_head_in_one_slice() {
  for (size_t lastBytes : {100u, 20000u}) {
    CountingFiles files;
    txb::Convo c;
    for (int i = 0; i < 40; ++i) c.ownerTurn("q", std::string(300, 'a'));
    c.openTurn();
    c.say(std::string(lastBytes, 'z'), false);
    tx::TxLog log(files, kKey);
    tx::HeadState head;
    TXB_OK(log.open(head));
    appendItems(log, c.items);
    TXB_OK(log.open(head));
    head.pinned = {"pin"};
    TXB_OK(log.writeHead(head));
    files.maxSegRead = 0;
    tx::TxLog reader(files, kKey);
    tx::HeadState got;
    TXB_OK(reader.open(got));
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)got.pinned.size(), "a good head.json was rebuilt");
    if (lastBytes < tx::kFillSliceBytes) TEST_ASSERT_TRUE(files.maxSegRead <= tx::kFillSliceBytes);
  }
}

// A cut that wrote everything but the line end: the bytes stay dropped, and storing the
// same q again with other content gives back the new item, never the old one.
void test_a_tail_missing_only_its_line_end_is_never_completed() {
  txb::MemFiles files;
  txb::Convo c;
  for (int i = 0; i < 2; ++i) c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  std::string seg, line;
  TXB_OK(tx::segmentPath(kKey, 1, seg));
  tx::Item old = c.items.back();
  old.q = c.q + 1;
  old.kind = tx::ItemKind::User;
  old.x = "the old content";
  TXB_OK(tx::encodeLine(old, line));
  files.files[seg] += line.substr(0, line.size() - 1);
  tx::TxLog again(files, kKey);
  TXB_OK(again.open(head));
  TEST_ASSERT_EQUAL_UINT64(c.q + 1, head.nextQ);
  tx::Item fresh = old;
  fresh.x = "the new content";
  tx::StoreState st;
  TXB_OK(again.append(fresh, st));
  tx::TxArena arena;
  initArena(arena);
  TXB_OK(again.fill(tx::HeadState{}, arena));
  TEST_ASSERT_EQUAL((int)c.items.size() + 1, (int)arena.count());
  tx::Item got;
  TXB_OK(arena.get(arena.count() - 1, got));
  TEST_ASSERT_EQUAL_STRING("the new content", got.x.c_str());
}

// A cut or refusal at the rename leaves head.json.tmp newer than head.json: open reads it.
void test_a_temp_newer_than_head_wins() {
  txb::MemFiles files;
  txb::Convo c;
  c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  TXB_OK(log.writeHead(head));
  head.pinned = {"new pin"};
  files.refuseRename = true;
  TEST_ASSERT_TRUE(log.writeHead(head).err == tx::Err::Io);
  files.clearFaults();
  tx::TxLog reader(files, kKey);
  tx::HeadState got;
  TXB_OK(reader.open(got));
  TEST_ASSERT_EQUAL(1, (int)got.pinned.size());
}

// The 16 KB cap is on the written bytes: escapes count.
void test_a_head_over_its_cap_after_escaping_is_refused() {
  txb::MemFiles files;
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  head.pinned.assign(tx::kHeadMaxPinned, std::string(tx::kHeadMaxString, '\x01'));
  TEST_ASSERT_TRUE(log.writeHead(head).err == tx::Err::InvalidArgument);
  std::string hp;
  TXB_OK(tx::headPath(kKey, hp));
  TEST_ASSERT_EQUAL(0, (int)files.files.count(hp));
}

// head.json names the last line it saw (end_q) and the line before the fill start must be
// one a digest covers: a crc-valid head.json of another state of the log is rebuilt.
void test_a_foreign_head_is_rebuilt() {
  txb::MemFiles files;
  uint64_t thru = 0;
  const txb::Convo c = days(8, thru);
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  TXB_OK(log.open(head));
  head.pinned = {"marker"};
  TXB_OK(log.writeHead(head));
  std::string hp;
  TXB_OK(tx::headPath(kKey, hp));
  const std::string saved = unseal(files.files[hp]);
  const auto forged = [&](const std::string& key, uint64_t from, uint64_t to) {
    std::string body = saved;
    const std::string a = "\"" + key + "\":" + std::to_string(from), b = "\"" + key + "\":" + std::to_string(to);
    TEST_ASSERT_TRUE_MESSAGE(body.find(a) != std::string::npos, a.c_str());
    body.replace(body.find(a), a.size(), b);
    files.files[hp] = seal(body);
    tx::TxLog reader(files, kKey);
    tx::HeadState got;
    TXB_OK(reader.open(got));
    return got;
  };
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)forged("end_q", c.q, c.q - 1).pinned.size(), "end_q of another log");
  const tx::HeadState g = forged("thru", thru, thru - 2);   // the fill start is past a line it does not cover
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)g.pinned.size(), "thru of another log");
  TEST_ASSERT_EQUAL_UINT64(thru, g.thru);
  files.files[hp] = seal(saved);
  tx::TxLog reader(files, kKey);
  tx::HeadState ok;
  TXB_OK(reader.open(ok));
  TEST_ASSERT_EQUAL(1, (int)ok.pinned.size());
}

// Pointers past the log are refused; a stray q of 2^64 - 1 does not wrap next_q to 0.
void test_pointers_and_q_stay_in_range() {
  txb::MemFiles files;
  txb::Convo c;
  c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  head.thru = c.q + 1;
  TEST_ASSERT_TRUE(log.writeHead(head).err == tx::Err::InvalidArgument);
  head.thru = 0;
  head.stub = 1000;
  TEST_ASSERT_TRUE(log.writeHead(head).err == tx::Err::InvalidArgument);
  const std::string huge = seal("{\"k\":\"h\",\"q\":18446744073709551615,\"x\":\"x\",\"cls\":\"notice\"");
  tx::StoreState st;
  TXB_OK(log.appendLine(huge.data(), huge.size(), st));
  tx::TxLog reader(files, kKey);
  TXB_OK(reader.open(head));
  TEST_ASSERT_TRUE(head.nextQ == UINT64_MAX);
}

// A head.json with a key a newer writer added (string or list) is still read.
void test_a_head_with_newer_keys_is_read() {
  txb::MemFiles files;
  txb::Convo c;
  c.ownerTurn("q", "a");
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  head.pinned = {"p"};
  TXB_OK(log.writeHead(head));
  std::string hp;
  TXB_OK(tx::headPath(kKey, hp));
  files.files[hp] = seal(unseal(files.files[hp]) + ",\"later\":\"x\",\"more\":[\"a\",\"b\"],\"n\":7");
  tx::TxLog reader(files, kKey);
  tx::HeadState got;
  TXB_OK(reader.open(got));
  TEST_ASSERT_EQUAL(1, (int)got.pinned.size());
}

// Upper case is refused (FAT folders ignore case: two keys would share a folder); a bulk
// payload over its cap is refused whole.
void test_keys_are_lower_case_and_bulk_is_capped() {
  TEST_ASSERT_FALSE(tx::chatKeyOk("Abc"));
  TEST_ASSERT_TRUE(tx::chatKeyOk("abc"));
  txb::MemFiles files;
  tx::TxLog log(files, kKey);
  std::string h, out;
  TEST_ASSERT_TRUE(log.appendBulk(3, std::string(tx::kBulkMaxBytes + 1, 'b'), h).err == tx::Err::TooLarge);
  TEST_ASSERT_EQUAL(0, (int)files.appends);
  TXB_OK(log.appendBulk(3, std::string(tx::kBulkMaxBytes, 'b'), h));
  TEST_ASSERT_TRUE(log.readBulk("b/3.txb@0+524289", 0, 10, out).err == tx::Err::InvalidArgument);
}

// One turn larger than the arena: the fill never gives up the turn it is filling, so the
// arena keeps that turn's start (its t item) and reports the rest as dropped.
void test_one_turn_larger_than_the_arena_keeps_its_start() {
  txb::MemFiles files;
  txb::Convo c;
  c.openTurn();
  c.owner("big");
  for (int i = 0; i < 60; ++i) {
    const std::string id = c.call("read_file", R"({"path":"a"})");
    c.receipt(id, "read_file", std::string(1500, 'r'));
  }
  tx::TxLog log(files, kKey);
  tx::HeadState head;
  TXB_OK(log.open(head));
  appendItems(log, c.items);
  tx::TxArena arena;
  initArena(arena, 16 * 1024);
  tx::FillReport rep;
  TXB_OK(log.fill(tx::HeadState{}, arena, rep));
  tx::ArenaEntry first;
  TXB_OK(arena.header(0, first));
  TEST_ASSERT_EQUAL_UINT64(1, first.q);
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)(arena.count() + rep.dropped));
}

// A bulk payload is written in appends of at most 32 KB (Ruling 19: about 50 ms of card lock
// each) and reads back whole; a refused chunk is NotStored and the next payload's handle
// is still exact.
void test_bulk_is_written_in_lock_sized_chunks() {
  struct Sizes : txb::MemFiles {
    std::vector<size_t> lens;
    bool append(const std::string& path, const char* data, size_t len) override {
      lens.push_back(len);
      return txb::MemFiles::append(path, data, len);
    }
  } files;
  tx::TxLog log(files, kKey);
  const std::string big = txb::payload("BIG", 100 * 1024), small = txb::payload("SMALL", 5000);
  std::string h, out;
  TXB_OK(log.appendBulk(4, big, h));
  TEST_ASSERT_EQUAL(4, (int)files.lens.size());
  for (size_t n : files.lens) TEST_ASSERT_TRUE(n <= tx::kBulkChunkBytes);
  TXB_OK(log.readBulk(h, 0, big.size(), out));
  TEST_ASSERT_TRUE(out == big);
  files.refuseAppend = (long)files.appends + 1;   // the second chunk of the next payload
  TEST_ASSERT_TRUE(log.appendBulk(4, big, h).err == tx::Err::NotStored);
  TEST_ASSERT_TRUE(h.empty());
  files.clearFaults();
  TXB_OK(log.appendBulk(4, small, h));
  TXB_OK(log.readBulk(h, 0, small.size(), out));
  TEST_ASSERT_TRUE(out == small);
}

inline void run() {
  RUN_TEST(test_restart_fill_gives_the_same_arena_bytes);
  RUN_TEST(test_restart_fill_is_one_bounded_sequential_read);
  RUN_TEST(test_store_keeps_a_refused_item_in_ram_as_not_stored);
  RUN_TEST(test_no_storage_opens_closed_and_never_stores);
  RUN_TEST(test_stale_head_is_caught_up_to_the_rebuild);
  RUN_TEST(test_untrusted_head_is_rebuilt);
  RUN_TEST(test_cut_at_every_append_loses_only_that_item);
  RUN_TEST(test_write_head_locates_a_new_thru);
  RUN_TEST(test_fill_into_a_small_arena_keeps_the_newest_turns);
  RUN_TEST(test_garbage_without_a_line_end_is_dropped);
  RUN_TEST(test_paths_property_over_every_byte);
  RUN_TEST(test_bulk_handles_cannot_name_a_path);
  RUN_TEST(test_partial_refusal_never_glues);
  RUN_TEST(test_head_round_trips_its_fields);
  RUN_TEST(test_a_torn_tail_at_the_end_is_dropped);
  RUN_TEST(test_a_head_behind_the_segment_is_not_trusted);
  RUN_TEST(test_fill_drops_crc_valid_lines_it_cannot_read);
  RUN_TEST(test_open_checks_the_head_in_one_slice);
  RUN_TEST(test_a_tail_missing_only_its_line_end_is_never_completed);
  RUN_TEST(test_a_temp_newer_than_head_wins);
  RUN_TEST(test_a_head_over_its_cap_after_escaping_is_refused);
  RUN_TEST(test_a_foreign_head_is_rebuilt);
  RUN_TEST(test_pointers_and_q_stay_in_range);
  RUN_TEST(test_a_head_with_newer_keys_is_read);
  RUN_TEST(test_keys_are_lower_case_and_bulk_is_capped);
  RUN_TEST(test_one_turn_larger_than_the_arena_keeps_its_start);
  RUN_TEST(test_bulk_is_written_in_lock_sized_chunks);
}

}  // namespace store_tests
