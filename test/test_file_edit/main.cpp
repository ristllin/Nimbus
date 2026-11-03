#include <unity.h>

#include <cstdio>
#include <string>

#include "nimbus/orch/file_append.h"   // kKeyedMaxBytes
#include "nimbus/orch/file_edit.h"
#include "nimbus/orch/turn_effects.h"

// files.edit's portable rules (file_edit.h): the decision (caps, policy,
// honest counts) and the streaming matcher (bounded carry, chunk-straddling
// matches, copy-through after the limit, a partial span at EOF is not a match).
// The device streams the file through the matcher in chunks; these tests feed
// the same content at every chunk size 1..8 so a boundary bug cannot hide
// behind one lucky split.

using namespace nimbus::orch;

namespace {

struct Run {
  std::string out;
  size_t found = 0, applied = 0;
};

Run streamAll(const std::string& src, const std::string& find, const std::string& replace,
              bool countOnly, size_t applyLimit, size_t chunk) {
  TextEditStream s(find, replace, countOnly);
  s.setApplyLimit(applyLimit);
  Run r;
  for (size_t i = 0; i < src.size(); i += chunk)
    s.feed(src.data() + i, std::min(chunk, src.size() - i), r.out);
  s.finish(r.out);
  r.found = s.found();
  r.applied = s.applied();
  return r;
}

// The out the matcher must produce, computed the plain way.
std::string expectOut(const std::string& src, const std::string& find,
                      const std::string& replace, size_t applyLimit) {
  std::string out;
  size_t from = 0, applied = 0;
  for (;;) {
    if (applied >= applyLimit) {
      out += src.substr(from);
      return out;
    }
    const size_t pos = src.find(find, from);
    if (pos == std::string::npos) return out + src.substr(from);
    out += src.substr(from, pos - from);
    out += replace;
    from = pos + find.size();
    applied++;
  }
}

}  // namespace

static std::string readSubsystem() {
  std::FILE* f = std::fopen("src/agent/files_subsystem.cpp", "rb");
  if (!f) { TEST_FAIL_MESSAGE("src/agent/files_subsystem.cpp unreadable"); return std::string(); }
  std::string src;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
  std::fclose(f);
  return src;
}

void setUp() {}
void tearDown() {}

// ---- the decision ------------------------------------------------------------

static void test_validate_the_caps_and_the_policy() {
  EditPolicy pol;
  TEST_ASSERT_EQUAL_STRING("", validateEdit(3, 0, pol).c_str());   // empty replace = delete
  TEST_ASSERT_TRUE(validateEdit(0, 1, pol).find("find text is empty") != std::string::npos);
  TEST_ASSERT_TRUE(validateEdit(kEditFindMaxBytes + 1, 1, pol).find("longer than") !=
                  std::string::npos);
  TEST_ASSERT_TRUE(validateEdit(1, kEditReplaceMaxBytes + 1, pol).find("replacement is longer") !=
                  std::string::npos);
  EditPolicy bad;
  bad.count = 0;
  TEST_ASSERT_TRUE(validateEdit(1, 1, bad).find("between 1 and") != std::string::npos);
  bad.count = kEditMaxMatches + 1;
  TEST_ASSERT_TRUE(validateEdit(1, 1, bad).find("between 1 and") != std::string::npos);
}

static void test_no_match_refuses_plainly() {
  EditDecision d = decideEdit(4, 2, EditPolicy{}, 0);
  TEST_ASSERT_FALSE(d.ok);
  TEST_ASSERT_EQUAL(0, d.apply);
  TEST_ASSERT_TRUE(d.err.find("no match") != std::string::npos);
  TEST_ASSERT_TRUE(d.err.find("nothing changed") != std::string::npos);
}

static void test_first_mode_replaces_the_first_count() {
  // The default: the first match only, however many there are.
  EditDecision d = decideEdit(3, 5, EditPolicy{}, 3);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL(1, d.apply);
  TEST_ASSERT_EQUAL(3, (int)d.matches);
  TEST_ASSERT_EQUAL(2, (int)d.sizeDelta);   // (5 - 3) * 1
  // count N: the first N matches, bounded by what is there.
  EditPolicy pol;
  pol.count = 2;
  d = decideEdit(3, 1, pol, 5);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL(2, d.apply);
  TEST_ASSERT_EQUAL(-4, (int)d.sizeDelta);  // (1 - 3) * 2, a shrink
  pol.count = 7;
  d = decideEdit(3, 1, pol, 3);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL(3, d.apply);            // fewer matches than asked: all of them
  // all: every match.
  EditPolicy all;
  all.all = true;
  d = decideEdit(3, 5, all, 3);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL(3, d.apply);
  TEST_ASSERT_EQUAL(6, (int)d.sizeDelta);
  // all beyond the hard cap: refused WITH the count.
  d = decideEdit(1, 1, all, (size_t)kEditMaxMatches + 1);
  TEST_ASSERT_FALSE(d.ok);
  TEST_ASSERT_EQUAL(0, d.apply);
  TEST_ASSERT_TRUE(d.err.find("too many matches (201") != std::string::npos);
}

// ---- the matcher: counting ----------------------------------------------------

static void test_counting_is_non_overlapping_and_chunk_safe() {
  TEST_ASSERT_EQUAL(2, (int)streamAll("aaaa", "aa", "", true, 0, 4).found);   // not 3
  TEST_ASSERT_EQUAL(0, (int)streamAll("xxab", "abcd", "", true, 0, 4).found);
  TEST_ASSERT_EQUAL(1, (int)streamAll("xxabcdyy", "abcd", "", true, 0, 4).found);
  // The match straddles every chunk boundary at some size; all sizes agree.
  for (size_t chunk = 1; chunk <= 8; chunk++)
    TEST_ASSERT_EQUAL(1, (int)streamAll("xxabcdyy", "abcd", "", true, 0, chunk).found);
  TEST_ASSERT_EQUAL(3, (int)streamAll("a-b-a-b-a", "a", "", true, 0, 3).found);
  // Counting emits nothing - not even the final non-matching tail: finish()'s
  // post-scan append is guarded, the header's "countOnly leaves out alone"
  // (prism v4.6.2 row 3; an unguarded finish leaked up to find.size()-1 bytes).
  Run r = streamAll("one tail", "one", "", true, 0, 3);
  TEST_ASSERT_EQUAL(1, (int)r.found);
  TEST_ASSERT_EQUAL(0, (int)r.out.size());
  Run r2 = streamAll("one two one", "one", "", true, 0, 3);
  TEST_ASSERT_EQUAL(2, (int)r2.found);
  TEST_ASSERT_EQUAL(0, (int)r2.out.size());
}

// ---- the matcher: replacing ---------------------------------------------------

static void test_first_only_replaces_the_first_match() {
  Run r = streamAll("one two one two", "one", "1", false, 1, 4);
  TEST_ASSERT_EQUAL(1, (int)r.applied);
  // In replace mode the count stops at the limit (the scan pass is what
  // counts the whole file); the second match copies through untouched.
  TEST_ASSERT_EQUAL(1, (int)r.found);
  TEST_ASSERT_EQUAL_STRING("1 two one two", r.out.c_str());
}

static void test_all_replaces_every_match() {
  Run r = streamAll("one two one two", "one", "1", false, 2, 4);
  TEST_ASSERT_EQUAL(2, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("1 two 1 two", r.out.c_str());
}

static void test_copy_through_after_the_limit_keeps_later_matches() {
  // The limit is hit mid-file: everything left copies through unchanged,
  // including later matches.
  Run r = streamAll("aXaYa", "a", "R", false, 1, 5);
  TEST_ASSERT_EQUAL(1, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("RXaYa", r.out.c_str());
  // The size bookkeeping: (R - a) * 1 = no change here.
  TEST_ASSERT_EQUAL(5, (int)r.out.size());
}

static void test_a_replacement_is_never_rescanned() {
  // Replace "a" with "aa" on "aaa", limit 2: the first two matches become
  // "aa" each, the third byte copies through - 5 bytes, not a cascade.
  Run r = streamAll("aaa", "a", "aa", false, 2, 3);
  TEST_ASSERT_EQUAL(2, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("aaaaa", r.out.c_str());
}

static void test_a_partial_span_at_eof_is_not_a_match() {
  Run r = streamAll("xxab", "abcd", "zz", false, 2, 2);
  TEST_ASSERT_EQUAL(0, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("xxab", r.out.c_str());
  Run whole = streamAll("xxab", "abcd", "zz", false, 2, 8);
  TEST_ASSERT_EQUAL_STRING(r.out.c_str(), whole.out.c_str());
}

static void test_deletion_and_growth_sizes() {
  Run del = streamAll("keep DROPTHIS keep", "DROPTHIS", "", false, 1, 5);
  TEST_ASSERT_EQUAL_STRING("keep  keep", del.out.c_str());
  Run grow = streamAll("x axb x", "a", "a-b-c-d", false, 1, 3);
  TEST_ASSERT_EQUAL_STRING("x a-b-c-dxb x", grow.out.c_str());
  TEST_ASSERT_EQUAL(6, (int)(grow.out.size() - 7));   // (7 - 1) * 1 growth
}

// ---- the property: every chunk size agrees with the plain way ----------------

static void test_every_chunk_split_agrees_with_the_whole() {
  const char* srcs[] = {"the quick brown fox\njumps over\nthe quick brown dog\n",
                       "date,meal,kcal\n2026-10-29,breakfast,400\n2026-10-29,lunch,760\n",
                       "aaabaaabaaab", "no matches here", ""};
  const char* finds[] = {"quick brown", "2026-10-29,lunch,760", "aaab", "zz", "the"};
  const char* reps[] = {"slow red", "2026-10-29,lunch,720", "x", "yy", "THE"};
  for (size_t i = 0; i < 5; i++) {
    const size_t matches = streamAll(srcs[i], finds[i], "", true, 0, 8).found;
    for (size_t limit = 0; limit <= matches; limit++) {
      const std::string want = expectOut(srcs[i], finds[i], reps[i], limit);
      for (size_t chunk = 1; chunk <= 8; chunk++) {
        const Run r = streamAll(srcs[i], finds[i], reps[i], false, limit, chunk);
        char label[96];
        snprintf(label, sizeof label, "case %zu limit %zu chunk %zu", i, limit, chunk);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.out.c_str(), label);
        TEST_ASSERT_EQUAL_MESSAGE((int)std::min(limit, matches), (int)r.applied, label);
      }
    }
  }
}

// ---- the document-shaped cases ------------------------------------------------

static void test_editing_a_row_of_a_log_by_its_exact_span() {
  // The motivating shape: fix one bad row without touching the others.
  const std::string log = "date,meal,kcal\n2026-10-29,breakfast,400\n"
                          "2026-10-29,lunch,760\n2026-10-29,dinner,650\n";
  Run r = streamAll(log, "2026-10-29,lunch,760", "2026-10-29,lunch,720", false, 1, 7);
  TEST_ASSERT_EQUAL(1, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-29,breakfast,400\n"
                           "2026-10-29,lunch,720\n2026-10-29,dinner,650\n",
                           r.out.c_str());
  // The other rows are byte-identical (a prefix and suffix check).
  const size_t rowAt = log.find("2026-10-29,lunch");
  TEST_ASSERT_TRUE(r.out.substr(0, rowAt) == log.substr(0, rowAt));
  TEST_ASSERT_TRUE(r.out.substr(r.out.find("2026-10-29,dinner")) ==
                  log.substr(log.find("2026-10-29,dinner")));
}

static void test_a_whole_line_is_just_a_longer_span() {
  const std::string doc = "# Title\nold line\nnew tail\n";
  Run r = streamAll(doc, "old line\n", "replaced line\n", false, 1, 4);
  TEST_ASSERT_EQUAL_STRING("# Title\nreplaced line\nnew tail\n", r.out.c_str());
}

// ---- the device wiring pin ---------------------------------------------------
// files.edit and the web upload session share the ONE scratch slot
// /mem/files/.part; every writer of the slot refuses an active session through
// uploadActiveLocked() (files_subsystem.cpp's own convention). The edit's
// arbitration rides the shared gate - editDoc calls appendGate, whose body
// carries the uploadActiveLocked() check - and it must run BEFORE
// substituteDoc consumes the slot (the one Lock held through the edit keeps a
// session from starting mid-flight). The device source is scanned because the
// device code does not build on the host (the test_tool_taint /
// test_store_full pattern).
static void test_the_edit_gates_the_upload_slot_before_touching_it() {
  std::FILE* f = std::fopen("src/agent/files_subsystem.cpp", "rb");
  if (!f) { TEST_FAIL_MESSAGE("src/agent/files_subsystem.cpp unreadable"); return; }
  std::string src;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
  std::fclose(f);
  // The shared gate carries the upload-session check.
  const size_t gateAt = src.find("std::string appendGate(");
  TEST_ASSERT_TRUE(gateAt != std::string::npos);
  TEST_ASSERT_TRUE(src.find("uploadActiveLocked()", gateAt) != std::string::npos);
  // editDoc runs the gate before substituteDoc touches the slot.
  const size_t editAt = src.find("ToolResult editDoc(");
  TEST_ASSERT_TRUE(editAt != std::string::npos);
  const size_t gateCall = src.find("appendGate(project, name, who)", editAt);
  const size_t subCall = src.find("substituteDoc(path", editAt);
  TEST_ASSERT_TRUE(gateCall != std::string::npos);
  TEST_ASSERT_TRUE(subCall != std::string::npos);
  TEST_ASSERT_TRUE(gateCall < subCall);
}

// The merge-back: files.edit came from a line where a replace was remove-then-rename. On
// this line every whole-file replace goes through commitPart (file_swap.h), so a power cut
// or an index refusal never leaves the file gone. The edit must not bypass it.
static void test_the_edit_commits_through_the_swap() {
  std::FILE* f = std::fopen("src/agent/files_subsystem.cpp", "rb");
  if (!f) { TEST_FAIL_MESSAGE("src/agent/files_subsystem.cpp unreadable"); return; }
  std::string src;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
  std::fclose(f);
  const size_t sub = src.find("static std::string substituteDoc(");
  const size_t commit = src.find("static bool commitEdit(", sub);
  const size_t edit = src.find("ToolResult editDoc(", commit);
  const size_t after = src.find("std::string readDocText(", edit);
  TEST_ASSERT_TRUE(sub != std::string::npos && commit != std::string::npos);
  TEST_ASSERT_TRUE(edit != std::string::npos && after != std::string::npos);
  const std::string body = src.substr(sub, after - sub);
  TEST_ASSERT_TRUE(body.find("return commitPart(e, err)") != std::string::npos);
  TEST_ASSERT_TRUE(body.find(".rename(") == std::string::npos);
  TEST_ASSERT_TRUE(body.find("remove(path") == std::string::npos);
}

// files.edit holds the card lock across two whole-file passes and a rewrite, so it has the
// keyed path's ceiling: a bigger file is refused with the way out, before a byte is read.
static void test_a_file_over_the_ceiling_is_refused_with_the_way_out() {
  TEST_ASSERT_EQUAL_STRING("", editSizeRefusal(0).c_str());
  TEST_ASSERT_EQUAL_STRING("", editSizeRefusal(kKeyedMaxBytes).c_str());
  const std::string over = editSizeRefusal(kKeyedMaxBytes + 1);
  TEST_ASSERT_TRUE(over.find("over 1 MB") != std::string::npos);
  TEST_ASSERT_TRUE(over.find("nothing was changed") != std::string::npos);
  TEST_ASSERT_TRUE(over.find("new file") != std::string::npos);
  TEST_ASSERT_FALSE(editSizeRefusal(8u * 1024 * 1024).empty());   // the store's largest file

  const std::string src = readSubsystem();
  const size_t count = src.find("static std::string countDocMatches(");
  TEST_ASSERT_TRUE(count != std::string::npos);
  const size_t size = src.find("oldSize = f.size();", count);
  const size_t refuse = src.find("editSizeRefusal(oldSize)", count);
  const size_t read = src.find("f.read(", count);
  TEST_ASSERT_TRUE(size != std::string::npos && refuse != std::string::npos && read != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(size < refuse && refuse < read, "the ceiling runs before the first read");
}

// The reply is the claim check's evidence. Replacing a span with the same text changed
// nothing, so it must back no removal or replacement, and no log row; a real edit must.
static void test_a_replacement_identical_to_the_find_text_is_no_change() {
  TEST_ASSERT_TRUE(editChangesNothing("2026-10-30,lunch,Rice,400\n", "2026-10-30,lunch,Rice,400\n"));
  TEST_ASSERT_FALSE(editChangesNothing("2026-10-30,lunch,Rice,400\n", "2026-10-30,lunch,Rice,400"));
  TEST_ASSERT_FALSE(editChangesNothing("Rice", ""));
  // The class: when the spans differ, the substitution changes the bytes. This is why the
  // one comparison above is the whole no-op case.
  const std::string text = "abab abab\n";
  const char* spans[] = {"a", "b", "ab", "ba", "abab", " ", "\n"};
  for (const char* f : spans)
    for (const char* r : spans) {
      const Run run = streamAll(text, f, r, false, 1, 3);
      TEST_ASSERT_EQUAL_MESSAGE(std::string(f) == r, run.out == text, f);
    }

  EditDecision d = decideEdit(4, 4, EditPolicy{}, 3);
  TEST_ASSERT_TRUE(d.ok);
  const std::string same = renderEditNoChange("food/food-log.csv", d);
  TEST_ASSERT_TRUE_MESSAGE(same.find("(no change") != std::string::npos, same.c_str());
  TurnEffects none;
  recordToolEffect(none, FxKind::FileWrite, false, same, kFxLogRow);
  TEST_ASSERT_FALSE(none.anyOk(FxKind::FileReplace));
  TEST_ASSERT_FALSE(none.anyOkWith(FxKind::FileWrite, kFxLogRow));

  const std::string real = renderEditReply("food/food-log.csv", d, 90);
  TEST_ASSERT_EQUAL_STRING("edited food/food-log.csv: replaced 1 of 3 matches (now 90 bytes); 2 more remain (all: true replaces them)",
                           real.c_str());
  TurnEffects landed;
  recordToolEffect(landed, FxKind::FileWrite, false, real, kFxLogRow);
  TEST_ASSERT_TRUE(landed.anyOk(FxKind::FileReplace));
  TEST_ASSERT_TRUE(landed.anyOkWith(FxKind::FileWrite, kFxLogRow));

  // The device runs the check before the file is touched, and answers in these words.
  const std::string src = readSubsystem();
  const size_t edit = src.find("ToolResult editDoc(");
  TEST_ASSERT_TRUE(edit != std::string::npos);
  const size_t noop = src.find("editChangesNothing(find, replace)", edit);
  const size_t sub = src.find("substituteDoc(path", edit);
  TEST_ASSERT_TRUE(noop != std::string::npos && sub != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(noop < sub, "a no-op must not rewrite the file");
  TEST_ASSERT_TRUE(src.find("renderEditNoChange(", edit) != std::string::npos);
  TEST_ASSERT_TRUE(src.find("renderEditReply(", edit) != std::string::npos);
}

// ---- the keyed-CSV row diff (RCA 2026-11-01/02 cause 15) ----------------------
// The food log's own shape: a day delete and a from-scratch re-entry dropped the
// fries row and changed a protein figure, and the reply said only "replaced 1 of
// 1 match(es)". The find text is what was there, so the diff is computed from the
// spans themselves.

static const char kFoodHeader[] = "date,meal,items,kcal,protein_g,key";

static std::string foodRow(const std::string& meal, const std::string& kcal,
                           const std::string& protein) {
  return "2026-11-02," + meal + ",\"Some food\"," + kcal + "," + protein +
         ",2026-11-02-" + meal + "\n";
}

// The seven rows the 19:12 delete took out (the RCA's day).
static std::string sevenRows() {
  const char* meals[] = {"breakfast", "snack1", "snack2", "lunch1", "lunch2", "dinner", "drink1"};
  std::string rows;
  for (const char* m : meals) rows += foodRow(m, "100", "5.0");
  return rows;
}

static void test_a_whole_day_delete_names_its_rows() {
  const CsvEditDiff d = diffKeyedEdit(kFoodHeader, sevenRows(), "");
  TEST_ASSERT_TRUE_MESSAGE(d.keyed, "a keyed CSV's whole-day delete is a keyed edit");
  TEST_ASSERT_EQUAL(7, (int)d.removed.size());
  TEST_ASSERT_EQUAL(0, (int)d.added.size());
  TEST_ASSERT_EQUAL(0, (int)d.changed.size());
  bool fries = false;
  for (const CsvEditRow& r : d.removed)
    if (r.date == "2026-11-02" && r.key == "snack1") fries = true;
  TEST_ASSERT_TRUE_MESSAGE(fries, "the removed rows name their date and key");
  const std::string lines = renderKeyedEditDiff(d);
  TEST_ASSERT_TRUE(lines.find("rows removed:") != std::string::npos);
  TEST_ASSERT_TRUE(lines.find("2026-11-02 snack1") != std::string::npos);
  TEST_ASSERT_TRUE(lines.find("2026-11-02 drink1") != std::string::npos);
}

// The re-entry shape: one row dropped, one figure changed, one row new - each
// named, against what was there.
static void test_a_day_rewrite_names_added_changed_and_removed() {
  const std::string before = foodRow("breakfast", "66", "3.3") + foodRow("snack1", "327", "31.8") +
                             foodRow("snack2", "120", "4.3");
  const std::string after = foodRow("breakfast", "66", "3.3") + foodRow("snack2", "120", "4.0") +
                            foodRow("fries", "50", "0.6");
  const CsvEditDiff d = diffKeyedEdit(kFoodHeader, before, after);
  TEST_ASSERT_TRUE(d.keyed);
  TEST_ASSERT_EQUAL(1, (int)d.removed.size());
  TEST_ASSERT_EQUAL(1, (int)d.added.size());
  TEST_ASSERT_EQUAL(1, (int)d.changed.size());
  TEST_ASSERT_EQUAL_STRING("snack1", d.removed[0].key.c_str());
  TEST_ASSERT_EQUAL_STRING("fries", d.added[0].key.c_str());
  TEST_ASSERT_EQUAL_STRING("snack2", d.changed[0].row.key.c_str());
  TEST_ASSERT_EQUAL_STRING("protein_g", d.changed[0].col.c_str());
  TEST_ASSERT_EQUAL_STRING("4.3", d.changed[0].was.c_str());
  TEST_ASSERT_EQUAL_STRING("4.0", d.changed[0].now.c_str());
  const std::string lines = renderKeyedEditDiff(d);
  TEST_ASSERT_TRUE(lines.find("rows removed: 2026-11-02 snack1") != std::string::npos);
  TEST_ASSERT_TRUE(lines.find("rows added: 2026-11-02 fries") != std::string::npos);
  TEST_ASSERT_TRUE(lines.find("changed: 2026-11-02 snack2 protein_g 4.3 -> 4.0") != std::string::npos);
  // The report rides after the receipt, and the claim check still reads the
  // receipt: the diff lines must not unseat the "replaced A of B" proof.
  EditDecision dec = decideEdit(before.size(), after.size(), EditPolicy{}, 1);
  TEST_ASSERT_TRUE(dec.ok);
  const std::string reply = renderEditReply("food/meal_log.csv", dec, 200) + "\n" + lines;
  TurnEffects fx;
  recordToolEffect(fx, FxKind::FileWrite, false, reply, kFxLogRow);
  TEST_ASSERT_TRUE(fx.anyOk(FxKind::FileReplace));
  TEST_ASSERT_TRUE(fx.anyOkWith(FxKind::FileWrite, kFxLogRow));
}

// A surgical mid-row edit (a figure alone) is not a row-level change: the rows
// do not parse, the diff says nothing, and the reply keeps its today shape.
static void test_a_mid_row_edit_says_nothing() {
  const CsvEditDiff d = diffKeyedEdit(kFoodHeader, "4.3", "4.0");
  TEST_ASSERT_FALSE(d.keyed);
  TEST_ASSERT_EQUAL_STRING("", renderKeyedEditDiff(d).c_str());
  // A span whose rows hold no date column, and a header with no key column
  // (every non-date column a plain number): never a guess.
  TEST_ASSERT_FALSE(diffKeyedEdit("meal,items,kcal", foodRow("breakfast", "66", "3.3"), "").keyed);
  TEST_ASSERT_FALSE(diffKeyedEdit("date,kcal,protein_g",
                                  "2026-11-02,100,5.0\n2026-11-02,200,6.0\n", "").keyed);
}

// A find text with no dated rows (a note line, a fragment) replaced by dated
// rows: the key column is picked over both spans, so an added row is named by
// its key, never by a figure value read from the replace span alone.
static void test_an_undated_find_span_does_not_key_rows_by_a_figure() {
  const CsvEditDiff d = diffKeyedEdit("date,kcal,meal",
                                      "# a note line\n",
                                      "2026-11-02,450,breakfast\n2026-11-02,865,lunch\n");
  TEST_ASSERT_TRUE_MESSAGE(d.keyed, "dated rows on the replace side make it a keyed edit");
  TEST_ASSERT_EQUAL(2, (int)d.added.size());
  TEST_ASSERT_EQUAL_STRING("breakfast", d.added[0].key.c_str());
  TEST_ASSERT_EQUAL_STRING("lunch", d.added[1].key.c_str());
  const std::string lines = renderKeyedEditDiff(d);
  TEST_ASSERT_TRUE(lines.find("rows added: 2026-11-02 breakfast") != std::string::npos);
  TEST_ASSERT_TRUE(lines.find("450") == std::string::npos);   // a figure value is never the key
}

// The report is bounded: a bigger rewrite names the first rows and counts the
// rest, never a runaway tool result.
static void test_the_row_lists_are_bounded() {
  std::string rows;
  for (int i = 0; i < 12; i++) rows += foodRow("meal" + std::to_string(i), "100", "5.0");
  const CsvEditDiff d = diffKeyedEdit(kFoodHeader, rows, "");
  TEST_ASSERT_EQUAL(12, (int)d.removed.size());
  const std::string lines = renderKeyedEditDiff(d);
  TEST_ASSERT_TRUE(lines.find("+4 more") != std::string::npos);
  TEST_ASSERT_TRUE(lines.find("meal11") == std::string::npos);   // past the cap, counted not named
}

// The removed-rows note: a re-add that skips a row is named, and a complete
// re-add says nothing.
static void test_not_readded_names_the_missing_row() {
  std::vector<CsvEditRow> remembered;
  const char* meals[] = {"breakfast", "snack1", "snack2", "lunch1", "lunch2", "dinner", "fries"};
  for (const char* m : meals) remembered.push_back({"2026-11-02", m});
  std::vector<CsvEditRow> readded = remembered;
  readded.pop_back();   // the from-scratch re-entry dropped the fries row
  const std::vector<CsvEditRow> missing = notReadded(remembered, readded);
  TEST_ASSERT_EQUAL(1, (int)missing.size());
  TEST_ASSERT_EQUAL_STRING("fries", missing[0].key.c_str());
  const std::string warn = renderNotReadded(missing);
  TEST_ASSERT_TRUE(warn.find("2026-11-02 fries") != std::string::npos);
  TEST_ASSERT_TRUE(warn.find("not") != std::string::npos);
  TEST_ASSERT_EQUAL(0, (int)notReadded(remembered, remembered).size());
  TEST_ASSERT_EQUAL_STRING("", renderNotReadded({}).c_str());
}

// The device wires the diff into files.edit's reply and the note into both
// write paths: the report exists only if the seams carry it. editDoc's row
// report lives in keyedEditReport (split out of the writer for the complexity
// gate), so the seam is the writer's call plus the helper's own calls; the
// note rides the reconcileRemovedRows helper (defined before both writers),
// so the seam there is the helper's call from each writer plus the helper's
// own two calls.
static void test_the_device_reports_the_diff_and_the_missing_readd() {
  const std::string src = readSubsystem();
  const size_t edit = src.find("ToolResult editDoc(");
  TEST_ASSERT_TRUE(edit != std::string::npos);
  TEST_ASSERT_TRUE(src.find("keyedEditReport(", edit) != std::string::npos);
  const size_t report = src.find("static std::string keyedEditReport(");
  TEST_ASSERT_TRUE(report != std::string::npos);
  TEST_ASSERT_TRUE(src.find("diffKeyedEdit(", report) != std::string::npos);
  TEST_ASSERT_TRUE(src.find("renderKeyedEditDiff(", report) != std::string::npos);
  TEST_ASSERT_TRUE(src.find("reconcileRemovedRows(", report) != std::string::npos);
  TEST_ASSERT_TRUE(src.find("rememberRemovedRows(", report) != std::string::npos);
  const size_t upsert = src.find("ToolResult upsertRows(");
  TEST_ASSERT_TRUE(upsert != std::string::npos);
  TEST_ASSERT_TRUE(src.find("reconcileRemovedRows(", upsert) != std::string::npos);
  const size_t reconcile = src.find("static std::string reconcileRemovedRows(");
  TEST_ASSERT_TRUE(reconcile != std::string::npos);
  TEST_ASSERT_TRUE(src.find("notReadded(", reconcile) != std::string::npos);
  TEST_ASSERT_TRUE(src.find("renderNotReadded(", reconcile) != std::string::npos);
  TEST_ASSERT_TRUE(src.find("g_removedRows") != std::string::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_validate_the_caps_and_the_policy);
  RUN_TEST(test_no_match_refuses_plainly);
  RUN_TEST(test_first_mode_replaces_the_first_count);
  RUN_TEST(test_counting_is_non_overlapping_and_chunk_safe);
  RUN_TEST(test_first_only_replaces_the_first_match);
  RUN_TEST(test_all_replaces_every_match);
  RUN_TEST(test_copy_through_after_the_limit_keeps_later_matches);
  RUN_TEST(test_a_replacement_is_never_rescanned);
  RUN_TEST(test_a_partial_span_at_eof_is_not_a_match);
  RUN_TEST(test_deletion_and_growth_sizes);
  RUN_TEST(test_every_chunk_split_agrees_with_the_whole);
  RUN_TEST(test_editing_a_row_of_a_log_by_its_exact_span);
  RUN_TEST(test_a_whole_line_is_just_a_longer_span);
  RUN_TEST(test_the_edit_gates_the_upload_slot_before_touching_it);
  RUN_TEST(test_the_edit_commits_through_the_swap);
  RUN_TEST(test_a_file_over_the_ceiling_is_refused_with_the_way_out);
  RUN_TEST(test_a_replacement_identical_to_the_find_text_is_no_change);
  RUN_TEST(test_a_whole_day_delete_names_its_rows);
  RUN_TEST(test_a_day_rewrite_names_added_changed_and_removed);
  RUN_TEST(test_a_mid_row_edit_says_nothing);
  RUN_TEST(test_an_undated_find_span_does_not_key_rows_by_a_figure);
  RUN_TEST(test_the_row_lists_are_bounded);
  RUN_TEST(test_not_readded_names_the_missing_row);
  RUN_TEST(test_the_device_reports_the_diff_and_the_missing_readd);
  return UNITY_END();
}
