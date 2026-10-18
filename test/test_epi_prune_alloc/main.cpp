// Allocation-injection tests for the episodic retention pass and the index
// backfill (v4.5.14). On the device every allocation under 128 B lands on the
// scarce internal heap (the PSRAM spill threshold), and a retention pass that
// built a std::set of hashes and a vector of blob names held hundreds of such
// pieces at once - the pass that overlapped a turn before a field crash. The
// rule asserted here: every container these passes build goes through the
// injected PSRAM working allocator, so the number of live default-heap
// allocations stays a small constant however many rows, days or blobs the card
// holds.
//
// This binary replaces the global operator new/delete to count live
// allocations; that is why it is its own test directory.
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/psram_alloc.h"

using namespace nimbus::orch;

// ---- live-allocation tracking (default heap) -------------------------------
namespace {
bool g_armed = false;
long g_live = 0, g_peak = 0;            // all default-heap blocks made while armed
long g_liveSmall = 0, g_peakSmall = 0;  // those under 128 B: the device's internal-heap class
long g_workingCalls = 0;                // allocations through the PSRAM working hooks
constexpr uint64_t kMagic = 0x55414c4c4f43ull;
struct alignas(16) Hdr {
  uint64_t magic;
  uint32_t size;
  uint32_t armed;
};
void resetCounts() { g_live = g_peak = g_liveSmall = g_peakSmall = g_workingCalls = 0; }
void* wsAlloc(std::size_t n) {
  if (g_armed) g_workingCalls++;
  return std::malloc(n);
}
void wsFree(void* p) { std::free(p); }
}  // namespace

void* operator new(std::size_t n) {
  Hdr* h = static_cast<Hdr*>(std::malloc(n + sizeof(Hdr)));
  if (!h) throw std::bad_alloc();
  h->magic = kMagic;
  h->size = (uint32_t)n;
  h->armed = g_armed ? 1u : 0u;
  if (g_armed) {
    if (++g_live > g_peak) g_peak = g_live;
    if (n < 128 && ++g_liveSmall > g_peakSmall) g_peakSmall = g_liveSmall;
  }
  return h + 1;
}
void operator delete(void* p) noexcept {
  if (!p) return;
  Hdr* h = static_cast<Hdr*>(p) - 1;
  if (h->magic != kMagic) { std::free(p); return; }
  if (h->armed) {
    g_live--;
    if (h->size < 128) g_liveSmall--;
  }
  h->magic = 0;
  std::free(h);
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

// ---- a streaming in-memory card ---------------------------------------------
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
    if (it == files.end() || off < 0 || len < 0 || (size_t)(off + len) > it->second.size())
      return std::string();
    return it->second.substr((size_t)off, (size_t)len);
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    forEachName(dir, [&](const char* n) { out.emplace_back(n); });
    return out;
  }
  void forEachName(const std::string& dir,
                   const std::function<void(const char*)>& fn) const override {
    const std::string prefix = dir + "/";
    for (auto it = files.lower_bound(prefix);
         it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
      const char* rest = it->first.c_str() + prefix.size();
      if (std::strchr(rest, '/') == nullptr) fn(rest);
    }
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool exists(const std::string& p) const override { return files.count(p) > 0; }
};

const char* kDir = "/mem/episodic";
const char* kBlobs = "/mem/blobs";
const uint32_t kDay0 = 20700;

// `days` day-streams of `rowsPerDay` rows; every row references its own blob
// sidecar, and `orphans` more sidecars are referenced by nothing.
void writeCard(FakeFs& fs, int days, int rowsPerDay, int orphans) {
  AppendLogEpisodicStore w(fs, kDir, 64);
  uint32_t id = 1;
  for (int d = 0; d < days; d++)
    for (int i = 0; i < rowsPerDay; i++, id++) {
      char idb[16], blob[64];
      snprintf(idb, sizeof(idb), "m%08x", id);
      snprintf(blob, sizeof(blob), "%s/%08x%08x.txt", kBlobs, id * 2654435761u, id);
      EpisodicMessage m;
      m.id = idb;
      m.sessionId = "123456789";
      m.tsHours = (kDay0 + (uint32_t)d) * 24 + (uint32_t)(i % 24);
      m.role = "assistant";
      m.kind = MsgKind::ToolOutput;
      m.text = std::string("tool output for ") + idb + std::string(400, 'x');
      m.blobPath = blob;
      fs.files[blob] = "x";
      w.addMessage(m);
    }
  for (int i = 0; i < orphans; i++) {
    char blob[64];
    snprintf(blob, sizeof(blob), "%s/0e%014x.txt", kBlobs, (unsigned)i);
    fs.files[blob] = "o";
  }
}

void setUp() { setWorkingAllocators(wsAlloc, wsFree); }
void tearDown() {
  g_armed = false;
  setWorkingAllocators(nullptr, nullptr);
}

struct Peak {
  long peak = 0, peakSmall = 0, working = 0;
};

Peak measurePrune(int days, int rowsPerDay, int orphans, int oldDays) {
  FakeFs fs;
  writeCard(fs, days, rowsPerDay, orphans);
  AppendLogEpisodicStore st(fs, kDir, 64);
  st.hydrate(0, 0);   // a complete index, so the blob sweep runs
  Peak p;
  {
    resetCounts();
    g_armed = true;
    EpiPruneReport rep = st.prune(kDay0 + (uint32_t)oldDays, kBlobs);
    g_armed = false;
    p.peak = g_peak;
    p.peakSmall = g_peakSmall;
    p.working = g_workingCalls;
    TEST_ASSERT_FALSE(rep.blobGcSkipped);
    TEST_ASSERT_EQUAL_INT(oldDays, (int)rep.removedDayFiles.size());
    TEST_ASSERT_EQUAL_INT(orphans + oldDays * rowsPerDay, (int)rep.removedBlobs.size());
  }
  return p;
}

// The retention pass: a small card and a card with 20x the rows, days and
// blobs hold the same handful of default-heap blocks at peak; the containers
// (day list, reference set, orphan list, report) all ride the PSRAM hooks.
static void test_prune_containers_use_the_injected_psram_allocator() {
  const Peak small = measurePrune(/*days=*/4, /*rowsPerDay=*/25, /*orphans=*/10, /*oldDays=*/1);
  const Peak large = measurePrune(/*days=*/40, /*rowsPerDay=*/50, /*orphans=*/200, /*oldDays=*/10);
  TEST_ASSERT_TRUE(small.working > 0);
  TEST_ASSERT_TRUE(large.working > small.working);   // the big pass built bigger PSRAM containers
  TEST_ASSERT_TRUE(large.peakSmall <= 6);            // no small-block build-up
  TEST_ASSERT_TRUE(large.peak <= small.peak + 2);    // and nothing that grows with the card
}

// The index backfill: every slice holds the same handful of default-heap blocks
// at peak, from the first slice to the last, on a card ten times bigger.
static void test_backfill_slices_use_the_injected_psram_allocator() {
  long worstSmall = 0, worstAll = 0, working = 0;
  for (int scale : {1, 10}) {
    FakeFs fs;
    writeCard(fs, 4 * scale, 60, 0);
    AppendLogEpisodicStore st(fs, kDir, 64);
    st.hydrate(/*maxRows=*/30, /*maxBytes=*/0);
    TEST_ASSERT_TRUE(st.hydrateTruncated());
    for (int step = 0; step < 100000; step++) {
      resetCounts();
      g_armed = true;
      const bool more = st.backfillStep();
      g_armed = false;
      if (g_peakSmall > worstSmall) worstSmall = g_peakSmall;
      if (g_peak > worstAll) worstAll = g_peak;
      working += g_workingCalls;
      if (!more) break;
    }
    TEST_ASSERT_TRUE(st.indexComplete());
    TEST_ASSERT_EQUAL_INT(4 * scale * 60, st.messageCount());
  }
  TEST_ASSERT_TRUE(working > 0);
  TEST_ASSERT_TRUE(worstSmall <= 4);
  TEST_ASSERT_TRUE(worstAll <= 8);
}

// Counter-test for the instrument: the v4.5.13 pass built a std::set of blob
// hashes and a vector of blob names; that shape is caught as a small-block
// build-up that grows with the card, so a regression to it cannot pass the two
// tests above. (The set's nodes are counted everywhere. The strings' own
// buffers are counted where the C++ runtime routes them through the replaced
// operator new - Linux CI - but not on macOS, whose runtime builds std::string
// inside the shared library.)
static void test_the_meter_sees_the_old_containers() {
  FakeFs fs;
  writeCard(fs, 2, 10, 300);
  resetCounts();
  g_armed = true;
  {
    std::set<std::string> referenced;
    std::vector<std::string> present = fs.list(kBlobs);
    for (const auto& n : present) referenced.insert(n.substr(0, 16));
    TEST_ASSERT_EQUAL_INT(320, (int)present.size());
    TEST_ASSERT_EQUAL_INT(320, (int)referenced.size());
  }
  g_armed = false;
  TEST_ASSERT_TRUE(g_peakSmall >= 300);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_prune_containers_use_the_injected_psram_allocator);
  RUN_TEST(test_backfill_slices_use_the_injected_psram_allocator);
  RUN_TEST(test_the_meter_sees_the_old_containers);
  return UNITY_END();
}
