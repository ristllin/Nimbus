#include <unity.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/row_overlay.h"

// The episodic `after` cursor (lane H of the history fix): rows STRICTLY NEWER than a
// row id, OLDEST FIRST, a page at a time, on every backend - the in-memory reference,
// the SD log's RAM ring, its index (card reads in blocks under the per-call budget) and
// its cold region (the day-streams the boot scan left out, under the cold budget, with a
// byte cursor) - and a span the card will not give back named, never skipped silently.
// The fold reads its slice forward from the summary's last row with it (thru), so a
// cursor that drops or repeats a row is a hole in the summary or a row summarized twice.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

struct FakeFs : EpiFs {
  std::map<std::string, std::string> files;
  std::set<std::string> unreadable;   // paths whose reads fail (a bad sector)
  mutable int readRanges = 0;
  mutable long bytesRead = 0;

  long size(const std::string& path) const override {
    auto it = files.find(path);
    return it == files.end() ? 0 : (long)it->second.size();
  }
  long append(const std::string& path, const std::string& bytes) override {
    std::string& f = files[path];
    const long off = (long)f.size();
    f += bytes;
    return off;
  }
  std::string readAll(const std::string& path) const override {
    auto it = files.find(path);
    return it == files.end() ? std::string() : it->second;
  }
  std::string readRange(const std::string& path, long offset, long len) const override {
    readRanges++;
    auto it = files.find(path);
    if (it == files.end() || unreadable.count(path) || offset < 0 || len < 0) return std::string();
    if ((size_t)(offset + len) > it->second.size()) return std::string();
    bytesRead += len;
    return it->second.substr((size_t)offset, (size_t)len);
  }
  void forEachName(const std::string& dir, const std::function<void(const char*)>& fn) const override {
    const std::string prefix = dir + "/";
    for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0;
         ++it) {
      const char* rest = it->first.c_str() + prefix.size();
      if (std::strchr(rest, '/') == nullptr) fn(rest);
    }
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    forEachName(dir, [&](const char* n) { out.push_back(n); });
    return out;
  }
  bool remove(const std::string& path) override { return files.erase(path) > 0; }
  bool exists(const std::string& path) const override { return files.count(path) > 0; }
};

const uint32_t kDay0 = 20400;   // 2025-11-08, an epoch day: rows land in distinct day-streams

std::string idOf(uint32_t n) {
  char b[16];
  snprintf(b, sizeof(b), "m%08x", (unsigned)n);
  return b;
}

// One chat ("c1") interleaved with trace rows and another chat, over several days: the
// cursor must skip what the filters drop and keep the chat's rows in id order.
struct Corpus {
  std::vector<EpisodicMessage> all;   // oldest first
  uint32_t seq = 0;
  void add(uint32_t day, const std::string& sess, MsgKind kind, const std::string& role, size_t bytes) {
    EpisodicMessage m;
    m.id = idOf(++seq);
    m.sessionId = sess;
    m.tsHours = (kDay0 + day) * 24 + (seq % 24);
    m.role = role;
    m.kind = kind;
    m.text = "row " + m.id + " " + std::string(bytes, 'x');
    all.push_back(m);
  }
  // `days` days of `perDay` exchanges in c1, each followed by a trace row and a row of c2.
  static Corpus make(int days, int perDay, size_t bytes = 200) {
    Corpus c;
    for (int d = 0; d < days; d++)
      for (int i = 0; i < perDay; i++) {
        c.add((uint32_t)d, "c1", MsgKind::Message, "user", bytes);
        c.add((uint32_t)d, "c1", MsgKind::ToolOutput, "tool", bytes);
        c.add((uint32_t)d, "c2", MsgKind::Message, "user", bytes);
        c.add((uint32_t)d, "c1", MsgKind::Message, "assistant", bytes);
      }
    return c;
  }
  // The chat's conversation rows strictly after `after`, oldest first.
  std::vector<std::string> expectAfter(const std::string& after) const {
    std::vector<std::string> out;
    for (const auto& m : all)
      if (m.sessionId == "c1" && m.kind == MsgKind::Message && epiIdSuffix(m.id) > epiIdSuffix(after))
        out.push_back(m.id);
    return out;
  }
};

MsgQuery chatAfter(const std::string& after, int limit, bool cold = false) {
  MsgQuery q;
  q.sessionId = "c1";
  q.haveKind = true;
  q.kind = MsgKind::Message;
  q.limit = limit;
  q.after = after;
  q.coldScan = cold;
  return q;
}

// Every page of an after-read from `start` until the store says nothing newer is left.
std::vector<std::string> pageAll(const EpisodicStore& s, const std::string& start, int limit, bool cold,
                                 int* pages = nullptr, std::string* unreadable = nullptr) {
  std::vector<std::string> out;
  std::string cursor = start;
  for (int p = 0; p < 400; p++) {
    EpiQueryInfo info;
    const auto rows = s.query(chatAfter(cursor, limit, cold), &info);
    for (const auto& m : rows) out.push_back(m.id);
    if (unreadable && !info.unreadable.empty()) *unreadable = info.unreadable;
    if (pages) *pages = p + 1;
    if (!info.newerExists) break;
    TEST_ASSERT_FALSE_MESSAGE(info.nextAfter.empty(), "a page that says more is newer names its cursor");
    cursor = info.nextAfter;
  }
  return out;
}

void assertSame(const std::vector<std::string>& want, const std::vector<std::string>& got) {
  TEST_ASSERT_EQUAL_MESSAGE((int)want.size(), (int)got.size(), "rows after the cursor, no gap and no repeat");
  for (size_t i = 0; i < want.size(); i++) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
}

}  // namespace

// The reference store: strictly newer, oldest first, the limit, and `before` still bounds it.
static void test_in_memory_rows_after_a_cursor_oldest_first() {
  const Corpus c = Corpus::make(3, 10);
  InMemoryEpisodicStore s(100000);
  for (const auto& m : c.all) s.addMessage(m);
  const std::string after = idOf(37);
  const auto want = c.expectAfter(after);
  const auto page = s.query(chatAfter(after, 5));
  TEST_ASSERT_EQUAL(5, (int)page.size());
  for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_STRING(want[(size_t)i].c_str(), page[(size_t)i].id.c_str());
  assertSame(want, pageAll(s, after, 7, false));
  // Both cursors: the rows between them, oldest first.
  MsgQuery q = chatAfter(idOf(10), 100);
  q.before = idOf(30);
  const auto between = s.query(q);
  TEST_ASSERT_TRUE(!between.empty());
  for (const auto& m : between) {
    TEST_ASSERT_TRUE(epiIdSuffix(m.id) > 10 && epiIdSuffix(m.id) < 30);
  }
  for (size_t i = 1; i < between.size(); i++)
    TEST_ASSERT_TRUE(epiIdSuffix(between[i - 1].id) < epiIdSuffix(between[i].id));
  // Nothing newer than the newest row.
  EpiQueryInfo info;
  TEST_ASSERT_EQUAL(0, (int)s.query(chatAfter(c.all.back().id, 10), &info).size());
  TEST_ASSERT_FALSE(info.newerExists);
}

// The SD log, every row in its RAM ring: the same rows, and not one card read.
static void test_ring_resident_after_reads_no_card() {
  const Corpus c = Corpus::make(2, 12);
  FakeFs fs;
  AppendLogEpisodicStore s(fs, "/mem/episodic", 512);
  for (const auto& m : c.all) s.addMessage(m);
  fs.readRanges = 0;
  assertSame(c.expectAfter(idOf(9)), pageAll(s, idOf(9), 6, false));
  TEST_ASSERT_EQUAL_MESSAGE(0, fs.readRanges, "a ring-resident history answers from RAM");
}

// The index beyond the ring: rows come off the card in blocks under the per-call budget,
// and a stopped call names the cursor that continues exactly after its last row.
static void test_index_after_pages_under_the_read_budget() {
  const Corpus c = Corpus::make(4, 20, 9000);   // ~9 KB rows: the 128 KB budget stops a call early
  FakeFs fs;
  AppendLogEpisodicStore s(fs, "/mem/episodic", 16);
  for (const auto& m : c.all) s.addMessage(m);
  TEST_ASSERT_EQUAL_INT((int)c.all.size(), s.hydrate(0, 0));
  int pages = 0;
  const std::string after = idOf(3);
  assertSame(c.expectAfter(after), pageAll(s, after, 200, false, &pages));
  TEST_ASSERT_TRUE_MESSAGE(pages > 1, "the per-call budget paged the read");
  // Each call stays inside the budget (plus one row, the first read of a call).
  fs.bytesRead = 0;
  EpiQueryInfo info;
  s.query(chatAfter(after, 200), &info);
  TEST_ASSERT_TRUE(info.newerExists);
  TEST_ASSERT_TRUE_MESSAGE(fs.bytesRead <= (long)(kIndexBytesPerCall + kColdWindow), std::to_string(fs.bytesRead).c_str());
}

// The cold region (rows the boot scan never indexed): an after-read with coldScan walks it
// forward from the cursor, pages with a byte cursor under the cold budget and continues
// into the index with no gap. Without coldScan it does not read the cold region and says
// rows may be missing below the index.
static void test_cold_after_reads_forward_then_joins_the_index() {
  const Corpus c = Corpus::make(6, 15, 1500);
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 64);
    for (const auto& m : c.all) w.addMessage(m);
  }
  AppendLogEpisodicStore s(fs, "/mem/episodic", 64);
  s.hydrate(80, 0);   // a truncated boot: only the newest 80 rows are indexed
  TEST_ASSERT_TRUE(s.hydrateTruncated());
  const std::string after = idOf(5);   // the summary's last row, far below the index
  int pages = 0;
  const auto got = pageAll(s, after, 1000, true, &pages);
  assertSame(c.expectAfter(after), got);
  TEST_ASSERT_TRUE_MESSAGE(pages > 1, "the cold budget paged the walk");
  // Every call within the cold budget (and the index's).
  EpiQueryInfo info;
  fs.bytesRead = 0;
  s.query(chatAfter(after, 1000, true), &info);
  TEST_ASSERT_TRUE(fs.bytesRead <= (long)(kColdMaxBytes + kColdWindowMax + kIndexBytesPerCall));
  TEST_ASSERT_TRUE(info.newerExists);
  TEST_ASSERT_TRUE_MESSAGE(info.nextAfter.find(':') != std::string::npos, "a cold page continues by byte cursor");
  // An after read walks the gap whether or not it asked for a cold scan: its reader (the
  // fold, from the summary's last row) must never be handed the index's rows as if they
  // followed the cursor.
  assertSame(c.expectAfter(after), pageAll(s, after, 1000, false));
}

// Rows stored before the clock was set sit in the day-0 stream, which a restart indexes first:
// the index is then out of id order. An after read still pages in id order, losing nothing.
static void test_after_reads_follow_ids_when_the_index_does_not() {
  FakeFs fs;
  std::vector<std::string> want;
  uint32_t seq = 0;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 512);
    for (int i = 0; i < 30; i++) {
      EpisodicMessage m;
      m.id = idOf(++seq);
      m.sessionId = "c1";
      m.role = "user";
      m.kind = MsgKind::Message;
      // A reboot with no clock in the middle: rows 11-15 are stamped with boot hours (day 0).
      m.tsHours = (i >= 10 && i < 15) ? (uint32_t)i : (kDay0 + (i < 10 ? 0u : 1u)) * 24 + 1;
      m.text = "row " + m.id;
      w.addMessage(m);
      if (i >= 3) want.push_back(m.id);
    }
  }
  for (int ring : {512, 4}) {   // the RAM ring alone, and the index read off the card
    AppendLogEpisodicStore s(fs, "/mem/episodic", ring);
    s.hydrate(0, 0);
    std::vector<uint32_t> order;
    s.forEachIndexed([&](uint32_t, uint32_t, uint32_t, uint32_t sfx) { order.push_back(sfx); });
    TEST_ASSERT_EQUAL_UINT32(11, order.front());   // the day-0 rows come first in the index
    assertSame(want, pageAll(s, idOf(3), 4, false));
  }
}

// A chat row for the two no-clock tests: `hours` picks its day-stream.
static EpisodicMessage chatRow(uint32_t n, uint32_t hours, size_t bytes) {
  EpisodicMessage m;
  m.id = idOf(n);
  m.sessionId = "c1";
  m.role = "user";
  m.kind = MsgKind::Message;
  m.tsHours = hours;
  m.text = "row " + m.id + " " + std::string(bytes, 'x');
  return m;
}

// Rows stored before the clock was set sit in a no-clock day-stream (day 0), whose ids
// interleave with the dated days'. A boot scan that stopped early leaves it unindexed (day 0
// sorts oldest), and the card alone cannot put its rows in id order: a forward read below the
// index would hand day-0 rows out ahead of older dated rows, and a reader that pages by the
// last id (the fold) would then anchor past rows it never read. So the read names the stream
// and returns nothing (its reader waits), and once the background indexing has read the card
// the same read is whole and in id order.
static void test_a_cold_no_clock_stream_is_named_not_misordered() {
  FakeFs fs;
  std::vector<std::string> want;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 512);
    for (uint32_t i = 1; i <= 300; i++) {
      // Rows 41-120 arrive during a restart with no clock (boot hours 0-7: day 0).
      const uint32_t hours = (i > 40 && i <= 120) ? (i - 41) / 10 : (kDay0 + (i <= 40 ? 0u : 1u)) * 24 + 1;
      w.addMessage(chatRow(i, hours, 200));
      if (i > 5) want.push_back(idOf(i));
    }
  }
  AppendLogEpisodicStore s(fs, "/mem/episodic", 16);
  s.hydrate(40, 0);   // a truncated boot scan: the newest rows only
  TEST_ASSERT_TRUE(s.hydrateTruncated());
  EpiQueryInfo info;
  const auto rows = s.query(chatAfter(idOf(5), 24), &info);
  TEST_ASSERT_TRUE_MESSAGE(rows.empty(), rows.empty() ? "" : rows.front().id.c_str());
  TEST_ASSERT_TRUE_MESSAGE(info.unreadable.compare(0, 2, "0:") == 0, info.unreadable.c_str());
  TEST_ASSERT_FALSE(info.newerExists);
  while (s.backfillStep()) {
  }
  TEST_ASSERT_FALSE(s.hydrateTruncated());
  assertSame(want, pageAll(s, idOf(5), 24, false));
}

// v5.1.2 fix review E: a cold no-clock stream blocks only the cursors it could hold rows after.
// A cursor newer than every row of the stream (the fold of a chat whose anchor is past the
// boot with no clock) reads on, in id order, while the stream is still cold.
static void test_a_cold_no_clock_stream_does_not_block_newer_cursors() {
  FakeFs fs;
  std::vector<std::string> want;
  const uint32_t kAfter = 200;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 512);
    for (uint32_t i = 1; i <= 300; i++) {
      // Rows 41-120 arrive during a restart with no clock (boot hours 0-7: day 0).
      const uint32_t hours = (i > 40 && i <= 120) ? (i - 41) / 10 : (kDay0 + (i <= 40 ? 0u : 1u + (i - 121) / 60)) * 24 + 1;
      w.addMessage(chatRow(i, hours, 200));
      if (i > kAfter) want.push_back(idOf(i));
    }
  }
  AppendLogEpisodicStore s(fs, "/mem/episodic", 16);
  s.hydrate(40, 0);   // the newest rows only: the no-clock stream stays cold
  TEST_ASSERT_TRUE(s.hydrateTruncated());
  EpiQueryInfo info;
  s.query(chatAfter(idOf(kAfter), 24), &info);
  TEST_ASSERT_TRUE_MESSAGE(info.unreadable.empty(), info.unreadable.c_str());
  assertSame(want, pageAll(s, idOf(kAfter), 24, false));
  EpiQueryInfo older;   // a cursor the stream could hold rows after is still blocked
  TEST_ASSERT_TRUE(s.query(chatAfter(idOf(50), 24), &older).empty());
  TEST_ASSERT_TRUE(older.unreadable.compare(0, 2, "0:") == 0);
}

// Fix review, candidate 2: the store says when a byte cursor is stale (its index completed, so
// the position the cursor names is gone), and only then. The real end of a cold walk reached
// through a byte cursor, while the index is still truncated, is an empty page with no flag.
static void test_the_store_names_a_stale_byte_cursor_and_only_that() {
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 512);
    for (uint32_t i = 1; i <= 400; i++) {   // c1's rows first, then other chats' rows on later days
      EpisodicMessage m = chatRow(i, (kDay0 + (i - 1) / 50) * 24 + 1, 600);
      if (i > 20) m.sessionId = "other";
      w.addMessage(m);
    }
  }
  AppendLogEpisodicStore s(fs, "/mem/episodic", 16);
  s.hydrate(30, 0);   // a truncated boot scan: c1 is all cold
  TEST_ASSERT_TRUE(s.hydrateTruncated());
  std::string cursor = idOf(20);   // after c1's last row: cold bytes with none of its rows
  bool sawByte = false;
  for (int call = 0; call < 50; call++) {
    EpiQueryInfo info;
    const auto rows = s.query(chatAfter(cursor, 24), &info);
    TEST_ASSERT_TRUE(rows.empty());
    TEST_ASSERT_FALSE_MESSAGE(info.cursorStale, "the real end is never named stale");
    if (!info.newerExists) break;   // the real end
    cursor = info.nextAfter;
    sawByte = sawByte || cursor.find(':') != std::string::npos;
  }
  TEST_ASSERT_TRUE_MESSAGE(sawByte, "the walk went on by byte cursor");
  while (s.backfillStep()) {
  }
  TEST_ASSERT_FALSE(s.hydrateTruncated());
  EpiQueryInfo stale;
  TEST_ASSERT_TRUE(s.query(chatAfter(cursor.find(':') != std::string::npos ? cursor : "3:0", 24), &stale).empty());
  TEST_ASSERT_TRUE_MESSAGE(stale.cursorStale, "a byte cursor on a complete index is named stale");
}

// A no-clock stream that is wholly indexed (it began this session) does not move where the
// cold walk starts: the walk still begins at the cursor's own dated day, so a page the cold
// budget cuts short continues by byte cursor past the cursor row, never from the first day
// (which would hand out rows at or before the cursor as if they followed it).
static void test_a_no_clock_stream_does_not_move_the_cold_start() {
  FakeFs fs;
  std::vector<std::string> want;
  const uint32_t kRows = 400, kAfter = 330;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 64);
    for (uint32_t i = 1; i <= kRows; i++) {   // five dated days of 80 rows
      w.addMessage(chatRow(i, (kDay0 + (i - 1) / 80) * 24 + 1, 600));
      if (i > kAfter) want.push_back(idOf(i));
    }
  }
  AppendLogEpisodicStore s(fs, "/mem/episodic", 64);
  s.hydrate(20, 0);   // rows 381-400 indexed: the cursor's day is cold below them
  TEST_ASSERT_TRUE(s.hydrateTruncated());
  for (uint32_t i = kRows + 1; i <= kRows + 6; i++) {   // this session, before the clock is set
    s.addMessage(chatRow(i, 0, 600));
    want.push_back(idOf(i));
  }
  assertSame(want, pageAll(s, idOf(kAfter), 1000, false));
}

// A day-stream the card will not read is named as an unreadable span; the rows after it
// still come back, in order.
static void test_an_unreadable_span_is_reported() {
  const Corpus c = Corpus::make(5, 10, 600);
  FakeFs fs;
  {
    AppendLogEpisodicStore w(fs, "/mem/episodic", 64);
    for (const auto& m : c.all) w.addMessage(m);
  }
  AppendLogEpisodicStore s(fs, "/mem/episodic", 64);
  s.hydrate(40, 0);
  const std::string bad = s.dayFile(kDay0 + 1);
  fs.unreadable.insert(bad);
  std::string span;
  const auto got = pageAll(s, idOf(2), 1000, true, nullptr, &span);
  TEST_ASSERT_FALSE_MESSAGE(span.empty(), "the unread day is named");
  TEST_ASSERT_TRUE_MESSAGE(span.find(std::to_string(kDay0 + 1)) == 0, span.c_str());
  // Every row of the other days, in order.
  std::vector<std::string> want;
  for (const auto& id : c.expectAfter(idOf(2))) {
    bool onBad = false;
    for (const auto& m : c.all)
      if (m.id == id) onBad = m.tsHours / 24 == kDay0 + 1;
    if (!onBad) want.push_back(id);
  }
  assertSame(want, got);
}

// The photo-record overlay applies to after-reads like any other read.
static void test_the_overlay_patches_after_reads() {
  const Corpus c = Corpus::make(1, 6);
  InMemoryEpisodicStore inner(1000);
  for (const auto& m : c.all) inner.addMessage(m);
  RowTextOverlay ov;
  ov.bind(&inner);
  const std::string target = c.expectAfter(idOf(0))[3];
  ov.put(target, "[image 1/1 ref=abc] a bowl of blueberries");
  bool seen = false;
  for (const auto& m : ov.query(chatAfter(idOf(0), 100)))
    if (m.id == target) seen = m.text == "[image 1/1 ref=abc] a bowl of blueberries";
  TEST_ASSERT_TRUE(seen);
}

// A text filter on an after-read is a yes/no filter: the order stays oldest first.
static void test_text_filter_keeps_the_order() {
  Corpus c = Corpus::make(1, 8);
  c.all[8].text = "blueberries 150 g";
  c.all[20].text = "blueberries 50 g";
  InMemoryEpisodicStore s(1000);
  for (const auto& m : c.all) s.addMessage(m);
  MsgQuery q = chatAfter(idOf(0), 10);
  q.textContains = "blueberries grams";
  const auto rows = s.query(q);
  TEST_ASSERT_EQUAL(2, (int)rows.size());
  TEST_ASSERT_EQUAL_STRING(c.all[8].id.c_str(), rows[0].id.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_in_memory_rows_after_a_cursor_oldest_first);
  RUN_TEST(test_ring_resident_after_reads_no_card);
  RUN_TEST(test_index_after_pages_under_the_read_budget);
  RUN_TEST(test_cold_after_reads_forward_then_joins_the_index);
  RUN_TEST(test_after_reads_follow_ids_when_the_index_does_not);
  RUN_TEST(test_a_cold_no_clock_stream_is_named_not_misordered);
  RUN_TEST(test_a_cold_no_clock_stream_does_not_block_newer_cursors);
  RUN_TEST(test_the_store_names_a_stale_byte_cursor_and_only_that);
  RUN_TEST(test_a_no_clock_stream_does_not_move_the_cold_start);
  RUN_TEST(test_an_unreadable_span_is_reported);
  RUN_TEST(test_the_overlay_patches_after_reads);
  RUN_TEST(test_text_filter_keeps_the_order);
  return UNITY_END();
}
