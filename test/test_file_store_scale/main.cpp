#include <unity.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nimbus/orch/file_store.h"

// Lane FSCAP: the file store never fills up while the SD card has room.
//
// The field failure this lane fixes (evidence/lumi-issues-20261104T200842Z, Lumi
// v4.9.0): files.count = 256 exactly, 23 MB used, quota 30.7 GB, card free 31 GB,
// and every new Telegram attachment refused with "too many files (remove some
// first)" - a COUNT cap, not a space limit. Owner ruling R6 (21 Oct): no SD file
// cap. These tests pin the new class: the default store has no entry count an
// owner can hit, load() never drops a row (an upgrade keeps every file), and a
// fixture shaped like Lumi's store migrates in place and then takes file 257.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

FileEntry mk(const std::string& proj, const std::string& name, uint32_t bytes,
             uint64_t hash = 0x1000) {
  FileEntry e;
  e.project = proj;
  e.name = name;
  e.bytes = bytes;
  e.createdAt = 1700000000;
  e.kind = fileKindForName(name);
  e.hash = hash;
  return e;
}

// One FILESv1 index row, the same shape dump() writes.
std::string row(const std::string& proj, const std::string& name, uint32_t bytes,
                uint32_t createdAt, uint64_t hash, const std::string& owner = "") {
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s\t%s\t%u\t%u\t%u\t%016llx\t\t\t%s\n", proj.c_str(),
                name.c_str(), (unsigned)bytes, (unsigned)createdAt,
                (unsigned)fileKindForName(name), (unsigned long long)hash, owner.c_str());
  return buf;
}

// A fixture shaped like Lumi's store on 4 Nov 2026
// (evidence/lumi-issues-20261104T200842Z): 256 entries, about 23 MB, mostly the
// owner's chat attachments under "chat-<chatId>" named by update id, plus a few
// owner projects. Deterministic, so the test is reproducible byte for byte.
std::string lumiShapedIndex() {
  std::string blob = "FILESv1\n";
  const char* chat = "chat-6098532481";
  uint64_t hash = 0xAABBCCDD00000001ull;
  uint32_t total = 0;
  int n = 0;
  // 200 chat attachments: photos and documents named by update id, ~100 KB each.
  for (int i = 0; i < 200; i++) {
    char name[64];
    if (i % 4 == 0)
      std::snprintf(name, sizeof name, "photo-%d.jpg", 500000000 + i);
    else
      std::snprintf(name, sizeof name, "%d-report.pdf", 508949000 + i);
    const uint32_t bytes = 100 * 1024 + (uint32_t)(i * 37);
    blob += row(chat, name, bytes, 1793772117u + (uint32_t)i, hash + (uint64_t)i);
    total += bytes;
    ++n;
  }
  // 56 owner artifacts across other projects, ~40 KB each.
  const char* projs[] = {"uploads", "reports", "food", "notes"};
  for (int i = 0; i < 56; i++) {
    char name[64];
    std::snprintf(name, sizeof name, "doc-%d.md", i);
    const uint32_t bytes = 40 * 1024 + (uint32_t)(i * 11);
    blob += row(projs[i % 4], name, bytes, 1793772117u + 1000u + (uint32_t)i,
                hash + 0x10000ull + (uint64_t)i, "owner");
    total += bytes;
    ++n;
  }
  (void)total;
  return blob;
}

// ---- counting allocator: per-call memory measurement --------------------------
// Every allocation rides a 16-byte header (malloc stays 16-aligned) so delete can
// subtract its size; g_live tracks live bytes, g_peak the high-water mark.
// (The replacement operators must live at global scope, so the anonymous namespace
// closes here and reopens below.)
}  // namespace
static size_t g_live = 0, g_peak = 0;
static const size_t kHdr = 16;

void* operator new(size_t n) {
  void* p = std::malloc(n + kHdr);
  if (!p) throw std::bad_alloc();
  std::memcpy(p, &n, sizeof n);
  g_live += n;
  if (g_live > g_peak) g_peak = g_live;
  return (char*)p + kHdr;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* q) noexcept {
  if (!q) return;
  const char* p = (const char*)q - kHdr;
  size_t n;
  std::memcpy(&n, p, sizeof n);
  g_live -= n;
  std::free((void*)p);
}
void operator delete[](void* q) noexcept { operator delete(q); }
void operator delete(void* q, size_t) noexcept { operator delete(q); }
void operator delete[](void* q, size_t) noexcept { operator delete(q); }

namespace {

struct M {
  double ms;
  long   liveDelta;
  size_t peakDelta;
};

template <typename F>
static M measure(F&& f) {
  const size_t live0 = g_live, peak0 = g_peak;
  const auto t0 = std::chrono::steady_clock::now();
  f();
  const auto t1 = std::chrono::steady_clock::now();
  M m;
  m.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  m.liveDelta = (long)g_live - (long)live0;
  m.peakDelta = g_peak - peak0;
  return m;
}

static void report(const char* what, const M& m, double msCap, size_t memCap) {
  std::printf("  %-34s %9.3f ms  live %+8ld B  peak %9zu B\n", what, m.ms, m.liveDelta,
              m.peakDelta);
  TEST_ASSERT_TRUE_MESSAGE(m.ms < msCap, what);
  TEST_ASSERT_TRUE_MESSAGE(m.peakDelta <= memCap, what);
}

// A 10,000-entry store: 50 projects (some chat-attachment projects), unique
// hashes, mixed owners - the scale the DoD measures at.
static void build10k(FileStore& s) {
  std::string err;
  for (int i = 0; i < 10000; i++) {
    char proj[32], name[32];
    if (i % 10 == 0)
      std::snprintf(proj, sizeof proj, "chat-%ld", 6098530000L + i / 100);
    else
      std::snprintf(proj, sizeof proj, "proj-%d", i % 50);
    std::snprintf(name, sizeof name, "f%d.txt", i);
    FileEntry e = mk(proj, name, 100 + (uint32_t)(i % 900), 0xE0000ull + (uint64_t)i);
    if (i % 3 == 0) e.owner = "owner";
    TEST_ASSERT_TRUE_MESSAGE(s.add(e, err), err.c_str());
  }
}

}  // namespace

// The Lumi failure shape, exactly: 256 entries in, one more attachment out. On the
// old 256-entry cap the 257th add is refused with kErrTooManyFiles; the card had
// 31 GB free. The default store must take it.
static void test_default_store_takes_the_257th_file() {
  FileStore s;   // default limits: what a device mounts with
  std::string err;
  for (int i = 0; i < 256; i++)
    TEST_ASSERT_TRUE_MESSAGE(s.add(mk("chat-6098532481", "photo-" + std::to_string(i) + ".jpg",
                                      100 * 1024, 0xA0000ull + (uint64_t)i), err),
                             err.c_str());
  TEST_ASSERT_EQUAL(256, int(s.count()));
  // The 257th file: refused by a count cap on the old code, taken now.
  TEST_ASSERT_TRUE_MESSAGE(s.add(mk("chat-6098532481", "photo-256.jpg", 100 * 1024, 0xB0001ull), err),
                           err.c_str());
  TEST_ASSERT_EQUAL(257, int(s.count()));
  std::string pre;
  TEST_ASSERT_FALSE(s.wouldExceed("chat-6098532481", "photo-257.jpg", 100 * 1024, pre));
  TEST_ASSERT_TRUE(s.hasRoom());
  // And the refusal that used to fire is not a full-store refusal any more.
  TEST_ASSERT_FALSE(s.fullRefusal(FileStore::kErrTooManyFiles));
}

// load() must keep every valid row, whatever the entry limit says: an upgrade
// never loses a file. A 300-row index loads whole (the old cap dropped rows past
// 256 silently).
static void test_load_keeps_every_row_past_the_old_cap() {
  std::string blob = "FILESv1\n";
  for (int i = 0; i < 300; i++)
    blob += row("bulk", "f" + std::to_string(i) + ".txt", 10, 1700000000u + (uint32_t)i,
                0xC0000ull + (uint64_t)i);
  FileStore s;
  TEST_ASSERT_TRUE(s.load(blob));
  TEST_ASSERT_EQUAL(300, int(s.count()));
  TEST_ASSERT_NOT_NULL(s.find("bulk", "f0.txt"));
  TEST_ASSERT_NOT_NULL(s.find("bulk", "f299.txt"));
  // Even an explicit small cap does not make load() drop rows: the cap gates new
  // adds only, so a downgrade-then-upgrade never loses data.
  FileStore::Limits lim;
  lim.maxEntries = 100;
  FileStore capped(lim);
  TEST_ASSERT_TRUE(capped.load(blob));
  TEST_ASSERT_EQUAL(300, int(capped.count()));
  // The cap still gates NEW entries on the capped store.
  std::string err;
  TEST_ASSERT_FALSE(capped.add(mk("bulk", "new.txt", 10), err));
  TEST_ASSERT_EQUAL_STRING(FileStore::kErrTooManyFiles, err.c_str());
}

// Migration, on a fixture shaped like Lumi's store: 256 entries load in place,
// every file is kept with its bytes/hash/owner, the index round-trips, and the
// next attachment (the one Lumi refused) saves and persists.
static void test_lumi_shaped_store_migrates_in_place() {
  const std::string blob = lumiShapedIndex();
  FileStore s;
  TEST_ASSERT_TRUE(s.load(blob));
  TEST_ASSERT_EQUAL(256, int(s.count()));
  // Spot-check the shape: a chat attachment and an owner artifact survive intact.
  const FileEntry* photo = s.find("chat-6098532481", "photo-500000000.jpg");
  TEST_ASSERT_NOT_NULL(photo);
  TEST_ASSERT_EQUAL_UINT32(100 * 1024, photo->bytes);
  TEST_ASSERT_EQUAL(int(FileKind::Image), int(photo->kind));
  TEST_ASSERT_EQUAL_UINT64(0xAABBCCDD00000001ull, photo->hash);
  const FileEntry* doc = s.find("food", "doc-2.md");
  TEST_ASSERT_NOT_NULL(doc);
  TEST_ASSERT_EQUAL_STRING("owner", doc->owner.c_str());
  // Round-trip: dump -> load keeps all 256 rows and the same bytes.
  FileStore back;
  TEST_ASSERT_TRUE(back.load(s.dump()));
  TEST_ASSERT_EQUAL(256, int(back.count()));
  TEST_ASSERT_EQUAL_UINT64(s.totalBytes(), back.totalBytes());
  TEST_ASSERT_NOT_NULL(back.find("chat-6098532481", "photo-500000196.jpg"));
  // The upgrade then takes the file Lumi refused (errlog: "attach store failed:
  // too many files"), and it persists through another dump/load.
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(s.add(mk("chat-6098532481", "508950000-new.json", 702, 0xD0001ull), err),
                           err.c_str());
  TEST_ASSERT_EQUAL(257, int(s.count()));
  FileStore again;
  TEST_ASSERT_TRUE(again.load(s.dump()));
  TEST_ASSERT_EQUAL(257, int(again.count()));
  TEST_ASSERT_NOT_NULL(again.find("chat-6098532481", "508950000-new.json"));
}

// ---- paged listing: bounded per call at any index size -------------------------

static void test_list_page_is_bounded_per_call() {
  FileStore s;
  build10k(s);
  TEST_ASSERT_EQUAL(10000, int(s.count()));
  TEST_ASSERT_EQUAL(100, int(s.listPage("", 0, 100).size()));
  TEST_ASSERT_EQUAL(100, int(s.listPage("", 9900, 100).size()));
  TEST_ASSERT_EQUAL(0, int(s.listPage("", 10000, 100).size()));
  TEST_ASSERT_EQUAL(10000, int(s.listPage("", 0, 0).size()));   // 0 = no cap (internal scans)
  // Pages tile the listing without overlap or gaps.
  auto a = s.listPage("", 0, 4000);
  auto b = s.listPage("", 4000, 4000);
  auto c = s.listPage("", 8000, 4000);
  TEST_ASSERT_EQUAL(4000, int(a.size()));
  TEST_ASSERT_EQUAL(4000, int(b.size()));
  TEST_ASSERT_EQUAL(2000, int(c.size()));
  TEST_ASSERT_EQUAL_STRING(a[0]->name.c_str(), s.listPage("", 0, 1)[0]->name.c_str());
  TEST_ASSERT_EQUAL_STRING(b[0]->name.c_str(), s.listPage("", 4000, 1)[0]->name.c_str());
  // Project filter pages within the project only.
  const size_t inProj = s.list("proj-7").size();
  TEST_ASSERT_EQUAL(inProj, s.listPage("proj-7", 0, 0).size());
  TEST_ASSERT_EQUAL(100, int(s.listPage("proj-7", 0, 100).size()));
  for (const auto* e : s.listPage("proj-7", 0, 100)) TEST_ASSERT_EQUAL_STRING("proj-7", e->project.c_str());
  // A small store pages the same way.
  FileStore small;
  std::string err;
  for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(small.add(mk("p", "n" + std::to_string(i) + ".txt", 1), err));
  TEST_ASSERT_EQUAL(5, int(small.listPage("", 0, 100).size()));
  TEST_ASSERT_EQUAL(2, int(small.listPage("", 3, 100).size()));
  TEST_ASSERT_EQUAL(0, int(small.listPage("", 5, 100).size()));
}

// ---- eviction plan: oldest regenerable copies first, never owner artifacts -----

static void test_eviction_plan_picks_regenerable_copies_oldest_first() {
  FileStore s;
  std::string err;
  // Owner artifacts: never evicted.
  TEST_ASSERT_TRUE(s.add(mk("uploads", "report.md", 5000, 0x1), err));
  TEST_ASSERT_TRUE(s.add(mk("notes", "todo.md", 3000, 0x2), err));
  // Chat attachments, oldest first: all regenerable (the archive keeps the original).
  FileEntry a1 = mk("chat-1", "photo-1.jpg", 4000, 0x11); a1.createdAt = 100;
  FileEntry a2 = mk("chat-1", "photo-2.jpg", 4000, 0x12); a2.createdAt = 200;
  FileEntry a3 = mk("chat-2", "doc-3.pdf", 6000, 0x13);  a3.createdAt = 300;
  TEST_ASSERT_TRUE(s.add(a1, err));
  TEST_ASSERT_TRUE(s.add(a2, err));
  TEST_ASSERT_TRUE(s.add(a3, err));
  // A same-content pair: the OLDER copy is regenerable, the newer is the original.
  FileEntry d1 = mk("uploads", "copy-a.bin", 2000, 0x99); d1.createdAt = 150;
  FileEntry d2 = mk("uploads", "copy-b.bin", 2000, 0x99); d2.createdAt = 250;
  TEST_ASSERT_TRUE(s.add(d1, err));
  TEST_ASSERT_TRUE(s.add(d2, err));

  TEST_ASSERT_TRUE(s.isRegenerableCopy(*s.find("chat-1", "photo-1.jpg")));
  TEST_ASSERT_TRUE(s.isRegenerableCopy(*s.find("uploads", "copy-a.bin")));
  TEST_ASSERT_FALSE(s.isRegenerableCopy(*s.find("uploads", "copy-b.bin")));   // newest of the hash
  TEST_ASSERT_FALSE(s.isRegenerableCopy(*s.find("uploads", "report.md")));    // owner artifact
  TEST_ASSERT_FALSE(s.isRegenerableCopy(*s.find("notes", "todo.md")));
  TEST_ASSERT_TRUE(FileStore::isAttachmentProject("chat-6098532481"));
  TEST_ASSERT_TRUE(FileStore::isAttachmentProject("chat"));
  TEST_ASSERT_FALSE(FileStore::isAttachmentProject("uploads"));
  TEST_ASSERT_FALSE(FileStore::isAttachmentProject("chatx"));

  // Need 4000 B: the oldest regenerable copy (photo-1, 4000 B) covers it alone.
  auto plan = s.planEviction(4000);
  TEST_ASSERT_EQUAL(1, int(plan.size()));
  TEST_ASSERT_EQUAL_STRING("photo-1.jpg", plan[0]->name.c_str());
  // Need 8000 B: photo-1(4000) + copy-a(2000) + photo-2(4000) = 10000 >= 8000, oldest
  // first (createdAt 100, 150, 200), still no owner artifact.
  plan = s.planEviction(8000);
  TEST_ASSERT_EQUAL(3, int(plan.size()));
  TEST_ASSERT_EQUAL_STRING("photo-1.jpg", plan[0]->name.c_str());
  TEST_ASSERT_EQUAL_STRING("copy-a.bin", plan[1]->name.c_str());
  TEST_ASSERT_EQUAL_STRING("photo-2.jpg", plan[2]->name.c_str());
  // Need 12000 B: photo-1, photo-2, then copy-a (createdAt 150 < doc-3's 300? no:
  // order is createdAt: 100, 150(copy-a), 200, 250 is not regenerable, 300) ->
  // photo-1(100), copy-a(150), photo-2(200) = 4000+2000+4000 = 10000 < 12000, next doc-3(300) 6000 -> 16000 >= 12000.
  plan = s.planEviction(12000);
  TEST_ASSERT_EQUAL(4, int(plan.size()));
  TEST_ASSERT_EQUAL_STRING("photo-1.jpg", plan[0]->name.c_str());
  TEST_ASSERT_EQUAL_STRING("copy-a.bin", plan[1]->name.c_str());
  TEST_ASSERT_EQUAL_STRING("photo-2.jpg", plan[2]->name.c_str());
  TEST_ASSERT_EQUAL_STRING("doc-3.pdf", plan[3]->name.c_str());
  for (const auto* e : plan) TEST_ASSERT_FALSE(e->project == "notes" || e->name == "report.md");
  // A need no regenerable copy can cover: all of them, never an owner artifact.
  plan = s.planEviction(1ull << 40);
  TEST_ASSERT_EQUAL(4, int(plan.size()));
  uint64_t freed = 0;
  for (const auto* e : plan) freed += e->bytes;
  TEST_ASSERT_EQUAL_UINT64(16000, freed);
  // No regenerable copies at all: an empty plan, the owner's files stay.
  FileStore own;
  TEST_ASSERT_TRUE(own.add(mk("uploads", "mine.md", 100, 0x5), err));
  TEST_ASSERT_TRUE(own.planEviction(1000).empty());

  // EXECUTING a plan: remove() erases from the index vector and shifts the rest,
  // so the plan's pointers dangle after the first remove. The caller must copy
  // the identities first - this test runs the eviction loop exactly as the
  // device and nimbusd do, and fails if the wrong entries go.
  {
    auto plan = s.planEviction(4000);   // photo-1 alone covers 4000 B
    TEST_ASSERT_EQUAL(1, int(plan.size()));
    struct Victim { std::string project, name; };
    std::vector<Victim> victims;
    for (const auto* e : plan) victims.push_back(Victim{e->project, e->name});
    for (const auto& v : victims) TEST_ASSERT_TRUE(s.remove(v.project, v.name));
    TEST_ASSERT_NULL(s.find("chat-1", "photo-1.jpg"));
    TEST_ASSERT_NOT_NULL(s.find("chat-1", "photo-2.jpg"));    // not in the plan: survives
    TEST_ASSERT_NOT_NULL(s.find("chat-2", "doc-3.pdf"));
    TEST_ASSERT_NOT_NULL(s.find("uploads", "copy-a.bin"));
    TEST_ASSERT_NOT_NULL(s.find("uploads", "report.md"));     // owner artifact: survives
    TEST_ASSERT_EQUAL(6, int(s.count()));
  }
}

// ---- the 10,000-file measurement: memory and time per call stay bounded ---------

static void test_10000_files_per_call_stays_bounded() {
  FileStore s;
  build10k(s);
  TEST_ASSERT_EQUAL(10000, int(s.count()));
  std::printf("  -- per-call cost at 10,000 files (host measurement, lane FSCAP DoD) --\n");

  std::string err;
  report("find (hit)", measure([&] { (void)s.find("proj-7", "f7.txt"); }), 50, 64 * 1024);
  report("find (miss)", measure([&] { (void)s.find("proj-7", "nope.txt"); }), 50, 64 * 1024);
  report("add (new entry)", measure([&] { (void)s.add(mk("proj-7", "new.txt", 100, 0xF1), err); }), 100, 8 * 1024 * 1024);
  report("add (replace)", measure([&] { (void)s.add(mk("proj-7", "f7.txt", 200, 0xF2), err); }), 50, 64 * 1024);
  report("remove", measure([&] { (void)s.remove("proj-7", "new.txt"); }), 100, 8 * 1024 * 1024);
  report("listPage(0,100)", measure([&] { (void)s.listPage("", 0, 100); }), 50, 64 * 1024);
  report("listPage(9900,100)", measure([&] { (void)s.listPage("", 9900, 100); }), 50, 64 * 1024);
  report("list() full scan", measure([&] { (void)s.list(""); }), 100, 256 * 1024);
  report("projects()", measure([&] { (void)s.projects(); }), 100, 128 * 1024);
  report("totalBytes()", measure([&] { (void)s.totalBytes(); }), 50, 1024);
  report("bytesFor(owner)", measure([&] { (void)s.bytesFor("owner"); }), 50, 1024);
  report("wouldExceed", measure([&] { std::string e; (void)s.wouldExceed("p", "n.txt", 10, e); }), 50, 64 * 1024);
  report("isRegenerableCopy", measure([&] { (void)s.isRegenerableCopy(*s.find("proj-7", "f7.txt")); }), 50, 64 * 1024);
  report("planEviction(1 MB)", measure([&] { (void)s.planEviction(1024 * 1024); }), 100, 256 * 1024);
  const std::string blob = s.dump();
  report("dump()", measure([&] { (void)s.dump(); }), 500, 8 * 1024 * 1024);
  report("load(dump)", measure([&] { FileStore t; (void)t.load(blob); }), 2000, 16 * 1024 * 1024);
  std::printf("  -- index at 10,000 files: %zu bytes on card, %zu entries --\n", blob.size(),
              s.count());
  TEST_ASSERT_EQUAL(10000, int(s.count()));
}

// ---- device wiring (a source scan: these paths cannot run on the host) ---------

static std::string readSrc(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// The class rules the device seam must keep: no fixed count rail left in the file
// subsystem, the eviction plan is consulted on a full store, listings page, and
// the search scan is capped per call.
static void test_device_wiring_fscap() {
  const std::string fs = readSrc("src/agent/files_subsystem.cpp");
  TEST_ASSERT_TRUE(fs.find("project is full (128") == std::string::npos);   // the count rail is gone
  TEST_ASSERT_TRUE(fs.find("planEviction(") != std::string::npos);          // eviction on the byte quota
  TEST_ASSERT_TRUE(fs.find("listPage(") != std::string::npos);               // paged listing
  TEST_ASSERT_TRUE(fs.find("kSearchScanMax") != std::string::npos);          // bounded search scan
  const std::string web = readSrc("src/net/web_files.cpp");
  TEST_ASSERT_TRUE(web.find("limit") != std::string::npos && web.find("offset") != std::string::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_store_takes_the_257th_file);
  RUN_TEST(test_load_keeps_every_row_past_the_old_cap);
  RUN_TEST(test_lumi_shaped_store_migrates_in_place);
  RUN_TEST(test_list_page_is_bounded_per_call);
  RUN_TEST(test_eviction_plan_picks_regenerable_copies_oldest_first);
  RUN_TEST(test_10000_files_per_call_stays_bounded);
  RUN_TEST(test_device_wiring_fscap);
  return UNITY_END();
}
