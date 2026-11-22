// test_tx_card_files - the transcript's card store (lane E2-DEV, nimbus/orch/tx/card_files.h) over a
// fake card with a FAT card's rules: a file needs its folder, rename does not replace an existing
// target, a folder listing shows folders too. Pins what the device binding promises the log:
//   - a write creates every missing folder on its path (/mem, /mem/tx, the chat, its b/);
//   - no card, a task that may not touch a file, or the internal memory floor: every write is
//     refused (false), nothing is written, and the refusal is counted;
//   - rename replaces its target (removed first, as the TxFiles contract allows);
//   - list of a missing folder is true with no names only when its parent listed whole without
//     it; an exists() or a listing that fails is false (cannot tell), never "empty" (lane
//     E2-STORE, finding S1); names are bare and folders are skipped;
//   - nothing outside /mem/tx is read or written;
//   - the card lock is held for every call and released after it, and each kind's longest hold
//     is kept;
//   - a TxLog over it gives back after a reopen what it stored (the restart the bench proves).
#include <unity.h>

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/card_files.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"

namespace tx = nimbus::orch::tx;

namespace {

class FakeCard : public tx::CardOps {
 public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{"/"};
  bool card = true, task = true;
  uint32_t largest = 64 * 1024;
  uint32_t ms = 0, stepMs = 3;   // every card call takes stepMs of virtual time
  int depth = 0, maxDepth = 0, locks = 0, calls = 0;
  int failExists = 0;            // the next N exists() calls answer false (a bad read, a refused allocation)
  bool failListNames = false;    // every listNames() fails

  bool present() override { return card; }
  bool mayWrite() override { return task; }
  uint32_t largestInternal() override { return largest; }
  uint32_t nowMs() override { return ms; }
  void lock() override {
    ++depth;
    ++locks;
    if (depth > maxDepth) maxDepth = depth;
  }
  void unlock() override { --depth; }
  bool exists(const std::string& p) override {
    step();
    if (failExists > 0) return --failExists, false;
    return files.count(p) || dirs.count(p);
  }
  bool isDir(const std::string& p) override { return step() && dirs.count(p); }
  bool mkdir(const std::string& p) override {
    step();
    if (!dirs.count(parent(p)) || files.count(p)) return false;
    return dirs.insert(p).second;
  }
  size_t append(const std::string& p, const char* d, size_t n) override {
    step();
    if (!dirs.count(parent(p)) || dirs.count(p)) return 0;
    files[p].append(d, n);
    return n;
  }
  bool readAt(const std::string& p, size_t off, char* buf, size_t want, size_t& got) override {
    step();
    got = 0;
    auto it = files.find(p);
    if (it == files.end()) return false;
    if (off >= it->second.size()) return true;
    got = it->second.copy(buf, want, off);
    return true;
  }
  bool fileSize(const std::string& p, size_t& out) override {
    step();
    auto it = files.find(p);
    if (it == files.end()) return false;
    out = it->second.size();
    return true;
  }
  size_t writeWhole(const std::string& p, const char* d, size_t n) override {
    step();
    if (!dirs.count(parent(p))) return 0;
    files[p].assign(d, n);
    return n;
  }
  bool rename(const std::string& from, const std::string& to) override {
    step();
    if (files.count(to) || !files.count(from)) return false;   // FAT: never over an existing file
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  bool remove(const std::string& p) override {
    step();
    return files.erase(p) > 0;
  }
  bool listFiles(const std::string& dir, std::vector<std::string>& names) override {
    step();
    const std::string pre = dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {
    step();
    names.clear();
    if (failListNames || !dirs.count(dir)) return false;
    const std::string pre = dir == "/" ? std::string("/") : dir + "/";
    auto child = [&](const std::string& p) {
      if (p.size() > pre.size() && p.rfind(pre, 0) == 0 && p.find('/', pre.size()) == std::string::npos)
        names.push_back(p.substr(pre.size()));
    };
    for (const auto& kv : files) child(kv.first);
    for (const std::string& d : dirs) child(d);
    return true;
  }

 private:
  bool step() {
    TEST_ASSERT_TRUE_MESSAGE(depth > 0, "every card step runs under the card lock");
    ms += stepMs;
    ++calls;
    return true;
  }
  static std::string parent(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == 0 ? "/" : p.substr(0, s);
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

static void test_a_write_creates_every_missing_folder() {
  FakeCard c;
  tx::CardTxFiles f(c);
  TEST_ASSERT_TRUE(f.append("/mem/tx/123/b/7.txb", "abc", 3));
  TEST_ASSERT_TRUE(c.dirs.count("/mem") && c.dirs.count("/mem/tx") && c.dirs.count("/mem/tx/123") &&
                   c.dirs.count("/mem/tx/123/b"));
  TEST_ASSERT_EQUAL_STRING("abc", c.files["/mem/tx/123/b/7.txb"].c_str());
  TEST_ASSERT_TRUE(f.writeWhole("/mem/tx/web/head.json.tmp", "{}\n"));
  TEST_ASSERT_TRUE(c.dirs.count("/mem/tx/web"));
  TEST_ASSERT_TRUE(f.append("/mem/tx/123/b/7.txb", "de", 2));
  TEST_ASSERT_EQUAL_STRING("abcde", c.files["/mem/tx/123/b/7.txb"].c_str());   // append only adds at the end
  TEST_ASSERT_EQUAL(0, c.depth);
}

static void test_refusals_write_nothing_and_are_counted() {
  FakeCard c;
  tx::CardTxFiles f(c);
  c.card = false;
  TEST_ASSERT_FALSE(f.append("/mem/tx/1/seg-000001.jsonl", "x", 1));
  TEST_ASSERT_FALSE(f.writeWhole("/mem/tx/1/head.json", "x"));
  size_t n = 0, got = 0;
  char buf[4];
  TEST_ASSERT_FALSE(f.size("/mem/tx/1/head.json", n));
  TEST_ASSERT_FALSE(f.readAt("/mem/tx/1/head.json", 0, buf, 4, got));
  std::vector<std::string> names;
  TEST_ASSERT_FALSE(f.list("/mem/tx/1", names));   // no card: Io, the log runs from RAM
  c.card = true;
  c.task = false;
  TEST_ASSERT_FALSE(f.append("/mem/tx/1/seg-000001.jsonl", "x", 1));
  c.task = true;
  c.largest = tx::kCardWriteInternalFloor - 1;
  TEST_ASSERT_FALSE(f.append("/mem/tx/1/seg-000001.jsonl", "x", 1));
  TEST_ASSERT_FALSE(f.rename("/mem/tx/1/a", "/mem/tx/1/b"));
  TEST_ASSERT_TRUE(c.files.empty());
  TEST_ASSERT_EQUAL(1, (int)c.dirs.size());   // not even a folder
  TEST_ASSERT_EQUAL(2u, f.holds().refusedNoCard);
  TEST_ASSERT_EQUAL(1u, f.holds().refusedTask);
  TEST_ASSERT_EQUAL(2u, f.holds().refusedFloor);
  c.largest = tx::kCardWriteInternalFloor;   // at the floor: allowed
  TEST_ASSERT_TRUE(f.append("/mem/tx/1/seg-000001.jsonl", "x", 1));
}

static void test_rename_replaces_its_target() {
  FakeCard c;
  tx::CardTxFiles f(c);
  TEST_ASSERT_TRUE(f.writeWhole("/mem/tx/1/head.json", "old"));
  TEST_ASSERT_TRUE(f.writeWhole("/mem/tx/1/head.json.tmp", "new"));
  TEST_ASSERT_TRUE(f.rename("/mem/tx/1/head.json.tmp", "/mem/tx/1/head.json"));
  TEST_ASSERT_EQUAL_STRING("new", c.files["/mem/tx/1/head.json"].c_str());
  TEST_ASSERT_EQUAL(0, (int)c.files.count("/mem/tx/1/head.json.tmp"));
  TEST_ASSERT_FALSE(f.rename("/mem/tx/1/missing", "/mem/tx/1/head.json"));
  TEST_ASSERT_EQUAL_STRING("new", c.files["/mem/tx/1/head.json"].c_str());   // a missing source removes nothing
}

static void test_list_is_bare_names_and_a_missing_folder_is_empty() {
  FakeCard c;
  tx::CardTxFiles f(c);
  std::vector<std::string> names{"stale"};
  TEST_ASSERT_TRUE(f.list("/mem/tx/9", names));
  TEST_ASSERT_TRUE(names.empty());
  TEST_ASSERT_TRUE(f.append("/mem/tx/9/seg-000001.jsonl", "a\n", 2));
  TEST_ASSERT_TRUE(f.append("/mem/tx/9/b/1.txb", "b", 1));
  TEST_ASSERT_TRUE(f.list("/mem/tx/9", names));
  TEST_ASSERT_EQUAL(1, (int)names.size());
  TEST_ASSERT_EQUAL_STRING("seg-000001.jsonl", names[0].c_str());
  TEST_ASSERT_TRUE(f.list("/mem/tx/9/b", names));
  TEST_ASSERT_EQUAL_STRING("1.txb", names[0].c_str());
}

static void test_nothing_outside_the_root() {
  FakeCard c;
  tx::CardTxFiles f(c);
  for (const char* p : {"/mem/fx/a", "/data/x", "/mem/tx", "/mem/tx/", "/mem/tx/1/../../x", "/mem/tx//x", "mem/tx/1/a"}) {
    TEST_ASSERT_FALSE_MESSAGE(f.append(p, "x", 1), p);
    TEST_ASSERT_FALSE_MESSAGE(f.writeWhole(p, "x"), p);
  }
  TEST_ASSERT_FALSE(f.rename("/mem/tx/1/a", "/data/a"));
  std::vector<std::string> names;
  TEST_ASSERT_FALSE(f.list("/mem/fx", names));
  TEST_ASSERT_TRUE(c.files.empty());
  TEST_ASSERT_EQUAL(0, c.calls);
}

static void test_each_call_holds_the_lock_once_and_the_longest_hold_is_kept() {
  FakeCard c;
  tx::CardTxFiles f(c);
  TEST_ASSERT_TRUE(f.append("/mem/tx/1/seg-000001.jsonl", "abc", 3));   // 4 folders checked, 3 made, 1 append
  TEST_ASSERT_EQUAL(1, c.maxDepth);
  TEST_ASSERT_EQUAL(0, c.depth);
  TEST_ASSERT_TRUE(f.holds().appendMs >= 3u * c.stepMs);
  const uint32_t first = f.holds().appendMs;
  TEST_ASSERT_TRUE(f.append("/mem/tx/1/seg-000001.jsonl", "d", 1));   // folders exist: a shorter hold
  TEST_ASSERT_EQUAL(first, f.holds().appendMs);
  TEST_ASSERT_EQUAL(2u, f.holds().appends);
  f.resetHolds();
  TEST_ASSERT_EQUAL(0u, f.holds().appendMs);
}

// The restart the bench proves, on the fake: what the log stored comes back after a reopen.
static void test_a_log_over_the_card_reopens_with_its_items() {
  FakeCard c;
  tx::CardTxFiles f(c);
  tx::HeadState head;
  {
    tx::TxLog log(f, "1001");
    TEST_ASSERT_TRUE(log.open(head).ok());   // a card with no folder yet: an empty chat
    tx::Item u;
    u.q = head.nextQ;
    u.kind = tx::ItemKind::User;
    u.turn = 1;
    u.t = 1794920520;
    u.x = "the cat is called Biscuit";
    tx::StoreState st = tx::StoreState::NotStored;
    TEST_ASSERT_TRUE(log.append(u, st).ok());
    TEST_ASSERT_TRUE(st == tx::StoreState::Stored);
  }
  tx::TxLog again(f, "1001");
  tx::HeadState h2;
  TEST_ASSERT_TRUE(again.open(h2).ok());
  tx::TxArena arena;
  TEST_ASSERT_TRUE(arena.init(64 * 1024, [](size_t n) { return std::malloc(n); }, [](void* q) { std::free(q); }).ok());
  TEST_ASSERT_TRUE(again.fill(h2, arena).ok());
  TEST_ASSERT_EQUAL(1, (int)arena.count());
  tx::Item back;
  TEST_ASSERT_TRUE(arena.get(0, back).ok());
  TEST_ASSERT_EQUAL_STRING("the cat is called Biscuit", back.x.c_str());
}

// Lane E2-STORE, finding S1: an exists() that fails once on a chat folder that is there is
// "cannot tell" (false), not "no files"; the parent's listing is the evidence.
static void test_a_failed_exists_is_cannot_tell_never_empty() {
  FakeCard c;
  tx::CardTxFiles f(c);
  TEST_ASSERT_TRUE(f.append("/mem/tx/9/seg-000001.jsonl", "a\n", 2));
  std::vector<std::string> names{"stale"};
  c.failExists = 1;
  TEST_ASSERT_FALSE(f.list("/mem/tx/9", names));
  TEST_ASSERT_TRUE(names.empty());
  TEST_ASSERT_TRUE(f.list("/mem/tx/9", names));   // the next call reads it whole
  TEST_ASSERT_EQUAL(1, (int)names.size());
  // The parent cannot be listed either: cannot tell, at every level.
  c.failExists = 1000;
  c.failListNames = true;
  TEST_ASSERT_FALSE(f.list("/mem/tx/9", names));
  TEST_ASSERT_FALSE(f.list("/mem/tx/new", names));
  c.failExists = 0;
  c.failListNames = false;
  TEST_ASSERT_EQUAL(0, c.depth);
}

// A folder that is truly absent: its parent lists without it (or the parent is absent the same
// way, up to the card's root).
static void test_an_absent_folder_needs_its_parent_listed() {
  FakeCard c;
  tx::CardTxFiles f(c);
  std::vector<std::string> names;
  TEST_ASSERT_TRUE(f.list("/mem/tx/1", names));   // an empty card: /, then /mem absent
  TEST_ASSERT_TRUE(names.empty());
  TEST_ASSERT_TRUE(f.append("/mem/tx/1/seg-000001.jsonl", "a\n", 2));
  TEST_ASSERT_TRUE(f.list("/mem/tx/2", names));   // /mem/tx lists without "2"
  TEST_ASSERT_TRUE(names.empty());
  c.failListNames = true;
  TEST_ASSERT_FALSE(f.list("/mem/tx/2", names));   // no listing: no proof
  c.failListNames = false;
  c.files["/mem/tx/3"] = "x";   // a file where the folder should be: not a folder, not absent
  TEST_ASSERT_FALSE(f.list("/mem/tx/3", names));
}

static tx::Item userItem(uint64_t q, uint32_t turn, const char* text) {
  tx::Item u;
  u.q = q;
  u.kind = tx::ItemKind::User;
  u.turn = turn;
  u.t = 1794920520;
  u.x = text;
  return u;
}

// The S1 path end to end: a stored conversation, then one failed exists() at open. open() is
// Io, nothing is written anywhere (no line, no head file), and the next open gives it all back.
static void test_a_failed_exists_at_open_writes_nothing_and_the_next_open_has_it_all() {
  FakeCard c;
  tx::CardTxFiles f(c);
  {
    tx::TxLog log(f, "1001");
    tx::HeadState h;
    TEST_ASSERT_TRUE(log.open(h).ok());
    for (uint64_t q = 1; q <= 3; ++q) {
      tx::StoreState st = tx::StoreState::NotStored;
      TEST_ASSERT_TRUE(log.append(userItem(q, (uint32_t)q, "kept"), st).ok());
      TEST_ASSERT_TRUE(st == tx::StoreState::Stored);
    }
    TEST_ASSERT_TRUE(log.writeHead(h).ok());
  }
  const std::map<std::string, std::string> before = c.files;
  tx::TxLog log(f, "1001");
  tx::HeadState h;
  c.failExists = 1;
  const tx::Status st = log.open(h);
  TEST_ASSERT_TRUE(st.err == tx::Err::Io);
  TEST_ASSERT_FALSE(log.isOpen());
  tx::StoreState state = tx::StoreState::Stored;
  (void)log.append(userItem(1, 1, "lost?"), state);
  TEST_ASSERT_TRUE(state == tx::StoreState::NotStored);
  TEST_ASSERT_FALSE(log.writeHead(h).ok());
  TEST_ASSERT_TRUE(c.files == before);   // not one byte anywhere
  TEST_ASSERT_TRUE(log.open(h).ok());
  TEST_ASSERT_EQUAL(4, (int)h.nextQ);
  tx::TxArena arena;
  TEST_ASSERT_TRUE(arena.init(64 * 1024, [](size_t n) { return std::malloc(n); }, [](void* q) { std::free(q); }).ok());
  TEST_ASSERT_TRUE(log.fill(h, arena).ok());
  TEST_ASSERT_EQUAL(3, (int)arena.count());
}

// A store whose listing comes back empty although files are there (the S1 shape from any
// binding): open() still refuses the fresh chat, because a head file or the first segment reads.
class BlindList : public tx::TxFiles {
 public:
  explicit BlindList(tx::TxFiles& f) : f_(f) {}
  bool append(const std::string& p, const char* d, size_t n) override { return f_.append(p, d, n); }
  bool readAt(const std::string& p, size_t o, char* b, size_t w, size_t& g) override { return f_.readAt(p, o, b, w, g); }
  bool size(const std::string& p, size_t& out) override { return f_.size(p, out); }
  bool writeWhole(const std::string& p, const std::string& d) override { return f_.writeWhole(p, d); }
  bool rename(const std::string& a, const std::string& b) override { return f_.rename(a, b); }
  bool remove(const std::string& p) override { return f_.remove(p); }
  bool list(const std::string&, std::vector<std::string>& names) override {
    names.clear();
    return true;
  }

 private:
  tx::TxFiles& f_;
};

static void storeThree(tx::TxFiles& f, bool head) {
  tx::TxLog log(f, "1001");
  tx::HeadState h;
  TEST_ASSERT_TRUE(log.open(h).ok());
  for (uint64_t q = 1; q <= 3; ++q) {
    tx::StoreState st = tx::StoreState::NotStored;
    TEST_ASSERT_TRUE(log.append(userItem(q, (uint32_t)q, "kept"), st).ok());
  }
  if (head) TEST_ASSERT_TRUE(log.writeHead(h).ok());
}

static void test_open_refuses_a_fresh_chat_while_stored_state_reads() {
  for (int withHead = 0; withHead < 2; ++withHead) {
    FakeCard c;
    tx::CardTxFiles f(c);
    storeThree(f, withHead != 0);
    if (!withHead) TEST_ASSERT_EQUAL(0, (int)c.files.count("/mem/tx/1001/head.json"));
    BlindList blind(f);
    tx::TxLog log(blind, "1001");
    tx::HeadState h;
    const std::map<std::string, std::string> before = c.files;
    TEST_ASSERT_TRUE(log.open(h).err == tx::Err::Io);
    TEST_ASSERT_FALSE(log.isOpen());
    TEST_ASSERT_TRUE(c.files == before);
  }
  // A head file that names nothing stored (written before the first line) is still a fresh chat.
  FakeCard c;
  tx::CardTxFiles f(c);
  {
    tx::TxLog log(f, "1001");
    tx::HeadState h;
    TEST_ASSERT_TRUE(log.open(h).ok());
    h.pinned.push_back("pinned before any line");
    TEST_ASSERT_TRUE(log.writeHead(h).ok());
  }
  tx::TxLog log(f, "1001");
  tx::HeadState h;
  TEST_ASSERT_TRUE(log.open(h).ok());
}

// Defect S1 of an earlier build appended a session's lines with restarted numbers to the end of
// the first segment. No device ran it, but the reader must not trip on such a file: the first
// line of each q wins, the rest are skipped.
static void test_a_segment_with_restarted_numbers_reads_without_a_fault() {
  FakeCard c;
  tx::CardTxFiles f(c);
  storeThree(f, true);
  std::string line;   // what the old fresh branch wrote: lines q 1 and 2 again, at the end
  for (uint64_t q = 1; q <= 2; ++q) {
    TEST_ASSERT_TRUE(tx::encodeLine(userItem(q, (uint32_t)q, "restarted"), line).ok());
    TEST_ASSERT_TRUE(f.append("/mem/tx/1001/seg-000001.jsonl", line.data(), line.size()));
  }
  tx::TxLog log(f, "1001");
  tx::HeadState h;
  TEST_ASSERT_TRUE(log.open(h).ok());
  tx::TxArena arena;
  TEST_ASSERT_TRUE(arena.init(64 * 1024, [](size_t n) { return std::malloc(n); }, [](void* q) { std::free(q); }).ok());
  TEST_ASSERT_TRUE(log.fill(h, arena).ok());
  TEST_ASSERT_EQUAL(3, (int)arena.count());
  tx::Item first;
  TEST_ASSERT_TRUE(arena.get(0, first).ok());
  TEST_ASSERT_EQUAL_STRING("kept", first.x.c_str());
  TEST_ASSERT_TRUE(h.nextQ >= 4);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_write_creates_every_missing_folder);
  RUN_TEST(test_refusals_write_nothing_and_are_counted);
  RUN_TEST(test_rename_replaces_its_target);
  RUN_TEST(test_list_is_bare_names_and_a_missing_folder_is_empty);
  RUN_TEST(test_nothing_outside_the_root);
  RUN_TEST(test_each_call_holds_the_lock_once_and_the_longest_hold_is_kept);
  RUN_TEST(test_a_log_over_the_card_reopens_with_its_items);
  RUN_TEST(test_a_failed_exists_is_cannot_tell_never_empty);
  RUN_TEST(test_an_absent_folder_needs_its_parent_listed);
  RUN_TEST(test_a_failed_exists_at_open_writes_nothing_and_the_next_open_has_it_all);
  RUN_TEST(test_open_refuses_a_fresh_chat_while_stored_state_reads);
  RUN_TEST(test_a_segment_with_restarted_numbers_reads_without_a_fault);
  return UNITY_END();
}
