// edit_file with line (file_aids.h): the streamed find and replace and the line pick.
//
// Class rules proven here, over generated files of three equal domains (a food log, an
// expense log, a training log) with repeated identical lines:
//   - a line pick replaces exactly the whole-line match at that line, or fails; it never
//     changes any other row, even one holding old as part of its text, so a stale number
//     (after an earlier removal or an append from elsewhere) fails with the current match
//     lines, or hits a row with the very same text;
//   - every result's line numbers are current: each "at" line of the new file starts with
//     the new text, each "still at" line holds old, the shift names the rows that moved;
//   - the edit agrees with an independent whole-string reference for single, picked and
//     all-matches edits, at every chunking the card gives (old across chunk boundaries);
//   - a card fault at every step leaves the old file or the new one, never a mix, and
//     never a part file behind;
//   - a refused allocation at any point is the no-memory error and changes nothing;
//   - a file that changed between the passes, or since the sha8 the caller read, is not
//     written.
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "../support/fake_aid_card.h"
#include "nimbus/orch/file_aids.h"

using namespace nimbus::orch::aids;
using aidfake::Card;
using aidfake::Rng;

void setUp() {}
void tearDown() { aidfake::disarm(); }

namespace {

std::vector<std::string> splitLines(const std::string& f) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a < f.size()) {
    size_t b = f.find('\n', a);
    if (b == std::string::npos) { out.push_back(f.substr(a)); break; }
    out.push_back(f.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

// 1-based line of byte offset off.
uint32_t lineAt(const std::string& f, size_t off) {
  uint32_t l = 1;
  for (size_t i = 0; i < off && i < f.size(); ++i) l += f[i] == '\n';
  return l;
}

std::vector<size_t> matchesOf(const std::string& f, const std::string& old) {
  std::vector<size_t> out;
  for (size_t p = f.find(old); p != std::string::npos; p = f.find(old, p + old.size())) out.push_back(p);
  return out;
}

bool wholeLineAt(const std::string& f, size_t p, size_t n) {
  if (p > 0 && f[p - 1] != '\n') return false;
  const size_t e = p + n;
  if (n && f[e - 1] == '\n') return true;
  return e == f.size() || f[e] == '\n' || (f[e] == '\r' && e + 1 < f.size() && f[e + 1] == '\n') ||
         (f[e] == '\r' && e + 1 == f.size());
}

// The reference: the new file, or "" with ok=false.
struct RefEdit {
  bool ok = false;
  std::string file;
  size_t replaced = 0;
};
// The matches an edit replaces (empty: the edit is an error).
std::vector<size_t> refPicks(const std::string& f, const EditRequest& e) {
  const auto ms = matchesOf(f, e.oldText);
  if (e.all) return ms;
  if (!e.line) return ms.size() == 1 ? ms : std::vector<size_t>();
  std::vector<size_t> pick;
  for (size_t p : ms)
    if (lineAt(f, p) == e.line && wholeLineAt(f, p, e.oldText.size())) pick.push_back(p);
  return pick.size() == 1 ? pick : std::vector<size_t>();
}
// Where the bytes a replacement at p takes end: old, and for a removal of whole lines
// the line end after it.
size_t refTakenEnd(const std::string& f, const EditRequest& e, size_t p) {
  size_t end = p + e.oldText.size();
  const bool removal = e.newText.empty() && wholeLineAt(f, p, e.oldText.size()) && e.oldText.back() != '\n';
  if (!removal || end >= f.size()) return end;
  if (f.compare(end, 2, "\r\n") == 0) return end + 2;
  return end + (f[end] == '\n' || (f[end] == '\r' && end + 1 == f.size()));
}
RefEdit reference(const std::string& f, const EditRequest& e) {
  RefEdit r;
  const auto pick = refPicks(f, e);
  if (pick.empty()) return r;
  size_t at = 0;
  for (size_t p : pick) {
    r.file += f.substr(at, p - at) + e.newText;
    at = refTakenEnd(f, e, p);
  }
  r.file += f.substr(at);
  r.ok = true;
  r.replaced = pick.size();
  return r;
}

struct Domain {
  const char* header;
  std::function<std::string(Rng&, int day)> row;
};
const std::vector<Domain>& domains() {
  static const std::vector<Domain> d = {
      {"date,time,item,grams,kcal",
       [](Rng& g, int day) {
         return "2026-11-" + std::to_string(10 + day) + ",08:00,oats," + std::to_string(40 + g.below(3)) + ",150";
       }},
      {"date,payee,category,amount",
       [](Rng& g, int day) { return "2026-11-" + std::to_string(10 + day) + ",cafe,misc," + std::to_string(3 + g.below(3)) + ".50"; }},
      {"date,exercise,sets,reps,kg",
       [](Rng& g, int day) { return "2026-11-" + std::to_string(10 + day) + ",squat,3,5," + std::to_string(100 + g.below(3)); }},
  };
  return d;
}
std::string makeFile(Rng& g, const Domain& d, int rows) {
  std::string f = std::string(d.header) + "\n";
  for (int i = 0; i < rows; ++i) f += d.row(g, int(g.below(3))) + (g.chance(5) ? "\r\n" : "\n");
  if (g.chance(20) && !f.empty()) f.pop_back();   // no final newline
  return f;
}

EditOutcome run(Card& c, const EditRequest& e, uint64_t hash = 0) {
  return editFile(c, c, c.file.size(), hash, "p/t.csv", e);
}

// Every line number a successful result names is true of the new file.
void checkLinesAreCurrent(const std::string& before, const EditRequest& e, const EditOutcome& o, const std::string& after) {
  TEST_ASSERT_EQUAL_UINT64(after.size(), o.newSize);
  TEST_ASSERT_EQUAL_UINT64(before.size(), o.oldSize);
  TEST_ASSERT_EQUAL_UINT64(splitLines(after).size(), o.newLines);
  TEST_ASSERT_TRUE(o.newHash == fnv64(kFnv64Basis, after));
  const auto lines = splitLines(after);
  if (!e.newText.empty())
    for (uint32_t l : o.at) {
      TEST_ASSERT_TRUE(l >= 1 && l <= lines.size() + 1);
      // the new text starts somewhere on that line
      const std::string first = e.newText.substr(0, e.newText.find('\n'));
      TEST_ASSERT_TRUE_MESSAGE(l <= lines.size() && lines[l - 1].find(first) != std::string::npos, "at line holds the new text");
    }
  const auto still = matchesOf(after, e.oldText);
  TEST_ASSERT_EQUAL(still.size(), o.stillAt.count);
  for (size_t k = 0; k < o.stillAt.lines.size(); ++k) TEST_ASSERT_EQUAL_UINT32(lineAt(after, still[k]), o.stillAt.lines[k]);
  if (o.shiftFrom) {
    // The row that was at shiftFrom is now at shiftFrom + shift.
    const auto old = splitLines(before);
    TEST_ASSERT_TRUE(o.shiftFrom <= old.size());
    const int64_t now = int64_t(o.shiftFrom) + o.shift;
    TEST_ASSERT_TRUE(now >= 1 && size_t(now) <= lines.size());
    TEST_ASSERT_EQUAL_STRING(old[o.shiftFrom - 1].c_str(), lines[size_t(now - 1)].c_str());
  }
}

}  // namespace

// ---- the property of record: a pick never touches another row -----------------------
static void test_a_line_pick_replaces_exactly_that_line_or_fails() {
  size_t picks = 0, refusals = 0;
  for (uint64_t seed = 1; seed <= 120; ++seed) {
    Rng g(seed * 131);
    const Domain& d = domains()[seed % 3];
    const std::string file = makeFile(g, d, 4 + int(g.below(30)));
    const auto lines = splitLines(file);
    // Pick a row's text (often a repeated one), sometimes only part of it, and a line
    // number that may be right, stale by one, or anywhere.
    const size_t src = 1 + g.below(lines.size() - 1);
    std::string old = lines[src];
    if (!old.empty() && old.back() == '\r') old.pop_back();
    if (g.chance(25)) old = old.substr(old.size() / 2);   // part of a row
    EditRequest e;
    e.oldText = old;
    e.newText = g.chance(40) ? "" : d.row(g, 9);
    e.line = uint32_t(g.chance(50) ? src + 1 : 1 + g.below(lines.size() + 2));
    for (size_t chunk : {size_t(4096)}) {
      Card c;
      c.file = file;
      (void)chunk;
      const auto o = run(c, e);
      const auto ref = reference(file, e);
      TEST_ASSERT_EQUAL_MESSAGE(ref.ok, o.ok, o.error.c_str());
      if (!o.ok) {
        TEST_ASSERT_EQUAL_STRING(file.c_str(), c.file.c_str());
        ++refusals;
        continue;
      }
      ++picks;
      TEST_ASSERT_EQUAL_STRING(ref.file.c_str(), c.file.c_str());
      // Only the picked line changed: every other line is as it was.
      const auto after = splitLines(c.file);
      std::vector<std::string> expect = lines;
      std::string picked = expect[e.line - 1];
      if (!picked.empty() && picked.back() == '\r') picked.pop_back();
      TEST_ASSERT_EQUAL_STRING(old.c_str(), picked.c_str());   // the line held exactly old
      checkLinesAreCurrent(file, e, o, c.file);
    }
  }
  TEST_ASSERT_TRUE(picks >= 30);
  TEST_ASSERT_TRUE(refusals >= 30);
}

static void test_a_stale_number_after_a_removal_fails_with_the_current_lines() {
  for (const Domain& d : domains()) {
    Rng g(3);
    const std::string a = d.row(g, 0), b = d.row(g, 1), x = d.row(g, 2) + "-other";
    std::string f = std::string(d.header) + "\n" + a + "\n" + x + "\n" + a + "\n" + b + "\n" + a + "\n";
    // Lines: 2 a, 3 x, 4 a, 5 b, 6 a.
    Card c;
    c.file = f;
    EditRequest e;
    e.oldText = a;
    auto o = run(c, e);
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_EQUAL_STRING("old is in p/t.csv 3 times, at lines 2, 4, 6; line picks one, all=true replaces all",
                             o.error.c_str());
    // Remove line 2: the rest move up and the result says so.
    e.line = 2;
    e.newText = "";
    o = run(c, e);
    TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
    TEST_ASSERT_EQUAL_STRING(
        ("replaced 1 in p/t.csv at line 2; file is " + std::to_string(c.file.size()) + " B (was " +
         std::to_string(f.size()) + " B), 5 lines, sha8=" + sha8Hex(fnv64(kFnv64Basis, c.file)) +
         "; lines that were 3 and later are now 1 lower; old is still at lines 3, 5")
            .c_str(),
        renderEdit("p/t.csv", o).c_str());
    // The model asks for line 4 again (its old number): that is now b, so it fails.
    e.line = 4;
    const std::string before = c.file;
    o = run(c, e);
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_EQUAL_STRING("no match on line 4; matches at lines 3, 5", o.error.c_str());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), c.file.c_str());
    // Part of a row is never picked: line 2 holds x, which contains old's text only in part.
    e.oldText = x.substr(0, 10);
    e.line = 2;
    o = run(c, e);
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_EQUAL_STRING(("old is part of line 2, not the whole line; line 2 is: " + x).c_str(), o.error.c_str());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), c.file.c_str());
  }
}

static void test_an_append_between_read_and_pick_is_caught_by_sha8() {
  Rng g(8);
  const Domain& d = domains()[1];
  std::string f = makeFile(g, d, 10);
  if (f.back() != '\n') f += '\n';
  const uint64_t readHash = fnv64(kFnv64Basis, f);
  Card c;
  c.file = f + "2026-11-12,cafe,misc,3.50\n";   // appended since the read
  const auto lines = splitLines(f);
  EditRequest e;
  e.oldText = lines[3];
  if (e.oldText.back() == '\r') e.oldText.pop_back();
  e.newText = "x";
  e.line = 4;
  e.ifSha8 = sha8Hex(readHash);
  const auto o = run(c, e);
  TEST_ASSERT_FALSE(o.ok);
  TEST_ASSERT_EQUAL_STRING(("p/t.csv changed since sha8=" + e.ifSha8 + " (now " + sha8Hex(fnv64(kFnv64Basis, c.file)) +
                            "); nothing written").c_str(),
                           o.error.c_str());
  e.ifSha8 = sha8Hex(fnv64(kFnv64Basis, c.file));
  TEST_ASSERT_TRUE(run(c, e).ok);
}

// ---- reference agreement, all modes and chunk boundaries ----------------------------
static void test_single_and_all_edits_agree_with_the_reference() {
  for (uint64_t seed = 1; seed <= 200; ++seed) {
    Rng g(seed);
    const Domain& d = domains()[seed % 3];
    const std::string file = makeFile(g, d, 1 + int(g.below(400)));   // up to about 12 KB: several chunks
    const auto lines = splitLines(file);
    EditRequest e;
    const std::string& src = lines[g.below(lines.size())];
    const size_t a = g.below(src.size());
    e.oldText = src.substr(a, 1 + g.below(src.size() - a));
    if (g.chance(20)) e.oldText += "\n" + lines[(g.below(lines.size()))].substr(0, 3);   // across a line end
    e.newText = g.chance(30) ? "" : std::string(g.below(12), 'z') + (g.chance(20) ? "\nextra" : "");
    if (e.newText == e.oldText) e.newText += "!";
    e.all = g.chance(50);
    Card c;
    c.file = file;
    const auto o = run(c, e);
    const auto ref = reference(file, e);
    TEST_ASSERT_EQUAL_MESSAGE(ref.ok, o.ok, o.error.c_str());
    if (o.ok) {
      TEST_ASSERT_EQUAL_STRING(ref.file.c_str(), c.file.c_str());
      TEST_ASSERT_EQUAL(ref.replaced, o.replaced);
      checkLinesAreCurrent(file, e, o, c.file);
      TEST_ASSERT_TRUE(c.maxWrite <= kAidChunk);
      TEST_ASSERT_TRUE(c.maxRead <= kAidChunk);
    } else {
      TEST_ASSERT_EQUAL_STRING(file.c_str(), c.file.c_str());
      TEST_ASSERT_EQUAL(0, c.swaps);
    }
    TEST_ASSERT_FALSE(c.partOpen);
  }
}

static void test_errors_state_the_fact() {
  EditRequest e;
  TEST_ASSERT_EQUAL_STRING("old is empty", checkEdit(e).c_str());
  e.oldText = "a";
  e.newText = "a";
  TEST_ASSERT_EQUAL_STRING("old and new are the same", checkEdit(e).c_str());
  e.newText = "b";
  e.all = true;
  e.line = 3;
  TEST_ASSERT_EQUAL_STRING("line picks one match and all=true replaces every match; not both", checkEdit(e).c_str());
  e.all = false;
  e.ifSha8 = "xyz";
  TEST_ASSERT_EQUAL_STRING("if_sha8 is 8 hex digits", checkEdit(e).c_str());
  e.ifSha8.clear();
  TEST_ASSERT_EQUAL_STRING("", checkEdit(e).c_str());
  Card c;
  c.file = "date,amount\n2026-11-19,5\n";
  e.oldText = "zzz";
  e.line = 0;
  auto o = run(c, e);
  TEST_ASSERT_EQUAL_STRING("old is not in p/t.csv (25 B)", o.error.c_str());
  e.oldText = "2026-11-19,5";
  e.line = 9;
  o = run(c, e);
  TEST_ASSERT_EQUAL_STRING("no match on line 9; matches at line 2", o.error.c_str());
  // More than eight match lines are listed with the count of the rest.
  c.file = "h\n";
  for (int i = 0; i < 12; ++i) c.file += "r\n";
  e.oldText = "r";
  e.line = 0;
  o = run(c, e);
  TEST_ASSERT_EQUAL_STRING("old is in p/t.csv 12 times, at lines 2, 3, 4, 5, 6, 7, 8, 9 +4; line picks one, all=true replaces all",
                           o.error.c_str());
  // A long line is quoted cut at 120 B on a UTF-8 boundary.
  std::string longLine = std::string(119, 'a') + "\xC3\xA9tail";
  c.file = "h\n" + longLine + "\n";
  e.oldText = "tail";
  e.line = 2;
  o = run(c, e);
  TEST_ASSERT_EQUAL_STRING(("old is part of line 2, not the whole line; line 2 is: " + std::string(119, 'a') + " ...").c_str(),
                           o.error.c_str());
}

static void test_a_removal_takes_the_line_end_and_crlf_files_stay_crlf() {
  Card c;
  c.file = "h\r\na\r\nb\r\nc";
  EditRequest e;
  e.oldText = "b";
  e.newText = "";
  e.line = 3;
  auto o = run(c, e);
  TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
  TEST_ASSERT_EQUAL_STRING("h\r\na\r\nc", c.file.c_str());
  e.oldText = "c";
  e.line = 3;
  o = run(c, e);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("h\r\na\r\n", c.file.c_str());
  // A multi-line pick: two whole lines from line 2.
  c.file = "h\nx\ny\nz\n";
  e.oldText = "x\ny";
  e.newText = "q";
  e.line = 2;
  o = run(c, e);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("h\nq\nz\n", c.file.c_str());
  TEST_ASSERT_EQUAL_STRING(("replaced 1 in p/t.csv at line 2; file is 6 B (was 8 B), 3 lines, sha8=" +
                            sha8Hex(fnv64(kFnv64Basis, c.file)) + "; lines that were 4 and later are now 1 lower")
                               .c_str(),
                           renderEdit("p/t.csv", o).c_str());
}

// ---- faults and refusals --------------------------------------------------------------
static void test_a_card_fault_at_every_step_leaves_the_old_file_or_the_new_one() {
  Rng g(12);
  for (const Domain& d : domains()) {
    const std::string file = makeFile(g, d, 600);   // about 18 KB: several reads and writes
    EditRequest e;
    e.oldText = "2026-11-1";
    e.newText = "2026-12-1";
    e.all = true;
    Card clean;
    clean.file = file;
    TEST_ASSERT_TRUE(run(clean, e).ok);
    const std::string want = clean.file;
    const long steps = clean.calls;
    TEST_ASSERT_TRUE(steps >= 10);
    for (long k = 1; k <= steps; ++k) {
      Card c;
      c.file = file;
      c.failAt = k;
      const auto o = run(c, e);
      TEST_ASSERT_FALSE(o.ok);
      TEST_ASSERT_TRUE(o.error == "card read failed" || o.error == "card write failed; nothing changed");
      TEST_ASSERT_EQUAL_STRING(file.c_str(), c.file.c_str());   // a failed swap leaves the old file
      TEST_ASSERT_FALSE(c.partOpen);
    }
    TEST_ASSERT_TRUE(want != file);
  }
}

static void test_a_refused_allocation_at_any_point_changes_nothing() {
  Rng g(13);
  for (const Domain& d : domains()) {
    const std::string file = makeFile(g, d, 300);
    EditRequest e;
    e.oldText = splitLines(file)[2];
    if (e.oldText.back() == '\r') e.oldText.pop_back();
    e.newText = "";
    e.line = 3;
    long calls = 0;
    {
      Card c;
      c.file = file;
      aidfake::arm(-1);
      TEST_ASSERT_TRUE(run(c, e).ok);
      calls = aidfake::gAllocCalls;
      aidfake::disarm();
    }
    TEST_ASSERT_TRUE(calls >= 2);
    for (long k = 1; k <= calls; ++k) {
      Card c;
      c.file = file;
      aidfake::arm(k);
      const auto o = run(c, e);
      aidfake::disarm();
      TEST_ASSERT_FALSE(o.ok);
      TEST_ASSERT_EQUAL_STRING(kAidNoMemory, o.error.c_str());
      TEST_ASSERT_EQUAL_STRING(file.c_str(), c.file.c_str());
      TEST_ASSERT_EQUAL(0, c.swaps);
      TEST_ASSERT_FALSE(c.partOpen);
      TEST_ASSERT_EQUAL(0, aidfake::gLiveBlocks);
    }
  }
}

static void test_a_file_changed_between_the_passes_is_not_written() {
  Card c;
  c.file = "h\na\nb\na\n";
  EditRequest e;
  e.oldText = "b";
  e.newText = "c";
  long passOneReads = 0;
  {
    Card probe;
    probe.file = c.file;
    TEST_ASSERT_TRUE(run(probe, e).ok);
    passOneReads = long(probe.reads / 2);
  }
  // Right after pass 1, an append from elsewhere lands.
  c.beforeCall = [&](long n) {
    if (n == passOneReads + 1) c.file += "b\n";
  };
  const auto o = run(c, e);
  TEST_ASSERT_FALSE(o.ok);
  TEST_ASSERT_EQUAL_STRING("p/t.csv changed during the edit; nothing written", o.error.c_str());
  TEST_ASSERT_EQUAL_STRING("h\na\nb\na\nb\n", c.file.c_str());
  TEST_ASSERT_FALSE(c.partOpen);
}

// ---- a line-picked correction per domain ----------------------------------------------
static void test_a_line_picked_correction_is_exact_on_each_domain() {
  for (const Domain& d : domains()) {
    Rng g(21);
    std::string f = std::string(d.header) + "\n";
    std::vector<std::string> rows;
    for (int i = 0; i < 15; ++i) rows.push_back(d.row(g, 0));
    rows[9] = rows[4];   // an identical re-log
    for (const auto& r : rows) f += r + "\n";
    Card c;
    c.file = f;
    EditRequest e;
    e.oldText = rows[9];
    e.newText = rows[9] + "0";
    e.line = 11;   // rows[9] is line 11
    const auto o = run(c, e);
    TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
    auto after = splitLines(c.file);
    TEST_ASSERT_EQUAL_STRING((rows[9] + "0").c_str(), after[10].c_str());
    TEST_ASSERT_EQUAL_STRING(rows[4].c_str(), after[5].c_str());   // the twin is untouched
    TEST_ASSERT_EQUAL(1, o.at.size());
    TEST_ASSERT_EQUAL_UINT32(11, o.at[0]);
  }
}

// From the hostile review: a removal names the line it removed, never a line past the end.
static void test_a_removal_names_the_line_it_removed() {
  Card c;
  c.file = "h\na,1\na,1\n";
  EditRequest e;
  e.oldText = "a,1";
  e.newText = "";
  e.line = 3;
  const auto o = run(c, e);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("h\na,1\n", c.file.c_str());
  TEST_ASSERT_EQUAL_STRING(("replaced 1 in p/t.csv at line 3; file is 6 B (was 10 B), 2 lines, sha8=" +
                            sha8Hex(fnv64(kFnv64Basis, c.file)) + "; old is still at line 2").c_str(),
                           renderEdit("p/t.csv", o).c_str());
}

// A whole-line match that ends exactly at a 4 KB read boundary, with its line end in the
// next read (LF and CRLF): still a whole line.
static void test_a_pick_at_a_read_boundary_is_a_whole_line() {
  for (const char* end : {"\n", "\r\n"}) {
    for (size_t shift = 0; shift < 3; ++shift) {
      const std::string row = "2026-11-19,cafe,misc,3.50";
      std::string f = "h\n";
      while (f.size() + 40 < kAidChunk - shift - row.size()) f += "2026-11-18,rail,misc,2.00\n";
      f += std::string(kAidChunk - shift - row.size() - f.size() - 1, 'y') + "\n";
      const uint32_t line = uint32_t(std::count(f.begin(), f.end(), '\n') + 1);
      f += row + end + "2026-11-20,x,y,1" + end;
      TEST_ASSERT_EQUAL(kAidChunk - shift, f.find(row) + row.size());
      Card c;
      c.file = f;
      EditRequest e;
      e.oldText = row;
      e.newText = "2026-11-19,cafe,misc,4.50";
      e.line = line;
      const auto o = run(c, e);
      TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
      e.newText = "";
      e.oldText = "2026-11-19,cafe,misc,4.50";
      TEST_ASSERT_TRUE(run(c, e).ok);
      TEST_ASSERT_TRUE(c.file.find(std::string("4.50")) == std::string::npos);
      TEST_ASSERT_EQUAL(0, c.file.compare(c.file.size() - std::string("2026-11-20,x,y,1").size() - std::strlen(end),
                                          std::string::npos, std::string("2026-11-20,x,y,1") + end));
    }
  }
}

// Removals with all=true name the lines they were removed from, in the old numbering.
static void test_removals_name_their_old_lines() {
  Card c;
  c.file = "h\nx\ny\nx\nz\n";
  EditRequest e;
  e.oldText = "x";
  e.newText = "";
  e.all = true;
  const auto o = run(c, e);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("h\ny\nz\n", c.file.c_str());
  TEST_ASSERT_EQUAL(2, o.at.size());
  TEST_ASSERT_EQUAL_UINT32(2, o.at[0]);
  TEST_ASSERT_EQUAL_UINT32(4, o.at[1]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_line_pick_replaces_exactly_that_line_or_fails);
  RUN_TEST(test_a_stale_number_after_a_removal_fails_with_the_current_lines);
  RUN_TEST(test_an_append_between_read_and_pick_is_caught_by_sha8);
  RUN_TEST(test_single_and_all_edits_agree_with_the_reference);
  RUN_TEST(test_errors_state_the_fact);
  RUN_TEST(test_a_removal_takes_the_line_end_and_crlf_files_stay_crlf);
  RUN_TEST(test_a_card_fault_at_every_step_leaves_the_old_file_or_the_new_one);
  RUN_TEST(test_a_refused_allocation_at_any_point_changes_nothing);
  RUN_TEST(test_a_file_changed_between_the_passes_is_not_written);
  RUN_TEST(test_a_line_picked_correction_is_exact_on_each_domain);
  RUN_TEST(test_a_removal_names_the_line_it_removed);
  RUN_TEST(test_a_pick_at_a_read_boundary_is_a_whole_line);
  RUN_TEST(test_removals_name_their_old_lines);
  return UNITY_END();
}
