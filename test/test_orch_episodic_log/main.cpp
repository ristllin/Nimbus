#include <unity.h>
#include <algorithm>

#include <map>
#include <set>
#include <string>

#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/blob_store.h"

using namespace nimbus::orch;

// ---- in-memory EpiFs that counts every op, so the tests can PROVE the
// O(1)-append + zero-read-fast-path + skip-out-of-window properties, not just
// the query results. -------------------------------------------------------
struct FakeFs : EpiFs {
  std::map<std::string, std::string> files;
  mutable int appends = 0, readAlls = 0, readRanges = 0, lists = 0, removes = 0;

  void reset() { appends = readAlls = readRanges = lists = removes = 0; }

  long size(const std::string& path) const override {
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
    return it == files.end() ? std::string() : it->second;
  }
  std::string readRange(const std::string& path, long offset, long len) const override {
    readRanges++;
    auto it = files.find(path);
    if (it == files.end() || offset < 0 || len < 0) return std::string();
    if ((size_t)(offset + len) > it->second.size()) return std::string();
    return it->second.substr(offset, len);
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
static void test_text_match_case_and_order_insensitive() {
  TEST_ASSERT_TRUE(epiTextMatch("The Bilge Pump serial is BP-4471", "bilge pump"));
  TEST_ASSERT_TRUE(epiTextMatch("The Bilge Pump serial is BP-4471", "pump bilge"));
  TEST_ASSERT_TRUE(epiTextMatch("The Bilge Pump serial is BP-4471", "BP-4471 serial"));
  TEST_ASSERT_FALSE(epiTextMatch("The Bilge Pump serial", "bilge anchor"));
  TEST_ASSERT_TRUE(epiTextMatch("anything", ""));        // no filter
  TEST_ASSERT_TRUE(epiTextMatch("anything", "   "));     // whitespace-only: no filter
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

int main(int, char**) {
  UNITY_BEGIN();
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
  return UNITY_END();
}
