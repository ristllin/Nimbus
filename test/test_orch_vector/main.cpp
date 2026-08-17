#include <unity.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/psram_alloc.h"
#include "nimbus/orch/vector_memory.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// Little-endian blob writers matching orch_vector_memory.cpp's format, so a test can
// hand-craft a CORRUPT blob the public serialize() can't produce.
static void bpu16(std::string& s, uint16_t v) { s.push_back((char)(v & 0xff)); s.push_back((char)((v >> 8) & 0xff)); }
static void bpu32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xff)); }
static void bpf32(std::string& s, float f) { uint32_t u; std::memcpy(&u, &f, 4); bpu32(s, u); }
static void bpstr(std::string& s, const std::string& v) { bpu16(s, (uint16_t)v.size()); s += v; }
static void bpentry(std::string& s, const std::string& id, int vecLen) {
  bpf32(s, 0.5f); bpu32(s, 720); bpu32(s, 0); s.push_back(0);
  bpstr(s, id); bpstr(s, "content"); bpstr(s, "system");
  bpu16(s, (uint16_t)vecLen);
  for (int i = 0; i < vecLen; i++) s.push_back((char)(i + 1));
}

// R7 review fix: deserialize must NOT store a vec whose width != the header dims_, or
// search/dedup/findNearest (which read dims_ bytes/vec) would read out of bounds. A
// clean prefix is kept; the corrupt entry + everything after is dropped; returns false.
static void test_deserialize_rejects_width_mismatch_no_oob() {
  std::string blob;
  blob.append("VM1", 3);
  bpu16(blob, 4);          // header dims = 4
  bpu32(blob, 2);          // 2 entries
  bpentry(blob, "good", 4);  // width 4 == dims -> valid, kept
  bpentry(blob, "bad", 2);   // width 2 != dims -> corrupt, must be rejected

  VectorMemory vm;
  bool ok = vm.deserialize(blob);
  TEST_ASSERT_FALSE(ok);              // corruption reported
  TEST_ASSERT_EQUAL(1, vm.size());    // clean prefix kept, bad-width entry NOT stored
  // No out-of-bounds read on the width-4 survivor.
  TEST_ASSERT_EQUAL(1, (int)vm.search(std::vector<int8_t>(4, (int8_t)1), 5).size());
  TEST_ASSERT_EQUAL(0, vm.deduplicate());
}

// R7-B1: the working set routes through the installable allocator hook - the seam the
// device overrides to put the VDB in PSRAM instead of internal SRAM. Proven here with
// a counting hook (host default = malloc); the vec buffers + entries array must flow
// through it, and recall must stay correct across the custom allocator.
static long g_wsBytes = 0;
static int  g_wsCalls = 0;
static void* wsCountingAlloc(std::size_t n) { g_wsCalls++; g_wsBytes += (long)n; return std::malloc(n); }
static void  wsCountingFree(void* p) { std::free(p); }

static void test_working_allocator_seam_routes_vectors() {
  g_wsBytes = 0; g_wsCalls = 0;
  setWorkingAllocators(wsCountingAlloc, wsCountingFree);
  {
    VectorMemory vm;
    vm.configure(8);
    for (int i = 0; i < 20; i++) {
      VecEntry e;
      e.id = "m" + std::to_string(i);
      e.content = "c";
      e.vec = std::vector<int8_t>(8, (int8_t)(i + 1));
      vm.add(e, /*dedup*/ false);
    }
    TEST_ASSERT_EQUAL(20, vm.size());
    // The 8-byte int8 vec buffers + the growing entries array went through OUR hook.
    TEST_ASSERT_TRUE(g_wsCalls > 0);
    TEST_ASSERT_TRUE(g_wsBytes >= 20 * 8);
    // Round-trip correctness is unaffected by the custom allocator: a distinctive
    // query (one axis dominant) recalls the entry whose vec matches that axis.
    VecEntry needle;
    needle.id = "needle";
    needle.content = "n";
    needle.vec = {120, 0, 0, 0, 0, 0, 0, 0};
    vm.add(needle, /*dedup*/ false);
    auto hits = vm.search(std::vector<int8_t>{100, 0, 0, 0, 0, 0, 0, 0}, 3);
    TEST_ASSERT_EQUAL(3, (int)hits.size());
    TEST_ASSERT_EQUAL_STRING("needle", hits[0].id.c_str());  // nearest by direction
  }  // vm destructs here (frees via wsCountingFree) BEFORE we restore the default
  setWorkingAllocators(nullptr, nullptr);  // restore malloc/free for the other tests
}

// 4-dim int8 vectors for easy reasoning about cosine geometry.
static std::vector<int8_t> v4(int a, int b, int c, int d) {
  return std::vector<int8_t>{(int8_t)a, (int8_t)b, (int8_t)c, (int8_t)d};
}
static VecEntry mk(const std::string& id, std::vector<int8_t> vec, float imp = 0.5f) {
  VecEntry e; e.id = id; e.content = "c-" + id; e.importance = imp; e.vec = std::move(vec);
  return e;
}
static VectorMemory make4() { VectorMemory m; m.configure(4); return m; }

// ---- quantize + cosine ------------------------------------------------------

static void test_quantize_range() {
  auto q = VectorMemory::quantize({1.0f, -1.0f, 0.5f, -2.0f});
  TEST_ASSERT_EQUAL_INT8(127, q[0]);
  TEST_ASSERT_EQUAL_INT8(-127, q[1]);
  TEST_ASSERT_EQUAL_INT8(64, q[2]);   // round(0.5*127)=64
  TEST_ASSERT_EQUAL_INT8(-127, q[3]); // clamped
}

static void test_cosine_geometry() {
  // same direction -> distance 0 (magnitude ignored)
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.0f, cosineDistanceI8(v4(100,0,0,0), v4(50,0,0,0)));
  // orthogonal -> distance 1
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 1.0f, cosineDistanceI8(v4(100,0,0,0), v4(0,100,0,0)));
  // opposite -> distance 2
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 2.0f, cosineDistanceI8(v4(100,0,0,0), v4(-100,0,0,0)));
  // zero vector -> distance 1 (no direction), never NaN
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 1.0f, cosineDistanceI8(v4(0,0,0,0), v4(100,0,0,0)));
}

// ---- add + dedup ------------------------------------------------------------

static void test_add_and_width_enforced() {
  VectorMemory m = make4();
  TEST_ASSERT_TRUE(m.add(mk("a", v4(100,0,0,0))));
  TEST_ASSERT_EQUAL_INT(1, m.size());
  // wrong width rejected
  VecEntry bad = mk("bad", std::vector<int8_t>{1,2,3});
  TEST_ASSERT_FALSE(m.add(bad));
  TEST_ASSERT_EQUAL_INT(1, m.size());
}

static void test_dedup_skips_and_bumps_importance() {
  VectorMemory m = make4();
  m.add(mk("a", v4(100,0,0,0), 0.4f));
  // near-identical direction, higher importance -> skipped, but bumps existing
  bool inserted = m.add(mk("a2", v4(60,0,0,0), 0.9f));
  TEST_ASSERT_FALSE(inserted);
  TEST_ASSERT_EQUAL_INT(1, m.size());
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.9f, m.getAll()[0].importance);  // bumped to max
}

static void test_non_duplicate_inserts() {
  VectorMemory m = make4();
  m.add(mk("a", v4(100,0,0,0)));
  TEST_ASSERT_TRUE(m.add(mk("b", v4(0,100,0,0))));  // orthogonal, distinct
  TEST_ASSERT_EQUAL_INT(2, m.size());
}

// ---- search -----------------------------------------------------------------

static void test_search_topk_ordered() {
  VectorMemory m = make4();
  m.add(mk("x", v4(100,0,0,0)));
  m.add(mk("y", v4(0,100,0,0)));
  m.add(mk("z", v4(-100,0,0,0)));
  auto hits = m.search(v4(90,10,0,0), 2);  // closest to x
  TEST_ASSERT_EQUAL_INT(2, (int)hits.size());
  TEST_ASSERT_EQUAL_STRING("x", hits[0].id.c_str());  // nearest first
  TEST_ASSERT_TRUE(hits[0].distance <= hits[1].distance);
}

static void test_search_empty_and_bad_width() {
  VectorMemory m = make4();
  TEST_ASSERT_EQUAL_INT(0, (int)m.search(v4(1,0,0,0), 5).size());  // empty store
  m.add(mk("a", v4(100,0,0,0)));
  TEST_ASSERT_EQUAL_INT(0, (int)m.search(std::vector<int8_t>{1,2}, 5).size());  // bad width
}

// ---- decay + prune (decay/prune rules) -------------------------------------

static void test_decay_respects_permanent_and_floor() {
  VectorMemory m = make4();
  m.add(mk("a", v4(100,0,0,0), 0.5f));
  VecEntry perm = mk("p", v4(0,100,0,0), 0.5f); perm.permanentFlag = true;
  m.add(perm);
  m.decayImportance(0.9f);
  auto all = m.getAll();
  float impA = 0, impP = 0;
  for (auto& e : all) { if (e.id=="a") impA=e.importance; if (e.id=="p") impP=e.importance; }
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.45f, impA);   // decayed
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.5f, impP);    // permanent unchanged
}

static void test_prune_ttl_and_low_importance() {
  VectorMemory m = make4();
  VecEntry old = mk("old", v4(100,0,0,0), 0.5f); old.createdAtHours = 0; old.ttlHours = 10;
  VecEntry fresh = mk("fresh", v4(0,100,0,0), 0.5f); fresh.createdAtHours = 15; fresh.ttlHours = 10;
  VecEntry weak = mk("weak", v4(0,0,100,0), 0.02f); weak.ttlHours = -1;  // below MIN_IMPORTANCE
  m.add(old); m.add(fresh); m.add(weak);
  int removed = m.pruneExpired(/*nowHours=*/20);
  TEST_ASSERT_EQUAL_INT(2, removed);           // old (age 20>10) + weak (imp<0.05)
  TEST_ASSERT_EQUAL_INT(1, m.size());          // fresh survives (age 5<10)
  TEST_ASSERT_EQUAL_STRING("fresh", m.getAll()[0].id.c_str());
}

static void test_prune_exempts_permanent_and_creator() {
  VectorMemory m = make4();
  VecEntry perm = mk("perm", v4(100,0,0,0), 0.01f); perm.permanentFlag = true; perm.createdAtHours=0; perm.ttlHours=1;
  VecEntry creator = mk("cre", v4(0,100,0,0), 0.01f); creator.creatorFlag = true; creator.createdAtHours=0; creator.ttlHours=1;
  m.add(perm); m.add(creator);
  TEST_ASSERT_EQUAL_INT(0, m.pruneExpired(9999));  // both exempt despite low imp + past TTL
  TEST_ASSERT_EQUAL_INT(2, m.size());
}

// ---- admin: markPermanent / flush / dedupe ---------------------------------

static void test_mark_permanent_and_flush_variants() {
  VectorMemory m = make4();
  m.add(mk("a", v4(100,0,0,0)));
  m.add(mk("b", v4(0,100,0,0)));
  TEST_ASSERT_TRUE(m.markPermanent("a"));
  TEST_ASSERT_EQUAL_INT(1, m.flushNonPermanent());  // b gone, a kept
  TEST_ASSERT_EQUAL_INT(1, m.size());
  TEST_ASSERT_EQUAL_INT(1, m.flushAll());
  TEST_ASSERT_EQUAL_INT(0, m.size());
}

static void test_mark_temporary_unpins() {
  VectorMemory m = make4();
  m.add(mk("a", v4(100,0,0,0)));
  TEST_ASSERT_TRUE(m.markPermanent("a"));
  TEST_ASSERT_EQUAL_INT(0, m.flushNonPermanent());  // pinned -> survives
  TEST_ASSERT_TRUE(m.markTemporary("a"));            // unpin
  TEST_ASSERT_EQUAL_INT(1, m.flushNonPermanent());  // no longer permanent -> evictable
  TEST_ASSERT_EQUAL_INT(0, m.size());
  TEST_ASSERT_FALSE(m.markTemporary("nope"));        // unknown id -> false
}

static void test_getall_sorted_by_importance() {
  VectorMemory m = make4();
  m.add(mk("lo", v4(100,0,0,0), 0.2f));
  m.add(mk("hi", v4(0,100,0,0), 0.9f));
  auto all = m.getAll();
  TEST_ASSERT_EQUAL_STRING("hi", all[0].id.c_str());  // importance-desc
}

static void test_deduplicate_keeps_higher_importance() {
  VectorMemory m = make4();
  m.add(mk("a", v4(100,0,0,0), 0.3f));
  m.add(mk("b", v4(0,100,0,0), 0.5f));            // distinct
  // inject a near-dup of 'a' with dedup disabled, higher importance
  m.add(mk("a_dup", v4(60,0,0,0), 0.8f), /*dedup=*/false);
  TEST_ASSERT_EQUAL_INT(3, m.size());
  int removed = m.deduplicate();
  TEST_ASSERT_EQUAL_INT(1, removed);              // one of the a-twins dropped
  TEST_ASSERT_EQUAL_INT(2, m.size());
  // the surviving a-twin must be the higher-importance one (0.8)
  bool hasHi = false;
  for (auto& e : m.getAll()) if (std::fabs(e.importance - 0.8f) < 1e-4) hasHi = true;
  TEST_ASSERT_TRUE(hasHi);
}

// ---- persistence round-trip -------------------------------------------------
static void test_serialize_roundtrip() {
  VectorMemory m = make4();
  VecEntry a = mk("alpha", v4(100, 20, -30, 5), 0.7f);
  a.content = "the quick brown fox"; a.source = "user"; a.ttlHours = 240; a.createdAtHours = 42;
  a.permanentFlag = true;
  m.add(a);
  m.add(mk("beta", v4(0, 100, 0, 0), 0.3f));

  std::string blob = m.serialize();
  VectorMemory m2; m2.configure(4);
  TEST_ASSERT_TRUE(m2.deserialize(blob));
  TEST_ASSERT_EQUAL_INT(4, m2.dims());
  TEST_ASSERT_EQUAL_INT(2, m2.size());
  auto all = m2.getAll();  // importance-desc -> alpha (0.7) first
  TEST_ASSERT_EQUAL_STRING("alpha", all[0].id.c_str());
  TEST_ASSERT_EQUAL_STRING("the quick brown fox", all[0].content.c_str());
  TEST_ASSERT_EQUAL_STRING("user", all[0].source.c_str());
  TEST_ASSERT_TRUE(all[0].permanentFlag);
  TEST_ASSERT_EQUAL_INT(240, all[0].ttlHours);
  TEST_ASSERT_EQUAL_UINT32(42, all[0].createdAtHours);
  TEST_ASSERT_EQUAL_INT8(100, all[0].vec[0]);
  TEST_ASSERT_EQUAL_INT8(-30, all[0].vec[2]);
  TEST_ASSERT_EQUAL_STRING("beta", m2.search(v4(0, 90, 0, 0), 1)[0].id.c_str());  // recall works
}

static void test_deserialize_rejects_garbage() {
  VectorMemory m; m.configure(4);
  TEST_ASSERT_FALSE(m.deserialize("not a vm blob"));
  TEST_ASSERT_EQUAL_INT(0, m.size());
  TEST_ASSERT_FALSE(m.deserialize(""));
  std::string hdr = "VM1"; hdr.push_back(4); hdr.push_back(0);  // dims=4 then EOF (no count)
  TEST_ASSERT_FALSE(m.deserialize(hdr));
}

static void test_empty_store_roundtrip() {
  VectorMemory m; m.configure(128);
  std::string blob = m.serialize();
  VectorMemory m2; m2.configure(4);
  TEST_ASSERT_TRUE(m2.deserialize(blob));
  TEST_ASSERT_EQUAL_INT(128, m2.dims());  // dims restored from header
  TEST_ASSERT_EQUAL_INT(0, m2.size());
}

// ---- max-entries cap + score eviction (UX2) --------------------------------
static VecEntry mkFull(const std::string& id, std::vector<int8_t> v, float imp,
                       int32_t ttl, uint32_t cr) {
  VecEntry e; e.id = id; e.content = "c-" + id; e.importance = imp;
  e.vec = std::move(v); e.ttlHours = ttl; e.createdAtHours = cr; return e;
}

static void test_cap_evicts_lowest_retention_score() {
  VectorMemory m = make4();
  m.setMaxEntries(3);
  m.add(mkFull("hi",  v4(100,0,0,0), 0.9f, 720, 0));
  m.add(mkFull("mid", v4(0,100,0,0), 0.5f, 720, 0));
  m.add(mkFull("lo",  v4(0,0,100,0), 0.1f, 720, 0));  // lowest importance -> lowest score
  TEST_ASSERT_EQUAL_INT(3, m.size());
  m.add(mkFull("new", v4(0,0,0,100), 0.5f, 720, 100)); // at cap -> evict "lo"
  TEST_ASSERT_EQUAL_INT(3, m.size());
  bool hasLo = false, hasNew = false;
  for (const auto& e : m.getAll()) { if (e.id == "lo") hasLo = true; if (e.id == "new") hasNew = true; }
  TEST_ASSERT_FALSE(hasLo);
  TEST_ASSERT_TRUE(hasNew);
}

static void test_cap_never_evicts_permanent() {
  VectorMemory m = make4();
  m.setMaxEntries(2);
  VecEntry p = mkFull("perm", v4(100,0,0,0), 0.1f, 720, 0); p.permanentFlag = true;
  m.add(p);
  m.add(mkFull("a", v4(0,100,0,0), 0.9f, 720, 0));   // higher importance, NOT permanent
  m.add(mkFull("b", v4(0,0,100,0), 0.5f, 720, 1));   // at cap -> evicts "a" (perm exempt)
  bool hasPerm = false, hasA = false;
  for (const auto& e : m.getAll()) { if (e.id == "perm") hasPerm = true; if (e.id == "a") hasA = true; }
  TEST_ASSERT_TRUE(hasPerm);   // pinned data survives even at the lowest importance
  TEST_ASSERT_FALSE(hasA);
}

static void test_boost_accessed_bumps_and_resets_ttl() {
  VectorMemory m = make4();
  m.add(mkFull("a", v4(100,0,0,0), 0.5f, 720, 10));
  int n = m.boostAccessed({"a", "missing"}, 0.3f, 100);
  TEST_ASSERT_EQUAL_INT(1, n);
  auto all = m.getAll();
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.8f, all[0].importance);        // 0.5 + 0.3
  TEST_ASSERT_EQUAL_UINT32(100, all[0].createdAtHours);           // TTL clock reset
  m.boostAccessed({"a"}, 0.5f, 0);                                // clamps at 1.0
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 1.0f, m.getAll()[0].importance);
}

// ---- composite recall() -----------------------------------------------------

static RecallParams rp(int k) { RecallParams p; p.k = k; return p; }

// Query-time TTL: an expired entry (age > ttl) is not recalled even before prune.
static void test_recall_skips_expired_at_query_time() {
  VectorMemory m = make4();
  m.add(mkFull("fresh", v4(100,0,0,0), 0.6f, 100, 1000));   // age 0 at now=1000
  m.add(mkFull("stale", v4(0,0,100,0), 0.6f, 100, 800));    // distinct dir; age 200 > ttl 100
  auto hits = m.recall(v4(100,0,0,0), rp(5), /*now*/1000);
  TEST_ASSERT_EQUAL_INT(1, (int)hits.size());              // stale filtered at query time
  TEST_ASSERT_EQUAL_STRING("fresh", hits[0].id.c_str());
  TEST_ASSERT_EQUAL_INT(2, m.size());   // recall does NOT delete (that is prune's job)
}

// Recency breaks a sim/importance tie: newer beats older when sim+importance equal.
// Vectors are >0.05 apart (survive dedup) and >0.10 apart (survive collapse), yet
// symmetric about the query so their similarity is equal.
static void test_recall_recency_breaks_tie() {
  VectorMemory m = make4();
  m.add(mkFull("old", v4(100, 25,0,0), 0.5f, -1, 0));    // older
  m.add(mkFull("new", v4(100,-25,0,0), 0.5f, -1, 950));  // newer, equal sim to query
  auto hits = m.recall(v4(100,0,0,0), rp(2), /*now*/1000);
  TEST_ASSERT_EQUAL_INT(2, (int)hits.size());
  TEST_ASSERT_EQUAL_STRING("new", hits[0].id.c_str());   // recency wins the ordering
  TEST_ASSERT_TRUE(hits[0].score > hits[1].score);
}

// Near-duplicates collapse: two ~identical vectors yield one hit.
static void test_recall_collapses_near_duplicates() {
  VectorMemory m = make4();
  m.add(mkFull("a",  v4(100,0,0,0), 0.6f, -1, 1000));
  m.add(mkFull("a2", v4(99, 1,0,0), 0.6f, -1, 1000));   // within kQueryDupDist of a
  m.add(mkFull("b",  v4(0,100,0,0), 0.6f, -1, 1000));   // distinct direction
  auto hits = m.recall(v4(100,0,0,0), rp(5), 1000);
  // a (or a2) appears once; the twin is collapsed. b may also appear (distinct).
  int aCount = 0;
  for (auto& h : hits) if (h.id == "a" || h.id == "a2") aCount++;
  TEST_ASSERT_EQUAL_INT(1, aCount);
}

// Relevance threshold filters out below-similarity hits.
static void test_recall_relevance_threshold() {
  VectorMemory m = make4();
  m.add(mkFull("hit",  v4(100,0,0,0), 0.6f, -1, 1000));
  m.add(mkFull("orth", v4(0,100,0,0), 0.6f, -1, 1000));   // sim 0 to the query
  RecallParams p = rp(5); p.relevanceThreshold = 0.5f;
  auto hits = m.recall(v4(100,0,0,0), p, 1000);
  TEST_ASSERT_EQUAL_INT(1, (int)hits.size());
  TEST_ASSERT_EQUAL_STRING("hit", hits[0].id.c_str());
}


// ---- MMR (Release C1: mutation-resistant after prism proved v1 vacuous) ----
// Geometry (numerically derived; see the C-review): q=(100,50,20,0),
// A=(70,80,20,0) simA=.9279, B=(45,30,40,0) simB=.8900, C=(40,0,0,0) simC=.8805.
// - dist(A,B)=.127 > kQueryDupDist .10  -> the dup-collapse can NOT do MMR's job
// - composite spread (simA-simB)*impComp(.8) = .030 < kTieEpsilon .05 -> gate ON
// - MMR slot 2: val(C)=.299 > val(B)=.237 -> C, while plain top-2 picks B.
// Entries are added with dedup=false (v1's cluster was silently swallowed by
// add-time dedup, leaving a 2-entry store where any code path passed).
static void test_mmr_diversifies_on_tie() {
  VectorMemory m = make4();
  m.add(mkFull("A", v4(70, 80, 20, 0), 0.6f, 0, 1000), /*dedup*/false);
  m.add(mkFull("B", v4(45, 30, 40, 0), 0.6f, 0, 1000), false);
  m.add(mkFull("C", v4(40, 0, 0, 0),   0.6f, 0, 1000), false);
  auto hits = m.recall(v4(100, 50, 20, 0), rp(2), /*now*/1000);
  TEST_ASSERT_EQUAL_INT(2, (int)hits.size());
  TEST_ASSERT_EQUAL_STRING("A", hits[0].id.c_str());   // best stays best
  // The LOAD-BEARING assertion: MMR picks the DISTINCT entry where plain
  // top-2 would pick B (simB > simC) - kill the MMR term and this fails.
  TEST_ASSERT_EQUAL_STRING("C", hits[1].id.c_str());
}

// A CLEAR winner (composite spread >= epsilon) keeps plain score order: with
// k=2 the gate condition is genuinely evaluated (v1 used k=1, where neither the
// gate nor the penalty is reachable) and the runner-up follows raw score.
static void test_mmr_gate_stays_off_for_clear_winner() {
  VectorMemory m = make4();
  m.add(mkFull("exact", v4(100, 0, 0, 0), 0.6f, 0, 1000), false);
  m.add(mkFull("far",   v4(60, 80, 0, 0), 0.6f, 0, 1000), false);   // sim .6
  m.add(mkFull("faroff", v4(0, 60, 80, 0), 0.6f, 0, 1000), false);  // sim ~0
  auto hits = m.recall(v4(100, 0, 0, 0), rp(2), 1000);
  TEST_ASSERT_EQUAL_INT(2, (int)hits.size());
  TEST_ASSERT_EQUAL_STRING("exact", hits[0].id.c_str());
  TEST_ASSERT_EQUAL_STRING("far", hits[1].id.c_str());   // plain order preserved
}

// ---- windowed dedup (Release C2) --------------------------------------------
static void test_deduplicate_window_limits_scan() {
  VectorMemory m = make4();
  // Two near-dup pairs: one OLD (insertion order first), one NEW (last).
  m.add(mkFull("oldA", v4(100, 0, 0, 0), 0.5f, 0, 10), /*dedup*/false);
  m.add(mkFull("oldB", v4(100, 1, 0, 0), 0.4f, 0, 10), false);
  m.add(mkFull("mid",  v4(0, 100, 0, 0), 0.5f, 0, 10), false);
  m.add(mkFull("newA", v4(0, 0, 100, 0), 0.5f, 0, 10), false);
  m.add(mkFull("newB", v4(0, 0, 100, 1), 0.4f, 0, 10), false);
  // Window covers only the newest 3 -> the NEW pair collapses, the OLD survives.
  int removed = m.deduplicate(3);
  TEST_ASSERT_EQUAL_INT(1, removed);
  TEST_ASSERT_EQUAL_INT(4, m.size());
  bool oldA=false, oldB=false;
  for (auto& e : m.getAll()) { if (e.id=="oldA") oldA=true; if (e.id=="oldB") oldB=true; }
  TEST_ASSERT_TRUE(oldA); TEST_ASSERT_TRUE(oldB);   // outside the window: untouched
  // Full scan (0) then also collapses the old pair.
  TEST_ASSERT_EQUAL_INT(1, m.deduplicate(0));
  TEST_ASSERT_EQUAL_INT(3, m.size());
}


// ---- last-recall usage stamp + optional trailing codec block ----------------

static void test_last_recall_stamped_on_hits_only() {
  VectorMemory m = make4();
  m.add(mk("alpha", v4(100, 0, 0, 0), 0.7f));
  m.add(mk("beta",  v4(0, 100, 0, 0), 0.6f));
  // clockless probe (nowHours=0, the dedup path) must NOT stamp
  (void)m.search(v4(100, 0, 0, 0), 1, 0);
  TEST_ASSERT_EQUAL_UINT32(0, m.getAll()[0].lastRecallHours);
  // a real search stamps ONLY the returned top-k
  auto hits = m.search(v4(100, 0, 0, 0), 1, 500);
  TEST_ASSERT_EQUAL_INT(1, (int)hits.size());
  uint32_t a = 0, b = 0;
  for (const auto& e : m.getAll()) {
    if (e.id == "alpha") a = e.lastRecallHours;
    if (e.id == "beta")  b = e.lastRecallHours;
  }
  TEST_ASSERT_EQUAL_UINT32(500, a);   // the hit
  TEST_ASSERT_EQUAL_UINT32(0,   b);   // the also-ran
}

static void test_last_recall_blob_roundtrip_and_old_format() {
  VectorMemory m = make4();
  m.add(mk("alpha", v4(100, 0, 0, 0), 0.7f));
  m.add(mk("beta",  v4(0, 100, 0, 0), 0.6f));
  (void)m.search(v4(100, 0, 0, 0), 1, 500);   // stamp alpha (< the 720 h default TTL!)
  std::string blob = m.serialize();

  // round-trip carries the stamp
  VectorMemory m2; m2.configure(4);
  TEST_ASSERT_TRUE(m2.deserialize(blob));
  uint32_t a = 0;
  for (const auto& e : m2.getAll()) if (e.id == "alpha") a = e.lastRecallHours;
  TEST_ASSERT_EQUAL_UINT32(500, a);

  // an OLD-format blob (no trailing LR1 block) loads clean with stamps at 0
  const size_t lrBytes = 3 + 4 * m.size();
  std::string oldBlob = blob.substr(0, blob.size() - lrBytes);
  VectorMemory m3; m3.configure(4);
  TEST_ASSERT_TRUE(m3.deserialize(oldBlob));
  TEST_ASSERT_EQUAL_INT(2, m3.size());
  for (const auto& e : m3.getAll())
    TEST_ASSERT_EQUAL_UINT32(0, e.lastRecallHours);
}


static void test_restamp_heals_presync_lastrecall() {
  VectorMemory m = make4();
  m.add(mk("alpha", v4(100, 0, 0, 0), 0.7f));
  // simulate a recall stamped in the boot-relative window (tiny hour number)
  (void)m.search(v4(100, 0, 0, 0), 1, 5);   // lastRecallHours = 5 (bogus, pre-sync)
  // threshold like the real caller (anything below a real-epoch hour is pre-sync)
  m.restampPreSync(1000000, 470000);
  for (const auto& e : m.getAll())
    TEST_ASSERT_EQUAL_UINT32(0, e.lastRecallHours);   // reset to "never", not ~56y
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_mmr_diversifies_on_tie);
  RUN_TEST(test_mmr_gate_stays_off_for_clear_winner);
  RUN_TEST(test_deduplicate_window_limits_scan);
  RUN_TEST(test_recall_skips_expired_at_query_time);
  RUN_TEST(test_recall_recency_breaks_tie);
  RUN_TEST(test_recall_collapses_near_duplicates);
  RUN_TEST(test_recall_relevance_threshold);
  RUN_TEST(test_cap_evicts_lowest_retention_score);
  RUN_TEST(test_cap_never_evicts_permanent);
  RUN_TEST(test_boost_accessed_bumps_and_resets_ttl);
  RUN_TEST(test_quantize_range);
  RUN_TEST(test_cosine_geometry);
  RUN_TEST(test_add_and_width_enforced);
  RUN_TEST(test_dedup_skips_and_bumps_importance);
  RUN_TEST(test_non_duplicate_inserts);
  RUN_TEST(test_search_topk_ordered);
  RUN_TEST(test_search_empty_and_bad_width);
  RUN_TEST(test_decay_respects_permanent_and_floor);
  RUN_TEST(test_prune_ttl_and_low_importance);
  RUN_TEST(test_prune_exempts_permanent_and_creator);
  RUN_TEST(test_mark_permanent_and_flush_variants);
  RUN_TEST(test_mark_temporary_unpins);
  RUN_TEST(test_getall_sorted_by_importance);
  RUN_TEST(test_deduplicate_keeps_higher_importance);
  RUN_TEST(test_serialize_roundtrip);
  RUN_TEST(test_last_recall_stamped_on_hits_only);
  RUN_TEST(test_last_recall_blob_roundtrip_and_old_format);
  RUN_TEST(test_restamp_heals_presync_lastrecall);
  RUN_TEST(test_deserialize_rejects_garbage);
  RUN_TEST(test_empty_store_roundtrip);
  RUN_TEST(test_working_allocator_seam_routes_vectors);
  RUN_TEST(test_deserialize_rejects_width_mismatch_no_oob);
  return UNITY_END();
}
