#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/msg_batch.h"
#include "nimbus/telegram_offset.h"

// The getUpdates offset (nimbus/telegram_offset.h) and the poll cycle that uses it
// (nimbus::orch::drainCycle, the same code src/agent/telegram.cpp runs).
//
// The class these pin (the 14 Oct 2026 replay loop): a full settings storage made the
// offset write fail, the loop re-read the stale stored offset after every drain, and
// Telegram served the same page again on every poll. The rule: RAM is the truth while
// the device runs, the stored copy only seeds it after a restart. Properties over
// EVERY failure pattern of the offset write across 8 poll cycles, each also with a
// restart at every point of every cycle:
//   - with no restart, every update is dispatched exactly once;
//   - with a restart, every update is still dispatched, and only the updates of the
//     one page in flight at the restart can run twice;
//   - each poll carries the RAM cursor, so the page drained last cycle is confirmed
//     (pending 0) by the next poll whatever the write did;
//   - write attempts stay bounded: one per newly acked page plus at most one retry a
//     minute while a write is owed (no retry storm on a full NVS).
// Plus a fresh device (no stored offset), the token swap, and a counter-test that
// runs the v4.5.13 loop shape against the same fakes and watches it replay.

using nimbus::core::nextTelegramOffset;
using nimbus::core::TgOffsetCursor;
using nimbus::orch::ClassifiedUpdate;
using nimbus::orch::DrainResult;
using nimbus::orch::MessageBatcher;
using nimbus::orch::RawPage;

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

// ---- fakes ---------------------------------------------------------------------

// Telegram's getUpdates: a fetch at `offset` confirms (deletes) every queued update
// below it, then serves up to `limit` of the rest, oldest first.
struct FakeTelegram {
  std::vector<int32_t> queue;   // unconfirmed update ids, ascending
  std::vector<int32_t> page;    // the page handle(i) indexes
  int32_t nextId = 1000;
  int limit = 10;
  std::vector<int32_t> offsetsSeen;

  void push(int n) {
    for (int i = 0; i < n; ++i) queue.push_back(nextId++);
  }
  RawPage fetch(int32_t offset) {
    offsetsSeen.push_back(offset);
    queue.erase(std::remove_if(queue.begin(), queue.end(),
                               [&](int32_t id) { return id < offset; }),
                queue.end());
    page.assign(queue.begin(), queue.begin() + std::min<size_t>(queue.size(), (size_t)limit));
    RawPage rp;
    rp.count = (int)page.size();
    rp.limit = limit;
    return rp;
  }
  bool holds(int32_t id) const { return std::find(queue.begin(), queue.end(), id) != queue.end(); }
};

// The one NVS int key. A non-zero write fails while `full`; writing 0 is an erase,
// which lands even on a full partition (store::setTelegramOffset(0) -> clearKey).
struct FakeStore {
  bool    present = false;
  int32_t value = 0;
  bool    full = false;
  int     writes = 0;

  int32_t read() const { return present ? value : 0; }
  bool write(int32_t v) {
    ++writes;
    if (v == 0) { present = false; value = 0; return true; }
    if (full) return false;
    present = true;
    value = v;
    return true;
  }
};

// Where a restart lands inside one poll cycle.
enum class Phase { BeforeFetch, AfterFetch, AfterFirstTurn, AfterAdvance, AfterPersist, Count };
struct Restart {};   // thrown to cut a cycle short exactly where the power went

// The device's poll loop, reduced to the offset path: drainCycle (fetch at the cursor,
// one turn per bucket, advance), then persist. Updates come from up to three chats;
// each text carries its id as "#<id>#" so a turn's dispatched ids can be read back.
struct Device {
  TgOffsetCursor cursor;
  MessageBatcher batch;
  std::map<int32_t, int> dispatched;     // id -> times a turn carried it
  std::vector<int32_t> pageInFlight;      // the ids of the page fetched last
  int newAcks = 0;

  void boot(const FakeStore& st) {
    cursor = TgOffsetCursor{};
    cursor.seed(st.read());
    batch.clear();
  }

  static ClassifiedUpdate textUpdate(int32_t id) {
    ClassifiedUpdate u;
    u.updateId = id;
    u.action = ClassifiedUpdate::Action::BatchText;
    u.chatId = std::to_string(1 + id % 3);
    u.from = "u";
    u.text = "#" + std::to_string(id) + "#";
    return u;
  }

  void recordTurn(size_t i) {
    const std::string t = batch.render(i).c_str();
    for (size_t p = t.find('#'); p != std::string::npos; p = t.find('#', p + 1)) {
      const size_t q = t.find('#', p + 1);
      if (q == std::string::npos) break;
      dispatched[(int32_t)std::stol(t.substr(p + 1, q - p - 1))]++;
      p = q;
    }
  }

  // One cycle; throws Restart at `cut` (Phase::Count = never).
  void cycle(FakeTelegram& tg, FakeStore& st, uint32_t now, Phase cut) {
    if (cut == Phase::BeforeFetch) throw Restart{};
    int turns = 0;
    const int32_t before = cursor.offset();
    nimbus::orch::drainCycle(
        batch, cursor,
        [&](int32_t off) {
          RawPage rp = tg.fetch(off);
          pageInFlight = tg.page;
          if (cut == Phase::AfterFetch) throw Restart{};
          return rp;
        },
        [&](int i) {
          ClassifiedUpdate u = textUpdate(tg.page[(size_t)i]);
          if (!cursor.admit(u.updateId)) u.action = ClassifiedUpdate::Action::AckOnly;
          return u;
        },
        [&](size_t i) {
          recordTurn(i);
          if (++turns == 1 && cut == Phase::AfterFirstTurn) throw Restart{};
        });
    if (cursor.offset() > before) ++newAcks;
    if (cut == Phase::AfterAdvance) throw Restart{};
    cursor.persist(now, [&](int32_t v) { return st.write(v); });
    if (cut == Phase::AfterPersist) throw Restart{};
  }
};

// Updates arriving before each of the 8 cycles (a 12-message backlog spans two pages;
// zeros are idle cycles), and the time each cycle takes: a cycle with updates runs
// its turns, an idle one is a 1 s pause between local turns or a 30 s long-poll.
static const int kArrivals[8] = {3, 0, 2, 4, 0, 1, 12, 0};
static const uint32_t kStepMs[8] = {2500, 1000, 2500, 2500, 30000, 2500, 2500, 1000};

struct RunResult {
  std::map<int32_t, int> dispatched;
  std::set<int32_t> restartPage;   // the page in flight when the restart hit
  int32_t pushed = 0;
  bool bounded = true;             // write attempts within the bound in every boot
  bool pendingZero = true;         // each poll confirmed the page drained before it
};

// Run 8 cycles with the given write-failure mask, restarting once at (restartCycle,
// phase) (restartCycle < 0: never), then drain to empty with a healthy store.
static RunResult run(unsigned mask, int restartCycle, Phase phase) {
  FakeTelegram tg;
  FakeStore st;
  Device dev;
  dev.boot(st);
  RunResult res;
  uint32_t now = 0, bootAt = 0;
  std::vector<int32_t> lastDrained;
  auto checkBound = [&]() {
    const uint32_t up = now - bootAt;
    const int bound = dev.newAcks + (int)(up / TgOffsetCursor::kRetryQuietMs) + 1;
    if ((int)dev.cursor.writeAttempts() > bound) res.bounded = false;
  };
  for (int c = 0; c < 8; ++c) {
    tg.push(kArrivals[c]);
    res.pushed += kArrivals[c];
    now += kStepMs[c];
    st.full = (mask >> c) & 1u;
    const Phase cut = (c == restartCycle) ? phase : Phase::Count;
    try {
      dev.cycle(tg, st, now, cut);
      // pending 0 next cycle: what this cycle's poll confirmed includes every update
      // the previous cycle drained, whether or not its write landed.
      for (int32_t id : lastDrained)
        if (tg.holds(id)) res.pendingZero = false;
      lastDrained = dev.pageInFlight;
      checkBound();
    } catch (const Restart&) {
      if (phase != Phase::BeforeFetch)
        res.restartPage.insert(dev.pageInFlight.begin(), dev.pageInFlight.end());
      else
        res.restartPage.insert(lastDrained.begin(), lastDrained.end());
      const auto keep = dev.dispatched;
      dev = Device{};
      dev.dispatched = keep;
      dev.boot(st);
      bootAt = now;
      lastDrained.clear();
    }
  }
  st.full = false;
  for (int k = 0; k < 6; ++k) {
    now += 2500;
    dev.cycle(tg, st, now, Phase::Count);
  }
  res.dispatched = dev.dispatched;
  return res;
}

// The property, over every failure mask of 8 cycles and every restart point.
static void test_every_failure_mask_every_restart_point() {
  int runs = 0;
  for (unsigned mask = 0; mask < 256; ++mask) {
    for (int rc = -1; rc < 8; ++rc) {
      for (int ph = 0; ph < (int)Phase::Count; ++ph) {
        if (rc < 0 && ph > 0) break;   // the no-restart run, once per mask
        const RunResult r = run(mask, rc, (Phase)ph);
        ++runs;
        char msg[96];
        snprintf(msg, sizeof msg, "mask=0x%02x restart=%d phase=%d", mask, rc, ph);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r.pushed, (int)r.dispatched.size(), msg);   // none lost
        for (const auto& kv : r.dispatched) {
          if (rc < 0) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, kv.second, msg);   // exactly once
          } else {
            TEST_ASSERT_TRUE_MESSAGE(kv.second <= 2, msg);
            if (kv.second == 2)   // only the page in flight at the restart replays
              TEST_ASSERT_TRUE_MESSAGE(r.restartPage.count(kv.first) == 1, msg);
          }
        }
        TEST_ASSERT_TRUE_MESSAGE(r.restartPage.size() <= 10, msg);   // at most one page
        TEST_ASSERT_TRUE_MESSAGE(r.bounded, msg);
        if (rc < 0) TEST_ASSERT_TRUE_MESSAGE(r.pendingZero, msg);
      }
    }
  }
  TEST_ASSERT_EQUAL_INT(256 * (1 + 8 * (int)Phase::Count), runs);
}

// The 14 Oct shape: every write fails for a whole stretch of pages. Each poll still
// carries the RAM cursor (never the stale stored value), and the stored copy stays
// behind (tgOffsetSaved false) until a write lands.
static void test_polls_carry_the_ram_cursor_on_a_full_store() {
  FakeTelegram tg;
  FakeStore st;
  st.full = true;
  Device dev;
  dev.boot(st);
  uint32_t now = 0;
  for (int c = 0; c < 8; ++c) {
    tg.push(kArrivals[c]);
    now += kStepMs[c];
    const int32_t carried = dev.cursor.offset();
    dev.cycle(tg, st, now, Phase::Count);
    TEST_ASSERT_EQUAL_INT32(carried, tg.offsetsSeen.back());
  }
  TEST_ASSERT_FALSE(dev.cursor.saved());
  TEST_ASSERT_EQUAL_INT32(0, st.read());   // nothing landed
  for (const auto& kv : dev.dispatched) TEST_ASSERT_EQUAL_INT(1, kv.second);
  st.full = false;   // one erase elsewhere frees room: the next due write lands
  now += TgOffsetCursor::kRetryQuietMs;
  dev.cursor.persist(now, [&](int32_t v) { return st.write(v); });
  TEST_ASSERT_TRUE(dev.cursor.saved());
  TEST_ASSERT_EQUAL_INT32(dev.cursor.offset(), st.read());
}

// A full store while idle: after the page's own attempt, the owed write is retried
// once a minute (not once per 1 s cycle), and once an erase elsewhere frees room it
// lands within a minute (the bench leg "erasing one setting lets the write land").
static void test_idle_retry_once_a_minute_then_lands() {
  FakeTelegram tg;
  FakeStore st;
  st.full = true;
  Device dev;
  dev.boot(st);
  tg.push(2);
  uint32_t now = 2500;
  dev.cycle(tg, st, now, Phase::Count);
  TEST_ASSERT_EQUAL_UINT32(1, dev.cursor.writeAttempts());
  for (int i = 0; i < 180; ++i) dev.cycle(tg, st, now += 1000, Phase::Count);
  TEST_ASSERT_EQUAL_UINT32(4, dev.cursor.writeAttempts());   // +3 retries in 180 s
  TEST_ASSERT_FALSE(dev.cursor.saved());
  st.full = false;
  int cycles = 0;
  while (!dev.cursor.saved() && cycles < 120) {
    dev.cycle(tg, st, now += 1000, Phase::Count);
    ++cycles;
  }
  TEST_ASSERT_TRUE(dev.cursor.saved());
  TEST_ASSERT_TRUE(cycles <= 60);
  TEST_ASSERT_EQUAL_INT32(1002, st.read());
  for (const auto& kv : dev.dispatched) TEST_ASSERT_EQUAL_INT(1, kv.second);
}

// Counter-test: the v4.5.13 loop shape (commit, then RE-READ the stored offset after
// every drain) against the same fakes replays the same page on every cycle once the
// write fails. The suite above must be able to see this class.
static void test_v4513_reload_shape_replays_on_a_full_store() {
  FakeTelegram tg;
  FakeStore st;
  st.full = true;
  MessageBatcher batch;
  std::map<int32_t, int> dispatched;
  tg.push(4);
  int32_t offset = st.read();
  for (int c = 0; c < 8; ++c) {
    const DrainResult r = nimbus::orch::drainOnePage(
        batch, offset, [&](int32_t off) { return tg.fetch(off); },
        [&](int i) { return Device::textUpdate(tg.page[(size_t)i]); });
    for (int32_t id : tg.page) dispatched[id]++;
    if (r.ackOffset > offset) st.write(r.ackOffset);   // result ignored, as it was
    offset = st.read();                                // the reload
  }
  for (const auto& kv : dispatched) TEST_ASSERT_EQUAL_INT(8, kv.second);   // the loop
}

// Fresh device: no stored offset reads 0, the first page is dispatched once, and the
// first ack is written at once.
static void test_fresh_device_first_page_once() {
  FakeTelegram tg;
  FakeStore st;   // absent key
  Device dev;
  dev.boot(st);
  TEST_ASSERT_EQUAL_INT32(0, dev.cursor.offset());
  TEST_ASSERT_TRUE(dev.cursor.saved());
  tg.push(3);
  dev.cycle(tg, st, 2500, Phase::Count);
  dev.cycle(tg, st, 5000, Phase::Count);
  TEST_ASSERT_EQUAL_INT(3, (int)dev.dispatched.size());
  for (const auto& kv : dev.dispatched) TEST_ASSERT_EQUAL_INT(1, kv.second);
  TEST_ASSERT_EQUAL_INT32(1003, st.read());
  TEST_ASSERT_EQUAL_UINT32(1, dev.cursor.writeAttempts());
  TEST_ASSERT_TRUE(tg.queue.empty());   // pending 0
}

// Token swap: the old bot's offset is far above the new bot's ids. Resetting through
// the cursor serves the new bot from its first update, and its 0 is an erase that lands
// even on a full store. Counter: keeping the old offset skips every new-bot message.
static void test_token_swap_resets_through_the_cursor() {
  FakeStore st;
  Device dev;
  dev.boot(st);
  FakeTelegram oldBot;
  oldBot.push(5);
  dev.cycle(oldBot, st, 2500, Phase::Count);
  TEST_ASSERT_EQUAL_INT32(1005, dev.cursor.offset());

  FakeTelegram newBot;
  newBot.nextId = 40;
  newBot.push(3);
  st.full = true;
  dev.cursor.reset();
  dev.cursor.persist(3000, [&](int32_t v) { return st.write(v); });
  TEST_ASSERT_TRUE(dev.cursor.saved());   // the erase landed on a full store
  TEST_ASSERT_FALSE(st.present);
  dev.cycle(newBot, st, 5500, Phase::Count);
  for (int32_t id : {40, 41, 42}) TEST_ASSERT_EQUAL_INT(1, dev.dispatched[id]);

  // Counter: the stale offset (1005) on the new bot confirms and skips 40..42 unseen.
  FakeTelegram skipped;
  skipped.nextId = 40;
  skipped.push(3);
  skipped.fetch(1005);
  TEST_ASSERT_TRUE(skipped.page.empty());
  TEST_ASSERT_TRUE(skipped.queue.empty());
}

// The retry cap, directly: a failing write is tried once per newly acked page and
// otherwise at most once per kRetryQuietMs; a landed write stops the retries.
static void test_persist_retry_is_capped() {
  TgOffsetCursor c;
  c.seed(100);
  int tries = 0;
  bool land = false;
  auto w = [&](int32_t) { ++tries; return land; };
  TEST_ASSERT_FALSE(c.persist(0, w));   // nothing owed
  c.advance(110);
  TEST_ASSERT_TRUE(c.persist(1000, w));   // new page: at once
  TEST_ASSERT_FALSE(c.saved());
  for (uint32_t t = 2000; t < 61000; t += 1000) TEST_ASSERT_FALSE(c.persist(t, w));
  TEST_ASSERT_EQUAL_INT(1, tries);
  TEST_ASSERT_TRUE(c.persist(61000, w));   // one retry a minute
  TEST_ASSERT_EQUAL_INT(2, tries);
  c.advance(120);
  TEST_ASSERT_TRUE(c.persist(61500, w));   // another new page: at once again
  TEST_ASSERT_EQUAL_INT(3, tries);
  land = true;
  TEST_ASSERT_TRUE(c.persist(121500, w));
  TEST_ASSERT_TRUE(c.saved());
  for (uint32_t t = 121500; t < 400000; t += 5000) TEST_ASSERT_FALSE(c.persist(t, w));
  TEST_ASSERT_EQUAL_INT(4, tries);
  TEST_ASSERT_EQUAL_UINT32(4, c.writeAttempts());
  TEST_ASSERT_EQUAL_UINT32(3, c.writeFails());
}

// The cursor never moves backward, and an update below it is dropped and counted.
static void test_cursor_forward_only_and_stale_drop() {
  TgOffsetCursor c;
  c.seed(-5);   // never written by this firmware: reads as 0
  TEST_ASSERT_EQUAL_INT32(0, c.offset());
  TEST_ASSERT_TRUE(c.advance(50));
  TEST_ASSERT_FALSE(c.advance(40));
  TEST_ASSERT_FALSE(c.advance(50));
  TEST_ASSERT_EQUAL_INT32(50, c.offset());
  TEST_ASSERT_TRUE(c.admit(50));
  TEST_ASSERT_TRUE(c.admit(60));
  TEST_ASSERT_FALSE(c.admit(49));
  TEST_ASSERT_EQUAL_UINT32(1, c.staleDropped());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_confirms_with_plus_one);
  RUN_TEST(test_advances_across_batch);
  RUN_TEST(test_never_moves_backwards);
  RUN_TEST(test_every_failure_mask_every_restart_point);
  RUN_TEST(test_polls_carry_the_ram_cursor_on_a_full_store);
  RUN_TEST(test_idle_retry_once_a_minute_then_lands);
  RUN_TEST(test_v4513_reload_shape_replays_on_a_full_store);
  RUN_TEST(test_fresh_device_first_page_once);
  RUN_TEST(test_token_swap_resets_through_the_cursor);
  RUN_TEST(test_persist_retry_is_capped);
  RUN_TEST(test_cursor_forward_only_and_stale_drop);
  return UNITY_END();
}
