// Degraded keyword recall allocates O(k), never O(N) (prism gate wave 1, item 5).
//
// While embeddings are parked or keyless, every turn's automatic recall and the
// memory.search fallback run a keyword scan over the live vector store. The scan
// used to iterate VectorMemory::getAll(), a deep copy of every entry with its vector
// bytes: on the device a PSRAM copy of the vectors plus one small internal-heap block
// per entry for its text, on every turn, exactly while the device may already be
// short of memory. The rule asserted here, on a 5,000-entry store:
//
//   - the default-heap allocations (internal SRAM on the device for small blocks)
//     depend on k, the hits returned, and never on the store size or on the length
//     of the pending queue (read by reference);
//   - the working-set allocations (PSRAM on the device) are bounded by the scan cap;
//   - the result is exactly what the copying scan returned (same cap, ranking, read
//     boundary, expiry and pending tags), checked against a reference copy of it;
//   - below the recall memory floors the scan is skipped and allocates nothing.
//
// This binary replaces the global operator new/delete to count allocations; that is
// why it is its own test directory.
#include <unity.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/orch/lexical.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/pending_embed.h"
#include "nimbus/orch/psram_alloc.h"

using namespace nimbus::orch;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

// ---- allocation counting (default heap and the working-set hooks) -------------
namespace {
bool g_armed = false;
long g_allocs = 0, g_bytes = 0, g_live = 0, g_peakLive = 0;   // default heap, while armed
long g_wsAllocs = 0, g_wsBytes = 0;                            // working-set hooks
constexpr uint64_t kMagic = 0x524543414c4cull;
struct alignas(16) Hdr {
  uint64_t magic;
  uint32_t size;
  uint32_t armed;
};

void* wsAlloc(std::size_t n) {
  if (g_armed) {
    g_wsAllocs++;
    g_wsBytes += (long)n;
  }
  return std::malloc(n);
}
void wsFree(void* p) { std::free(p); }

struct Measure {
  long allocs = 0, bytes = 0, peakLive = 0, wsAllocs = 0, wsBytes = 0;
};
template <typename Fn>
Measure measure(Fn&& fn) {
  g_allocs = g_bytes = g_live = g_peakLive = g_wsAllocs = g_wsBytes = 0;
  g_armed = true;
  fn();
  g_armed = false;
  Measure m;
  m.allocs = g_allocs;
  m.bytes = g_bytes;
  m.peakLive = g_peakLive;
  m.wsAllocs = g_wsAllocs;
  m.wsBytes = g_wsBytes;
  return m;
}
}  // namespace

void* operator new(std::size_t n) {
  Hdr* h = static_cast<Hdr*>(std::malloc(n + sizeof(Hdr)));
  if (!h) throw std::bad_alloc();
  h->magic = kMagic;
  h->size = (uint32_t)n;
  h->armed = g_armed ? 1u : 0u;
  if (g_armed) {
    g_allocs++;
    g_bytes += (long)n;
    g_live += (long)n;
    if (g_live > g_peakLive) g_peakLive = g_live;
  }
  return h + 1;
}
void operator delete(void* p) noexcept {
  if (!p) return;
  Hdr* h = static_cast<Hdr*>(p) - 1;
  if (h->magic != kMagic) {
    std::free(p);
    return;
  }
  if (h->armed && g_armed) g_live -= (long)h->size;
  h->magic = 0;
  std::free(h);
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

void setUp() { setWorkingAllocators(wsAlloc, wsFree); }
void tearDown() { setWorkingAllocators(nullptr, nullptr); }

// ---- a realistic full store ---------------------------------------------------
namespace {
constexpr int kDims = 256;            // 256 int8 bytes of vector per entry
constexpr uint32_t kNow = 500000;     // the expiry clock, in hours
// A member namespace longer than either standard library's short-string buffer (15
// bytes on the device, 22 on the host), so a per-entry copy of it would allocate.
const std::string kMemberNs = "chat:-100123456789012345678";
const char* const kTopics[] = {
    "my locker code is on the blue card", "the coffee order is a flat white",
    "dentist appointment moved to friday", "the blue door number is 41",
    "parking spot is on level two",       "the wifi router lives in the hall",
    "the gym locker is number 12"};

// The fixtures are built before Unity starts (a 5,000-entry store takes a moment),
// so a fixture failure stops the binary instead of asserting.
[[noreturn]] void fixtureFailed(const char* what) {
  std::fprintf(stderr, "fixture failed: %s\n", what);
  std::abort();
}

// Every entry's text is 80 bytes (over a short-string buffer, under the device's
// 128 B PSRAM spill threshold), so allocation sizes compare across stores.
std::string content(int i) {
  char buf[96];
  snprintf(buf, sizeof(buf), "fact %05d: %s", i, kTopics[i % 7]);
  std::string s(buf);
  s.resize(80, '.');
  return s;
}

// Importance with many ties, some decayed below the prune floor; some entries past
// their TTL; pins exempt from both; owner, legacy (empty) and member namespaces.
void fillStore(VectorMemory& vec, int n) {
  vec = VectorMemory();
  vec.configure(kDims);
  const float imps[] = {0.9f, 0.7f, 0.5f, 0.3f, 0.04f};
  for (int i = 0; i < n; ++i) {
    VecEntry e;
    e.id = "m" + std::to_string(i);
    e.content = content(i);
    e.importance = imps[(i * 7) % 5];
    e.ttlHours = i % 11 == 0 ? 12 : 720;
    e.createdAtHours = kNow - 100;
    e.ns = i % 3 == 0 ? kMemberNs : (i % 17 == 0 ? std::string() : std::string(kOwnerNs));
    e.permanentFlag = i % 13 == 0;
    e.vec.assign(kDims, (int8_t)(i % 100));
    if (!vec.add(e, /*dedup=*/false)) fixtureFailed("store add");
  }
}

void fillPending(PendingEmbeds& q, int n, const char* text) {
  q = PendingEmbeds();
  for (int i = 0; i < n; ++i) {
    PendingEmbed e;
    e.content = std::string(text) + " " + std::to_string(i);
    e.id = "p" + std::to_string(i);
    e.ns = i % 4 == 0 ? kMemberNs : std::string(kOwnerNs);
    e.createdAtHours = kNow - 1;
    e.clockSynced = true;
    if (q.enqueue(e) != PendingEmbeds::Add::Added) fixtureFailed("queue add");
  }
}

// ---- the copying scan the fix replaced, kept as the reference -----------------
bool refNsAllows(const std::string& ns, const std::vector<std::string>& allow) {
  if (allow.empty()) return true;
  const std::string eff = ns.empty() ? std::string(kOwnerNs) : ns;
  for (const auto& a : allow)
    if (a == eff) return true;
  return false;
}
bool refExpired(const VecEntry& e, uint32_t nowHours) {
  if (e.permanentFlag || e.creatorFlag) return false;
  if (e.importance < VectorMemory::kMinImportance) return true;
  if (e.ttlHours <= 0) return false;
  if (nowHours == 0) return false;
  const uint32_t age = nowHours >= e.createdAtHours ? nowHours - e.createdAtHours : 0;
  return (int64_t)age > (int64_t)e.ttlHours;
}
std::vector<std::string> referenceHits(const VectorMemory& vec, const PendingEmbeds* pending,
                                       const std::string& query, const RecallScope& scope) {
  const LexQuery lq = lexParse(query);
  std::vector<std::pair<int, std::string>> found;
  size_t scanned = 0;
  for (const auto& e : vec.getAll()) {
    if (++scanned > kLexScanMax) break;
    if (!refNsAllows(e.ns, scope.nsAllow) || refExpired(e, scope.nowHours)) continue;
    const int sc = lexCoverage(e.content, lq);
    if (sc > 0) found.push_back({sc, "[" + std::to_string((int)(e.importance * 100)) + "%] " + e.content});
  }
  if (pending)
    for (const auto& p : pending->getAll(scope.nsAllow)) {
      const int sc = lexCoverage(p.content, lq);
      if (sc > 0) found.push_back({sc, "[pending, not stored yet] " + p.content});
    }
  std::stable_sort(found.begin(), found.end(),
                   [](const std::pair<int, std::string>& x, const std::pair<int, std::string>& y) {
                     return x.first > y.first;
                   });
  std::vector<std::string> out;
  for (const auto& f : found) {
    if ((int)out.size() >= scope.k) break;
    out.push_back(f.second);
  }
  return out;
}

RecallScope scopeFor(std::vector<std::string> allow, int k, uint32_t now = kNow) {
  RecallScope s;
  s.nsAllow = std::move(allow);
  s.nowHours = now;
  s.k = k;
  return s;
}

// Fixtures the tests share (built once: a 5,000-entry store takes a moment).
VectorMemory g_big, g_half, g_small;
PendingEmbeds g_queue, g_noMatchQueue, g_empty;
}  // namespace

// The non-copying scan returns exactly what the copying one did: the same scan cap
// (2,000 most important entries, storage order within a tie), the same ranking
// (keyword hits, then scan order; queued facts after stored ones on a tie), the same
// read boundary (member, owner with legacy entries, unscoped, nobody), the same
// expiry rule (clocked and clockless) and the same tags.
static void test_scan_matches_the_copying_scan() {
  const char* queries[] = {"locker code", "blue", "coffee flat white", "zebra", "the and of"};
  const std::vector<std::vector<std::string>> allows = {
      {kOwnerNs}, {kMemberNs}, {}, {"\x01none"}};
  int compared = 0, nonEmpty = 0;
  for (const VectorMemory* vec : {&g_small, &g_big}) {
    for (const PendingEmbeds* q : {(const PendingEmbeds*)nullptr, (const PendingEmbeds*)&g_queue}) {
      for (const char* query : queries) {
        for (const auto& allow : allows) {
          for (int k : {1, 5, 20}) {
            for (uint32_t now : {kNow, 0u}) {
              const RecallScope sc = scopeFor(allow, k, now);
              const RecallOut got = lexicalRecall(*vec, q, query, sc, "x");
              const std::vector<std::string> want = referenceHits(*vec, q, query, sc);
              TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), got.hits.size(), query);
              for (size_t i = 0; i < want.size(); ++i)
                TEST_ASSERT_EQUAL_STRING_MESSAGE(want[i].c_str(), got.hits[i].c_str(), query);
              ++compared;
              if (!want.empty()) ++nonEmpty;
            }
          }
        }
      }
    }
  }
  TEST_ASSERT_TRUE(nonEmpty > compared / 3);   // not vacuous: most cases found hits
}

// At 5,000 entries (1.28 MB of vectors in the store) the degraded recall allocates
// k hit texts, their list and the cause on the default heap, and the scan cap's
// (importance, index) slots plus k ranked hits through the working-set hooks.
static void test_recall_at_5000_entries_allocates_o_k() {
  TEST_ASSERT_EQUAL_INT(5000, g_big.size());
  const std::string query = "locker code", cause = "out of embedding credit";
  for (int k : {1, 5, 20, 100}) {
    const RecallScope sc = scopeFor({kOwnerNs}, k);
    RecallOut out;
    const Measure m = measure([&] { out = lexicalRecall(g_big, &g_noMatchQueue, query, sc, cause); });
    TEST_ASSERT_EQUAL_INT(k, (int)out.hits.size());
    // Default heap: the cause, the hit list, one text per hit.
    TEST_ASSERT_TRUE_MESSAGE(m.allocs <= 2 + k, std::to_string(m.allocs).c_str());
    TEST_ASSERT_TRUE_MESSAGE(m.bytes <= 256 + 160L * k, std::to_string(m.bytes).c_str());
    // Working set: the slots (at most the scan cap) and the k + 1 ranked hits.
    TEST_ASSERT_TRUE_MESSAGE(m.wsAllocs <= 2, std::to_string(m.wsAllocs).c_str());
    TEST_ASSERT_TRUE_MESSAGE(m.wsBytes <= (long)(kLexScanMax * 8) + 64L * (k + 1),
                             std::to_string(m.wsBytes).c_str());
    // A copy of the store would be at least its vector bytes.
    TEST_ASSERT_TRUE(m.bytes + m.wsBytes < 5000L * kDims / 20);
  }
}

// The class rule: the allocation does not grow with the store or with the queue. The
// same recall over 2,500 and 5,000 entries allocates the same, and a queue of 64
// facts that match nothing costs nothing over an empty one (read by reference).
static void test_allocation_does_not_grow_with_the_store_or_the_queue() {
  const auto run = [](const VectorMemory& vec, const PendingEmbeds& q) {
    return measure([&] {
      const RecallOut out = lexicalRecall(vec, &q, "locker code", scopeFor({kOwnerNs}, 5), "x");
      TEST_ASSERT_EQUAL_INT(5, (int)out.hits.size());
    });
  };
  const Measure half = run(g_half, g_empty);
  const Measure full = run(g_big, g_empty);
  TEST_ASSERT_EQUAL_INT(half.allocs, full.allocs);
  TEST_ASSERT_EQUAL_INT(half.bytes, full.bytes);
  TEST_ASSERT_EQUAL_INT(half.wsAllocs, full.wsAllocs);
  TEST_ASSERT_EQUAL_INT(half.wsBytes, full.wsBytes);
  const Measure queued = run(g_big, g_noMatchQueue);
  TEST_ASSERT_EQUAL_INT(64, (int)g_noMatchQueue.size());
  TEST_ASSERT_EQUAL_INT(full.allocs, queued.allocs);
  TEST_ASSERT_EQUAL_INT(full.bytes, queued.bytes);
  // Many matches (every locker fact in the scan) still build only k texts.
  const Measure many = measure([&] {
    const RecallOut out = lexicalRecall(g_big, &g_queue, "locker", scopeFor({}, 5), "x");
    TEST_ASSERT_EQUAL_INT(5, (int)out.hits.size());
  });
  TEST_ASSERT_TRUE_MESSAGE(many.allocs <= 3 + 5, std::to_string(many.allocs).c_str());
}

// The memory.search fallback (the model's own search while embeddings are down) runs
// the same scan: its allocation does not grow with the store either.
static void test_search_tool_fallback_does_not_grow_with_the_store() {
  const auto run = [](VectorMemory& vec) {
    MemoryContext ctx;
    ctx.vec = &vec;
    ctx.embed = [](const std::string&, std::string& err) -> std::vector<int8_t> {
      err = "HTTP 429 credit_balance_exhausted";
      return {};
    };
    ctx.nowHours = [] { return kNow; };
    ToolRegistry reg;
    registerMemoryTools(reg, ctx);
    JsonDocument d;
    deserializeJson(d, R"({"query":"locker code","n_results":5})");
    const Principal owner = principalForRole("1001", Role::Admin);
    ToolResult r;
    const Measure m = measure([&] { r = reg.dispatch("memory.search", d.as<JsonObjectConst>(), owner); });
    TEST_ASSERT_TRUE_MESSAGE(r.output.find("Found 5 memories (keyword match") != std::string::npos,
                             (r.output + r.error).c_str());
    return m;
  };
  run(g_half);   // warm-up: the first billing refusal may set process-wide state once
  const Measure half = run(g_half);
  const Measure full = run(g_big);
  TEST_ASSERT_EQUAL_INT(half.allocs, full.allocs);
  TEST_ASSERT_EQUAL_INT(half.bytes, full.bytes);
  TEST_ASSERT_EQUAL_INT(half.wsBytes, full.wsBytes);
  TEST_ASSERT_TRUE(full.bytes + full.wsBytes < 5000L * kDims / 20);
}

// Below the recall memory floors the device asks for no scan: nothing is visited or
// ranked (no working-set allocation at all), no hit is returned, and the cause line
// says the keyword recall was skipped for low memory.
static void test_low_memory_skips_the_scan() {
  RecallScope sc = scopeFor({kOwnerNs}, 5);
  sc.lowMemory = true;
  const std::string query = "locker code", cause = "out of embedding credit";
  RecallOut out;
  const Measure m = measure([&] { out = lexicalRecall(g_big, &g_queue, query, sc, cause); });
  TEST_ASSERT_TRUE(out.hits.empty());
  TEST_ASSERT_EQUAL_STRING((std::string("out of embedding credit") + kRecallLowMemoryTail).c_str(),
                           out.degraded.c_str());
  TEST_ASSERT_TRUE(out.degraded.find("low on memory") != std::string::npos);
  TEST_ASSERT_EQUAL_INT(0, m.wsAllocs);
  TEST_ASSERT_TRUE_MESSAGE(m.allocs <= 2, std::to_string(m.allocs).c_str());   // the cause words
  // Counter-check: the same call with memory to spare does scan and find.
  sc.lowMemory = false;
  TEST_ASSERT_EQUAL_INT(5, (int)lexicalRecall(g_big, &g_queue, "locker code", sc, "x").hits.size());
}

// A NaN importance (a corrupt stored blob) cannot break the scan order: it ranks as
// 0 (shown as 0%), the hits stay ranked, and every other entry keeps its place.
static void test_nan_importance_ranks_as_zero() {
  VectorMemory vec;
  vec.configure(kDims);
  for (int i = 0; i < 1500; ++i) {   // under the scan cap: every entry is scanned
    VecEntry e;
    e.id = "n" + std::to_string(i);
    e.content = content(i);
    e.importance = i % 5 == 0 ? std::numeric_limits<float>::quiet_NaN() : 0.2f + 0.1f * (float)(i % 7);
    e.permanentFlag = true;   // never expired, so NaN entries stay in the scan
    e.ns = kOwnerNs;
    e.vec.assign(kDims, (int8_t)(i % 100));
    TEST_ASSERT_TRUE(vec.add(e, false));
  }
  const RecallOut out = lexicalRecall(vec, nullptr, "locker", scopeFor({kOwnerNs}, 2000), "x");
  TEST_ASSERT_TRUE(out.hits.size() > 100);
  int last = 100, zeros = 0;
  for (const std::string& h : out.hits) {
    const int pct = std::atoi(h.c_str() + 1);   // "[NN%] ..."
    TEST_ASSERT_TRUE_MESSAGE(pct <= last, h.c_str());   // one keyword each: importance order
    last = pct;
    if (pct == 0) ++zeros;
  }
  TEST_ASSERT_TRUE(zeros > 0);   // the NaN entries are there, ranked last
}

// The device seam asks the floors before the scan: keywordRecall marks the scope low
// on memory from the same check the pending drain waits on (free heap at the recall
// floor and the largest internal block at the turn floor), before it takes the lock.
static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}
static std::string functionBody(const std::string& src, const std::string& head) {
  const size_t at = src.find(head);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, head.c_str());
  const size_t end = src.find("\n}\n", at);
  TEST_ASSERT_TRUE_MESSAGE(end != std::string::npos, head.c_str());
  return src.substr(at, end - at);
}
static void test_device_keyword_recall_holds_the_recall_floors() {
  const std::string src = readFile("src/agent/memory_subsystem.cpp");
  const std::string ok = functionBody(src, "static bool recallHeapOk() {");
  TEST_ASSERT_TRUE(ok.find("ESP.getFreeHeap() < ORCH_RECALL_MIN_HEAP") != std::string::npos);
  TEST_ASSERT_TRUE(ok.find("heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(ok.find("ORCH_TURN_MIN_LARGEST_BLOCK") != std::string::npos);
  const std::string kw = functionBody(src, "static nimbus::orch::RecallOut keywordRecall(");
  const size_t gate = kw.find("scope.lowMemory = !recallHeapOk();");
  TEST_ASSERT_TRUE(gate != std::string::npos);
  TEST_ASSERT_TRUE(gate < kw.find("Lock g;"));
  TEST_ASSERT_TRUE(kw.find("lexicalRecall(") != std::string::npos);
  const std::string drain = functionBody(src, "bool embedDrainDue() {");
  TEST_ASSERT_TRUE(drain.find("return recallHeapOk();") != std::string::npos);
}

int main(int, char**) {
  fillStore(g_small, 300);
  fillStore(g_half, 2500);
  fillStore(g_big, 5000);
  fillPending(g_queue, 64, "the locker code waiting note");
  fillPending(g_noMatchQueue, 64, "a waiting note about the garden hose");
  UNITY_BEGIN();
  RUN_TEST(test_scan_matches_the_copying_scan);
  RUN_TEST(test_recall_at_5000_entries_allocates_o_k);
  RUN_TEST(test_allocation_does_not_grow_with_the_store_or_the_queue);
  RUN_TEST(test_search_tool_fallback_does_not_grow_with_the_store);
  RUN_TEST(test_low_memory_skips_the_scan);
  RUN_TEST(test_nan_importance_ranks_as_zero);
  RUN_TEST(test_device_keyword_recall_holds_the_recall_floors);
  return UNITY_END();
}
