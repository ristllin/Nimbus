#include <unity.h>

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "nimbus/exchange_trace.h"
#include "nimbus/held_slot.h"

// L24 merge review (blocker): a turn now catches std::bad_alloc and ends honestly
// instead of aborting the device. Anything the turn's code held by hand across a
// failing allocation stays held: the single TLS work slot was the worst case (every
// later turn, embed, connector and fetch failed "tls work-slot busy" until a
// restart). Pinned here: the slot guard gives the slot back on every path out, an
// exception included; the provider exchange's finish runs exactly once on every
// path, an exception included; and, as a class rule, no code a turn can reach takes
// the work slot by hand (a new raw acquire fails this suite).

using nimbus::HeldSlot;
using namespace nimbus::heapwatch;

// ---- a fake counting semaphore: the device's work slot (one slot) ----------------
static int g_slotsFree = 1;
static int g_acquires = 0, g_releases = 0;
static bool fakeAcquire(uint32_t) {
  if (g_slotsFree <= 0) return false;
  --g_slotsFree;
  ++g_acquires;
  return true;
}
static void fakeRelease() {
  ++g_slotsFree;
  ++g_releases;
}

void setUp() { g_slotsFree = 1; g_acquires = g_releases = 0; }
void tearDown() {}

// The guard's contract: held until release() or scope end, never released twice, and a
// guard whose acquire timed out never releases anything.
static void test_the_guard_releases_once_on_every_normal_path() {
  {
    HeldSlot s(&fakeAcquire, &fakeRelease, 10);
    TEST_ASSERT_TRUE((bool)s);
    TEST_ASSERT_EQUAL(0, g_slotsFree);
  }
  TEST_ASSERT_EQUAL(1, g_slotsFree);   // scope end
  {
    HeldSlot s(&fakeAcquire, &fakeRelease, 10);
    s.release();
    TEST_ASSERT_FALSE(s.held());
    TEST_ASSERT_EQUAL(1, g_slotsFree);   // released early...
    s.release();                         // ...and a second release is a no-op
  }
  TEST_ASSERT_EQUAL(1, g_slotsFree);
  TEST_ASSERT_EQUAL(g_acquires, g_releases);
  {
    HeldSlot a(&fakeAcquire, &fakeRelease, 10);
    HeldSlot b(&fakeAcquire, &fakeRelease, 10);   // the only slot is taken: times out
    TEST_ASSERT_TRUE((bool)a);
    TEST_ASSERT_FALSE((bool)b);
  }
  TEST_ASSERT_EQUAL(1, g_slotsFree);   // the timed-out guard released nothing
  TEST_ASSERT_EQUAL(g_acquires, g_releases);
}

// The blocker's class: a bad_alloc thrown while the slot is held (a body growing, a
// header copy) unwinds through the guard, and the slot is free for the next caller.
static void deviceShapedCall(bool throwInside) {
  HeldSlot slot(&fakeAcquire, &fakeRelease, 10);
  if (!slot) return;
  std::string body;
  if (throwInside) throw std::bad_alloc();   // stands for body.append failing
  body = "ok";
  slot.release();
}

static void test_a_bad_alloc_inside_a_held_slot_gives_it_back() {
  bool caught = false;
  try {
    deviceShapedCall(true);
  } catch (const std::bad_alloc&) {
    caught = true;
  }
  TEST_ASSERT_TRUE(caught);
  TEST_ASSERT_EQUAL(1, g_slotsFree);
  // The next caller gets it at once (before the fix: "tls work-slot busy" forever).
  deviceShapedCall(false);
  TEST_ASSERT_EQUAL(2, g_acquires);
  TEST_ASSERT_EQUAL(1, g_slotsFree);
}

// ---- the provider exchange: finish runs once on every path, a throw included -----
struct ThrowingIo {
  bool throwInBody = false;
  HeapSample sample() { return {30000u, 15000u}; }
  bool connect() { return true; }
  bool writeHead() { return true; }
  size_t writeBody(size_t, size_t want) { return want; }
  int readStatus() { return 200; }
  bool readHeaders(int) { return true; }
  bool readBody() {
    if (throwInBody) throw std::bad_alloc();
    return true;
  }
};

static void test_the_exchange_finish_runs_once_when_the_body_read_throws() {
  for (int throwing = 0; throwing < 2; ++throwing) {
    setUp();
    ThrowingIo io;
    io.throwInBody = throwing != 0;
    ExchangeTrace t;
    t.host = "api.anthropic.com";
    t.path = "/v1/messages";
    int finishes = 0;
    bool caught = false;
    HeldSlot slot(&fakeAcquire, &fakeRelease, 10);
    try {
      tracedExchange(io, t, [&](ExchangeTrace&) {
        ++finishes;
        slot.release();   // the device's finish gives the slot back
      });
    } catch (const std::bad_alloc&) {
      caught = true;
    }
    TEST_ASSERT_EQUAL(throwing != 0, caught);
    TEST_ASSERT_EQUAL(1, finishes);
    TEST_ASSERT_EQUAL(1, g_slotsFree);
  }
}

// A finish that itself fails while the exchange unwinds is swallowed (it must not
// escape a destructor and abort); the original exception carries on.
static void test_a_failing_finish_on_the_unwind_path_does_not_abort() {
  ThrowingIo io;
  io.throwInBody = true;
  ExchangeTrace t;
  t.host = "h";
  t.path = "/";
  int finishes = 0;
  bool caught = false;
  try {
    tracedExchange(io, t, [&](ExchangeTrace&) {
      ++finishes;
      throw std::bad_alloc();
    });
  } catch (const std::bad_alloc&) {
    caught = true;
  }
  TEST_ASSERT_TRUE(caught);
  TEST_ASSERT_EQUAL(1, finishes);
}

// ---- the class rule: no hand-held work slot on any path a turn can reach ----------

static std::string readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static void listSources(const std::string& dir, std::vector<std::string>& out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string p = dir + "/" + name;
    if (e->d_type == DT_DIR) {
      listSources(p, out);
    } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".cpp") == 0) {
      out.push_back(p);
    }
  }
  closedir(d);
}

// Files that may still take the slot by hand: tasks no turn runs on (the relay task,
// the cloud key mint, the provider verify and voice-catalog workers, the update task,
// the sound sync task, the main loop's index backfill), where a failed allocation
// still aborts and the restart frees the slot. A new raw acquire anywhere else fails.
static const std::set<std::string> kHandHeldOk = {
    "src/sys/tls_arbiter.cpp",          "src/net/relay_client.cpp",
    "src/net/cloud_mint.cpp",           "src/agent/provider_verify.cpp",
    "src/agent/adapters/tts_voices.cpp", "src/sys/ota_update.cpp",
    "src/sfx/sfx_sync.cpp",             "src/agent/memory_subsystem.cpp",
};

static void test_no_code_a_turn_reaches_holds_the_work_slot_by_hand() {
  std::vector<std::string> files;
  listSources("src", files);
  TEST_ASSERT_TRUE_MESSAGE(files.size() > 50, "the source walk found the tree");
  std::vector<std::string> offenders;
  int guarded = 0;
  for (const std::string& f : files) {
    const std::string src = readFile(f);
    if (src.find("arbiter::WorkSlot slot(") != std::string::npos) ++guarded;
    if (kHandHeldOk.count(f)) continue;
    if (src.find("arbiter::acquireWork(") != std::string::npos ||
        src.find("arbiter::releaseWork()") != std::string::npos)
      offenders.push_back(f);
  }
  std::string msg = "these files take the TLS work slot by hand on a path a turn can reach:";
  for (const auto& o : offenders) msg += " " + o;
  TEST_ASSERT_TRUE_MESSAGE(offenders.empty(), msg.c_str());
  // Counter-test: the guard is really in use where the turn goes (the provider
  // exchange, embeddings, connectors, url fetch, image, TTS, file fetch, moderation,
  // the multipart upload).
  TEST_ASSERT_TRUE(guarded >= 9);
  // The transport's finish gives the slot back through the guard, and the menu of
  // hand-held exceptions still exists (a moved file must be re-listed, not dropped).
  TEST_ASSERT_TRUE(readFile("src/agent/transport_tls.cpp").find("slot.release();") != std::string::npos);
  for (const std::string& f : kHandHeldOk) TEST_ASSERT_FALSE_MESSAGE(readFile(f).empty(), f.c_str());
}

// The other holds a turn can carry across a throw are scope guards too: the card lock
// in the durable log write, the session and results mutexes, the file store's upload
// slot at the turn's tool sites.
static void test_the_other_turn_holds_are_scope_guards() {
  const std::string errlog = readFile("src/sys/errlog_fs.cpp");
  TEST_ASSERT_TRUE(errlog.find("~Held() { agent::memory::unlock(); }") != std::string::npos);
  const std::string orch = readFile("src/agent/orchestrator.cpp");
  TEST_ASSERT_TRUE(orch.find("xSemaphoreTake(g_sessMux") == std::string::npos);
  TEST_ASSERT_TRUE(orch.find("xSemaphoreTake(g_resultsMux") == std::string::npos);
  TEST_ASSERT_TRUE(orch.find("MuxHold hold(g_resultsMux)") != std::string::npos);
  for (const char* f : {"src/agent/pdf_tool.cpp", "src/agent/adapters/provider_file_fetch.cpp"})
    TEST_ASSERT_TRUE_MESSAGE(readFile(f).find("files::OpenWrite session;") != std::string::npos, f);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_guard_releases_once_on_every_normal_path);
  RUN_TEST(test_a_bad_alloc_inside_a_held_slot_gives_it_back);
  RUN_TEST(test_the_exchange_finish_runs_once_when_the_body_read_throws);
  RUN_TEST(test_a_failing_finish_on_the_unwind_path_does_not_abort);
  RUN_TEST(test_no_code_a_turn_reaches_holds_the_work_slot_by_hand);
  RUN_TEST(test_the_other_turn_holds_are_scope_guards);
  return UNITY_END();
}
