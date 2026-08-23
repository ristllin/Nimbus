// test_posix_stores - offline (T1) proof that nimbusd's POSIX store layer gives
// the daemon RESTART-SAFE memory, the load-bearing Phase 0 claim.
//
// No network, no providers, no clock sync: every check is deterministic. It
// exercises the SAME portable stores the device runs (AppendLogEpisodicStore,
// VectorMemory) over nimbusd's PosixEpiFs + atomic file writer, then throws the
// store instances away and rebuilds them from the on-disk files - exactly what a
// process restart does - and asserts the memory came back.
#include <memory>
#include <string>

#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/vector_memory.h"
#include "posix_fs.h"
#include "test_util.h"

using namespace nimbusd;
namespace orch = nimbus::orch;

// Build a deterministic unit vector so recall is real (no embedder needed): a
// one-hot int8 vector in dimension `slot`.
static std::vector<int8_t> oneHot(int dims, int slot) {
  std::vector<int8_t> v(dims, 0);
  v[slot % dims] = 127;
  return v;
}

static orch::EpisodicMessage msg(const std::string& sid, const std::string& role,
                                 const std::string& text, uint32_t tsHours) {
  orch::EpisodicMessage m;
  m.sessionId = sid;
  m.role = role;
  m.kind = orch::MsgKind::Message;
  m.text = text;
  m.tsHours = tsHours;
  return m;
}

// ---- 1. fsutil: atomic write + mkdirs + read round-trip ---------------------
static void testFsutil(ndtest::Ctx& c) {
  const std::string dir = ndtest::scratchDir("fsutil");
  ndtest::rmTree(dir);
  const std::string path = dir + "/a/b/c/data.bin";

  ndtest::Ctx& t = c;
  t.ok(fsutil::writeFileAtomic(path, "hello\0world"), "writeFileAtomic creates nested dirs");

  std::string got;
  t.ok(fsutil::readFile(path, got), "readFile reads it back");
  t.eqi((long)got.size(), 5, "atomic write stored the exact bytes (up to the NUL)");

  // The .tmp sidecar must not survive a successful write.
  std::string leftover;
  t.ok(!fsutil::readFile(path + ".tmp", leftover), "no .tmp file left after rename");

  // Overwrite in place is atomic and replaces contents.
  t.ok(fsutil::writeFileAtomic(path, "second"), "overwrite succeeds");
  fsutil::readFile(path, got);
  t.eq(got, "second", "overwrite replaced the contents");

  ndtest::rmTree(dir);
}

// ---- 2. episodic append-log survives a restart ------------------------------
// Write messages through one store, destroy it, hydrate a fresh store from the
// same directory, and assert the history + query semantics came back.
static void testEpisodicRestart(ndtest::Ctx& t) {
  const std::string root = ndtest::scratchDir("epi");
  ndtest::rmTree(root);
  const std::string dir = root + "/mem/episodic";

  {
    PosixEpiFs fs;
    orch::AppendLogEpisodicStore store(fs, dir, /*recentCap=*/8);
    store.hydrate(0, 0);  // empty dir -> nothing
    store.addMessage(msg("owner", "user", "the spare key is behind the owl", 100));
    store.addMessage(msg("owner", "assistant", "noted, behind the owl", 101));
    store.addMessage(msg("work", "user", "deploy at noon", 102));
    t.eqi(store.messageCount(), 3, "three messages written");
  }  // store + fs destroyed == process exit

  {
    // "Restart": brand-new store objects, same directory on disk.
    PosixEpiFs fs;
    orch::AppendLogEpisodicStore store(fs, dir, /*recentCap=*/8);
    const int n = store.hydrate(0, 0);
    t.eqi(n, 3, "hydrate rebuilt all three messages from disk");
    t.eqi(store.messageCount(), 3, "messageCount after restart");

    orch::MsgQuery q;
    q.sessionId = "owner";
    q.limit = 10;
    auto rows = store.query(q);
    t.eqi((long)rows.size(), 2, "the 'owner' session's two rows survived");

    orch::MsgQuery q2;
    q2.textContains = "owl";
    q2.limit = 10;
    auto hits = store.query(q2);
    t.ok(!hits.empty() && hits[0].text.find("owl") != std::string::npos,
         "full-text query finds the recalled fact after restart");
  }

  ndtest::rmTree(root);
}

// ---- 2b. out-of-ring records read back by their append offset ---------------
// With more messages than the recent-window cap, a query must pull the older
// records off disk via readRange(offset,len) using the offset append() returned.
// A wrong offset (the ofstream::tellp-in-append-mode trap on some libstdc++)
// makes those records decode as garbage and vanish. Force that path here so the
// Linux Docker `make test` guards the offset cross-platform.
static void testEpisodicColdRead(ndtest::Ctx& t) {
  const std::string root = ndtest::scratchDir("epicold");
  ndtest::rmTree(root);
  const std::string dir = root + "/mem/episodic";
  PosixEpiFs fs;
  orch::AppendLogEpisodicStore store(fs, dir, /*recentCap=*/2);  // tiny ring
  store.hydrate(0, 0);
  for (int i = 0; i < 6; i++)
    store.addMessage(msg("owner", "user", "record-" + std::to_string(i), 100 + i));
  t.eqi(store.messageCount(), 6, "six messages written (recentCap 2, so 4 are out-of-ring)");

  orch::MsgQuery q;
  q.sessionId = "owner";
  q.limit = 10;
  auto rows = store.query(q);
  t.eqi((long)rows.size(), 6, "query returns all six (the 4 out-of-ring read via offset)");
  // The oldest record (record-0) is out of the ring; it must come back intact -
  // which only works if its append offset was recorded correctly.
  bool foundOldest = false;
  for (const auto& m : rows) if (m.text == "record-0") foundOldest = true;
  t.ok(foundOldest, "the oldest out-of-ring record read back intact by its offset");
  ndtest::rmTree(root);
}

// ---- 3. vector memory persists across a restart (atomic vectors.bin) --------
static void testVectorRestart(ndtest::Ctx& t) {
  const std::string root = ndtest::scratchDir("vec");
  ndtest::rmTree(root);
  const std::string binPath = root + "/mem/vectors.bin";
  const int dims = 64;

  {
    orch::VectorMemory vec;
    vec.configure(dims);
    orch::VecEntry e;
    e.id = "v1";
    e.content = "the workshop combination is 1-9-8-4";
    e.importance = 0.9f;
    e.ttlHours = -1;  // never expires
    e.vec = oneHot(dims, 3);
    t.ok(vec.add(e), "vector added");
    t.eqi(vec.size(), 1, "one vector in the store");
    // Persist with the same tmp->rename discipline the daemon flush uses.
    t.ok(fsutil::writeFileAtomic(binPath, vec.serialize()), "vectors.bin written atomically");
  }

  {
    orch::VectorMemory vec;
    vec.configure(dims);
    std::string blob;
    t.ok(fsutil::readFile(binPath, blob), "vectors.bin read back after restart");
    t.ok(vec.deserialize(blob), "deserialize accepted the blob");
    t.eqi(vec.size(), 1, "the vector survived the restart");

    auto hits = vec.search(oneHot(dims, 3), 1);
    t.ok(!hits.empty() && hits[0].content.find("1-9-8-4") != std::string::npos,
         "associative recall returns the persisted fact after restart");
  }

  ndtest::rmTree(root);
}

// ---- 4. a torn vectors.bin never crashes the boot (tolerant deserialize) ----
static void testTornBlob(ndtest::Ctx& t) {
  orch::VectorMemory vec;
  vec.configure(64);
  // Garbage / truncated blob: deserialize must return false and leave a usable
  // (empty) store rather than throw - the daemon logs a partial load and runs.
  const bool ok = vec.deserialize("not a real vectors.bin blob");
  t.ok(!ok, "a garbage blob is rejected (returns false)");
  t.eqi(vec.size(), 0, "the store is left empty and usable, not corrupt");
}

int main() {
  ndtest::Ctx c;
  c.suite = "posix_stores";
  std::printf("=== posix_stores (T1, offline) ===\n");
  std::printf("  -- fsutil atomic write --\n");
  testFsutil(c);
  std::printf("  -- episodic append-log restart --\n");
  testEpisodicRestart(c);
  std::printf("  -- episodic out-of-ring offset read --\n");
  testEpisodicColdRead(c);
  std::printf("  -- vector memory restart --\n");
  testVectorRestart(c);
  std::printf("  -- tolerant deserialize --\n");
  testTornBlob(c);

  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
