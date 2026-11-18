// test_tx_log - the transcript on storage (PRD section 2; lane RB-B0b writes it, lane
// RB-T2 makes it pass): appends survive a reopen byte for byte, head.json is only a cache
// (rebuilt after deletion, safe across a cut between temp and rename), a torn tail is
// dropped and never glues onto the next line, a refused append is NotStored, one append
// per item and no rewrite, segments roll at 1 MB without splitting a line, bulk payloads
// page exactly, and nothing lands outside /mem/tx/<chatKey>/.
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "../support/tx_battery/items.h"
#include "../support/tx_battery/mem_files.h"
#include "../support/tx_battery/pending.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"
#include "store_tests.h"

namespace tx = nimbus::orch::tx;

void setUp() {}
void tearDown() {}

namespace {

const char* kChat = "1001";

// Every item kind at least once, in the order a day produces them.
txb::Convo everyKind() {
  txb::Convo c;
  c.openTurn();
  c.owner("what did I eat today?", {"pab12cd34"});
  const std::string id = c.call("ledger_write", R"({"day":"2026-11-17"})", true);
  c.intent(id, "ledger_write");
  c.receipt(id, "ledger_write", "row L00A3 added");
  c.add(tx::ItemKind::RoundMeta).x = "opaque-signed-thinking";
  c.note("[device] limit reached (rounds)", tx::NoteClass::Limit);
  c.say("Logged skyr, 150 g.");
  c.close();
  c.move(4, "cold");
  tx::Item& s = c.add(tx::ItemKind::Digest);
  s.day = "2026-11-16";
  s.x = "Mon 16 Nov: the boat engine.";
  s.thru = 3;
  for (int i = 0; i < 6; ++i) c.ownerTurn("question " + std::to_string(i), "answer " + std::to_string(i));
  return c;
}

std::string encoded(const tx::Item& it) {
  std::string line;
  TXB_OK(tx::encodeLine(it, line));
  return line;
}

void appendAll(tx::TxLog& log, const std::vector<tx::Item>& items) {
  for (const tx::Item& it : items) {
    tx::StoreState state = tx::StoreState::NotStored;
    TXB_OK(log.append(it, state));
    TEST_ASSERT_TRUE(state == tx::StoreState::Stored);
  }
}

std::vector<std::string> allLines(txb::MemFiles& files) {
  std::string dir;
  TXB_OK(tx::txDir(kChat, dir));
  std::vector<std::string> lines;
  for (const std::string& seg : files.segments(dir)) {
    const std::string& s = files.files[seg];
    size_t at = 0, nl;
    while ((nl = s.find('\n', at)) != std::string::npos) {
      lines.push_back(s.substr(at, nl - at + 1));
      at = nl + 1;
    }
  }
  return lines;
}

// Every byte in the chat's segments, whole lines or not.
size_t segmentBytes(txb::MemFiles& files) {
  std::string dir;
  TXB_OK(tx::txDir(kChat, dir));
  size_t n = 0;
  for (const std::string& seg : files.segments(dir)) n += files.files[seg].size();
  return n;
}

tx::Status fillArena(tx::TxLog& log, const tx::HeadState& head, tx::TxArena& arena) {
  tx::Status st = arena.init(tx::kActiveChatArenaBytes, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); });
  return st.ok() ? log.fill(head, arena) : st;
}

// A reopened log fills the arena with the same items, byte for byte, in q order.
void test_reopen_gives_back_every_item_byte_for_byte() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  const txb::Convo c = everyKind();
  {
    tx::TxLog log(files, kChat);
    tx::HeadState head;
    TXB_OK(log.open(head));
    appendAll(log, c.items);
  }
  tx::TxLog log(files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  TEST_ASSERT_EQUAL_UINT64(c.q + 1, head.nextQ);
  tx::HeadState from0 = head;
  from0.thruOffset = 0;
  from0.thru = 0;
  tx::TxArena arena;
  TXB_OK(fillArena(log, from0, arena));
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)arena.count());
  for (size_t i = 0; i < c.items.size(); ++i) {
    tx::Item got;
    TXB_OK(arena.get(i, got));
    TEST_ASSERT_EQUAL_STRING(encoded(c.items[i]).c_str(), encoded(got).c_str());
  }
}

// head.json is a cache: deleted, it is rebuilt from the segments with the same facts.
void test_head_is_rebuilt_after_deletion() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  const txb::Convo c = everyKind();
  tx::HeadState before;
  {
    tx::TxLog log(files, kChat);
    TXB_OK(log.open(before));
    appendAll(log, c.items);
    TXB_OK(log.open(before));
    TXB_OK(log.writeHead(before));
  }
  std::string hp;
  TXB_OK(tx::headPath(kChat, hp));
  TEST_ASSERT_TRUE_MESSAGE(files.files.count(hp), "writeHead wrote no head.json");
  files.files.erase(hp);
  tx::TxLog log(files, kChat);
  tx::HeadState after;
  TXB_OK(log.open(after));
  TEST_ASSERT_EQUAL_UINT64(before.nextQ, after.nextQ);
  TEST_ASSERT_EQUAL_UINT64(3, after.thru);   // the newest s item's thru
  TEST_ASSERT_EQUAL_UINT64(4, after.stub);   // the newest x item's pointer
  TEST_ASSERT_EQUAL_UINT64(before.thruOffset, after.thruOffset);
  TEST_ASSERT_EQUAL(before.seg, after.seg);
}

// A cut between the temp write and the rename leaves a state open() accepts and that
// agrees with the segments.
void test_cut_between_temp_and_rename_keeps_the_state() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  const txb::Convo c = everyKind();
  {
    tx::TxLog log(files, kChat);
    tx::HeadState head;
    TXB_OK(log.open(head));
    appendAll(log, c.items);
    TXB_OK(log.open(head));
    files.killAtRename = (long)files.renames;
    bool cut = false;
    try {
      (void)log.writeHead(head);
    } catch (const txb::Killed&) {
      cut = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(cut, "writeHead never renamed (head.json must be written by temp and rename)");
  }
  files.clearFaults();
  tx::TxLog log(files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  TEST_ASSERT_EQUAL_UINT64(c.q + 1, head.nextQ);
  TEST_ASSERT_EQUAL_UINT64(3, head.thru);
}

// A cut that tears an append drops that line and nothing else; the next append after the
// reopen is a whole line of its own (a torn tail never glues onto it).
void test_torn_tail_is_dropped_and_never_glued() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  txb::Convo c = everyKind();
  {
    tx::TxLog log(files, kChat);
    tx::HeadState head;
    TXB_OK(log.open(head));
    appendAll(log, c.items);
    files.killAtAppend = (long)files.appends;
    files.tornKill = true;
    tx::StoreState state;
    bool cut = false;
    try {
      (void)log.append(c.items.back(), state);   // any item: it is torn half way
    } catch (const txb::Killed&) {
      cut = true;
    }
    TEST_ASSERT_TRUE(cut);
  }
  files.clearFaults();
  tx::TxLog log(files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  TEST_ASSERT_EQUAL_UINT64(c.q + 1, head.nextQ);
  tx::Item next;
  next.q = head.nextQ;
  next.kind = tx::ItemKind::User;
  next.x = "after the cut";
  tx::StoreState state;
  TXB_OK(log.append(next, state));
  const std::vector<std::string> lines = allLines(files);
  size_t decoded = 0;
  for (const std::string& l : lines) {
    tx::Item it;
    decoded += tx::decodeLine(l.data(), l.size(), it).ok();
  }
  TEST_ASSERT_EQUAL_MESSAGE((int)c.items.size() + 1, (int)decoded, "a torn tail cost a whole item");
}

// No card, lock not taken: the append answers NotStored and the item is not half there.
void test_refused_append_is_not_stored() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  const txb::Convo c = everyKind();
  tx::TxLog log(files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  files.refuseAll = true;
  tx::StoreState state = tx::StoreState::Stored;
  const tx::Status st = log.append(c.items.front(), state);
  TEST_ASSERT_TRUE(state == tx::StoreState::NotStored);
  TEST_ASSERT_TRUE_MESSAGE(st.err == tx::Err::NotStored, tx::errSlug(st.err));
  files.refuseAll = false;
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)segmentBytes(files), "a refused append left bytes behind");
}

// One append per item, nothing rewritten while appending (PRD: one append per persist point).
void test_one_append_per_item_and_no_rewrite() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  const txb::Convo c = everyKind();
  tx::TxLog log(files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  const size_t appends = files.appends, whole = files.wholeWrites, renames = files.renames;
  appendAll(log, c.items);
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)(files.appends - appends));
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)(files.wholeWrites - whole), "an append rewrote a file");
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)(files.renames - renames), "an append renamed a file");
}

// Segments roll at 1 MB: none is larger, each ends on a line end, every line decodes.
void test_segments_roll_at_one_megabyte_without_splitting_a_line() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  tx::TxLog log(files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  txb::Convo c;
  c.openTurn();
  for (int i = 0; i < 50; ++i) c.add(tx::ItemKind::RoundMeta).x = txb::payload("META" + std::to_string(i), 50000);
  appendAll(log, c.items);
  std::string dir;
  TXB_OK(tx::txDir(kChat, dir));
  const std::vector<std::string> segs = files.segments(dir);
  TEST_ASSERT_TRUE_MESSAGE(segs.size() >= 3, "2.5 MB of items in fewer than 3 segments");
  for (const std::string& seg : segs) {
    const std::string& s = files.files[seg];
    TEST_ASSERT_TRUE_MESSAGE(s.size() <= tx::kSegmentRollBytes, seg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(!s.empty() && s.back() == '\n', seg.c_str());
  }
  TEST_ASSERT_EQUAL((int)c.items.size(), (int)allLines(files).size());
}

// Bulk payloads come back exactly, whole and by page.
void test_bulk_payloads_page_exactly() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  tx::TxLog log(files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  const std::string a = txb::payload("BULK-A", 31250), b = txb::payload("BULK-B \xC3\xA9\n", 7000);
  std::string ha, hb, got;
  TXB_OK(log.appendBulk(3, a, ha));
  TXB_OK(log.appendBulk(3, b, hb));
  TEST_ASSERT_TRUE(ha != hb);
  TXB_OK(log.readBulk(ha, 0, a.size(), got));
  TEST_ASSERT_TRUE(got == a);
  TXB_OK(log.readBulk(hb, 0, b.size(), got));
  TEST_ASSERT_TRUE(got == b);
  TXB_OK(log.readBulk(ha, 1000, 4096, got));
  TEST_ASSERT_TRUE(got == a.substr(1000, 4096));
  TXB_OK(log.readBulk(hb, 6990, 4096, got));
  TEST_ASSERT_TRUE_MESSAGE(got == b.substr(6990), "a page past the end returns the rest only");
}

// A line over 60,000 B is refused whole, never cut, and nothing is written.
void test_a_line_over_the_ceiling_is_refused_whole() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  tx::TxLog log(files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  tx::Item big;
  big.q = 1;
  big.kind = tx::ItemKind::RoundMeta;
  big.x = std::string(tx::kMaxLineBytes, 'k');
  tx::StoreState state;
  const tx::Status st = log.append(big, state);
  TEST_ASSERT_TRUE_MESSAGE(st.err == tx::Err::TooLarge, tx::errSlug(st.err));
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)segmentBytes(files), "part of a refused line was written");
}

// Every path is under /mem/tx/<chatKey>/; an unsafe chat key has no path at all.
void test_paths_stay_inside_the_chat_folder() {
  TXB_WAITS_FOR("PENDING RB-T2");
  std::string s;
  TXB_OK(tx::txDir(kChat, s));
  TEST_ASSERT_EQUAL_STRING("/mem/tx/1001", s.c_str());
  const char* bad[] = {"", "..", "../1001", "a/b", "a\\b", "x y", "\n"};
  for (const char* key : bad) TEST_ASSERT_TRUE_MESSAGE(tx::txDir(key, s).err == tx::Err::InvalidArgument, key);
  TXB_OK(tx::segmentPath(kChat, 1, s));
  TEST_ASSERT_EQUAL_STRING("/mem/tx/1001/seg-000001.jsonl", s.c_str());
  TXB_OK(tx::headPath(kChat, s));
  TEST_ASSERT_EQUAL_STRING("/mem/tx/1001/head.json", s.c_str());
  TXB_OK(tx::bulkPath(kChat, 12, s));
  TEST_ASSERT_EQUAL_STRING("/mem/tx/1001/b/12.txb", s.c_str());
  TXB_OK(tx::digestArchivePath(kChat, "2026-11", s));
  TEST_ASSERT_EQUAL_STRING("/mem/tx/1001/digests-2026-11.md", s.c_str());
  TXB_OK(tx::migratedPath(kChat, s));
  TEST_ASSERT_EQUAL_STRING("/mem/tx/1001/migrated.json", s.c_str());
}

void test_nothing_is_written_outside_the_chat_folder() {
  TXB_WAITS_FOR("PENDING RB-T2", "RB-T1");
  txb::MemFiles files;
  {
    tx::TxLog log(files, kChat);
    tx::HeadState head;
    TXB_OK(log.open(head));
    appendAll(log, everyKind().items);
    std::string h;
    TXB_OK(log.appendBulk(2, "payload", h));
    TXB_OK(log.open(head));
    TXB_OK(log.writeHead(head));
  }
  for (const auto& kv : files.files)
    TEST_ASSERT_TRUE_MESSAGE(kv.first.rfind("/mem/tx/1001/", 0) == 0, kv.first.c_str());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reopen_gives_back_every_item_byte_for_byte);
  RUN_TEST(test_head_is_rebuilt_after_deletion);
  RUN_TEST(test_cut_between_temp_and_rename_keeps_the_state);
  RUN_TEST(test_torn_tail_is_dropped_and_never_glued);
  RUN_TEST(test_refused_append_is_not_stored);
  RUN_TEST(test_one_append_per_item_and_no_rewrite);
  RUN_TEST(test_segments_roll_at_one_megabyte_without_splitting_a_line);
  RUN_TEST(test_bulk_payloads_page_exactly);
  RUN_TEST(test_a_line_over_the_ceiling_is_refused_whole);
  RUN_TEST(test_paths_stay_inside_the_chat_folder);
  RUN_TEST(test_nothing_is_written_outside_the_chat_folder);
  store_tests::run();   // lane RB-T2's own cases
  return UNITY_END();
}
