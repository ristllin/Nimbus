// test_seen_index - which Telegram updates engine 2 already took (PRD section 7, lane RB-TG1).
//
// The class: an update Telegram serves again (a settings partition that ignores the offset
// write, a power cycle that empties RAM and the RTC record) is answered once, however often it
// comes back; and no update the device never took is ever counted as seen. The index rule is
// tested over keys, floors and the boot rebuild; the definition of done runs the plain engine
// (scripted model) behind a fake Telegram that re-serves one photo 200 times across a reboot.
#include <unity.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "nimbus/orch/seen_index.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;
using nimbus::orch::kSeenKeys;
using nimbus::orch::SeenIndex;

void setUp() {}
void tearDown() {}

namespace {

void ready(SeenIndex& ix) {
  TEST_ASSERT_TRUE(ix.init([](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }).ok());
}

// ---- the index rule ----

void test_a_noted_key_is_seen_and_other_chats_are_not() {
  SeenIndex ix;
  ready(ix);
  TEST_ASSERT_FALSE(ix.seen("1001", 500));
  ix.note("1001", 500);
  TEST_ASSERT_TRUE(ix.seen("1001", 500));
  TEST_ASSERT_FALSE(ix.seen("1001", 501));
  TEST_ASSERT_FALSE(ix.seen("1001", 499));   // no floor yet: an older id was never taken
  TEST_ASSERT_FALSE(ix.seen("2002", 500));
  ix.note("1001", 500);   // a second note of the same key holds one slot
  TEST_ASSERT_EQUAL(1, (int)ix.size());
  ix.note("1001", 0);   // id 0 is no update
  TEST_ASSERT_FALSE(ix.seen("1001", 0));
  TEST_ASSERT_EQUAL(1, (int)ix.size());
}

void test_an_index_the_allocator_refused_sees_nothing_and_keeps_intake_running() {
  SeenIndex ix;
  TEST_ASSERT_FALSE(ix.init([](size_t) -> void* { return nullptr; }, [](void* p) { std::free(p); }).ok());
  TEST_ASSERT_FALSE(ix.ready());
  ix.note("1001", 7);
  TEST_ASSERT_FALSE(ix.seen("1001", 7));
  ix.rebuild("1001", 9, {7});
  TEST_ASSERT_FALSE(ix.seen("1001", 7));
}

void test_the_newest_1024_keys_stay_and_the_oldest_leaves_into_the_floor() {
  SeenIndex ix;
  ready(ix);
  // Two chats interleaved, out of order inside each pair (a joined later id first).
  for (uint64_t i = 0; i < kSeenKeys + 300; ++i) {
    const uint64_t id = 1000 + (i ^ 1);
    ix.note(i % 3 ? "1001" : "2002", id);
  }
  TEST_ASSERT_EQUAL((int)kSeenKeys, (int)ix.size());
  // Every key noted is still seen: the newest as keys, the older through their chat's floor.
  for (uint64_t i = 0; i < kSeenKeys + 300; ++i) {
    const uint64_t id = 1000 + (i ^ 1);
    TEST_ASSERT_TRUE_MESSAGE(ix.seen(i % 3 ? "1001" : "2002", id), "a noted id was lost at eviction");
  }
  // The floors stay under the kept keys: nothing newer than the evicted ids is assumed.
  TEST_ASSERT_TRUE(ix.floorOf("1001") < 1000 + 300 + 2);
  TEST_ASSERT_TRUE(ix.floorOf("2002") < 1000 + 300 + 2);
  TEST_ASSERT_FALSE(ix.seen("1001", 1000 + kSeenKeys + 400));
}

// ---- the boot rebuild ----

void test_rebuild_from_owner_rows_and_hw_marks_every_stored_id() {
  SeenIndex ix;
  ready(ix);
  ix.rebuild("1001", 512, {500, 501, 505, 512});
  for (uint64_t id : {500u, 501u, 505u, 512u}) TEST_ASSERT_TRUE(ix.seen("1001", id));
  for (uint64_t id : {1u, 300u, 499u}) TEST_ASSERT_TRUE_MESSAGE(ix.seen("1001", id), "older than the window");
  TEST_ASSERT_FALSE(ix.seen("1001", 513));
  TEST_ASSERT_FALSE(ix.seen("2002", 499));
}

// The overtaken message: RB-TG2 joined text 503 into turn {500, 503} while photo 502 was queued;
// the device restarted before 502's turn. 502 must still be answered.
void test_an_id_between_rows_that_no_row_holds_is_not_seen() {
  SeenIndex ix;
  ready(ix);
  ix.rebuild("1001", 503, {500, 503});
  TEST_ASSERT_FALSE_MESSAGE(ix.seen("1001", 502), "an overtaken queued message would be dropped");
  TEST_ASSERT_FALSE(ix.seen("1001", 501));
  TEST_ASSERT_TRUE(ix.seen("1001", 503));
}

void test_a_chat_with_no_row_in_the_window_is_floored_at_hw() {
  SeenIndex ix;
  ready(ix);
  ix.rebuild("1001", 640, {});
  TEST_ASSERT_TRUE(ix.seen("1001", 640));
  TEST_ASSERT_TRUE(ix.seen("1001", 1));
  TEST_ASSERT_FALSE(ix.seen("1001", 641));
  // Rows that stop short of hw (the fill did not reach the newest rows): hw floors them.
  SeenIndex iy;
  ready(iy);
  iy.rebuild("1001", 700, {600, 610});
  TEST_ASSERT_TRUE(iy.seen("1001", 690));
  TEST_ASSERT_FALSE(iy.seen("1001", 701));
}

// ---- the definition of done ----

// Settings storage that is full: every write is refused (the 14 Oct NVS state).
struct FullSettings {
  uint64_t offset = 0;
  size_t refused = 0;
  bool write(uint64_t v) {
    (void)v;
    ++refused;
    return false;
  }
};

// One update Telegram keeps serving: its offset is never confirmed, because the device's
// checkpoint is refused and the reboot empties the RAM cursor.
struct Update {
  std::string chat;
  uint64_t id;
  std::string caption;
  tx::Part photo;
};

struct Intake {
  txb::Battery& b;
  SeenIndex& ix;
  FullSettings& settings;
  size_t dropped = 0;

  void serve(const Update& u) {
    if (ix.seen(u.chat, u.id)) {
      ++dropped;
      return;
    }
    plain::TurnOutcome out;
    TXB_OK(b.ownerFrom({u.id}, u.caption, out, {u.photo}));
    ix.note(u.chat, u.id);
    (void)settings.write(u.id + 1);   // the offset checkpoint: refused
  }

  void reboot(bool rebuild) {
    TXB_OK(b.powerCycle());
    ix.clear();   // RAM is gone
    if (rebuild) TXB_OK(nimbus::orch::rebuildSeen(ix, b.chatKey, *b.engine->head(), *b.arena));
  }
};

size_t ownerRows(txb::Battery& b) {
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  size_t n = 0;
  for (const tx::Item& it : items) n += it.kind == tx::ItemKind::User;
  return n;
}

void redeliver(bool rebuild, size_t& requests, size_t& sends, size_t& rows) {
  txb::Battery b;
  b.model.push(sp::say("A plate of roast potatoes."));
  b.model.push(sp::say("A plate of roast potatoes, again."));   // only a second answer would take it
  SeenIndex ix;
  ready(ix);
  FullSettings settings;
  Intake in{b, ix, settings};
  Update u{b.chatKey, 5001, "what is this?", tx::Part{tx::PartKind::Image, "pa1b2c3d4", "image/jpeg", 90000}};
  for (int r = 0; r < 200; ++r) {
    if (r == 100) in.reboot(rebuild);
    in.serve(u);
  }
  TEST_ASSERT_EQUAL(0, (int)settings.offset);   // the offset never reached settings
  requests = b.model.requests().size();
  sends = b.sent.size();
  rows = ownerRows(b);
}

void test_200_redeliveries_with_a_photo_across_a_reboot_with_settings_full() {
  size_t requests = 0, sends = 0, rows = 0;
  redeliver(true, requests, sends, rows);
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)requests, "model calls");
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)sends, "replies");
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)rows, "owner rows");
}

// Counter-test: the same run without the boot rebuild answers the photo a second time, so the
// property above sees the class (an index that forgets at a reboot).
void test_counter_without_the_rebuild_the_photo_is_answered_twice() {
  size_t requests = 0, sends = 0, rows = 0;
  redeliver(false, requests, sends, rows);
  TEST_ASSERT_EQUAL(2, (int)requests);
  TEST_ASSERT_EQUAL(2, (int)sends);
  TEST_ASSERT_EQUAL(2, (int)rows);
}

// The rebuild from a real engine's arena: an owner row is seen after a reboot, a later update is
// not, and the ids of every owner row are read (several turns, several ids in one turn).
void test_rebuild_reads_every_owner_row_of_the_filled_arena() {
  txb::Battery b;
  for (int i = 0; i < 3; ++i) b.model.push(sp::say("ok"));
  plain::TurnOutcome out;
  TXB_OK(b.ownerFrom({700}, "one", out));
  TXB_OK(b.ownerFrom({702, 703}, "two and three", out));
  TXB_OK(b.ownerFrom({705}, "four", out));
  TXB_OK(b.powerCycle());
  SeenIndex ix;
  ready(ix);
  TXB_OK(nimbus::orch::rebuildSeen(ix, b.chatKey, *b.engine->head(), *b.arena));
  for (uint64_t id : {700u, 702u, 703u, 705u}) TEST_ASSERT_TRUE(ix.seen(b.chatKey, id));
  TEST_ASSERT_FALSE(ix.seen(b.chatKey, 701));   // between rows, held by none
  TEST_ASSERT_FALSE(ix.seen(b.chatKey, 704));
  TEST_ASSERT_FALSE(ix.seen(b.chatKey, 706));
  TEST_ASSERT_EQUAL(705, (int)b.engine->head()->hw);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_noted_key_is_seen_and_other_chats_are_not);
  RUN_TEST(test_an_index_the_allocator_refused_sees_nothing_and_keeps_intake_running);
  RUN_TEST(test_the_newest_1024_keys_stay_and_the_oldest_leaves_into_the_floor);
  RUN_TEST(test_rebuild_from_owner_rows_and_hw_marks_every_stored_id);
  RUN_TEST(test_an_id_between_rows_that_no_row_holds_is_not_seen);
  RUN_TEST(test_a_chat_with_no_row_in_the_window_is_floored_at_hw);
  RUN_TEST(test_200_redeliveries_with_a_photo_across_a_reboot_with_settings_full);
  RUN_TEST(test_counter_without_the_rebuild_the_photo_is_answered_twice);
  RUN_TEST(test_rebuild_reads_every_owner_row_of_the_filled_arena);
  return UNITY_END();
}
