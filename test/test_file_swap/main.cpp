#include <unity.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/file_swap.h"

// file_swap (file_swap.h): replacing a whole file on a card whose rename cannot
// overwrite (FAT). The class rule: whatever single operation fails, and wherever
// the power is cut, the file is never gone - after the call (and, for a power
// cut, after the next boot's recovery) it holds either the old copy or the new
// one, byte for byte.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

const SwapPaths kPaths{"/mem/files/.part", "/mem/files/.bak", "/mem/files/.swapnote"};
const std::string kDst = "/mem/files/owner/food-log.csv";
const std::string kOld = "date,meal,kcal\n2026-10-24,dinner,990\n";
const std::string kNew = "date,meal,kcal\n2026-10-24,dinner,990\n2026-10-25,breakfast,945\n";

// An in-RAM card. Like FAT, a rename onto an existing name fails. Every write
// operation (rename, remove, write) counts; `failAt` makes that one fail, and
// `cutAt` cuts the power there: it and every later write does nothing.
class FakeFs : public SwapFs {
 public:
  std::map<std::string, std::string> files;
  int ops = 0, failAt = -1, cutAt = -1;
  std::vector<int> fails;   // more write operations that fail
  bool cut = false;

  bool exists(const std::string& p) override { return files.count(p) > 0; }
  bool rename(const std::string& a, const std::string& b) override {
    if (!step() || !files.count(a) || files.count(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) override { return step() && files.erase(p) > 0; }
  bool writeText(const std::string& p, const std::string& t) override {
    if (!step()) return false;
    files[p] = t;
    return true;
  }
  std::string readText(const std::string& p) override {
    auto it = files.find(p);
    return it == files.end() ? std::string() : it->second;
  }
  long long size(const std::string& p) override {
    auto it = files.find(p);
    return it == files.end() ? -1 : (long long)it->second.size();
  }
  // The next boot: power is back, no fault armed; the recovery runs, then the
  // device drops a stale part (files_subsystem begin()).
  void reboot() {
    cut = false;
    failAt = cutAt = -1;
    recoverSwap(*this, kPaths);
    files.erase(kPaths.part);
  }

 private:
  bool step() {
    if (cut) return false;
    const int i = ops++;
    if (i == cutAt) cut = true;
    return !cut && i != failAt && std::find(fails.begin(), fails.end(), i) == fails.end();
  }
};

FakeFs seeded(bool withOld) {
  FakeFs fs;
  if (withOld) fs.files[kDst] = kOld;
  fs.files[kPaths.part] = kNew;
  return fs;
}

// The device's commit: swap in, the index accepts (or refuses), then done (or undo).
bool commit(FakeFs& fs, bool indexAccepts, std::string& err) {
  if (!swapIn(fs, kPaths, kDst, err)) return false;
  if (!indexAccepts) {
    swapUndo(fs, kPaths, kDst);
    return false;
  }
  swapDone(fs, kPaths);
  return true;
}

// How many write operations a full commit takes (the cut/fault sweep's range).
int opsOfCommit(bool withOld, bool indexAccepts) {
  FakeFs fs = seeded(withOld);
  std::string err;
  commit(fs, indexAccepts, err);
  return fs.ops;
}

}  // namespace

static void test_swap_replaces_and_leaves_nothing_behind() {
  // The index refused the new copy: the old one is back, nothing else is left.
  {
    FakeFs fs = seeded(true);
    std::string err;
    TEST_ASSERT_FALSE(commit(fs, false, err));
    TEST_ASSERT_EQUAL_STRING(kOld.c_str(), fs.files[kDst].c_str());
    TEST_ASSERT_EQUAL(1, (int)fs.files.size());
    FakeFs fresh = seeded(false);
    TEST_ASSERT_FALSE(commit(fresh, false, err));
    TEST_ASSERT_TRUE(fresh.files.empty());   // a refused new file leaves nothing behind
  }
  FakeFs fs = seeded(true);
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(commit(fs, true, err), err.c_str());
  TEST_ASSERT_EQUAL_STRING(kNew.c_str(), fs.files[kDst].c_str());
  TEST_ASSERT_EQUAL(1, (int)fs.files.size());   // no part, no backup, no note
  // A new file (nothing stored yet).
  FakeFs fresh = seeded(false);
  TEST_ASSERT_TRUE(commit(fresh, true, err));
  TEST_ASSERT_EQUAL_STRING(kNew.c_str(), fresh.files[kDst].c_str());
  TEST_ASSERT_EQUAL(1, (int)fresh.files.size());
}

// Any ONE failing operation: the call either completes (new copy in place) or
// reports the failure with the old copy in place - after the next boot at worst.
static void test_any_single_failure_keeps_the_old_copy() {
  for (bool withOld : {true, false}) {
    const int n = opsOfCommit(withOld, true);
    for (int k = 0; k < n; k++) {
      FakeFs fs = seeded(withOld);
      fs.failAt = k;
      std::string err;
      const bool ok = commit(fs, true, err);
      fs.reboot();
      const std::string where = "fail at op " + std::to_string(k) + (withOld ? " (replace)" : " (new)");
      if (ok) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(kNew.c_str(), fs.files[kDst].c_str(), where.c_str());
      } else if (withOld) {
        TEST_ASSERT_FALSE_MESSAGE(err.empty(), where.c_str());
        TEST_ASSERT_TRUE_MESSAGE(fs.files.count(kDst) > 0, where.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(kOld.c_str(), fs.files[kDst].c_str(), where.c_str());
      } else {
        TEST_ASSERT_TRUE_MESSAGE(fs.files.count(kDst) == 0 || fs.files[kDst] == kNew, where.c_str());
      }
      TEST_ASSERT_TRUE_MESSAGE(fs.files.count(kPaths.note) == 0, where.c_str());
    }
  }
}

// The power cut at every write operation of the commit (and the index-refused
// undo path): after the next boot the file holds the old or the new copy, and is
// never missing.
static void test_power_cut_anywhere_never_loses_the_file() {
  for (bool accepts : {true, false}) {
    const int n = opsOfCommit(true, accepts);
    for (int k = 0; k <= n; k++) {
      FakeFs fs = seeded(true);
      fs.cutAt = k;
      std::string err;
      commit(fs, accepts, err);
      fs.reboot();
      const std::string where = "cut at op " + std::to_string(k) + (accepts ? "" : " (undo)");
      TEST_ASSERT_TRUE_MESSAGE(fs.files.count(kDst) > 0, where.c_str());
      const std::string& got = fs.files[kDst];
      // (A cut after the new copy landed but before the index answered keeps the
      // new copy: nothing was lost, and the index heals at the next full write.)
      TEST_ASSERT_TRUE_MESSAGE(got == kOld || got == kNew, where.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)fs.files.size(), where.c_str());
    }
  }
}

// Counter-test: the old commit (remove the stored file, then rename) loses the
// file when the power is cut between the two, or the rename fails. The sweep
// above would catch a return to it.
static void test_counter_old_remove_then_rename_loses_the_file() {
  FakeFs fs = seeded(true);
  fs.failAt = 1;   // the rename
  fs.remove(kDst);
  const bool renamed = fs.rename(kPaths.part, kDst);
  TEST_ASSERT_FALSE(renamed);
  TEST_ASSERT_TRUE(fs.files.count(kDst) == 0);
}

// A stale backup from an earlier finished swap never blocks or corrupts the next.
static void test_stale_backup_and_note_are_cleared() {
  FakeFs fs = seeded(true);
  fs.files[kPaths.bak] = "stale";
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(commit(fs, true, err), err.c_str());
  TEST_ASSERT_EQUAL_STRING(kNew.c_str(), fs.files[kDst].c_str());
  TEST_ASSERT_TRUE(fs.files.count(kPaths.bak) == 0);
  // A stale backup and a NEW file the index then refuses: the undo never moves
  // the stale backup into the new name.
  FakeFs n = seeded(false);
  n.files[kPaths.bak] = "stale";
  TEST_ASSERT_FALSE(commit(n, false, err));
  TEST_ASSERT_TRUE(n.files.empty());
  // A boot with a backup but no note drops the backup and leaves the file alone.
  FakeFs b;
  b.files[kDst] = kOld;
  b.files[kPaths.bak] = "stale";
  TEST_ASSERT_EQUAL_STRING("", recoverSwap(b, kPaths).c_str());
  TEST_ASSERT_TRUE(b.files.count(kPaths.bak) == 0);
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), b.files[kDst].c_str());
  // A boot after a cut between the two renames restores the old copy and says so.
  FakeFs c;
  c.files[kPaths.bak] = kOld;
  c.files[kPaths.note] = kDst;
  TEST_ASSERT_TRUE(recoverSwap(c, kPaths).find("restored " + kDst) == 0);
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), c.files[kDst].c_str());
  TEST_ASSERT_EQUAL(1, (int)c.files.size());
}

// A double fault (review): the new copy cannot be renamed in AND the old one
// cannot be put back, so it waits in the backup for the next boot. A second swap
// before that boot must not destroy it: it finishes the first one, or refuses.
static void test_a_pending_restore_is_never_overwritten() {
  FakeFs fs = seeded(true);
  // ops: 0 remove the stale backup, 1 write the note, 2 dst -> bak, 3 part -> dst
  // (fails), 4 remove the part, 5 bak -> dst (fails): the note stays.
  fs.fails = {3, 5};
  std::string err;
  TEST_ASSERT_FALSE(swapIn(fs, kPaths, kDst, err));
  TEST_ASSERT_TRUE_MESSAGE(err.find("restored at the next restart") != std::string::npos, err.c_str());
  TEST_ASSERT_TRUE(fs.files.count(kDst) == 0);   // the old copy waits in the backup
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), fs.files[kPaths.bak].c_str());
  // A second, healthy swap of ANOTHER file finishes the pending one first.
  fs.fails.clear();
  const std::string other = "/mem/files/owner/ledger.csv";
  fs.files[other] = "old ledger";
  fs.files[kPaths.part] = "new ledger";
  TEST_ASSERT_TRUE_MESSAGE(swapIn(fs, kPaths, other, err), err.c_str());
  swapDone(fs, kPaths);
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), fs.files[kDst].c_str());   // the food log is back
  TEST_ASSERT_EQUAL_STRING("new ledger", fs.files[other].c_str());
  TEST_ASSERT_TRUE(fs.files.count(kPaths.note) == 0 && fs.files.count(kPaths.bak) == 0);
  // While the pending restore still cannot be done, a new swap refuses.
  FakeFs g;
  g.files[kPaths.bak] = kOld;
  g.files[kPaths.note] = kDst;
  g.files[kPaths.part] = "new ledger";
  g.files[other] = "old ledger";
  g.fails = {0};   // the recovery's rename fails
  TEST_ASSERT_FALSE(swapIn(g, kPaths, other, err));
  TEST_ASSERT_TRUE(err.find("not finished") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), g.files[kPaths.bak].c_str());   // still safe in the backup
  TEST_ASSERT_EQUAL_STRING("old ledger", g.files[other].c_str());
}

// MUST (merge review): the exact double-fault sequence. The new copy cannot be
// renamed in AND the old one cannot be put back: the food log's only copy is the
// backup. While that is so, every write path to the log refuses (swapPendingFor);
// once the card answers again the backup is restored first. And should a stray
// small file appear at the log's name anyway, the next boot keeps the larger
// backup as <name>.recovered instead of deleting it.
static void test_double_fault_never_loses_the_log() {
  FakeFs fs = seeded(true);
  fs.fails = {3, 5};   // part -> dst fails, then bak -> dst fails
  std::string err;
  TEST_ASSERT_FALSE(swapIn(fs, kPaths, kDst, err));
  TEST_ASSERT_TRUE(fs.files.count(kDst) == 0);
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), fs.files[kPaths.bak].c_str());
  // The card still refuses renames: a plain append (or an upload, or a keyed
  // write) to the log is refused - it would recreate the log over its backup.
  fs.fails = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  TEST_ASSERT_TRUE(swapPendingFor(fs, kPaths, kDst));
  TEST_ASSERT_FALSE(swapPendingFor(fs, kPaths, "/mem/files/owner/other.csv"));   // other files carry on
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), fs.files[kPaths.bak].c_str());
  // The card answers again: the next write first restores the log, then proceeds.
  fs.fails.clear();
  TEST_ASSERT_FALSE(swapPendingFor(fs, kPaths, kDst));
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), fs.files[kDst].c_str());
  TEST_ASSERT_TRUE(fs.files.count(kPaths.bak) == 0 && fs.files.count(kPaths.note) == 0);
  // The old failure mode, replayed: a stray one-row file at the log's name while
  // the note is pending, then a boot. The larger backup is kept, never deleted.
  FakeFs b;
  b.files[kPaths.note] = kDst;
  b.files[kPaths.bak] = kOld + "2026-10-25,breakfast,945\n";
  b.files[kDst] = "2026-10-25,coffee,66\n";
  std::string aside;
  const std::string did = recoverSwap(b, kPaths, &aside);
  TEST_ASSERT_EQUAL_STRING((kDst + ".recovered").c_str(), aside.c_str());
  TEST_ASSERT_TRUE_MESSAGE(did.find("larger backup is kept as") != std::string::npos, did.c_str());
  TEST_ASSERT_EQUAL_STRING((kOld + "2026-10-25,breakfast,945\n").c_str(), b.files[aside].c_str());
  TEST_ASSERT_EQUAL_STRING("2026-10-25,coffee,66\n", b.files[kDst].c_str());
  TEST_ASSERT_TRUE(b.files.count(kPaths.note) == 0);
  // A backup no larger than the new copy is an ordinary finished swap: dropped.
  FakeFs c;
  c.files[kPaths.note] = kDst;
  c.files[kPaths.bak] = kOld;
  c.files[kDst] = kNew;
  aside.clear();
  recoverSwap(c, kPaths, &aside);
  TEST_ASSERT_TRUE(aside.empty() && c.files.count(kPaths.bak) == 0);
  // A backup that cannot be removed before a swap: refused, nothing changed.
  FakeFs d = seeded(true);
  d.files[kPaths.bak] = "stale";
  d.fails = {0};   // the stale backup's remove fails
  TEST_ASSERT_FALSE(swapIn(d, kPaths, kDst, err));
  TEST_ASSERT_TRUE(err.find("SD remove failed") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING(kOld.c_str(), d.files[kDst].c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_swap_replaces_and_leaves_nothing_behind);
  RUN_TEST(test_any_single_failure_keeps_the_old_copy);
  RUN_TEST(test_power_cut_anywhere_never_loses_the_file);
  RUN_TEST(test_counter_old_remove_then_rename_loses_the_file);
  RUN_TEST(test_stale_backup_and_note_are_cleared);
  RUN_TEST(test_a_pending_restore_is_never_overwritten);
  RUN_TEST(test_double_fault_never_loses_the_log);
  return UNITY_END();
}
