#include <unity.h>
#include <algorithm>
#include <array>

#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/retention.h"
#include "nimbus/orch/psram_alloc.h"   // CUM-185: prove the recent-ring/index cache routes to PSRAM
#include <cstdlib>
#include <functional>
#include <memory>

#include "../support/episodic_fixture.h"   // CUM-515: bench B's recorded history

using namespace nimbus::orch;

// ---- in-memory EpiFs that counts every op, so the tests can PROVE the
// O(1)-append + zero-read-fast-path + skip-out-of-window properties, not just
// the query results. -------------------------------------------------------
struct FakeFs : EpiFs {
  std::map<std::string, std::string> files;
  mutable int appends = 0, readAlls = 0, readRanges = 0, lists = 0, removes = 0, sizes = 0;
  mutable long bytesRead = 0;   // every byte a read handed back (boot-scan and slice budgets)

  void reset() { appends = readAlls = readRanges = lists = removes = sizes = 0; bytesRead = 0; }

  long size(const std::string& path) const override {
    sizes++;
    auto it = files.find(path);
    return it == files.end() ? 0 : (long)it->second.size();
  }

  bool failAppend = false;   // simulate a mid-op SD I/O error on write
  long append(const std::string& path, const std::string& bytes) override {
    appends++;
    if (failAppend) return -1;   // card vanished: the write fails
    std::string& f = files[path];
    long off = (long)f.size();
    f += bytes;
    return off;
  }
  std::string readAll(const std::string& path) const override {
    readAlls++;
    auto it = files.find(path);
    if (it == files.end()) return std::string();
    bytesRead += (long)it->second.size();
    return it->second;
  }
  bool failReads = false;    // simulate a card that lists but will not read
  std::string readRange(const std::string& path, long offset, long len) const override {
    readRanges++;
    auto it = files.find(path);
    if (failReads || it == files.end() || offset < 0 || len < 0) return std::string();
    if ((size_t)(offset + len) > it->second.size()) return std::string();
    bytesRead += len;
    return it->second.substr(offset, len);
  }
  // Streamed listing, like the device: no materialized vector of names.
  void forEachName(const std::string& dir,
                   const std::function<void(const char*)>& fn) const override {
    lists++;
    const std::string prefix = dir + "/";
    for (auto it = files.lower_bound(prefix);
         it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
      const char* rest = it->first.c_str() + prefix.size();
      if (std::strchr(rest, '/') != nullptr) continue;   // no recursion
      fn(rest);
    }
  }
  std::vector<std::string> list(const std::string& dir) const override {
    lists++;
    std::vector<std::string> out;
    std::string prefix = dir + "/";
    for (const auto& kv : files) {
      if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
      std::string rest = kv.first.substr(prefix.size());
      if (rest.find('/') != std::string::npos) continue;  // no recursion
      out.push_back(rest);
    }
    return out;
  }
  bool failRemove = false;  // simulate an SD I/O error on delete
  bool remove(const std::string& path) override {
    removes++;
    if (failRemove) return false;  // file survives on disk
    return files.erase(path) > 0;
  }
  bool exists(const std::string& path) const override { return files.count(path) > 0; }
};

void setUp() {}
void tearDown() {}

static EpisodicMessage msg(const std::string& id, const std::string& sess, uint32_t tsHours,
                           const std::string& role, MsgKind kind, const std::string& text) {
  EpisodicMessage m;
  m.id = id; m.sessionId = sess; m.tsHours = tsHours; m.role = role; m.kind = kind; m.text = text;
  return m;
}
// ts for a given epoch-day so messages land in distinct day-streams.
static uint32_t dayTs(uint32_t dayNum, uint32_t hourInDay = 0) { return dayNum * 24 + hourInDay; }

// ---- JSONL codec round-trip, incl. characters that MUST be escaped ----------
static void test_codec_roundtrip_escaping() {
  EpisodicMessage m = msg("m1", "s1", 42, "user", MsgKind::Message,
                          "quote:\" backslash:\\ newline:\n tab:\t done");
  m.blobPath = "/mem/blobs/abc.ogg";
  m.tags = "a,b";
  std::string line = encodeEpisodicLine(m);
  TEST_ASSERT_TRUE(line.find('\n') == std::string::npos);  // one line: newline escaped
  EpisodicMessage b;
  TEST_ASSERT_TRUE(decodeEpisodicLine(line, b));
  TEST_ASSERT_EQUAL_STRING(m.text.c_str(), b.text.c_str());
  TEST_ASSERT_EQUAL_STRING("/mem/blobs/abc.ogg", b.blobPath.c_str());
  TEST_ASSERT_TRUE(b.kind == MsgKind::Message);
  // a torn/garbage line is rejected, not silently accepted
  EpisodicMessage junk;
  TEST_ASSERT_FALSE(decodeEpisodicLine("{\"id\":\"m1\",\"ts\":", junk));
  TEST_ASSERT_FALSE(decodeEpisodicLine("", junk));
}

// ---- addMessage is O(1): exactly one append, no whole-file rewrite ----------
static void test_append_is_o1() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 256);
  for (int i = 1; i <= 20; i++)
    st.addMessage(msg("m" + std::to_string(i), "s1", dayTs(100, i), "user", MsgKind::Message, "x"));
  TEST_ASSERT_EQUAL_INT(20, st.messageCount());
  TEST_ASSERT_EQUAL_INT(20, fs.appends);   // one append per message, nothing more
  TEST_ASSERT_EQUAL_INT(0, fs.readAlls);   // never re-reads to rewrite
  TEST_ASSERT_EQUAL_INT(0, fs.readRanges); // small history -> served from ring
}

// ---- query parity with the in-memory reference, both ring + index paths -----
static void assert_same_query(const EpisodicStore& a, const EpisodicStore& b, const MsgQuery& q) {
  auto ra = a.query(q), rb = b.query(q);
  TEST_ASSERT_EQUAL_INT((int)ra.size(), (int)rb.size());
  for (size_t i = 0; i < ra.size(); i++)
    TEST_ASSERT_EQUAL_STRING(ra[i].id.c_str(), rb[i].id.c_str());
}
static void run_parity(int recentCap) {
  FakeFs fs;
  AppendLogEpisodicStore log(fs, "/mem/episodic", recentCap);
  InMemoryEpisodicStore ref;
  // spread across days, sessions, kinds
  for (uint32_t d = 100; d < 105; d++)
    for (int i = 0; i < 4; i++) {
      MsgKind k = (i % 2) ? MsgKind::LlmResponse : MsgKind::Message;
      std::string sess = (i < 2) ? "s1" : "s2";
      std::string txt = (i == 3) ? "shipping soon" : "chatter";
      auto m = msg("m" + std::to_string(d) + "_" + std::to_string(i), sess, dayTs(d, i),
                   "user", k, txt);
      log.addMessage(m);
      ref.addMessage(m);
    }
  MsgQuery all; all.limit = 100;
  assert_same_query(log, ref, all);
  MsgQuery sess; sess.sessionId = "s2"; sess.limit = 100;
  assert_same_query(log, ref, sess);
  MsgQuery kind; kind.haveKind = true; kind.kind = MsgKind::LlmResponse; kind.limit = 100;
  assert_same_query(log, ref, kind);
  MsgQuery win; win.sinceHours = dayTs(102); win.beforeHours = dayTs(104); win.limit = 100;
  assert_same_query(log, ref, win);
  MsgQuery txt; txt.textContains = "shipping"; txt.limit = 100;
  assert_same_query(log, ref, txt);
  MsgQuery lim; lim.limit = 3;
  assert_same_query(log, ref, lim);
}
static void test_query_parity_ring_path() { run_parity(1000); }  // whole history resident
static void test_query_parity_index_path() { run_parity(2); }    // forces the index+FS path

// ---- index skips out-of-window day files (no read for excluded days) --------
static void test_index_skips_out_of_window_days() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 2);  // tiny ring -> index path
  for (uint32_t d = 100; d <= 102; d++)
    for (int i = 0; i < 2; i++)
      st.addMessage(msg("m" + std::to_string(d) + std::to_string(i), "s1", dayTs(d, i),
                        "user", MsgKind::Message, "x"));
  TEST_ASSERT_EQUAL_INT(6, st.messageCount());
  fs.reset();
  MsgQuery mid; mid.sinceHours = dayTs(101); mid.beforeHours = dayTs(102); mid.limit = 100;
  auto r = st.query(mid);
  TEST_ASSERT_EQUAL_INT(2, (int)r.size());       // only day 101's two rows
  TEST_ASSERT_EQUAL_INT(2, fs.readRanges);        // exactly the 2 survivors read
  TEST_ASSERT_EQUAL_INT(0, fs.readAlls);          // day 100 + 102 files never opened
}

// ---- recent-window fast path: a resident history answers with zero FS reads --
static void test_recent_window_zero_reads() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 50);
  for (int i = 0; i < 10; i++)
    st.addMessage(msg("m" + std::to_string(i), "s1", dayTs(100, i), "user", MsgKind::Message, "x"));
  fs.reset();
  MsgQuery q; q.limit = 100;
  TEST_ASSERT_EQUAL_INT(10, (int)st.query(q).size());
  TEST_ASSERT_EQUAL_INT(0, fs.readRanges);
  TEST_ASSERT_EQUAL_INT(0, fs.readAlls);
}

// ---- uncapped history: the old 500-ring cap is lifted -----------------------
static void test_uncapped_history_beyond_500() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 100);  // ring << history
  for (int i = 1; i <= 600; i++)
    st.addMessage(msg("m" + std::to_string(i), "s1", dayTs(100 + i / 50, i % 24),
                      "user", MsgKind::Message, "x"));
  TEST_ASSERT_EQUAL_INT(600, st.messageCount());  // all 600 retained (was capped at 500)
  // All 600 stay reachable - but a single call is bounded (kIndexReadsPerCall),
  // because each row read off the index is a real card read (~100 ms on
  // hardware) and one unbounded query would tie the querying task up for a
  // minute. The caller pages with the cursor the store hands back.
  MsgQuery q; q.limit = 600;
  int total = 0, pages = 0;
  std::string before;
  for (; pages < 20; pages++) {
    q.before = before;
    EpiQueryInfo info;
    const int got = (int)st.query(q, &info).size();
    total += got;
    if (info.nextBefore.empty()) break;
    TEST_ASSERT_TRUE(info.nextBefore != before);   // must advance
    before = info.nextBefore;
  }
  TEST_ASSERT_EQUAL_INT(600, total);
}

// ---- hydrate rebuilds index/cache/sessions + resumes id counter -------------
static void test_hydrate_rebuilds_state() {
  FakeFs fs;
  {
    AppendLogEpisodicStore a(fs, "/mem/episodic", 4);
    a.addSession({"s1", 100, "openai", "task A", "active"});
    for (int i = 1; i <= 8; i++)
      a.addMessage(msg("m0000000" + std::to_string(i), "s1", dayTs(100, i),
                       "user", MsgKind::Message, "line " + std::to_string(i)));
    a.setSessionStatus("s1", "completed");
  }
  // fresh store over the same bytes
  AppendLogEpisodicStore b(fs, "/mem/episodic", 4);
  TEST_ASSERT_EQUAL_INT(8, b.hydrate());
  TEST_ASSERT_EQUAL_INT(8, b.messageCount());
  TEST_ASSERT_EQUAL_INT(1, (int)b.sessions("completed").size());  // LWW status survived
  MsgQuery q; q.limit = 100;
  auto r = b.query(q);
  TEST_ASSERT_EQUAL_INT(8, (int)r.size());
  TEST_ASSERT_EQUAL_STRING("m00000008", r[0].id.c_str());  // newest-first
  TEST_ASSERT_EQUAL_STRING("line 8", r[0].text.c_str());
  TEST_ASSERT_EQUAL_UINT32(9, b.nextIdHint());             // resumes past 0x8
}

// ---- hydrate tolerates a torn last line (power loss mid-append) --------------
static void test_hydrate_tolerates_torn_line() {
  FakeFs fs;
  {
    AppendLogEpisodicStore a(fs, "/mem/episodic", 100);
    a.addMessage(msg("m1", "s1", dayTs(100, 1), "user", MsgKind::Message, "good one"));
    a.addMessage(msg("m2", "s1", dayTs(100, 2), "user", MsgKind::Message, "good two"));
  }
  // simulate a half-written third line appended without a newline terminator
  fs.files[std::string("/mem/episodic/") + AppendLogEpisodicStore::civilDate(100) + ".jsonl"] +=
      "{\"id\":\"m3\",\"sid\":\"s1\",\"ts\":";
  AppendLogEpisodicStore b(fs, "/mem/episodic", 100);
  TEST_ASSERT_EQUAL_INT(2, b.hydrate());   // the two good rows load, torn line skipped
  MsgQuery q; q.limit = 10;
  TEST_ASSERT_EQUAL_INT(2, (int)b.query(q).size());
}

// ---- nextIdHint reflects the max id suffix, not the row count ---------------
static void test_next_id_hint() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 100);
  TEST_ASSERT_EQUAL_UINT32(0, st.nextIdHint());  // empty
  st.addMessage(msg("m00000005", "s", dayTs(100), "u", MsgKind::Message, "a"));
  st.addMessage(msg("m0000000a", "s", dayTs(100), "u", MsgKind::Message, "b"));  // 0x0a = 10
  TEST_ASSERT_EQUAL_UINT32(11, st.nextIdHint());
}

// ---- retention prune: old day-streams + unreferenced blobs ------------------
static void test_prune_old_days_and_blobs() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 4);
  // day 100 (old) has a message referencing blob "aaa"; day 130 (recent) references "bbb".
  EpisodicMessage oldMsg = msg("m1", "s1", dayTs(100, 1), "user", MsgKind::Audio, "old voice");
  oldMsg.blobPath = "/mem/blobs/aaa.ogg";
  st.addMessage(oldMsg);
  st.addMessage(msg("m2", "s1", dayTs(101, 1), "user", MsgKind::Message, "also old"));
  EpisodicMessage newMsg = msg("m3", "s1", dayTs(130, 1), "user", MsgKind::Audio, "new voice");
  newMsg.blobPath = "/mem/blobs/bbb.ogg";
  st.addMessage(newMsg);
  // blob sidecars present on the card: aaa (old-referenced), bbb (new-referenced),
  // ccc (orphan referenced by nobody).
  fs.files["/mem/blobs/aaa.ogg"] = "A";
  fs.files["/mem/blobs/bbb.ogg"] = "B";
  fs.files["/mem/blobs/ccc.ogg"] = "C";

  EpiPruneReport rep = st.prune(/*cutoffDayNum=*/120, "/mem/blobs");

  TEST_ASSERT_EQUAL_INT(1, rep.keptMessages);           // only day-130 survives
  TEST_ASSERT_EQUAL_INT(2, (int)rep.removedDayFiles.size());  // days 100 + 101 files gone
  // aaa was only referenced by the pruned old message -> now orphan -> removed;
  // ccc never referenced -> removed; bbb still referenced -> kept.
  TEST_ASSERT_EQUAL_INT(2, (int)rep.removedBlobs.size());
  TEST_ASSERT_TRUE(fs.files.count("/mem/blobs/bbb.ogg") == 1);
  TEST_ASSERT_TRUE(fs.files.count("/mem/blobs/aaa.ogg") == 0);
  TEST_ASSERT_TRUE(fs.files.count("/mem/blobs/ccc.ogg") == 0);
  // old day files are physically gone
  TEST_ASSERT_TRUE(fs.files.count(st.dayFile(100)) == 0);
  TEST_ASSERT_TRUE(fs.files.count(st.dayFile(130)) == 1);
  // surviving history still queryable + consistent
  MsgQuery q; q.limit = 10;
  auto r = st.query(q);
  TEST_ASSERT_EQUAL_INT(1, (int)r.size());
  // sessions.jsonl compacted (Release C4): the append-only file collapses to
  // exactly one row per live session at prune time. Session "s1" was upserted
  // by all three addMessage-adjacent addSession calls? (this test never called
  // addSession - add rows now and prune again to observe the compaction)
  EpisodicSession sess; sess.id = "s1"; sess.provider = "p"; sess.title = "t";
  st.addSession(sess);
  sess.title = "t2"; st.addSession(sess);   // upsert -> 2 appended rows
  sess.title = "t3"; st.addSession(sess);   // 3 rows on disk, 1 live session
  std::string before = fs.files["/mem/episodic/sessions.jsonl"];
  TEST_ASSERT_EQUAL_INT(3, (int)std::count(before.begin(), before.end(), '\n'));
  st.prune(120, "/mem/blobs");
  std::string after = fs.files["/mem/episodic/sessions.jsonl"];
  TEST_ASSERT_EQUAL_INT(1, (int)std::count(after.begin(), after.end(), '\n'));
  TEST_ASSERT_TRUE(after.find("t3") != std::string::npos);   // the LIVE upsert survives
  TEST_ASSERT_EQUAL_STRING("m3", r[0].id.c_str());
}

// ---- prune keeps index in sync with disk when a day-file delete FAILS -------
// (adversarial-review finding: an erased index record whose file survives on disk
// gets replayed by hydrate() on the next boot, and the report falsely omits it.)
static void test_prune_remove_failure_keeps_index_consistent() {
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 4);
    w.addMessage(msg("m1", "s1", dayTs(100, 1), "user", MsgKind::Message, "old"));
    w.addMessage(msg("m2", "s1", dayTs(130, 1), "user", MsgKind::Message, "new"));
  }
  AppendLogEpisodicStore st(fs, "/mem/episodic", 4);
  st.hydrate();
  fs.failRemove = true;  // the SD delete will fail; the day-100 file survives

  EpiPruneReport rep = st.prune(/*cutoffDayNum=*/120, "/mem/blobs");

  // the day-100 file could not be deleted, so it is NOT reported as removed and its
  // record is NOT dropped from the index (which would desync from disk + replay).
  TEST_ASSERT_EQUAL_INT(0, (int)rep.removedDayFiles.size());
  TEST_ASSERT_EQUAL_INT(2, st.messageCount());              // both rows retained
  TEST_ASSERT_TRUE(fs.files.count(st.dayFile(100)) == 1);   // file still on disk
  // a fresh hydrate matches the (unchanged) on-disk state - no phantom loss
  AppendLogEpisodicStore reload(fs, "/mem/episodic", 4);
  TEST_ASSERT_EQUAL_INT(2, reload.hydrate());
  // once the delete works, the prune completes cleanly
  fs.failRemove = false;
  EpiPruneReport rep2 = st.prune(120, "/mem/blobs");
  TEST_ASSERT_EQUAL_INT(1, (int)rep2.removedDayFiles.size());
  TEST_ASSERT_EQUAL_INT(1, st.messageCount());
  TEST_ASSERT_TRUE(fs.files.count(st.dayFile(100)) == 0);
}

// ---- civilDate is a stable, sortable YYYY-MM-DD -----------------------------
static void test_civil_date_sortable() {
  // epoch day 0 is 1970-01-01; +1 day later; a known modern date.
  TEST_ASSERT_EQUAL_STRING("1970-01-01", AppendLogEpisodicStore::civilDate(0).c_str());
  TEST_ASSERT_EQUAL_STRING("1970-01-02", AppendLogEpisodicStore::civilDate(1).c_str());
  // 2026-07-04 is epoch day 20638.
  TEST_ASSERT_EQUAL_STRING("2026-07-04", AppendLogEpisodicStore::civilDate(20638).c_str());
  // lexical order == chronological order (the property hydrate relies on)
  TEST_ASSERT_TRUE(AppendLogEpisodicStore::civilDate(100) < AppendLogEpisodicStore::civilDate(101));
  TEST_ASSERT_TRUE(AppendLogEpisodicStore::civilDate(20637) < AppendLogEpisodicStore::civilDate(20638));
}

// SUDDEN SD LOSS: a failed FS append must NOT silently drop the message - it
// stays in the RAM recent-window so the conversation's working set survives, and
// unpersistedCount() rises to signal the device to demote the tier. Re-insert
// (append succeeds again) resumes durable writes without losing the RAM tail.
static void test_append_failure_falls_back_to_ring() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 256);
  st.addMessage(msg("m1", "s1", dayTs(100, 1), "user", MsgKind::Message, "before loss"));
  TEST_ASSERT_EQUAL_INT(0, st.unpersistedCount());
  TEST_ASSERT_EQUAL_INT(1, st.messageCount());

  // Card vanishes mid-op: the next two appends fail.
  fs.failAppend = true;
  st.addMessage(msg("m2", "s1", dayTs(100, 2), "assistant", MsgKind::Message, "during loss A"));
  st.addMessage(msg("m3", "s1", dayTs(100, 3), "user", MsgKind::Message, "during loss B"));
  TEST_ASSERT_EQUAL_INT(2, st.unpersistedCount());      // both retained in RAM, not dropped
  TEST_ASSERT_EQUAL_INT(3, st.messageCount());          // count includes the unpersisted

  // The working set is still fully queryable (graceful - no crash, no silent loss).
  MsgQuery q; q.sessionId = "s1"; q.limit = 10;
  auto all = st.query(q);
  TEST_ASSERT_EQUAL_INT(3, (int)all.size());
  TEST_ASSERT_EQUAL_STRING("during loss B", all[0].text.c_str());   // newest-first
  TEST_ASSERT_EQUAL_STRING("before loss", all[2].text.c_str());

  // Card re-seated: durable writes resume; the RAM tail is not lost.
  fs.failAppend = false;
  st.addMessage(msg("m4", "s1", dayTs(100, 4), "assistant", MsgKind::Message, "after reinsert"));
  TEST_ASSERT_EQUAL_INT(2, st.unpersistedCount());      // still 2 RAM-only
  TEST_ASSERT_EQUAL_INT(4, st.messageCount());
  auto after = st.query(q);
  TEST_ASSERT_EQUAL_INT(4, (int)after.size());
  TEST_ASSERT_EQUAL_STRING("after reinsert", after[0].text.c_str());
}

// A device must be able to START no matter how much history it has written.
//
// The unbounded hydrate read every day-stream whole and JSON-parsed every row
// inside setup(), on the watchdog's task, before the device could serve anything.
// A board that had merely ACCUMULATED enough (~15 K rows - a chatty month) was
// reset mid-scan every boot, forever; power-cycling did not help, and retention
// prune could not save it because prune runs after boot. The device had written
// itself into a state it could not start from.
static void test_boot_scan_is_bounded_so_a_full_store_still_boots() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 64);
  for (int day = 0; day < 30; day++)
    for (int i = 0; i < 400; i++) {
      EpisodicMessage m;
      m.sessionId = "web"; m.role = "user"; m.kind = MsgKind::Message;
      m.tsHours = (uint32_t)(day * 24 + 1);
      m.text = "row";
      st.addMessage(m);
    }

  AppendLogEpisodicStore boot(fs, "/mem/episodic", 64);
  int yields = 0;
  const int indexed = boot.hydrate(kHydrateMaxRows, kHydrateMaxBytes,
                                   [&] { yields++; });

  // It stopped at the budget rather than reading everything...
  TEST_ASSERT_TRUE(indexed <= kHydrateMaxRows);
  TEST_ASSERT_TRUE(boot.hydrateTruncated());
  // ...it yielded so the caller could feed the watchdog...
  TEST_ASSERT_TRUE(yields > 0);
  // ...and what it kept is the NEWEST history, which is what any query wants.
  MsgQuery q; q.limit = 1;
  const auto rows = boot.query(q);
  TEST_ASSERT_EQUAL_INT(1, (int)rows.size());
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(29 * 24 + 1), rows[0].tsHours);
}

// A store that fits the budget must be indexed COMPLETELY - the bound must not
// quietly cost history on an ordinary device.
static void test_a_small_store_is_fully_indexed_and_not_truncated() {
  FakeFs fs;
  AppendLogEpisodicStore st(fs, "/mem/episodic", 64);
  for (int i = 0; i < 50; i++) {
    EpisodicMessage m;
    m.sessionId = "web"; m.role = "user"; m.kind = MsgKind::Message;
    m.tsHours = 100; m.text = "row";
    st.addMessage(m);
  }
  AppendLogEpisodicStore boot(fs, "/mem/episodic", 64);
  TEST_ASSERT_EQUAL_INT(50, boot.hydrate());
  TEST_ASSERT_FALSE(boot.hydrateTruncated());
}

// ---- v4.0.0 DEEP HISTORY ----------------------------------------------------
// The boot scan is budget-bounded, so on a real device the older rows sit on the
// card OUTSIDE the index - a plain query cannot see them, which is how "a month
// of chat" became unreachable. These pin the cold pass that reads them back.

// Fill ONE day-file with `n` rows (deliberately one file: a row cursor alone
// cannot page within a file, which is why the cursor is byte-resolution).
static void fillOneDay(FakeFs& fs, uint32_t dayNum, int n, const char* marker,
                       int markerAt) {
  AppendLogEpisodicStore st(fs, "/mem/episodic", 64);
  char id[16];
  for (int i = 0; i < n; i++) {
    snprintf(id, sizeof(id), "m%08x", i + 1);
    std::string text = (i == markerAt) ? std::string(marker)
                                       : ("filler row " + std::to_string(i));
    st.addMessage(msg(id, "chat1", dayTs(dayNum, (uint32_t)(i % 24)), "user",
                      MsgKind::Message, text));
  }
}

static void test_cold_scan_finds_what_the_index_cannot() {
  FakeFs fs;
  fillOneDay(fs, 19000, 400, "the bilge pump serial is BP-4471", /*markerAt=*/5);

  // Boot with a budget that indexes only the newest 50 rows - the marker (row 5)
  // is far below the floor, exactly the shipped device's situation.
  AppendLogEpisodicStore st(fs, "/mem/episodic", 16);
  st.hydrate(/*maxRows=*/50, /*maxBytes=*/0);
  TEST_ASSERT_TRUE(st.hydrateTruncated());

  MsgQuery q;
  q.textContains = "bilge pump";
  q.limit = 5;

  // RED: the default query must NOT reach it (this is the permanent mutation
  // check - flipping coldScan on by default, or making the hot pass read the
  // whole file, turns this assertion red).
  EpiQueryInfo hot;
  TEST_ASSERT_EQUAL(0, (int)st.query(q, &hot).size());
  TEST_ASSERT_TRUE(hot.olderExists);

  // GREEN: opted in, it is found - and the answer says how far back it looked.
  q.coldScan = true;
  EpiQueryInfo cold;
  std::vector<EpisodicMessage> got = st.query(q, &cold);
  TEST_ASSERT_EQUAL(1, (int)got.size());
  TEST_ASSERT_TRUE(got[0].text.find("BP-4471") != std::string::npos);
  TEST_ASSERT_TRUE(cold.coldBytes > 0);
}

static void test_cold_cursor_pages_within_one_day_file() {
  FakeFs fs;
  fillOneDay(fs, 19000, 400, "needle-XYZ", /*markerAt=*/3);
  AppendLogEpisodicStore st(fs, "/mem/episodic", 16);
  st.hydrate(/*maxRows=*/20, /*maxBytes=*/0);

  MsgQuery q;
  q.coldScan = true;
  q.limit = 10;                 // small pages: forces the cursor to do real work
  bool found = false;
  std::string before;
  for (int page = 0; page < 60 && !found; page++) {
    q.before = before;
    EpiQueryInfo info;
    std::vector<EpisodicMessage> rows = st.query(q, &info);
    for (const auto& m : rows)
      if (m.text.find("needle-XYZ") != std::string::npos) found = true;
    if (rows.empty() && info.nextBefore.empty()) break;
    // The cursor must ADVANCE, or a caller loops forever on the same page.
    TEST_ASSERT_TRUE(info.nextBefore.empty() || info.nextBefore != before);
    before = info.nextBefore;
    if (before.empty()) break;
  }
  TEST_ASSERT_TRUE(found);
}

static void test_cold_scan_honors_the_read_boundary() {
  FakeFs fs;
  {
    AppendLogEpisodicStore st(fs, "/mem/episodic", 8);
    char id[16];
    for (int i = 0; i < 200; i++) {
      snprintf(id, sizeof(id), "m%08x", i + 1);
      const bool mine = (i % 2) == 0;
      st.addMessage(msg(id, mine ? "chat1" : "chat2", dayTs(19000, 3), "user",
                        MsgKind::Message,
                        mine ? "secret-mine row" : "secret-theirs row"));
    }
  }
  AppendLogEpisodicStore st(fs, "/mem/episodic", 8);
  st.hydrate(/*maxRows=*/10, /*maxBytes=*/0);

  MsgQuery q;
  q.coldScan = true;
  q.textContains = "secret";
  q.limit = 100;
  q.sessionAllow = {"chat1"};   // the principal's boundary, enforced on disk too
  for (const auto& m : st.query(q, nullptr))
    TEST_ASSERT_EQUAL_STRING("chat1", m.sessionId.c_str());
}

static void test_cold_scan_budget_is_bounded_and_honest() {
  FakeFs fs;
  // Five day files, each comfortably large: one call must not walk them all.
  for (uint32_t d = 0; d < 5; d++) fillOneDay(fs, 19000 + d, 300, "none", -1);
  AppendLogEpisodicStore st(fs, "/mem/episodic", 8);
  st.hydrate(/*maxRows=*/5, /*maxBytes=*/0);

  MsgQuery q;
  q.coldScan = true;
  q.textContains = "no-such-text-anywhere";
  q.limit = 50;
  EpiQueryInfo info;
  st.query(q, &info);
  TEST_ASSERT_TRUE(info.coldFiles <= kColdMaxFiles);
  TEST_ASSERT_TRUE(info.coldBytes <= kColdMaxBytes);
  TEST_ASSERT_TRUE(info.olderExists);          // it stopped short - and says so
  TEST_ASSERT_TRUE(!info.nextBefore.empty());  // with the token to continue
}

static void test_before_cursor_pages_the_indexed_range() {
  FakeFs fs;
  fillOneDay(fs, 19000, 40, "none", -1);
  AppendLogEpisodicStore st(fs, "/mem/episodic", 4);   // ring smaller than history
  st.hydrate(0, 0);                                    // everything indexed

  MsgQuery q;
  q.limit = 10;
  std::vector<EpisodicMessage> p1 = st.query(q, nullptr);
  TEST_ASSERT_EQUAL(10, (int)p1.size());
  q.before = p1.back().id;
  std::vector<EpisodicMessage> p2 = st.query(q, nullptr);
  TEST_ASSERT_EQUAL(10, (int)p2.size());
  // Strictly older, no overlap.
  TEST_ASSERT_TRUE(epiIdSuffix(p2.front().id) < epiIdSuffix(p1.back().id));
}

// ---- epiTextMatch: what the substring filter could not do -------------------
// CUM-515 re-bless (reason): "bilge anchor" used to be pinned FALSE - the ALL-
// of-terms rule that returned nothing for "advisor supervisor PhD" on bench B.
// One counted word is now enough; ranking (not the filter) puts full matches
// first, and a needle sharing NO word with the row still misses.
static void test_text_match_case_and_order_insensitive() {
  TEST_ASSERT_TRUE(epiTextMatch("The Bilge Pump serial is BP-4471", "bilge pump"));
  TEST_ASSERT_TRUE(epiTextMatch("The Bilge Pump serial is BP-4471", "pump bilge"));
  TEST_ASSERT_TRUE(epiTextMatch("The Bilge Pump serial is BP-4471", "BP-4471 serial"));
  TEST_ASSERT_TRUE(epiTextMatch("The Bilge Pump serial", "bilge anchor"));    // any word
  TEST_ASSERT_FALSE(epiTextMatch("The Bilge Pump serial", "anchor diesel"));  // no word
  TEST_ASSERT_FALSE(epiTextMatch("The Bilge Pump serial", "the of what"));    // common words only
  TEST_ASSERT_TRUE(epiTextMatch("anything", ""));        // no filter
  TEST_ASSERT_TRUE(epiTextMatch("anything", "   "));     // whitespace-only: no filter
  TEST_ASSERT_TRUE(epiTextMatch("anything", "?!..."));   // punctuation-only: no filter
}

static void test_id_suffix_orders_rows() {
  TEST_ASSERT_EQUAL(0xa1f3u, epiIdSuffix("m0000a1f3"));
  TEST_ASSERT_EQUAL(0u, epiIdSuffix("no-hex-here-"));
  TEST_ASSERT_TRUE(epiIdSuffix("m00000010") > epiIdSuffix("m0000000f"));
}

// The shape that broke on hardware: TEN day files, and the needle buried in the
// NEWEST one below what the boot scan indexed. Anchoring the cold pass on a
// single global "index floor day" skipped that file completely - the 1.7 MB
// day-stream holding 3900 unindexed rows was never opened.
static void test_cold_scan_reaches_the_newest_files_unindexed_region() {
  FakeFs fs;
  for (uint32_t d = 0; d < 9; d++) {         // older days, a few rows each
    AppendLogEpisodicStore old(fs, "/mem/episodic", 8);
    char id[16];
    for (int i = 0; i < 3; i++) {
      snprintf(id, sizeof(id), "m%08x", (int)(d * 10 + i + 1));
      old.addMessage(msg(id, "chat1", dayTs(19000 + d, 2), "user", MsgKind::Message,
                         "older day filler"));
    }
  }
  fillOneDay(fs, 19009, 400, "the bilge pump serial is BP-4471", /*markerAt=*/2);

  AppendLogEpisodicStore st(fs, "/mem/episodic", 16);
  st.hydrate(/*maxRows=*/60, /*maxBytes=*/0);   // indexes only the newest day's tail
  TEST_ASSERT_TRUE(st.hydrateTruncated());

  MsgQuery q;
  q.textContains = "bilge pump";
  q.limit = 5;
  TEST_ASSERT_EQUAL(0, (int)st.query(q, nullptr).size());   // RED: not indexed

  q.coldScan = true;
  std::string before;
  bool found = false;
  for (int page = 0; page < 30 && !found; page++) {
    q.before = before;
    EpiQueryInfo info;
    for (const auto& m : st.query(q, &info))
      if (m.text.find("BP-4471") != std::string::npos) found = true;
    if (info.nextBefore.empty()) break;
    TEST_ASSERT_TRUE(info.nextBefore != before);   // the cursor must advance
    before = info.nextBefore;
  }
  TEST_ASSERT_TRUE(found);
}

static void test_day_number_comes_from_the_file_name() {
  // A file shorter than the old probe read decoded as day 0 and sorted itself
  // to the bottom of every comparison.
  TEST_ASSERT_EQUAL(19000u, dayNumFromName(AppendLogEpisodicStore::civilDate(19000) + ".jsonl"));
  TEST_ASSERT_EQUAL(0u, dayNumFromName("sessions.jsonl"));
  TEST_ASSERT_EQUAL(0u, dayNumFromName("short"));
}

// ⚠ REGRESSION (shipped bug, found on hardware): when the boot scan reads a day
// file's TAIL, the offsets it indexes must be FILE offsets. They were buffer
// offsets, so every row indexed from a file bigger than the scan window pointed
// at the wrong bytes - readRec decoded garbage and dropped the row, and
// episodic search silently returned nothing for the entire indexed range while
// the RAM ring quietly carried the feature. Every host test predating this used
// files smaller than the window, so nothing noticed.
static void test_hydrate_tail_read_indexes_file_offsets() {
  FakeFs fs;
  fillOneDay(fs, 19000, 600, "needle-TAILOFF", /*markerAt=*/590);   // near the END
  const long fsize = fs.size("/mem/episodic/" + AppendLogEpisodicStore::civilDate(19000) + ".jsonl");
  TEST_ASSERT_TRUE(fsize > 40000);            // comfortably over the window below

  AppendLogEpisodicStore st(fs, "/mem/episodic", /*recentCap=*/2);   // ring too small to help
  st.hydrate(/*maxRows=*/0, /*maxBytes=*/16 * 1024);                 // forces a TAIL read
  TEST_ASSERT_TRUE(st.hydrateTruncated());

  MsgQuery q;
  q.textContains = "needle-TAILOFF";
  q.limit = 5;
  std::vector<EpisodicMessage> got = st.query(q, nullptr);
  TEST_ASSERT_EQUAL(1, (int)got.size());      // the INDEX must read it back
  TEST_ASSERT_TRUE(got[0].text.find("needle-TAILOFF") != std::string::npos);

  // And a plain window read must return real rows, not silently-dropped garbage.
  MsgQuery all;
  all.limit = 20;
  TEST_ASSERT_EQUAL(20, (int)st.query(all, nullptr).size());
}

// A budget-limited cold pass must resume where it stopped. Reporting only the
// day (offset 0) reads as "this file is done", and the next call skipped it -
// so everything below the point actually reached became unreachable while the
// query still claimed to be paging.
static void test_cold_budget_cursor_resumes_mid_file() {
  FakeFs fs;
  fillOneDay(fs, 19000, 3000, "needle-DEEPEST", /*markerAt=*/1);   // ~600 KB, needle oldest
  AppendLogEpisodicStore st(fs, "/mem/episodic", 8);
  st.hydrate(/*maxRows=*/20, /*maxBytes=*/0);

  MsgQuery q;
  q.coldScan = true;
  q.textContains = "needle-DEEPEST";
  q.limit = 5;
  std::string before;
  bool found = false;
  int pages = 0;
  for (; pages < 25 && !found; pages++) {
    q.before = before;
    EpiQueryInfo info;
    for (const auto& m : st.query(q, &info))
      if (m.text.find("needle-DEEPEST") != std::string::npos) found = true;
    if (info.nextBefore.empty()) break;
    TEST_ASSERT_TRUE(info.nextBefore != before);
    before = info.nextBefore;
  }
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_TRUE(pages > 1);   // it really did take several bounded calls
}

// ⚠ REGRESSION (prism correctness HIGH): the ring-resident + truncated + byte-
// cursor path - the SHIPPED config (recentCap 512, one large active day-stream)
// that every small-ring cold test above misses. With the ring holding all the
// INDEXED rows, a cold-scan continuation carries a byte cursor ("<day>:<off>");
// the ring/index paths key off epiIdSuffix(before), which parses a byte cursor's
// trailing digits as a bogus hex id. Before the fix that re-emitted recent rows
// on every page and could strand the deepest cold history. Here the ring is big
// enough to hold the whole (truncated) index, so ringResident() is true.
static void test_cold_paging_with_resident_truncated_ring() {
  FakeFs fs;
  // ~800 rows of ~256 B => a ~200 KB day file; the boot scan reads only the
  // 128 KB tail (~500 rows), so the OLDEST rows are cold-only.
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 64);
    char id[16];
    const std::string pad(200, 'x');
    // EVERY row carries the search term "entry" AND the pad, so the ring HAS
    // matches (the condition the re-emit bug needs - a needle-only query would
    // leave queryRing empty and never re-emit). The deepest cold row also gets
    // the unique BP-9931 marker to prove the tail stays reachable.
    for (int i = 0; i < 800; i++) {
      snprintf(id, sizeof(id), "m%08x", i + 1);
      std::string text = (i == 1)
          ? ("entry bilge pump BP-9931 " + pad)
          : ("entry " + std::to_string(i) + " " + pad);
      w.addMessage(msg(id, "chat1", dayTs(19000, (uint32_t)(i % 24)), "user",
                       MsgKind::Message, text));
    }
  }
  // recentCap 600 > the ~500 rows the tail-read indexes => the ring holds the
  // WHOLE truncated index (ringResident() true), while ~300 oldest rows sit
  // below the index floor, on the card only.
  AppendLogEpisodicStore st(fs, "/mem/episodic", 600);
  const int indexed = st.hydrate();          // default budget => tail-read, truncates
  TEST_ASSERT_TRUE(st.hydrateTruncated());
  TEST_ASSERT_TRUE(indexed <= 600);          // ring can hold it => ringResident true

  MsgQuery q;
  q.coldScan = true;
  q.textContains = "entry";      // matches EVERY row => ring + cold both hit
  q.limit = 25;
  std::string before;
  bool found = false;
  std::set<std::string> seenIds;             // no row may repeat across pages
  int pages = 0;
  for (; pages < 80 && !found; pages++) {
    q.before = before;
    EpiQueryInfo info;
    for (const auto& m : st.query(q, &info)) {
      TEST_ASSERT_TRUE(seenIds.insert(m.id).second);   // <-- catches the re-emit bug
      if (m.text.find("BP-9931") != std::string::npos) found = true;
    }
    if (info.nextBefore.empty()) break;
    TEST_ASSERT_TRUE(info.nextBefore != before);
    before = info.nextBefore;
  }
  TEST_ASSERT_TRUE(found);                    // <-- catches the stranded-cold bug
}

// CUM-185: the recent-window ring + the index cache route their strings AND their
// container buffers through the PSRAM working-allocator seam (the same hook the VDB
// uses). Proven with a counting hook (host default = malloc); on-device the hook is
// heap_caps_malloc(MALLOC_CAP_SPIRAM), so routing here proves PSRAM residency there,
// lifting the measured -53 KB off the scarce internal heap at Orchestrator steady
// state. Correctness (query results) must be unchanged under the custom allocator.
static long g_epiWsBytes = 0;
static int  g_epiWsCalls = 0;
static void* epiWsCountingAlloc(std::size_t n) { g_epiWsCalls++; g_epiWsBytes += (long)n; return std::malloc(n); }
static void  epiWsCountingFree(void* p) { std::free(p); }

static void test_recent_ring_and_index_route_through_psram_seam() {
  g_epiWsBytes = 0; g_epiWsCalls = 0;
  setWorkingAllocators(epiWsCountingAlloc, epiWsCountingFree);
  {
    FakeFs fs;
    AppendLogEpisodicStore st(fs, "/mem/episodic", 100);
    // Strings must exceed the std::string SSO buffer (15 bytes) so PsString actually
    // heap-allocates - a short string would live inline and never hit the seam.
    const int N = 40;
    for (int i = 0; i < N; i++) {
      st.addMessage(msg("m" + std::to_string(1000 + i),
                        "session-longenough-" + std::to_string(i),   // > 15 chars -> heap
                        dayTs(200, i % 24), "user", MsgKind::Message,
                        "this episodic text is comfortably beyond SSO length #" + std::to_string(i)));
    }
    // The index (one IdxRec/message, PSRAM sessionId) + the recent ring (CachedMsg
    // with PSRAM id/sessionId/text/...) + both vector buffers flowed through OUR hook.
    TEST_ASSERT_TRUE(g_epiWsCalls > 0);
    TEST_ASSERT_TRUE(g_epiWsBytes >= (long)N * 40);   // at least the long strings, conservatively

    // Round-trip correctness is unaffected by the custom allocator: the ring answers
    // a text query with the right rows over the PSRAM-backed cache.
    // CUM-515 re-bless (reason): this pinned size()==1, i.e. the old ALL-words
    // rule. Every row holds "beyond SSO length", so a 4-word query now fills the
    // page (limit 5) - and ranks the one row holding all four words FIRST.
    MsgQuery q; q.limit = 5; q.textContains = "beyond SSO length #39";
    auto hits = st.query(q);
    TEST_ASSERT_EQUAL(5, (int)hits.size());
    TEST_ASSERT_EQUAL_STRING("m1039", hits[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("session-longenough-39", hits[0].sessionId.c_str());

    MsgQuery q2; q2.limit = N; q2.sessionId = "session-longenough-7";
    auto onlyOne = st.query(q2);
    TEST_ASSERT_EQUAL(1, (int)onlyOne.size());
    TEST_ASSERT_EQUAL_STRING("m1007", onlyOne[0].id.c_str());

    // Hydrate (the boot path) also rebuilds the PSRAM cache from the day-streams.
    AppendLogEpisodicStore st2(fs, "/mem/episodic", 100);
    int loaded = st2.hydrate(0, 0, nullptr);
    TEST_ASSERT_EQUAL(N, loaded);
    auto after = st2.query(q);
    TEST_ASSERT_EQUAL(5, (int)after.size());
    TEST_ASSERT_EQUAL_STRING("m1039", after[0].id.c_str());
  }  // stores destruct here (free via the hook) BEFORE we restore the default
  setWorkingAllocators(nullptr, nullptr);  // restore malloc/free for the other tests
}


// ============================================================================
// CUM-515 - the text rule is the same on EVERY backend (test the class).
// Bench B step 9: the model searched "advisor supervisor PhD"; the owner had
// only ever said "my advisor is Dr. Leila Mansour"; the ALL-words matcher
// returned "No matching episodic messages." and the model told the owner it
// did not know. Each property below runs against every place a text query is
// answered: the in-memory reference, the RAM ring, the SD index, and the cold
// pass below the boot-scan floor.
// ============================================================================
namespace cum515 {

const char* kFact     = "A few things to keep on file: my advisor is Dr. Leila Mansour.";
const char* kFull     = "PhD supervisor meeting: advisor wants chapter two by May.";
const char* kQuestion = "Who is my PhD advisor again?";

// Oldest first: the fact, 30 fillers, the all-words row, 30 fillers, the question.
std::vector<EpisodicMessage> corpus() {
  std::vector<EpisodicMessage> rows;
  char id[16];
  auto add = [&](const std::string& text) {
    snprintf(id, sizeof(id), "m%08x", (unsigned)rows.size() + 1);
    rows.push_back(msg(id, "web", dayTs(19000, (uint32_t)(rows.size() % 24)), "user",
                       MsgKind::Message, text));
  };
  add(kFact);
  for (int i = 0; i < 30; i++) add("filler chatter about the weather #" + std::to_string(i));
  add(kFull);
  for (int i = 30; i < 60; i++) add("filler chatter about the weather #" + std::to_string(i));
  add(kQuestion);
  return rows;
}

using Runner = std::function<std::vector<EpisodicMessage>(MsgQuery, EpiQueryInfo*)>;
struct Backend { const char* name; Runner run; };

// Every backend gets the same rows; each owns its store for the test's lifetime.
std::vector<Backend> backends(const std::vector<EpisodicMessage>& rows) {
  std::vector<Backend> out;
  auto mem = std::make_shared<InMemoryEpisodicStore>(2000);
  for (const auto& m : rows) mem->addMessage(m);
  out.push_back({"in-memory", [mem](MsgQuery q, EpiQueryInfo* i) { return mem->query(q, i); }});

  auto ringFs = std::make_shared<FakeFs>();
  auto ring = std::make_shared<AppendLogEpisodicStore>(*ringFs, "/mem/episodic", 1000);
  for (const auto& m : rows) ring->addMessage(m);
  out.push_back({"ring", [ringFs, ring](MsgQuery q, EpiQueryInfo* i) { return ring->query(q, i); }});

  auto idxFs = std::make_shared<FakeFs>();
  { AppendLogEpisodicStore w(*idxFs, "/mem/episodic", 4); for (const auto& m : rows) w.addMessage(m); }
  auto idx = std::make_shared<AppendLogEpisodicStore>(*idxFs, "/mem/episodic", 4);
  idx->hydrate(0, 0);   // everything indexed, ring far smaller: the SD index path
  out.push_back({"index", [idxFs, idx](MsgQuery q, EpiQueryInfo* i) { return idx->query(q, i); }});

  auto coldFs = std::make_shared<FakeFs>();
  { AppendLogEpisodicStore w(*coldFs, "/mem/episodic", 8); for (const auto& m : rows) w.addMessage(m); }
  auto cold = std::make_shared<AppendLogEpisodicStore>(*coldFs, "/mem/episodic", 16);
  cold->hydrate(/*maxRows=*/5, /*maxBytes=*/0);   // the fact + the all-words row are cold-only
  out.push_back({"cold", [coldFs, cold](MsgQuery q, EpiQueryInfo* i) {
                   q.coldScan = true;               // what memory.episodic always sets
                   return cold->query(q, i);
                 }});
  return out;
}

bool holds(const std::vector<EpisodicMessage>& rows, const char* text) {
  for (const auto& m : rows) if (m.text == text) return true;
  return false;
}

}  // namespace cum515

// 1 of 3 words is enough; case and punctuation never matter; best rows first.
static void test_cum515_partial_match_is_found_and_ranked_on_every_backend() {
  const auto rows = cum515::corpus();
  for (const auto& b : cum515::backends(rows)) {
    for (const char* text : {"advisor supervisor PhD", "ADVISOR Supervisor phd",
                             "advisor, supervisor; PhD?!", "\xE2\x80\x9C" "advisor\xE2\x80\x9D supervisor phd"}) {
      MsgQuery q; q.textContains = text; q.limit = 20;
      EpiQueryInfo info;
      const auto got = b.run(q, &info);
      std::string ctx = std::string(b.name) + " / " + text;
      TEST_ASSERT_EQUAL_INT_MESSAGE(3, (int)got.size(), ctx.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cum515::kFull, got[0].text.c_str(), ctx.c_str());     // 3 words
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cum515::kQuestion, got[1].text.c_str(), ctx.c_str()); // 2 words
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cum515::kFact, got[2].text.c_str(), ctx.c_str());     // 1 word
      TEST_ASSERT_EQUAL_INT_MESSAGE(3, info.matched, ctx.c_str());
    }
    // One real word + two that occur nowhere: the fact row is still found.
    MsgQuery one; one.textContains = "Mansour zzqxv wibblefrob"; one.limit = 20;
    const auto got = b.run(one, nullptr);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)got.size(), b.name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(cum515::kFact, got[0].text.c_str(), b.name);
  }
}

// A query made only of common words matches nothing (and reads nothing); a
// query with no words at all is no filter - both on every backend.
static void test_cum515_common_words_only_finds_nothing_on_every_backend() {
  const auto rows = cum515::corpus();
  for (const auto& b : cum515::backends(rows)) {
    MsgQuery stop; stop.textContains = "who is my... what was the"; stop.limit = 20;
    EpiQueryInfo info;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)b.run(stop, &info).size(), b.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, info.matched, b.name);
    MsgQuery none; none.textContains = " ?! "; none.limit = 5;
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)b.run(none, nullptr).size(), b.name);
  }
}

// One counted word keeps the exact pre-CUM-515 contract: newest-first, stops
// at `limit`, pages with no row lost (the paging tests above all hold).
static void test_cum515_single_word_stays_newest_first_on_every_backend() {
  const auto rows = cum515::corpus();
  for (const auto& b : cum515::backends(rows)) {
    MsgQuery q; q.textContains = "the advisor"; q.limit = 20;   // "the" is a common word
    const auto got = b.run(q, nullptr);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, (int)got.size(), b.name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(cum515::kQuestion, got[0].text.c_str(), b.name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(cum515::kFull, got[1].text.c_str(), b.name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(cum515::kFact, got[2].text.c_str(), b.name);
  }
}

// The ranked page is BOUNDED (kEpiRankedMax) even when the caller asked for no
// limit, and it says how many rows it left out.
static void test_cum515_ranked_page_is_bounded() {
  InMemoryEpisodicStore mem(2000);
  FakeFs fs;
  AppendLogEpisodicStore ring(fs, "/mem/episodic", 1000);
  char id[16];
  for (int i = 0; i < 600; i++) {
    snprintf(id, sizeof(id), "m%08x", i + 1);
    auto m = msg(id, "web", dayTs(19000, 1), "user", MsgKind::Message,
                 i == 3 ? "bilge pump serial BP-4471" : "pump log entry " + std::to_string(i));
    mem.addMessage(m);
    ring.addMessage(m);
  }
  for (EpisodicStore* st : {(EpisodicStore*)&mem, (EpisodicStore*)&ring}) {
    MsgQuery q; q.textContains = "bilge pump"; q.limit = 0;   // "no limit"
    EpiQueryInfo info;
    const auto got = st->query(q, &info);
    TEST_ASSERT_EQUAL_INT(kEpiRankedMax, (int)got.size());
    TEST_ASSERT_EQUAL_STRING("bilge pump serial BP-4471", got[0].text.c_str());   // oldest, but best
    TEST_ASSERT_EQUAL_INT(600, info.matched);
  }
}

// Ranked paging across the ring and the cold pass: the cursor advances, no row
// repeats, and a strong match deep in cold-only history is reached. Same shape
// as test_cold_paging_with_resident_truncated_ring (the shipped config), with a
// TWO-word query so every page is ranked.
static void test_cum515_ranked_paging_reaches_deep_history_without_repeats() {
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 64);
    char id[16];
    const std::string pad(200, 'x');
    for (int i = 0; i < 800; i++) {
      snprintf(id, sizeof(id), "m%08x", i + 1);
      std::string text = (i == 1) ? ("entry bilge pump BP-9931 " + pad)
                                  : ("entry " + std::to_string(i) + " " + pad);
      w.addMessage(msg(id, "chat1", dayTs(19000, (uint32_t)(i % 24)), "user",
                       MsgKind::Message, text));
    }
  }
  AppendLogEpisodicStore st(fs, "/mem/episodic", 600);
  st.hydrate();
  TEST_ASSERT_TRUE(st.hydrateTruncated());

  MsgQuery q;
  q.coldScan = true;
  q.textContains = "entry bilge";
  q.limit = 25;
  std::string before;
  bool found = false;
  std::set<std::string> seen;
  int pages = 0;
  for (; pages < 40 && !found; pages++) {
    q.before = before;
    EpiQueryInfo info;
    const auto got = st.query(q, &info);
    if (pages == 0) TEST_ASSERT_TRUE(info.matched > (int)got.size());   // honest: more matched
    for (const auto& m : got) {
      TEST_ASSERT_TRUE(seen.insert(m.id).second);
      if (m.text.find("BP-9931") != std::string::npos) {
        found = true;
        TEST_ASSERT_EQUAL_STRING(got[0].id.c_str(), m.id.c_str());   // best on its page
      }
    }
    if (info.nextBefore.empty()) break;
    TEST_ASSERT_TRUE(info.nextBefore != before);
    before = info.nextBefore;
  }
  TEST_ASSERT_TRUE(found);
}

// The exact L4 query, against bench B's recorded rows, through the backends
// the device actually uses (the RAM ring, and a cold scan below a truncated
// boot index - memory.episodic always sets coldScan).
static void test_cum515_l4_advisor_query_finds_the_earlier_conversation() {
  const auto rows = loadEpisodicFixture(kL4EpisodicFixture);
  TEST_ASSERT_EQUAL_INT(51, (int)rows.size());
  TEST_ASSERT_EQUAL_STRING("Who is my PhD advisor again?", rows.back().text.c_str());
  for (const auto& b : cum515::backends(rows)) {
    MsgQuery q;
    q.haveKind = true; q.kind = MsgKind::Message;              // {"kind":"message",
    q.textContains = "advisor supervisor PhD"; q.limit = 20;   //  "text":..., "limit":20}
    const auto got = b.run(q, nullptr);
    bool fact = false, reply = false;
    for (const auto& m : got) {
      if (m.role == "user" && m.text.find("my advisor is Dr. Leila Mansour") != std::string::npos) fact = true;
      if (m.role == "assistant" && m.text.find("Your advisor is **Dr. Leila Mansour**") != std::string::npos) reply = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(fact, b.name);
    TEST_ASSERT_TRUE_MESSAGE(reply, b.name);
    // The other step-9 searches that came back empty on the bench.
    for (const auto& c : kL4Recalls) {
      MsgQuery m2 = q;
      m2.textContains = c.text;
      bool ok = false;
      for (const auto& m : b.run(m2, nullptr))
        if (m.text.find(c.want) != std::string::npos) ok = true;
      std::string ctx = std::string(b.name) + " / " + c.text;
      TEST_ASSERT_TRUE_MESSAGE(ok, ctx.c_str());
    }
  }
}


// Every row holding EVERY word stays reachable by paging, on the index path
// and on the shipped cold-scan config, even with partial matches interleaved:
// a ranked page stops as soon as it is full of full matches, so its cursor
// never skips one (prism logic lens: the first cut dropped them below the cut).
static void test_cum515_ranked_paging_never_skips_a_full_match() {
  auto build = [](FakeFs& fs, int rows) {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 64);
    char id[16];
    const std::string pad(60, 'x');
    for (int i = 0; i < rows; i++) {
      snprintf(id, sizeof(id), "m%08x", i + 1);
      const bool full = (i % 3) != 0;
      w.addMessage(msg(id, "chat1", dayTs(19000, (uint32_t)(i % 24)), "user", MsgKind::Message,
                       (full ? "bilge pump row " : "pump only row ") + std::to_string(i) + " " + pad));
    }
  };
  struct Case { const char* name; int rows; int recentCap; bool truncate; };
  for (const Case& c : {Case{"index", 900, 16, false}, Case{"cold", 2400, 600, true}}) {
    FakeFs fs;
    build(fs, c.rows);
    AppendLogEpisodicStore st(fs, "/mem/episodic", c.recentCap);
    if (c.truncate) {
      st.hydrate();                              // default budget: tail-read, truncates
      TEST_ASSERT_TRUE_MESSAGE(st.hydrateTruncated(), c.name);
    } else {
      st.hydrate(0, 0);                          // everything indexed, tiny ring
    }
    MsgQuery q;
    q.coldScan = true;
    q.textContains = "bilge pump";
    q.limit = 20;
    std::set<std::string> fullSeen;
    std::string before;
    int pages = 0;
    for (; pages < 400; pages++) {
      q.before = before;
      EpiQueryInfo info;
      for (const auto& m : st.query(q, &info)) {
        if (m.text.find("bilge") == std::string::npos) continue;
        TEST_ASSERT_TRUE_MESSAGE(fullSeen.insert(m.id).second, c.name);   // never repeated
      }
      if (info.nextBefore.empty()) break;
      TEST_ASSERT_TRUE_MESSAGE(info.nextBefore != before, c.name);       // always advances
      before = info.nextBefore;
    }
    const int fullRows = c.rows - (c.rows + 2) / 3;
    TEST_ASSERT_EQUAL_INT_MESSAGE(fullRows, (int)fullSeen.size(), c.name);
  }
}

// The read boundary holds for a ranked page: better matches in a session the
// caller cannot read never displace, count toward, or become the cursor of
// its page (prism authz lens: the boundary test only covered unranked).
static void test_cum515_ranked_page_stays_inside_the_read_boundary() {
  std::vector<EpisodicMessage> rows;
  char id[16];
  for (int i = 0; i < 40; i++) {
    snprintf(id, sizeof(id), "m%08x", i + 1);
    const bool mine = (i % 2) == 0;
    rows.push_back(msg(id, mine ? "chat1" : "chat2", dayTs(19000, (uint32_t)(i % 24)), "user",
                       MsgKind::Message,
                       mine ? "pump note " + std::to_string(i)
                            : "bilge pump secret " + std::to_string(i)));
  }
  for (const auto& b : cum515::backends(rows)) {
    MsgQuery q;
    q.textContains = "bilge pump";
    q.sessionAllow = {"chat1"};
    // A page with room for everything searches every tier: exactly chat1's 20.
    q.limit = 50;
    EpiQueryInfo all;
    const auto every = b.run(q, &all);
    TEST_ASSERT_EQUAL_INT_MESSAGE(20, (int)every.size(), b.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(20, all.matched, b.name);      // chat2 never counted
    for (const auto& m : every) TEST_ASSERT_EQUAL_STRING_MESSAGE("chat1", m.sessionId.c_str(), b.name);
    // A small page: still only chat1 rows, and an id cursor names a chat1 row.
    q.limit = 5;
    EpiQueryInfo info;
    const auto got = b.run(q, &info);
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)got.size(), b.name);
    for (const auto& m : got) TEST_ASSERT_EQUAL_STRING_MESSAGE("chat1", m.sessionId.c_str(), b.name);
    TEST_ASSERT_TRUE_MESSAGE(info.matched >= 5 && info.matched <= 20, b.name);
    if (!info.nextBefore.empty() && info.nextBefore.find(':') == std::string::npos)
      TEST_ASSERT_TRUE_MESSAGE(epiIdSuffix(info.nextBefore) % 2 == 1, b.name);   // chat1 ids are odd
  }
}


// REGRESSION (pre-existing, found by the CUM-515 boundary test): a boot scan cut
// short by maxRows below the ring size left un-indexed rows in the RAM ring;
// they also sat below the cold pass's per-file floor, so a deep query returned
// them twice. The ring must be a subset of the index.
static void test_hydrate_ring_never_outgrows_the_index() {
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 8);
    char id[16];
    for (int i = 0; i < 40; i++) {
      snprintf(id, sizeof(id), "m%08x", i + 1);
      w.addMessage(msg(id, "chat1", dayTs(19000, (uint32_t)(i % 24)), "user", MsgKind::Message,
                       "pump note " + std::to_string(i)));
    }
  }
  AppendLogEpisodicStore st(fs, "/mem/episodic", /*recentCap=*/16);
  TEST_ASSERT_EQUAL_INT(5, st.hydrate(/*maxRows=*/5, /*maxBytes=*/0));
  for (const char* text : {"", "pump"}) {
    MsgQuery q;
    q.coldScan = true;
    q.textContains = text;
    q.limit = 100;
    std::set<std::string> ids;
    for (const auto& m : st.query(q, nullptr)) TEST_ASSERT_TRUE_MESSAGE(ids.insert(m.id).second, text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(40, (int)ids.size(), text);
  }
}

// ============================================================================
// v4.5.14: the retention guard and the index backfill. The class rule these
// encode: after ANY restart the episodic index converges to exactly the rows on
// the card (within the index cap), and nothing that consumes the index
// (retention, counts, floors) acts on a partial index as if it were complete.
// On v4.5.13 a device with 1,346 rows on its card indexed 399 after a restart,
// and the next retention pass deleted the blob sidecars of the other 947.
// ============================================================================
namespace lu {

const char* kDir = "/mem/episodic";
const char* kBlobs = "/mem/blobs";

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed * 2654435761u + 0x9E3779B9u) {}
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

std::string idOf(uint32_t n) {
  char id[16];
  snprintf(id, sizeof(id), "m%08x", n);
  return id;
}
std::string blobFor(uint32_t n, const char* ext) {
  char b[64];
  snprintf(b, sizeof(b), "/mem/blobs/%08x%08x.%s", n * 2654435761u, n, ext);
  return b;
}

// A row whose ENCODED line is exactly `lineBytes` long (the newline excluded).
EpisodicMessage sizedRow(uint32_t idNum, const std::string& sess, uint32_t ts, MsgKind kind,
                         const std::string& blob, int lineBytes) {
  EpisodicMessage m = msg(idOf(idNum), sess, ts, kind == MsgKind::Message ? "user" : "assistant",
                          kind, "");
  m.blobPath = blob;
  const int overhead = (int)encodeEpisodicLine(m).size();
  int pad = lineBytes - overhead;
  if (pad < 8) pad = 8;
  std::string text = "row " + m.id + " ";
  while ((int)text.size() < pad) text += "lorem ipsum dolor sit amet ";
  text.resize((size_t)pad);
  m.text = text;
  return m;
}

// Lumi's kind mix, cycled: tool output and images carry a blob sidecar.
struct KindPick { MsgKind kind; bool blob; const char* ext; };
KindPick kindAt(uint32_t i) {
  switch (i % 10) {
    case 0: case 3: case 6: return {MsgKind::ToolOutput, true, "txt"};
    case 1: case 4: case 7: return {MsgKind::Message, false, ""};
    case 8:                 return {MsgKind::Log, false, ""};
    case 9:                 return {i % 20 == 9 ? MsgKind::Image : MsgKind::LlmResponse,
                                    i % 20 == 9, "jpg"};
    default:                return {MsgKind::ToolOutput, false, ""};
  }
}
std::string sessAt(uint32_t i, MsgKind k) {
  if (k == MsgKind::Log) return "system";
  return (i % 7 == 0) ? "web" : "123456789";
}

using Snap = std::vector<std::array<uint32_t, 4>>;   // (day, offset, len, id suffix), oldest first
Snap snap(const AppendLogEpisodicStore& st) {
  Snap v;
  st.forEachIndexed([&](uint32_t d, uint32_t o, uint32_t l, uint32_t s) { v.push_back({d, o, l, s}); });
  return v;
}

int runBackfill(AppendLogEpisodicStore& st, size_t sliceBytes = kBackfillSliceBytes,
                int sliceRows = kBackfillSliceRows) {
  int steps = 0;
  while (st.backfillStep(sliceBytes, sliceRows) && steps < 200000) steps++;
  return steps;
}

// Every blob hash referenced by a decodable row in any day-stream on the card:
// the ground truth for "is this sidecar still needed".
std::set<std::string> blobsReferencedOnCard(const FakeFs& fs) {
  std::set<std::string> refs;
  const std::string prefix = std::string(kDir) + "/";
  for (const auto& kv : fs.files) {
    if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
    uint32_t day = 0;
    if (!parseDayStreamName(kv.first.c_str() + prefix.size(), day)) continue;
    std::istringstream in(kv.second);
    std::string line;
    while (std::getline(in, line)) {
      EpisodicMessage m;
      if (decodeEpisodicLine(line, m) && !m.blobPath.empty()) refs.insert(blobHashOf(m.blobPath));
    }
  }
  return refs;
}
std::set<std::string> blobsPresent(const FakeFs& fs) {
  std::set<std::string> out;
  const std::string prefix = std::string(kBlobs) + "/";
  for (const auto& kv : fs.files)
    if (kv.first.compare(0, prefix.size(), prefix) == 0) out.insert(blobHashOf(kv.first));
  return out;
}

// ---- generated histories: 1-40 day files, 0-3 MB, the Lumi kind mix ---------
// Row sizes follow the field averages (tool output ~890 B capped near 1 KB,
// message ~670 B, log ~260 B, image ~550 B). Written through a live store, so
// the writer's own index is the reference the restarted store must reach.
struct Gen {
  uint32_t nextId = 1;
  uint32_t lastDay = 20700;   // late September 2026
};
int sizeFor(Rng& r, MsgKind k) {
  int avg = 670;
  if (k == MsgKind::ToolOutput) avg = 890;
  else if (k == MsgKind::Log) avg = 260;
  else if (k == MsgKind::Image) avg = 550;
  else if (k == MsgKind::LlmResponse) avg = 920;
  int sz = avg / 2 + (int)r.below((uint32_t)avg);
  if (k == MsgKind::ToolOutput && sz > 1024) sz = 1024;
  return sz;
}
void addGenRow(FakeFs& fs, AppendLogEpisodicStore& w, Rng& r, Gen& g, uint32_t day, int hour) {
  const uint32_t i = g.nextId++;
  const KindPick kp = kindAt(r.next());
  const bool blob = kp.blob && r.below(4) != 0;
  const std::string bp = blob ? blobFor(i, kp.ext) : std::string();
  if (blob) fs.files[bp] = "x";
  w.addMessage(sizedRow(i, sessAt(i, kp.kind), day * 24 + (uint32_t)hour, kp.kind, bp,
                        sizeFor(r, kp.kind)));
}
void writeHistory(FakeFs& fs, AppendLogEpisodicStore& w, uint32_t seed, Gen& g) {
  Rng r(seed);
  const int days = 1 + (int)r.below(40);
  const size_t target = (seed % 7 == 0) ? 0 : (size_t)r.below(3u * 1024 * 1024);
  const size_t perDay = target / (size_t)days;
  for (int d = 0; d < days; d++) {
    g.lastDay += 1 + r.below(3);
    const size_t budget = perDay / 4 + (size_t)r.below((uint32_t)(perDay * 3 / 2 + 1));
    size_t used = 0;
    int i = 0;
    while (used < budget) {
      const long before = fs.size(w.dayFile(g.lastDay));
      addGenRow(fs, w, r, g, g.lastDay, std::min(23, i++ / 8));
      used += (size_t)(fs.size(w.dayFile(g.lastDay)) - before);
    }
  }
}

// ---- the Lumi-shaped fixture -------------------------------------------------
// Sixteen day-streams with the per-day byte totals and row counts of the field
// card (synthetic text): 1,346 rows, 805,548 B. The field study lists 293 rows
// for the eleven oldest files but 1,346 in all; the total is what was measured,
// so the oldest files carry 286. Two shapes matter for the v4.5.13 walk and are
// kept: the 15 Oct file's newest 129 rows fill its tail window (the row before
// them straddles the window edge), and the 14 Oct file's last row is small
// enough to fit the few hundred bytes the walk had left, so v4.5.13 indexed it
// as an isolated row below a hole.
struct DaySpec {
  const char* date;
  int rows;
  long bytes;
  int tailRows;      // newest rows sized separately ...
  long tailBytes;    // ... to this total
  int lastBodyRow;   // size of the row just before the tail (0 = no override)
};
const DaySpec kLumiDays[] = {
    {"2026-09-14", 26, 11941, 0, 0, 0}, {"2026-09-17", 26, 11941, 0, 0, 0},
    {"2026-09-20", 26, 11941, 0, 0, 0}, {"2026-09-23", 26, 11941, 0, 0, 0},
    {"2026-09-26", 26, 11941, 0, 0, 0}, {"2026-09-29", 26, 11941, 0, 0, 0},
    {"2026-10-02", 26, 11941, 0, 0, 0}, {"2026-10-05", 26, 11941, 0, 0, 0},
    {"2026-10-08", 26, 11941, 0, 0, 0}, {"2026-10-11", 26, 11941, 0, 0, 0},
    {"2026-10-13", 26, 11944, 0, 0, 0},
    {"2026-10-14", 494, 308748, 1, 400, 0},
    {"2026-10-15", 297, 175580, 129, 71700, 900},
    {"2026-10-16", 106, 67375, 0, 0, 0},
    {"2026-10-17", 105, 75728, 0, 0, 0},
    {"2026-10-18", 58, 46763, 0, 0, 0},
};
const int kLumiRows = 1346;
const long kLumiBytes = 805548;

// `n` row sizes (newline included) summing to exactly `bytes`, varying +-25%
// in symmetric pairs so the total is exact.
std::vector<int> sizesFor(int n, long bytes) {
  std::vector<int> out;
  if (n <= 0) return out;
  const int avg = (int)(bytes / n);
  const int rem = (int)(bytes - (long)avg * n);
  for (int i = 0; i < n; i++) {
    const int d = (int)((i / 2 * 37) % (avg / 4 + 1));
    out.push_back(avg + ((i % 2 == 0) ? d : -d));
  }
  if (n % 2 == 1) out[n - 1] = avg;   // the unpaired row carries no swing
  out[n - 1] += rem;
  return out;
}

struct LumiFixture {
  std::vector<uint32_t> days;            // oldest first
  std::map<std::string, std::string> blobOf;   // row id -> blob path
  int orphanBlobs = 0;
};

LumiFixture writeLumi(FakeFs& fs) {
  LumiFixture fx;
  AppendLogEpisodicStore w(fs, kDir, 512);
  uint32_t id = 1;
  for (const DaySpec& ds : kLumiDays) {
    const uint32_t day = dayNumFromName(std::string(ds.date) + ".jsonl");
    fx.days.push_back(day);
    const int bodyRows = ds.rows - ds.tailRows - (ds.lastBodyRow ? 1 : 0);
    std::vector<int> sizes = sizesFor(bodyRows, ds.bytes - ds.tailBytes - ds.lastBodyRow);
    if (ds.lastBodyRow) sizes.push_back(ds.lastBodyRow);
    const std::vector<int> tail = sizesFor(ds.tailRows, ds.tailBytes);
    sizes.insert(sizes.end(), tail.begin(), tail.end());
    for (int i = 0; i < ds.rows; i++, id++) {
      const KindPick kp = kindAt(id);
      const std::string bp = kp.blob ? blobFor(id, kp.ext) : std::string();
      if (kp.blob) { fs.files[bp] = "x"; fx.blobOf[idOf(id)] = bp; }
      EpisodicMessage m = sizedRow(id, sessAt(id, kp.kind), day * 24 + (uint32_t)(i * 24 / ds.rows),
                                   kp.kind, bp, sizes[(size_t)i] - 1);
      if (id % 97 == 0) {   // a few searchable needles; same length, so the sizes hold
        const std::string lead = "needle" + std::to_string(id % 7) + " ";
        m.text.replace(0, lead.size(), lead);
      }
      w.addMessage(m);
    }
  }
  for (int i = 0; i < 5; i++) {   // sidecars no row references
    fs.files[std::string(kBlobs) + "/0rphan0" + std::to_string(i) + "aaaaaaaa.txt"] = "o";
    fx.orphanBlobs++;
  }
  return fx;
}

// The v4.5.13 boot walk, modelled line for line (orch_episodic_log.cpp:564-633
// at c1ab939): newest file first, a 128 KB per-file tail window, and a stop
// test on bytes KEPT, which a nearly spent budget never trips. Kept here so the
// fixture is proven field-shaped: it must give the field's 399.
struct LegacyScan { int rows = 0; int filesOpened = 0; std::vector<uint32_t> sfx; };
LegacyScan legacyBootScan(const FakeFs& fs, const std::vector<uint32_t>& daysOldestFirst) {
  LegacyScan out;
  size_t bytesRead = 0;
  for (auto it = daysOldestFirst.rbegin(); it != daysOldestFirst.rend(); ++it) {
    if (out.rows >= kHydrateMaxRows || bytesRead >= kHydrateMaxBytes) break;
    const std::string& f = fs.files.at(std::string(kDir) + "/" +
                                       AppendLogEpisodicStore::civilDate(*it) + ".jsonl");
    const size_t room = kHydrateMaxBytes > bytesRead ? kHydrateMaxBytes - bytesRead
                                                     : kHydrateFileWindow;
    const size_t window = std::min(room, kHydrateFileWindow);
    out.filesOpened++;
    std::string buf = f;
    if (f.size() > window) {
      buf = f.substr(f.size() - window);
      const size_t nl = buf.find('\n');
      buf = (nl == std::string::npos) ? std::string() : buf.substr(nl + 1);
    }
    bytesRead += buf.size();
    std::vector<uint32_t> day;
    std::istringstream in(buf);
    std::string line;
    while (std::getline(in, line)) {
      EpisodicMessage m;
      if (decodeEpisodicLine(line, m)) day.push_back(epiIdSuffix(m.id));
    }
    for (auto d = day.rbegin(); d != day.rend() && out.rows < kHydrateMaxRows; ++d) {
      out.sfx.push_back(*d);
      out.rows++;
    }
  }
  return out;
}

bool isSuffixOf(const Snap& part, const Snap& whole) {
  if (part.size() > whole.size()) return false;
  return std::equal(part.begin(), part.end(), whole.end() - (long)part.size());
}

}  // namespace lu

// A cold-query cursor is caller input (the model echoes it back, and can echo a
// stale or invented one). Whatever it says, one call stays inside its read
// budget: an offset past the end of the file is clamped to the file, never
// walked down one empty read at a time.
static void test_cold_cursor_out_of_range_is_bounded() {
  FakeFs fs;
  fillOneDay(fs, 19000, 400, "needle-CLAMP", /*markerAt=*/1);
  AppendLogEpisodicStore st(fs, "/mem/episodic", 16);
  st.hydrate(/*maxRows=*/20, /*maxBytes=*/0);
  const long fsize = fs.size(st.dayFile(19000));
  const std::string day = "19000:";
  for (const std::string& cur :
       {day + "2147483647", day + "99999999999999999999", day + std::to_string(fsize + 1),
        day + std::to_string(fsize), std::string("25000:2147483647"), std::string("abc:def")}) {
    MsgQuery q;
    q.coldScan = true;
    q.limit = 5;
    q.before = cur;
    fs.reset();
    EpiQueryInfo info;
    const auto rows = st.query(q, &info);
    TEST_ASSERT_TRUE_MESSAGE(fs.readRanges <= 16, cur.c_str());
    TEST_ASSERT_TRUE_MESSAGE(info.coldBytes <= kColdMaxBytes + kColdWindowMax, cur.c_str());
    // An in-file cursor past the end reads from the end of the file.
    if (cur.compare(0, day.size(), day) == 0) TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)rows.size(), cur.c_str());
  }
}

// T1 - convergence property over generated histories: hydrate under the device
// budgets, then backfill to the end, must reproduce the writer's live index
// exactly (rows, days, offsets, lengths, order), its count and its id hint.
// On v4.5.13 this failed at about 400 rows.
static void test_reboot_converges_to_pre_reboot_index_property() {
  int truncatedCases = 0;
  for (uint32_t seed = 1; seed <= 24; seed++) {
    FakeFs fs;
    AppendLogEpisodicStore w(fs, lu::kDir, 512);
    lu::Gen g;
    lu::writeHistory(fs, w, seed, g);
    const lu::Snap ref = lu::snap(w);

    AppendLogEpisodicStore b(fs, lu::kDir, 512);
    b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
    if (b.hydrateTruncated()) truncatedCases++;
    lu::runBackfill(b);
    const std::string ctx = "seed " + std::to_string(seed) + " rows " + std::to_string(ref.size());
    TEST_ASSERT_TRUE_MESSAGE(lu::snap(b) == ref, ctx.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.messageCount(), b.messageCount(), ctx.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(w.nextIdHint(), b.nextIdHint(), ctx.c_str());
    TEST_ASSERT_FALSE_MESSAGE(b.hydrateTruncated(), ctx.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.indexComplete() && b.backfillStats().complete, ctx.c_str());
  }
  // The generator must actually exercise the truncated path, or this proves nothing.
  TEST_ASSERT_TRUE(truncatedCases >= 12);
}

// T2 - right after the boot scan the index is a CONTIGUOUS suffix of history
// (no isolated older rows), the scan opened at most one file beyond the ones it
// indexed, and it read no more than its byte budget.
static void test_boot_index_is_a_contiguous_suffix_and_bounded() {
  for (uint32_t seed = 1; seed <= 16; seed++) {
    FakeFs fs;
    AppendLogEpisodicStore w(fs, lu::kDir, 512);
    lu::Gen g;
    lu::writeHistory(fs, w, seed, g);
    const lu::Snap ref = lu::snap(w);
    fs.reset();
    AppendLogEpisodicStore b(fs, lu::kDir, 512);
    b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
    const lu::Snap boot = lu::snap(b);
    std::set<uint32_t> daysIndexed;
    for (const auto& r : boot) daysIndexed.insert(r[0]);
    const std::string ctx = "seed " + std::to_string(seed);
    TEST_ASSERT_TRUE_MESSAGE(lu::isSuffixOf(boot, ref), ctx.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.readAlls + fs.readRanges <= (int)daysIndexed.size() + 1, ctx.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.sizes <= (int)daysIndexed.size() + 1, ctx.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.bytesRead <= (long)kHydrateMaxBytes, ctx.c_str());
  }
}

// T3 - the field card's shape: the v4.5.13 walk gives the field's 399 rows from
// 15 opened files with isolated older rows; the new walk gives a contiguous
// ~400 from 4 files, and the backfill brings the index to all 1,346 rows.
static void test_lumi_shaped_fixture_reproduces_and_fixes_399() {
  FakeFs fs;
  const lu::LumiFixture fx = lu::writeLumi(fs);
  long total = 0;
  for (uint32_t d : fx.days)
    total += fs.size(std::string(lu::kDir) + "/" + AppendLogEpisodicStore::civilDate(d) + ".jsonl");
  TEST_ASSERT_EQUAL_INT32(lu::kLumiBytes, (int32_t)total);

  AppendLogEpisodicStore full(fs, lu::kDir, 512);
  TEST_ASSERT_EQUAL_INT(lu::kLumiRows, full.hydrate(0, 0));
  const lu::Snap ref = lu::snap(full);

  // The field defect, reproduced: 399 rows, every file opened, not a suffix.
  const lu::LegacyScan legacy = lu::legacyBootScan(fs, fx.days);
  TEST_ASSERT_EQUAL_INT(399, legacy.rows);
  TEST_ASSERT_EQUAL_INT((int)fx.days.size(), legacy.filesOpened);
  std::vector<uint32_t> refTail;
  for (auto it = ref.rbegin(); it != ref.rend() && refTail.size() < legacy.sfx.size(); ++it)
    refTail.push_back((*it)[3]);
  TEST_ASSERT_FALSE(refTail == legacy.sfx);   // the v4.5.13 index had a hole in it

  // The fix: a contiguous window from the newest four files...
  fs.reset();
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  const int boot = b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
  TEST_ASSERT_TRUE(boot >= 370 && boot <= 440);
  TEST_ASSERT_TRUE(b.hydrateTruncated());
  TEST_ASSERT_TRUE(lu::isSuffixOf(lu::snap(b), ref));
  TEST_ASSERT_TRUE(fs.readAlls + fs.readRanges <= 4);
  // ...and the backfill converges on the whole card.
  const int steps = lu::runBackfill(b);
  TEST_ASSERT_TRUE(steps > 0);
  TEST_ASSERT_EQUAL_INT(lu::kLumiRows, b.messageCount());
  TEST_ASSERT_TRUE(lu::snap(b) == ref);
  TEST_ASSERT_TRUE(b.indexComplete());
  TEST_ASSERT_EQUAL_INT(lu::kLumiRows - boot, b.backfillStats().rows);
}

// B2's host twin and T5's partner: on the truncated boot index of the field
// card, the v4.5.13 sweep would have deleted every sidecar referenced only by
// an unindexed row; the guarded prune deletes none and says so. Once the index
// is complete the sweep runs and takes exactly the orphans.
static void test_lumi_prune_defers_then_collects_only_orphans() {
  FakeFs fs;
  const lu::LumiFixture fx = lu::writeLumi(fs);
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
  const std::set<std::string> before = lu::blobsPresent(fs);

  // What v4.5.13 would have swept: present minus what the INDEX references.
  std::set<std::string> indexed;
  for (const auto& r : lu::snap(b)) {
    auto it = fx.blobOf.find(lu::idOf(r[3]));
    if (it != fx.blobOf.end()) indexed.insert(blobHashOf(it->second));
  }
  std::vector<std::string> present(before.begin(), before.end());
  const size_t wouldDelete = unreferencedBlobs(present, indexed).size();
  TEST_ASSERT_TRUE(wouldDelete > 300);   // the field lost about 296

  EpiPruneReport rep = b.prune(/*cutoffDayNum=*/fx.days.front(), lu::kBlobs);
  TEST_ASSERT_TRUE(rep.blobGcSkipped);
  TEST_ASSERT_EQUAL_INT(0, (int)rep.removedBlobs.size());
  TEST_ASSERT_TRUE(lu::blobsPresent(fs) == before);

  lu::runBackfill(b);
  TEST_ASSERT_TRUE(b.indexComplete());
  EpiPruneReport rep2 = b.prune(fx.days.front(), lu::kBlobs);
  TEST_ASSERT_FALSE(rep2.blobGcSkipped);
  TEST_ASSERT_EQUAL_INT(fx.orphanBlobs, (int)rep2.removedBlobs.size());
  TEST_ASSERT_EQUAL_INT((int)before.size(), rep2.blobsPresent);
  const std::set<std::string> after = lu::blobsPresent(fs);
  for (const auto& h : lu::blobsReferencedOnCard(fs))
    TEST_ASSERT_TRUE_MESSAGE(after.count(h) == 1, h.c_str());
}

// T4 - property: whatever state the index is in (truncated, partly backfilled,
// complete), prune never deletes a blob that any row on the card references,
// and it removes EVERY day-stream older than the cutoff, indexed or not.
static void test_prune_never_deletes_a_blob_referenced_on_card() {
  int deferred = 0, swept = 0;
  for (uint32_t seed = 1; seed <= 16; seed++) {
    FakeFs fs;
    AppendLogEpisodicStore w(fs, lu::kDir, 512);
    lu::Gen g;
    lu::writeHistory(fs, w, seed, g);
    for (int i = 0; i < 3; i++) fs.files[std::string(lu::kBlobs) + "/orphan" + std::to_string(i) + ".txt"] = "o";
    lu::Rng r(seed + 1000);
    AppendLogEpisodicStore b(fs, lu::kDir, 512);
    b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
    const int steps = (int)r.below(60);
    for (int i = 0; i < steps && b.backfillStep(); i++) {}
    if (r.below(3) == 0) lu::runBackfill(b);
    const bool complete = b.indexComplete();
    const uint32_t cutoff = g.lastDay - r.below(45);
    const std::set<std::string> blobsBefore = lu::blobsPresent(fs);
    EpiPruneReport rep = b.prune(cutoff, lu::kBlobs);
    const std::string ctx = "seed " + std::to_string(seed);
    const std::set<std::string> after = lu::blobsPresent(fs);
    for (const auto& h : lu::blobsReferencedOnCard(fs))
      TEST_ASSERT_TRUE_MESSAGE(after.count(h) == 1, ctx.c_str());
    for (const auto& kv : fs.files) {
      uint32_t day = 0;
      const std::string prefix = std::string(lu::kDir) + "/";
      if (kv.first.compare(0, prefix.size(), prefix) == 0 &&
          parseDayStreamName(kv.first.c_str() + prefix.size(), day))
        TEST_ASSERT_TRUE_MESSAGE(day >= cutoff, ctx.c_str());
    }
    if (complete) {
      swept++;
      TEST_ASSERT_FALSE_MESSAGE(rep.blobGcSkipped, ctx.c_str());
    } else {
      deferred++;
      TEST_ASSERT_TRUE_MESSAGE(rep.blobGcSkipped, ctx.c_str());
      TEST_ASSERT_TRUE_MESSAGE(after == blobsBefore, ctx.c_str());
    }
  }
  TEST_ASSERT_TRUE(deferred > 0 && swept > 0);   // both branches exercised
}

// T5 - counter-test for the loosened sweep: with a complete index (a boot scan
// that read everything, or a finished backfill) unreferenced blobs ARE still
// deleted, so the guard cannot quietly switch the sweep off for good.
static void test_prune_gc_still_collects_when_index_complete() {
  for (int path = 0; path < 2; path++) {
    FakeFs fs;
    lu::writeLumi(fs);
    AppendLogEpisodicStore b(fs, lu::kDir, 512);
    if (path == 0) {
      b.hydrate(0, 0);
    } else {
      b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
      lu::runBackfill(b);
    }
    TEST_ASSERT_TRUE(b.indexComplete());
    fs.files[std::string(lu::kBlobs) + "/feedfacefeedface.ogg"] = "late orphan";
    EpiPruneReport rep = b.prune(1, lu::kBlobs);
    TEST_ASSERT_FALSE(rep.blobGcSkipped);
    TEST_ASSERT_EQUAL_INT(6, (int)rep.removedBlobs.size());   // 5 orphans + the late one
    TEST_ASSERT_TRUE(fs.files.count(std::string(lu::kBlobs) + "/feedfacefeedface.ogg") == 0);
  }
}

// T6 - random interleavings of live appends, backfill slices and prunes end in
// the index a fresh unbudgeted boot scan of the final card builds, with no
// duplicate row across index and cold paging.
static void test_backfill_interleaves_with_appends_and_prune() {
  for (uint32_t seed = 1; seed <= 12; seed++) {
    FakeFs fs;
    lu::Gen g;
    {
      AppendLogEpisodicStore w(fs, lu::kDir, 512);
      lu::writeHistory(fs, w, seed + 100, g);
    }
    AppendLogEpisodicStore b(fs, lu::kDir, 512);
    b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
    lu::Rng r(seed);
    uint32_t cutoff = 20700;
    for (int op = 0; op < 400; op++) {
      const uint32_t k = r.below(10);
      if (k < 6) {
        b.backfillStep();
      } else if (k < 9) {
        if (r.below(8) == 0) g.lastDay++;   // a new day starts mid-backfill
        lu::addGenRow(fs, b, r, g, g.lastDay, 23);
      } else {
        cutoff += r.below(6);
        b.prune(cutoff, lu::kBlobs);
      }
    }
    lu::runBackfill(b);
    AppendLogEpisodicStore fresh(fs, lu::kDir, 512);
    fresh.hydrate(0, 0);
    const std::string ctx = "seed " + std::to_string(seed);
    TEST_ASSERT_TRUE_MESSAGE(lu::snap(b) == lu::snap(fresh), ctx.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(fresh.messageCount(), b.messageCount(), ctx.c_str());

    // Page the whole history through the model-facing path (cold scan on).
    // (A ring-resident store answers from RAM in one page, with no cursor.)
    MsgQuery q;
    q.coldScan = true;
    q.limit = b.messageCount() <= 512 ? b.messageCount() + 1 : 50;
    std::set<std::string> seen;
    std::string before;
    for (int page = 0; page < 2000; page++) {
      q.before = before;
      EpiQueryInfo info;
      for (const auto& m : b.query(q, &info))
        TEST_ASSERT_TRUE_MESSAGE(seen.insert(m.id).second, ctx.c_str());
      if (info.nextBefore.empty()) break;
      before = info.nextBefore;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(b.messageCount(), (int)seen.size(), ctx.c_str());
  }
}

// T7 - each slice is bounded: at most `sliceRows` rows, at most one window of
// reads (growing only for one long record, up to kBackfillWindowMax), and a
// 40 KB record is indexed whole.
static void test_backfill_slice_is_bounded() {
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, lu::kDir, 64);
    uint32_t id = 1;
    for (uint32_t d = 20700; d < 20703; d++)
      for (int i = 0; i < 300; i++, id++) {
        const int sz = (d == 20701 && i == 150) ? 40 * 1024 : 120 + (int)(id % 60);
        w.addMessage(lu::sizedRow(id, "web", d * 24 + (uint32_t)(i / 13), MsgKind::Message, "", sz));
      }
  }
  AppendLogEpisodicStore full(fs, lu::kDir, 64);
  full.hydrate(0, 0);
  AppendLogEpisodicStore b(fs, lu::kDir, 64);
  b.hydrate(/*maxRows=*/40, /*maxBytes=*/0);
  TEST_ASSERT_TRUE(b.hydrateTruncated());
  const int sliceRows = 5;
  int bigReads = 0;
  for (int step = 0; step < 100000; step++) {
    fs.reset();
    const int rowsBefore = b.backfillStats().rows;
    const bool more = b.backfillStep(kBackfillSliceBytes, sliceRows);
    TEST_ASSERT_TRUE(b.backfillStats().rows - rowsBefore <= sliceRows);
    TEST_ASSERT_TRUE(fs.bytesRead <= (long)kBackfillWindowMax);
    if (fs.bytesRead > (long)kBackfillSliceBytes) bigReads++;
    TEST_ASSERT_TRUE(fs.readRanges <= 8);   // one window: a read plus at most a few extensions
    if (!more) break;
  }
  TEST_ASSERT_TRUE(bigReads >= 1 && bigReads <= 2);   // only the long record grew the window
  TEST_ASSERT_TRUE(b.indexComplete());
  TEST_ASSERT_TRUE(lu::snap(b) == lu::snap(full));
}

// Counter-test for the window cap: a record longer than kBackfillWindowMax
// cannot be indexed whole, so the backfill skips it, keeps indexing the rows
// around it, and the index stays marked INCOMPLETE (the sweep keeps deferring).
// (A stored string tops out at 65,535 characters, but escaping can still make
// its line longer than the cap: a quote encodes as two bytes.)
static void test_backfill_over_long_record_keeps_index_incomplete() {
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, lu::kDir, 64);
    for (uint32_t i = 1; i <= 200; i++) {
      EpisodicMessage m = lu::sizedRow(i, "web", 20700 * 24 + i / 10, MsgKind::ToolOutput,
                                       i == 61 ? lu::blobFor(i, "txt") : "", 300);
      if (i == 60) {
        m.text.assign(65535, '"');
        TEST_ASSERT_TRUE(encodeEpisodicLine(m).size() > kBackfillWindowMax);
      }
      if (!m.blobPath.empty()) fs.files[m.blobPath] = "x";
      w.addMessage(m);
    }
  }
  AppendLogEpisodicStore b(fs, lu::kDir, 64);
  b.hydrate(/*maxRows=*/20, /*maxBytes=*/0);
  lu::runBackfill(b);
  TEST_ASSERT_TRUE(b.backfillStats().stalled);
  TEST_ASSERT_FALSE(b.resumeAfterReadErrors());   // not a read-error stop: no retry
  TEST_ASSERT_TRUE(b.hydrateTruncated());
  TEST_ASSERT_EQUAL_INT(199, b.messageCount());   // every row but the over-long one
  EpiPruneReport rep = b.prune(1, lu::kBlobs);
  TEST_ASSERT_TRUE(rep.blobGcSkipped);
  TEST_ASSERT_TRUE(fs.files.count(lu::blobFor(61, "txt")) == 1);
}

// A card that lists but will not read: the backfill retries, then gives up for
// this boot with the index still incomplete (fail closed, the sweep defers).
static void test_backfill_read_errors_fail_closed() {
  FakeFs fs;
  lu::writeLumi(fs);
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
  fs.failReads = true;
  const int steps = lu::runBackfill(b);
  TEST_ASSERT_TRUE(steps < kBackfillMaxReadErrors + 2);
  TEST_ASSERT_TRUE(b.backfillStats().stalled);
  TEST_ASSERT_TRUE(b.stalledOnReadErrors());
  TEST_ASSERT_FALSE(b.indexComplete());
  TEST_ASSERT_TRUE(b.prune(1, lu::kBlobs).blobGcSkipped);
  // The card reads again: after the device's pause the backfill resumes where
  // it stopped and completes; nothing is indexed twice.
  fs.failReads = false;
  TEST_ASSERT_TRUE(b.resumeAfterReadErrors());
  TEST_ASSERT_FALSE(b.resumeAfterReadErrors());   // nothing left to clear
  lu::runBackfill(b);
  TEST_ASSERT_TRUE(b.indexComplete());
  AppendLogEpisodicStore fresh(fs, lu::kDir, 512);
  fresh.hydrate(0, 0);
  TEST_ASSERT_TRUE(lu::snap(b) == lu::snap(fresh));
}

// A clock that jumped back appends rows to OLD day-streams, including a day the
// boot scan never indexed, the day it read only in part, and 1970-01-01 (before
// the clock is set). The backfill must still index every row exactly once, and
// prune must still spare every referenced blob.
static void test_backfill_with_a_clock_that_jumped_back() {
  FakeFs fs;
  const lu::LumiFixture fx = lu::writeLumi(fs);
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
  TEST_ASSERT_TRUE(b.hydrateTruncated());
  b.addMessage(lu::sizedRow(5000, "web", fx.days[3] * 24 + 5, MsgKind::Message, "", 300));
  b.addMessage(lu::sizedRow(5001, "web", fx.days[12] * 24 + 23, MsgKind::Message, "", 300));
  EpisodicMessage blobRow = lu::sizedRow(5002, "web", 5, MsgKind::ToolOutput, lu::blobFor(5002, "txt"), 300);
  fs.files[blobRow.blobPath] = "x";
  b.addMessage(blobRow);
  lu::runBackfill(b);
  TEST_ASSERT_TRUE(b.indexComplete());
  AppendLogEpisodicStore fresh(fs, lu::kDir, 512);
  fresh.hydrate(0, 0);
  lu::Snap got = lu::snap(b), want = lu::snap(fresh);
  std::sort(got.begin(), got.end());
  std::sort(want.begin(), want.end());
  TEST_ASSERT_TRUE(got == want);   // same rows: none twice, none missing (order aside)
  TEST_ASSERT_EQUAL_INT(lu::kLumiRows + 3, b.messageCount());
  EpiPruneReport rep = b.prune(/*cutoffDayNum=*/1, lu::kBlobs);   // takes 1970-01-01 only
  TEST_ASSERT_EQUAL_INT(1, (int)rep.removedDayFiles.size());
  const std::set<std::string> present = lu::blobsPresent(fs);
  for (const auto& h : lu::blobsReferencedOnCard(fs)) TEST_ASSERT_TRUE(present.count(h) == 1);
}

// T8 - the index cap degrades honestly: past it the backfill stops, the index
// stays marked incomplete, cold queries still reach the oldest row, and prune
// still defers the sweep.
static void test_index_cap_degrades_honestly() {
  TEST_ASSERT_EQUAL_INT(20000, kIndexMaxRows);
  FakeFs fs;
  lu::writeLumi(fs);
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  b.setIndexCap(600);
  b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
  lu::runBackfill(b);
  TEST_ASSERT_EQUAL_INT(600, b.messageCount());
  TEST_ASSERT_TRUE(b.backfillStats().capped);
  TEST_ASSERT_TRUE(b.hydrateTruncated());
  TEST_ASSERT_FALSE(b.backfillStep());   // stays stopped

  MsgQuery q;
  q.coldScan = true;
  q.textContains = lu::idOf(1);   // the oldest row on the card
  q.limit = 5;
  bool found = false;
  std::string before;
  for (int page = 0; page < 200 && !found; page++) {
    q.before = before;
    EpiQueryInfo info;
    for (const auto& m : b.query(q, &info))
      if (m.id == lu::idOf(1)) found = true;
    if (info.nextBefore.empty()) break;
    before = info.nextBefore;
  }
  TEST_ASSERT_TRUE(found);
  const std::set<std::string> blobs = lu::blobsPresent(fs);
  TEST_ASSERT_TRUE(b.prune(1, lu::kBlobs).blobGcSkipped);
  TEST_ASSERT_TRUE(lu::blobsPresent(fs) == blobs);
}

// T9 - fresh device: no episodic directory at all. Nothing is indexed, nothing
// is truncated, the backfill is a no-op and so is retention.
static void test_fresh_card_and_flash_tier() {
  FakeFs fs;
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  TEST_ASSERT_EQUAL_INT(0, b.hydrate(kHydrateMaxRows, kHydrateMaxBytes));
  TEST_ASSERT_FALSE(b.hydrateTruncated());
  TEST_ASSERT_TRUE(b.indexComplete());
  TEST_ASSERT_FALSE(b.backfillStep());
  TEST_ASSERT_TRUE(b.backfillStats().complete);
  EpiPruneReport rep = b.prune(20000, lu::kBlobs);
  TEST_ASSERT_EQUAL_INT(0, (int)rep.removedDayFiles.size());
  TEST_ASSERT_EQUAL_INT(0, (int)rep.removedBlobs.size());
  TEST_ASSERT_EQUAL_INT(0, rep.keptMessages);
  TEST_ASSERT_FALSE(rep.blobGcSkipped);
  // The card-less store keeps its own bounded ring; the append-log changes do
  // not reach it.
  InMemoryEpisodicStore flash(500);
  flash.addMessage(msg("m1", "web", 100, "user", MsgKind::Message, "hello"));
  TEST_ASSERT_EQUAL_INT(1, flash.messageCount());
}

// T10 - query parity: after hydrate + backfill, every query shape answers
// exactly like a store that indexed everything at boot.
static void test_query_parity_after_backfill() {
  FakeFs fs;
  const lu::LumiFixture fx = lu::writeLumi(fs);
  AppendLogEpisodicStore ref(fs, lu::kDir, 512);
  ref.hydrate(0, 0);
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
  lu::runBackfill(b);
  const uint32_t mid = fx.days[12] * 24;
  std::vector<MsgQuery> shapes(7);
  shapes[0].limit = 20;
  shapes[1].sessionId = "web"; shapes[1].limit = 30;
  shapes[2].haveKind = true; shapes[2].kind = MsgKind::ToolOutput; shapes[2].limit = 25;
  shapes[3].textContains = "needle3"; shapes[3].limit = 50;
  shapes[4].textContains = "needle3 needle5"; shapes[4].limit = 10;   // ranked
  shapes[5].sinceHours = mid; shapes[5].beforeHours = mid + 24; shapes[5].limit = 40;
  shapes[6].coldScan = true; shapes[6].limit = 15; shapes[6].sessionAllow = {"web", "system"};
  for (size_t i = 0; i < shapes.size(); i++) {
    std::string bb, rb;
    for (int page = 0; page < 200; page++) {
      MsgQuery qb = shapes[i], qr = shapes[i];
      qb.before = bb;
      qr.before = rb;
      EpiQueryInfo ib, ir;
      const auto gotB = b.query(qb, &ib);
      const auto gotR = ref.query(qr, &ir);
      const std::string ctx = "shape " + std::to_string(i) + " page " + std::to_string(page);
      TEST_ASSERT_EQUAL_INT_MESSAGE((int)gotR.size(), (int)gotB.size(), ctx.c_str());
      for (size_t k = 0; k < gotR.size(); k++)
        TEST_ASSERT_EQUAL_STRING_MESSAGE(gotR[k].id.c_str(), gotB[k].id.c_str(), ctx.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(ir.nextBefore.c_str(), ib.nextBefore.c_str(), ctx.c_str());
      if (ir.nextBefore.empty()) break;
      bb = ib.nextBefore;
      rb = ir.nextBefore;
    }
  }
}

// F1a - a day-stream the index never saw still ages out (v4.5.13 chose the
// days to delete from the index, so a truncated boot stopped retention for
// every unindexed day and the card grew without bound).
static void test_prune_ages_out_unindexed_days_by_name() {
  FakeFs fs;
  const lu::LumiFixture fx = lu::writeLumi(fs);
  fs.files[std::string(lu::kDir) + "/notes.jsonl"] = "{}\n";          // not a day-stream
  fs.files[std::string(lu::kDir) + "/2026-02-31.jsonl"] = "{}\n";     // impossible date
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  b.hydrate(kHydrateMaxRows, kHydrateMaxBytes);
  TEST_ASSERT_TRUE(b.indexFloorDay() > fx.days[10]);   // the September files are unindexed
  const uint32_t cutoff = fx.days[10];                  // everything before 13 Oct is old
  EpiPruneReport rep = b.prune(cutoff, lu::kBlobs);
  TEST_ASSERT_EQUAL_INT(10, (int)rep.removedDayFiles.size());
  for (int i = 0; i < 10; i++)
    TEST_ASSERT_TRUE(fs.files.count(std::string(lu::kDir) + "/" +
                                    AppendLogEpisodicStore::civilDate(fx.days[(size_t)i]) + ".jsonl") == 0);
  TEST_ASSERT_TRUE(fs.files.count(std::string(lu::kDir) + "/notes.jsonl") == 1);
  TEST_ASSERT_TRUE(fs.files.count(std::string(lu::kDir) + "/2026-02-31.jsonl") == 1);
  // The backfill then finishes over what is left, and only that.
  lu::runBackfill(b);
  AppendLogEpisodicStore fresh(fs, lu::kDir, 512);
  fresh.hydrate(0, 0);
  TEST_ASSERT_TRUE(lu::snap(b) == lu::snap(fresh));
}

static void test_day_stream_names_are_one_rule() {
  uint32_t d = 99;
  TEST_ASSERT_TRUE(parseDayStreamName("2026-10-18.jsonl", d));
  TEST_ASSERT_EQUAL_UINT32(dayNumFromName("2026-10-18.jsonl"), d);
  TEST_ASSERT_TRUE(parseDayStreamName("1970-01-01.jsonl", d));   // rows before the clock is set
  TEST_ASSERT_EQUAL_UINT32(0, d);
  for (const char* bad : {"sessions.jsonl", "2026-02-31.jsonl", "2026-7-4.jsonl", "2026-10-18.json",
                          "2026-10-18.jsonl.part", "x2026-10-18.jsonl", "1969-12-31.jsonl", ""})
    TEST_ASSERT_FALSE_MESSAGE(parseDayStreamName(bad, d), bad);
  // Round trip for every day of a year: what dayFile writes, the rule accepts.
  for (uint32_t day = 20454; day < 20454 + 366; day++) {
    const std::string name = AppendLogEpisodicStore::civilDate(day) + ".jsonl";
    TEST_ASSERT_TRUE(parseDayStreamName(name.c_str(), d));
    TEST_ASSERT_EQUAL_UINT32(day, d);
  }
}

// The archive exemption: one predicate, and every retention delete goes
// through it.
static void test_archive_paths_are_exempt_from_retention() {
  for (const char* p : {"/mem/tg", "/mem/tg/", "/mem/tg/123/2026-10-18/5.jpg", "/MEM/TG/x",
                        "/mem//tg/x", "/mem/./tg/x", "/mem/blobs/../tg/x", "/mem/x/../../etc"})
    TEST_ASSERT_TRUE_MESSAGE(isRetentionExempt(p), p);
  for (const char* p : {"/mem/tgx", "/mem/tg.jsonl", "/mem/blobs/abc.txt",
                        "/mem/episodic/2026-10-18.jsonl", "/mem/battery/d20000.jsonl", "/tg/x", ""})
    TEST_ASSERT_FALSE_MESSAGE(isRetentionExempt(p), p);

  // Wiring: a store pointed INTO the archive deletes nothing there, whether the
  // day-streams, the sessions file or the blob directory sit under the root.
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, "/mem/tg/episodic", 8);
    w.addSession({"s1", 1, "p", "t", "active"});
    w.addMessage(msg("m1", "s1", 100 * 24, "user", MsgKind::Message, "old"));
    w.addMessage(msg("m2", "s1", 20000 * 24, "user", MsgKind::Message, "new"));
  }
  fs.files["/mem/tg/blobs/aaaaaaaaaaaaaaaa.jpg"] = "photo";
  const auto before = fs.files;
  AppendLogEpisodicStore b(fs, "/mem/tg/episodic", 8);
  b.hydrate(0, 0);
  EpiPruneReport rep = b.prune(19000, "/mem/tg/blobs");
  TEST_ASSERT_EQUAL_INT(0, (int)rep.removedDayFiles.size());
  TEST_ASSERT_EQUAL_INT(0, (int)rep.removedBlobs.size());
  TEST_ASSERT_TRUE(fs.files == before);

  // Source guard: every delete in the store's retention code is preceded by
  // the predicate. A new fs_.remove( without one fails this count.
  std::ifstream in("lib/core/src/orch_episodic_log.cpp");
  TEST_ASSERT_TRUE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string src = ss.str();
  auto count = [&](const std::string& needle) {
    int n = 0;
    for (size_t at = src.find(needle); at != std::string::npos; at = src.find(needle, at + 1)) n++;
    return n;
  };
  TEST_ASSERT_TRUE(count("fs_.remove(") > 0);
  TEST_ASSERT_EQUAL_INT(count("fs_.remove("), count("isRetentionExempt("));
}

// The prune gate: while a turn is in flight the pass touches NOTHING (no
// listing, no read, no delete) and reports itself deferred.
static void test_retention_defers_while_a_turn_is_in_flight() {
  FakeFs fs;
  lu::writeLumi(fs);
  AppendLogEpisodicStore b(fs, lu::kDir, 512);
  b.hydrate(0, 0);
  const auto before = fs.files;
  fs.reset();
  const uint32_t nowDay = 20750;
  RetentionResult res = runEpisodicRetention(&b, nowDay, 30, lu::kBlobs, /*turnInFlight=*/true);
  TEST_ASSERT_TRUE(res.status == RetentionStatus::DeferredTurn);
  TEST_ASSERT_EQUAL_STRING("deferred-turn", retentionStatusName(res.status));
  TEST_ASSERT_EQUAL_INT(0, fs.lists + fs.readAlls + fs.readRanges + fs.removes + fs.appends);
  TEST_ASSERT_TRUE(fs.files == before);
  // The same pass with the turn over runs and prunes.
  res = runEpisodicRetention(&b, nowDay, 30, lu::kBlobs, false);
  TEST_ASSERT_TRUE(res.status == RetentionStatus::Ran);
  TEST_ASSERT_EQUAL_UINT32(nowDay - 30, res.cutoffDay);
  TEST_ASSERT_TRUE(res.report.removedDayFiles.size() > 0);
  // The other outcomes touch nothing either.
  fs.reset();
  TEST_ASSERT_TRUE(runEpisodicRetention(nullptr, nowDay, 30, lu::kBlobs, false).status ==
                   RetentionStatus::NoStore);
  TEST_ASSERT_TRUE(runEpisodicRetention(&b, nowDay, 0, lu::kBlobs, false).status ==
                   RetentionStatus::NoStore);
  TEST_ASSERT_TRUE(runEpisodicRetention(&b, 10, 30, lu::kBlobs, false).status ==
                   RetentionStatus::ClockUnset);
  TEST_ASSERT_EQUAL_INT(0, fs.lists + fs.removes);
}

// The schedule around the gate: due every period, a deferred pass stays due so
// the very next loop pass retries it (not six hours later), "deferred" is
// reported once per pass, and the period survives the millisecond wrap.
static void test_retention_schedule_retries_next_pass() {
  const uint32_t P = 6u * 3600u * 1000u;
  RetentionSchedule s(P);
  TEST_ASSERT_FALSE(s.due(1000));
  TEST_ASSERT_TRUE(s.due(P + 1));
  // A turn is in flight: deferred, logged once, still due on every later pass.
  TEST_ASSERT_TRUE(s.record(RetentionStatus::DeferredTurn, P + 1));
  int logs = 0;
  for (uint32_t t = P + 2; t < P + 200; t++) {
    TEST_ASSERT_TRUE(s.due(t));
    if (s.record(RetentionStatus::DeferredTurn, t)) logs++;
  }
  TEST_ASSERT_EQUAL_INT(0, logs);   // no log spam while the turn runs
  // The turn ends: the next pass runs, and the period restarts from there.
  TEST_ASSERT_TRUE(s.due(P + 200));
  TEST_ASSERT_FALSE(s.record(RetentionStatus::Ran, P + 200));
  TEST_ASSERT_FALSE(s.pending());
  TEST_ASSERT_FALSE(s.due(P + 201));
  TEST_ASSERT_FALSE(s.due(2 * P + 200));
  TEST_ASSERT_TRUE(s.due(2 * P + 201));
  // A later deferral is reported again (once).
  TEST_ASSERT_TRUE(s.record(RetentionStatus::DeferredTurn, 2 * P + 201));
  TEST_ASSERT_FALSE(s.record(RetentionStatus::DeferredBusy, 2 * P + 202));   // same pass, either reason
  TEST_ASSERT_TRUE(s.due(2 * P + 203));
  TEST_ASSERT_EQUAL_STRING("deferred-busy", retentionStatusName(RetentionStatus::DeferredBusy));
  // No store or no clock: re-arm for a full period, never a tight retry loop.
  TEST_ASSERT_FALSE(s.record(RetentionStatus::ClockUnset, 2 * P + 300));
  TEST_ASSERT_FALSE(s.due(2 * P + 301));
  // Wrap: a pass recorded just before 2^32 ms is due one period later.
  RetentionSchedule w(P);
  const uint32_t nearWrap = 0xFFFFFF00u;
  w.record(RetentionStatus::Ran, nearWrap);
  TEST_ASSERT_FALSE(w.due(nearWrap + 1000));
  TEST_ASSERT_TRUE(w.due(nearWrap + P + 1));
}

// Device wiring guards: the device runs retention through the gate with the
// real turn flag, re-arms through the schedule, drives the backfill from the
// loop beside it, and lists directories without opening every entry.
static std::string slurp(const char* path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
static void test_device_wiring_uses_the_gate_and_the_backfill() {
  const std::string mem = slurp("src/agent/memory_subsystem.cpp");
  TEST_ASSERT_TRUE(mem.find("runEpisodicRetention(") != std::string::npos);
  TEST_ASSERT_TRUE(mem.find("orchestrator::turnInFlight()") != std::string::npos);
  TEST_ASSERT_TRUE(mem.find("backfillStep(") != std::string::npos);
  TEST_ASSERT_TRUE(mem.find("acquireWork(0)") != std::string::npos);
  TEST_ASSERT_TRUE(mem.find("tryLock(50)") != std::string::npos);   // retention never waits on the lock
  TEST_ASSERT_TRUE(mem.find("resumeAfterReadErrors()") != std::string::npos);
  TEST_ASSERT_TRUE(mem.find("blob GC deferred: index incomplete") != std::string::npos);
  // No direct prune() call bypasses the gate.
  TEST_ASSERT_TRUE(mem.find("->prune(") == std::string::npos);
  const std::string main = slurp("src/main.cpp");
  TEST_ASSERT_TRUE(main.find("RetentionSchedule") != std::string::npos);
  TEST_ASSERT_TRUE(main.find("episodicBackfillTick(") != std::string::npos);
  TEST_ASSERT_TRUE(main.find("s_lastRetention") == std::string::npos);   // the old 6 h timer is gone
  const std::string fsrc = slurp("src/agent/epi_fs_arduino.h");
  TEST_ASSERT_TRUE(fsrc.find("forEachName(") != std::string::npos);
  TEST_ASSERT_TRUE(fsrc.find("readdir(") != std::string::npos);
  TEST_ASSERT_TRUE(fsrc.find("openNextFile(") == std::string::npos);   // no per-entry open
}


int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_recent_ring_and_index_route_through_psram_seam);
  RUN_TEST(test_boot_scan_is_bounded_so_a_full_store_still_boots);
  RUN_TEST(test_a_small_store_is_fully_indexed_and_not_truncated);
  RUN_TEST(test_codec_roundtrip_escaping);
  RUN_TEST(test_append_is_o1);
  RUN_TEST(test_query_parity_ring_path);
  RUN_TEST(test_query_parity_index_path);
  RUN_TEST(test_index_skips_out_of_window_days);
  RUN_TEST(test_recent_window_zero_reads);
  RUN_TEST(test_uncapped_history_beyond_500);
  RUN_TEST(test_hydrate_rebuilds_state);
  RUN_TEST(test_hydrate_tolerates_torn_line);
  RUN_TEST(test_next_id_hint);
  RUN_TEST(test_prune_old_days_and_blobs);
  RUN_TEST(test_prune_remove_failure_keeps_index_consistent);
  RUN_TEST(test_civil_date_sortable);
  RUN_TEST(test_append_failure_falls_back_to_ring);
  RUN_TEST(test_cold_scan_finds_what_the_index_cannot);
  RUN_TEST(test_cold_cursor_pages_within_one_day_file);
  RUN_TEST(test_cold_scan_honors_the_read_boundary);
  RUN_TEST(test_cold_scan_budget_is_bounded_and_honest);
  RUN_TEST(test_before_cursor_pages_the_indexed_range);
  RUN_TEST(test_text_match_case_and_order_insensitive);
  RUN_TEST(test_id_suffix_orders_rows);
  RUN_TEST(test_cold_scan_reaches_the_newest_files_unindexed_region);
  RUN_TEST(test_day_number_comes_from_the_file_name);
  RUN_TEST(test_hydrate_tail_read_indexes_file_offsets);
  RUN_TEST(test_cold_budget_cursor_resumes_mid_file);
  RUN_TEST(test_cold_paging_with_resident_truncated_ring);
  RUN_TEST(test_cum515_partial_match_is_found_and_ranked_on_every_backend);
  RUN_TEST(test_cum515_common_words_only_finds_nothing_on_every_backend);
  RUN_TEST(test_cum515_single_word_stays_newest_first_on_every_backend);
  RUN_TEST(test_cum515_ranked_page_is_bounded);
  RUN_TEST(test_cum515_ranked_paging_reaches_deep_history_without_repeats);
  RUN_TEST(test_cum515_l4_advisor_query_finds_the_earlier_conversation);
  RUN_TEST(test_cum515_ranked_paging_never_skips_a_full_match);
  RUN_TEST(test_cum515_ranked_page_stays_inside_the_read_boundary);
  RUN_TEST(test_hydrate_ring_never_outgrows_the_index);
  RUN_TEST(test_cold_cursor_out_of_range_is_bounded);
  RUN_TEST(test_reboot_converges_to_pre_reboot_index_property);
  RUN_TEST(test_boot_index_is_a_contiguous_suffix_and_bounded);
  RUN_TEST(test_lumi_shaped_fixture_reproduces_and_fixes_399);
  RUN_TEST(test_lumi_prune_defers_then_collects_only_orphans);
  RUN_TEST(test_prune_never_deletes_a_blob_referenced_on_card);
  RUN_TEST(test_prune_gc_still_collects_when_index_complete);
  RUN_TEST(test_backfill_interleaves_with_appends_and_prune);
  RUN_TEST(test_backfill_slice_is_bounded);
  RUN_TEST(test_backfill_over_long_record_keeps_index_incomplete);
  RUN_TEST(test_backfill_read_errors_fail_closed);
  RUN_TEST(test_backfill_with_a_clock_that_jumped_back);
  RUN_TEST(test_index_cap_degrades_honestly);
  RUN_TEST(test_fresh_card_and_flash_tier);
  RUN_TEST(test_query_parity_after_backfill);
  RUN_TEST(test_prune_ages_out_unindexed_days_by_name);
  RUN_TEST(test_day_stream_names_are_one_rule);
  RUN_TEST(test_archive_paths_are_exempt_from_retention);
  RUN_TEST(test_retention_defers_while_a_turn_is_in_flight);
  RUN_TEST(test_retention_schedule_retries_next_pass);
  RUN_TEST(test_device_wiring_uses_the_gate_and_the_backfill);
  return UNITY_END();
}
