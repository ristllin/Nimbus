#include <unity.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include "nimbus/orch/file_aids.h"
#include "nimbus/orch/file_append.h"
#include "nimbus/orch/file_swap.h"

// test_file_cut (lane RB-REC7, ruling 87): a write torn at EVERY byte offset of the block it was writing (and,
// for the swap, at every card operation; a torn note or a half-done rename is not modelled here), the
// host half of the bench's cut legs (tests/hil/test_files_bench.py; the bench itself cannot switch USB power).
//   - files.append: the file is the old one plus a prefix of the appended bytes. Whatever the prefix, the rows
//     before it are byte-identical, a read names a torn tail, and the next append fences it (a line end before
//     its text), so no row is ever glued to another.
//   - files.edit: the new file is streamed into a part file and swapped in (file_swap.h). A cut at any byte of the
//     part, or at any card operation of the swap, leaves after the next boot the old file or the new one, byte for
//     byte, never a mix.

using namespace nimbus::orch;
namespace aids = nimbus::orch::aids;

void setUp() {}
void tearDown() {}

namespace {

const std::string kOld =
    "date,category,note,amount,paid_with\n"
    "2026-11-18,coffee,kiosk espresso,2.40,card\n"
    "2026-11-18,groceries,weekly shop,48.37,card\n"
    "2026-11-18,transport,train to Harwick,17.90,card\n";
const std::string kRows = "2026-11-19,coffee,oat cappuccino,3.10,card\n2026-11-19,bus,day ticket,4.10,cash\n";
const std::string kNext = "2026-11-19,lunch,soup,5.60,card\n";

const SwapPaths kPaths{"/mem/files/.part", "/mem/files/.bak", "/mem/files/.swapnote"};
const std::string kDst = "/mem/files/money/spending.csv";

// A card for the edit: the part file takes bytes until the power goes (cutByte), and every swap operation counts
// toward cutOp; after the cut nothing lands. reboot() is the next boot: the swap recovery, then the device drops a
// stale part (files_subsystem begin()).
struct CutCard : aids::AidSource, aids::AidSink, SwapFs {
  std::map<std::string, std::string> files;
  long cutByte = -1, cutOp = -1, ops = 0;
  bool cut = false;

  // AidSource: the stored file.
  bool readAt(uint64_t off, size_t len, char* out, size_t& got) override {
    const std::string& f = files[kDst];
    got = off >= f.size() ? 0 : std::min(len, size_t(f.size() - off));
    std::memcpy(out, f.data() + off, got);
    return true;
  }
  // AidSink: the part file, then the swap.
  bool partWrite(std::string_view b, bool first) override {
    if (cut) return false;
    std::string& part = files[kPaths.part];
    if (first) part.clear();
    for (char c : b) {
      if (cutByte >= 0 && long(part.size()) >= cutByte) {
        cut = true;
        return false;
      }
      part.push_back(c);
    }
    return true;
  }
  bool partSwap() override {
    std::string err;
    return !cut && swapIn(*this, kPaths, kDst, err) && (swapDone(*this, kPaths), true);
  }
  void partDiscard() override {
    if (!cut) files.erase(kPaths.part);
  }
  // SwapFs.
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
  std::string readText(const std::string& p) override { return files.count(p) ? files[p] : std::string(); }
  long long size(const std::string& p) override { return files.count(p) ? (long long)files[p].size() : -1; }
  void reboot() {
    cut = false;
    cutByte = cutOp = -1;
    recoverSwap(*this, kPaths);
    files.erase(kPaths.part);
  }

 private:
  bool step() {
    if (cut) return false;
    if (ops++ == cutOp) cut = true;
    return !cut;
  }
};

aids::EditRequest fixRequest() {
  aids::EditRequest e;
  e.oldText = "2026-11-18,groceries,weekly shop,48.37,card";
  e.newText = "2026-11-18,groceries,weekly shop,51.80,card";
  return e;
}

const std::string kNew = [] {
  std::string s = kOld;
  s.replace(s.find("48.37"), 5, "51.80");
  return s;
}();

void edit(CutCard& c) {
  c.files[kDst] = kOld;
  aids::editFile(c, c, kOld.size(), aids::fnv64(aids::kFnv64Basis, kOld), kDst, fixRequest());
}

}  // namespace

// The append, torn at every byte offset of the appended block: the old rows are intact, the tail is named torn
// unless the cut fell on a line end, and the next append fences it.
static void test_an_append_torn_at_every_offset_is_fenced_by_the_next() {
  const AppendPlan first = planAppend(kRows, kOld.size(), kOld.back());
  TEST_ASSERT_TRUE(first.ok);
  for (size_t k = 0; k <= first.bytes.size(); k++) {
    const std::string torn = kOld + first.bytes.substr(0, k);
    const std::string where = "cut after " + std::to_string(k) + " of " + std::to_string(first.bytes.size()) + " B";
    TEST_ASSERT_TRUE_MESSAGE(torn.compare(0, kOld.size(), kOld) == 0, where.c_str());
    const bool atLineEnd = torn.back() == '\n';
    TEST_ASSERT_EQUAL_MESSAGE(!atLineEnd, aids::endsTorn(torn.size(), torn.back()), where.c_str());
    const AppendPlan next = planAppend(kNext, torn.size(), torn.back());
    TEST_ASSERT_TRUE_MESSAGE(next.ok, where.c_str());
    const std::string after = torn + next.bytes;
    TEST_ASSERT_EQUAL_STRING_MESSAGE((torn + (atLineEnd ? "" : "\n") + kNext).c_str(), after.c_str(), where.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(!atLineEnd, next.repairedCut, where.c_str());
  }
}

// The same with the shared aids' append facts (engine 2 and nimbusd): the fence and its note.
static void test_the_append_facts_fence_every_torn_offset() {
  for (size_t k = 1; k < kRows.size(); k++) {
    const std::string torn = kOld + kRows.substr(0, k);
    if (torn.back() == '\n') continue;
    const aids::AppendFacts f = aids::appendFacts(torn, true, torn.size(), 0, kNext);
    const std::string where = "cut after " + std::to_string(k) + " B";
    TEST_ASSERT_TRUE_MESSAGE(f.fence, where.c_str());
    TEST_ASSERT_TRUE_MESSAGE(f.note.find("newline") != std::string::npos, where.c_str());
  }
}

// The edit, cut at every byte of its part file: after the next boot the stored file is the old one.
static void test_an_edit_cut_at_every_byte_of_its_part_leaves_the_old_file() {
  CutCard probe;
  edit(probe);
  TEST_ASSERT_EQUAL_STRING(kNew.c_str(), probe.files[kDst].c_str());
  for (long k = 0; k <= long(kNew.size()); k++) {
    CutCard c;
    c.cutByte = k;
    edit(c);
    c.reboot();
    const std::string& got = c.files[kDst];
    const std::string where = "part cut at byte " + std::to_string(k);
    TEST_ASSERT_TRUE_MESSAGE(got == kOld || got == kNew, where.c_str());
    if (k < long(kNew.size())) TEST_ASSERT_EQUAL_STRING_MESSAGE(kOld.c_str(), got.c_str(), where.c_str());
    TEST_ASSERT_FALSE_MESSAGE(c.files.count(kPaths.part) > 0, where.c_str());
  }
}

// The edit, cut at every card operation of its swap: old or new, whole, never gone, never a mix.
static void test_an_edit_cut_at_every_swap_operation_leaves_old_or_new() {
  CutCard probe;
  edit(probe);
  const long n = probe.ops;
  TEST_ASSERT_TRUE(n > 0);
  for (long k = 0; k <= n; k++) {
    CutCard c;
    c.cutOp = k;
    edit(c);
    c.reboot();
    const std::string where = "swap cut at op " + std::to_string(k);
    TEST_ASSERT_TRUE_MESSAGE(c.files.count(kDst) > 0, where.c_str());
    const std::string& got = c.files[kDst];
    TEST_ASSERT_TRUE_MESSAGE(got == kOld || got == kNew, where.c_str());
  }
}

// Counter-test: a card that rewrites the stored file IN PLACE (no part file, no swap), run through the same edit
// and the same byte sweep, leaves a mix that is neither the old nor the new file. The class rule the sweeps above
// assert is one a regression to an in-place write would break.
struct InPlaceCard : CutCard {
  size_t at = 0;
  bool partWrite(std::string_view b, bool first) override {
    if (cut) return false;
    std::string& f = files[kDst];
    if (first) at = 0;
    for (char c : b) {
      if (cutByte >= 0 && long(at) >= cutByte) {
        cut = true;
        return false;
      }
      if (at < f.size()) f[at] = c; else f.push_back(c);
      at++;
    }
    return true;
  }
  bool partSwap() override { return !cut; }
  void partDiscard() override {}
};

static void test_counter_an_in_place_rewrite_leaves_a_mix() {
  int mixes = 0;
  for (long k = 0; k <= long(kNew.size()); k++) {
    InPlaceCard c;
    c.cutByte = k;
    edit(c);
    c.reboot();
    const std::string& got = c.files[kDst];
    mixes += got != kOld && got != kNew;
  }
  TEST_ASSERT_TRUE(mixes > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_append_torn_at_every_offset_is_fenced_by_the_next);
  RUN_TEST(test_the_append_facts_fence_every_torn_offset);
  RUN_TEST(test_an_edit_cut_at_every_byte_of_its_part_leaves_the_old_file);
  RUN_TEST(test_an_edit_cut_at_every_swap_operation_leaves_old_or_new);
  RUN_TEST(test_counter_an_in_place_rewrite_leaves_a_mix);
  return UNITY_END();
}
