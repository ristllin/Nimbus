#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/vector_archive.h"
#include "nimbus/orch/vector_memory.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// A dims-wide int8 vector pointing mostly along one axis (axis in [0,dims)).
static std::vector<int8_t> axisVec(int dims, int axis, int8_t mag = 100) {
  std::vector<int8_t> v(dims, 0);
  v[axis % dims] = mag;
  return v;
}

static VecEntry mkEntry(const std::string& id, const std::string& content, int axis,
                        const std::string& ns = "owner", int dims = 8) {
  VecEntry e;
  e.id = id;
  e.content = content;
  e.ns = ns;
  e.importance = 0.6f;
  e.ttlHours = 96;
  e.createdAtHours = 10;
  e.vec = axisVec(dims, axis);
  return e;
}

// ---- basic archive/size/getAll ---------------------------------------------
static void test_archive_stores_and_preserves_embedding() {
  VectorArchive ar;
  ar.configure(8);
  VecEntry e = mkEntry("a1", "the alarm code is 4417", 1);
  TEST_ASSERT_TRUE(ar.archive(e, 200));
  TEST_ASSERT_EQUAL_INT(1, ar.size());
  auto all = ar.getAll();
  TEST_ASSERT_EQUAL_INT(1, (int)all.size());
  TEST_ASSERT_EQUAL_STRING("the alarm code is 4417", all[0].content.c_str());
  // Embedding preserved verbatim (the whole point - no re-embed on restore).
  TEST_ASSERT_EQUAL_INT(8, (int)all[0].vec.size());
  TEST_ASSERT_EQUAL_INT(100, all[0].vec[1]);
}

static void test_wrong_width_rejected() {
  VectorArchive ar;
  ar.configure(8);
  VecEntry e = mkEntry("a1", "x", 0, "owner", 4);   // 4-wide into an 8-wide store
  TEST_ASSERT_FALSE(ar.archive(e, 1));
  TEST_ASSERT_EQUAL_INT(0, ar.size());
}

// ---- FIFO cap: oldest archived evicted first --------------------------------
static void test_fifo_cap_evicts_oldest() {
  VectorArchive ar;
  ar.configure(8);
  ar.setMaxEntries(2);
  ar.archive(mkEntry("a1", "first", 0), 100);
  ar.archive(mkEntry("a2", "second", 1), 101);
  TEST_ASSERT_EQUAL_INT(2, ar.size());
  ar.archive(mkEntry("a3", "third", 2), 102);   // over cap -> drop oldest (a1)
  TEST_ASSERT_EQUAL_INT(2, ar.size());
  auto all = ar.getAll();   // FIFO order: oldest first
  TEST_ASSERT_EQUAL_STRING("second", all[0].content.c_str());
  TEST_ASSERT_EQUAL_STRING("third", all[1].content.c_str());
}

static void test_same_id_replaces_and_moves_to_newest() {
  VectorArchive ar;
  ar.configure(8);
  ar.archive(mkEntry("a1", "old text", 0), 100);
  ar.archive(mkEntry("a2", "other", 1), 101);
  ar.archive(mkEntry("a1", "new text", 0), 102);   // same id -> replace, move to back
  TEST_ASSERT_EQUAL_INT(2, ar.size());
  auto all = ar.getAll();
  TEST_ASSERT_EQUAL_STRING("other", all[0].content.c_str());
  TEST_ASSERT_EQUAL_STRING("new text", all[1].content.c_str());
}

// ---- search: nearest by cosine, ns-scoped ----------------------------------
static void test_search_finds_nearest() {
  VectorArchive ar;
  ar.configure(8);
  ar.archive(mkEntry("a1", "coffee is a flat white", 3), 100);
  ar.archive(mkEntry("a2", "ships on friday", 5), 101);
  auto hits = ar.search(axisVec(8, 3), 5);
  TEST_ASSERT_TRUE(hits.size() >= 1);
  TEST_ASSERT_EQUAL_STRING("a1", hits[0].id.c_str());   // axis 3 is the coffee vector
}

static void test_search_respects_namespace() {
  VectorArchive ar;
  ar.configure(8);
  ar.archive(mkEntry("o1", "owner fact", 2, "owner"), 100);
  ar.archive(mkEntry("m1", "member fact", 2, "chat:m1"), 101);
  // The member may only see its own namespace.
  auto memberHits = ar.search(axisVec(8, 2), 5, {"chat:m1"});
  TEST_ASSERT_EQUAL_INT(1, (int)memberHits.size());
  TEST_ASSERT_EQUAL_STRING("m1", memberHits[0].id.c_str());
  // Unscoped (admin/maintenance) sees both.
  auto allHits = ar.search(axisVec(8, 2), 5, {});
  TEST_ASSERT_EQUAL_INT(2, (int)allHits.size());
}

// ---- take/visibility/remove -------------------------------------------------
static void test_take_removes_and_returns() {
  VectorArchive ar;
  ar.configure(8);
  ar.archive(mkEntry("a1", "restore me", 4), 100);
  VecEntry out;
  TEST_ASSERT_TRUE(ar.take("a1", out, {"owner"}));
  TEST_ASSERT_EQUAL_STRING("restore me", out.content.c_str());
  TEST_ASSERT_EQUAL_INT(8, (int)out.vec.size());   // embedding came back
  TEST_ASSERT_EQUAL_INT(0, ar.size());             // gone from the archive
}

static void test_take_denied_across_namespace() {
  VectorArchive ar;
  ar.configure(8);
  ar.archive(mkEntry("o1", "owner secret", 2, "owner"), 100);
  VecEntry out;
  TEST_ASSERT_FALSE(ar.take("o1", out, {"chat:m1"}));   // member can't take owner's
  TEST_ASSERT_EQUAL_INT(1, ar.size());                  // still there
  TEST_ASSERT_FALSE(ar.idVisible("o1", {"chat:m1"}));
  TEST_ASSERT_TRUE(ar.idVisible("o1", {"owner"}));
}

// ---- dirty tracking + serialize round-trip ----------------------------------
static void test_dirty_tracking() {
  VectorArchive ar;
  ar.configure(8);
  TEST_ASSERT_FALSE(ar.dirty());
  ar.archive(mkEntry("a1", "x", 0), 100);
  TEST_ASSERT_TRUE(ar.dirty());
  ar.markClean();
  TEST_ASSERT_FALSE(ar.dirty());
  VecEntry out;
  ar.take("a1", out, {});
  TEST_ASSERT_TRUE(ar.dirty());   // take mutates too
}

static void test_serialize_roundtrip() {
  VectorArchive ar;
  ar.configure(8);
  VecEntry e = mkEntry("a1", "durable archived fact", 3, "chat:m1");
  e.importance = 0.42f;
  e.ttlHours = 504;
  e.createdAtHours = 77;
  e.source = "self";
  ar.archive(e, 999);

  std::string blob = ar.serialize();
  VectorArchive ar2;
  TEST_ASSERT_TRUE(ar2.deserialize(blob));
  TEST_ASSERT_EQUAL_INT(8, ar2.dims());
  auto all = ar2.getAll();
  TEST_ASSERT_EQUAL_INT(1, (int)all.size());
  TEST_ASSERT_EQUAL_STRING("durable archived fact", all[0].content.c_str());
  TEST_ASSERT_EQUAL_STRING("chat:m1", all[0].ns.c_str());
  TEST_ASSERT_EQUAL_STRING("self", all[0].source.c_str());
  TEST_ASSERT_EQUAL_INT(504, all[0].ttlHours);
  TEST_ASSERT_EQUAL_INT(77, (int)all[0].createdAtHours);
  TEST_ASSERT_EQUAL_INT(100, all[0].vec[3]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.42f, all[0].importance);
}

static void test_deserialize_garbage_is_safe() {
  VectorArchive ar;
  ar.configure(8);
  TEST_ASSERT_FALSE(ar.deserialize("not a real blob"));
  TEST_ASSERT_EQUAL_INT(0, ar.size());
  TEST_ASSERT_FALSE(ar.deserialize(""));
  TEST_ASSERT_EQUAL_INT(0, ar.size());
}

// ---- the sink: VectorMemory::pruneExpired MOVES expired entries here --------
static void test_prune_moves_expired_into_sink() {
  VectorMemory vm;
  vm.configure(8);
  VectorArchive ar;
  ar.configure(8);
  vm.setArchiveSink(&ar);

  VecEntry live = mkEntry("m1", "old fact", 1);
  live.createdAtHours = 0;
  live.ttlHours = 10;          // expires by hour 10
  vm.add(live);
  VecEntry fresh = mkEntry("m2", "fresh fact", 2);
  fresh.createdAtHours = 100;
  fresh.ttlHours = 500;        // still alive at hour 120
  vm.add(fresh);

  int pruned = vm.pruneExpired(120);
  TEST_ASSERT_EQUAL_INT(1, pruned);          // only the old one expired
  TEST_ASSERT_EQUAL_INT(1, vm.size());       // fresh one remains live
  TEST_ASSERT_EQUAL_INT(1, ar.size());       // old one moved to the archive
  auto all = ar.getAll();
  TEST_ASSERT_EQUAL_STRING("old fact", all[0].content.c_str());
  TEST_ASSERT_EQUAL_INT(100, all[0].vec[1]);  // embedding preserved through the move
}

static void test_prune_without_sink_drops() {
  VectorMemory vm;
  vm.configure(8);
  // No sink attached (the no-SD device) - expired entries are dropped as before.
  VecEntry live = mkEntry("m1", "old fact", 1);
  live.createdAtHours = 0;
  live.ttlHours = 10;
  vm.add(live);
  int pruned = vm.pruneExpired(120);
  TEST_ASSERT_EQUAL_INT(1, pruned);
  TEST_ASSERT_EQUAL_INT(0, vm.size());
}

// ---- full lifecycle: live -> expire -> archive -> restore-shaped take -------
static void test_expire_archive_restore_lifecycle() {
  VectorMemory vm;
  vm.configure(8);
  VectorArchive ar;
  ar.configure(8);
  vm.setArchiveSink(&ar);

  VecEntry e = mkEntry("m1", "the museum closes at 5pm", 4);
  e.createdAtHours = 0;
  e.ttlHours = 24;
  vm.add(e);

  // It ages out and is archived (not lost).
  TEST_ASSERT_EQUAL_INT(1, vm.pruneExpired(100));
  TEST_ASSERT_EQUAL_INT(0, vm.size());
  TEST_ASSERT_EQUAL_INT(1, ar.size());

  // A normal recall/search over the LIVE store can't see it any more.
  auto liveHits = vm.search(axisVec(8, 4), 5, 100);
  TEST_ASSERT_EQUAL_INT(0, (int)liveHits.size());

  // But an archive search finds it, and take() brings the embedding back.
  auto arHits = ar.search(axisVec(8, 4), 5, {"owner"});
  TEST_ASSERT_EQUAL_INT(1, (int)arHits.size());
  VecEntry restored;
  TEST_ASSERT_TRUE(ar.take("m1", restored, {"owner"}));
  restored.createdAtHours = 100;   // restore resets the clock
  restored.ttlHours = 504;
  vm.add(restored, /*dedup=*/false);
  TEST_ASSERT_EQUAL_INT(1, vm.size());
  TEST_ASSERT_EQUAL_INT(0, ar.size());
  // It's live again and recallable.
  auto again = vm.search(axisVec(8, 4), 5, 100);
  TEST_ASSERT_EQUAL_INT(1, (int)again.size());
  TEST_ASSERT_EQUAL_STRING("the museum closes at 5pm", again[0].content.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_archive_stores_and_preserves_embedding);
  RUN_TEST(test_wrong_width_rejected);
  RUN_TEST(test_fifo_cap_evicts_oldest);
  RUN_TEST(test_same_id_replaces_and_moves_to_newest);
  RUN_TEST(test_search_finds_nearest);
  RUN_TEST(test_search_respects_namespace);
  RUN_TEST(test_take_removes_and_returns);
  RUN_TEST(test_take_denied_across_namespace);
  RUN_TEST(test_dirty_tracking);
  RUN_TEST(test_serialize_roundtrip);
  RUN_TEST(test_deserialize_garbage_is_safe);
  RUN_TEST(test_prune_moves_expired_into_sink);
  RUN_TEST(test_prune_without_sink_drops);
  RUN_TEST(test_expire_archive_restore_lifecycle);
  return UNITY_END();
}
