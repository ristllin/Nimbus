#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// file_edit - the portable rules behind files.edit (host-tested; the device
// and nimbusd only move bytes).
//
// The only way to change a stored document used to be a whole-file rewrite:
// files.read of every byte, then artifact.save of every byte - twice over the
// prompt, and a whole-file buffer on the device. files.edit substitutes an
// exact span (a phrase, a whole line's text) in place: the caller streams the
// file through the matcher in bounded chunks, the device writes the substituted
// stream to a temp sibling and renames it over the original - the original is
// untouched until the last byte is written, and nothing is ever held whole in
// memory (the matcher carries at most one chunk plus the unsettled tail).
namespace nimbus::orch {

// One edit call carries at most this much find text and replacement (a line, a
// paragraph - never a file), and replaces at most this many matches (a runaway
// span like "e" on a big file is refused with the count, not applied blindly).
inline constexpr size_t kEditFindMaxBytes = 2048;
inline constexpr size_t kEditReplaceMaxBytes = 4096;
inline constexpr int kEditMaxMatches = 200;

// What the caller asked for. all=false (the default) replaces the FIRST
// `count` matches (count 1 = the surgical default); all=true replaces every
// match (the hard cap above still guards the runaway case).
struct EditPolicy {
  bool all = false;
  int count = 1;
};

// The decision after the file was scanned and before any byte is rewritten.
// ok=false means nothing is written; `err` says why in plain words.
struct EditDecision {
  bool ok = false;
  std::string err;
  size_t matches = 0;    // matches found in the whole file
  int apply = 0;         // matches this call replaces
  int64_t sizeDelta = 0; // (replaceLen - findLen) * apply: the file's growth
};

// Validate the find/replace spans and the policy before anything is opened:
// "" when the edit may proceed to the scan (the same checks decideEdit makes).
std::string validateEdit(size_t findLen, size_t replaceLen, const EditPolicy& pol);

// Validate the spans and the policy against what the scan counted, and decide
// how many matches this call replaces. first-mode replaces min(count,
// matches); all-mode replaces every match but refuses (with the count) when
// the file holds more than kEditMaxMatches.
EditDecision decideEdit(size_t findLen, size_t replaceLen, const EditPolicy& pol,
                        size_t matches);

// The ceiling on the stored file, kKeyedMaxBytes (file_append.h): an edit holds the store
// lock across two whole-file passes and a rewrite, so a bigger file is refused with the
// way out instead of stalling every other card user. "" when the file may be edited.
std::string editSizeRefusal(size_t fileBytes);

// A replacement identical to the find text changes no byte (spans that differ always do),
// so the file is not touched and the reply says so: renderEditNoChange's "(no change" is
// what the claim check honors, and such a call backs no removal, replacement or log row.
bool editChangesNothing(const std::string& find, const std::string& replace);
std::string renderEditNoChange(const std::string& where, const EditDecision& d);

// The reply to an edit that landed: "edited p/n: replaced A of B match(es) (now N bytes)",
// which the claim check reads as the replacement's proof (turn_effects).
std::string renderEditReply(const std::string& where, const EditDecision& d, size_t newSize);

// The streaming matcher. Feed the file through it in chunks of any size - the
// carries never exceed one chunk plus (find.size() - 1) unsettled bytes, so a
// match that straddles a chunk boundary is found exactly where it lies.
//
//   countOnly: counts matches, emits nothing (the scan pass);
//   replace:   the first `apply` matches are substituted with `replace`;
//              once `apply` is reached the rest copies through unchanged
//              (no further matching), and the substituted text is never
//              rescanned (a replacement cannot match itself).
//
// Overlapping matches are applied left to right: after a substitution the scan
// resumes after the replaced span.
class TextEditStream {
 public:
  TextEditStream(const std::string& find, const std::string& replace, bool countOnly);

  // Consume one chunk of the file. Settled output (everything that can no
  // longer start or extend a match - substituted where the policy applies) is
  // appended to `out`; countOnly leaves `out` alone.
  void feed(const char* data, size_t len, std::string& out);
  // The file ended: scan and emit whatever is left (a partial match at the end
  // of the file is never a match - it copies through).
  void finish(std::string& out);

  size_t found() const { return found_; }     // matches seen so far
  size_t applied() const { return applied_; } // substitutions made so far
  // The unsettled carry (for the caller's own bounds; always < find + chunk).
  size_t carry() const { return carry_.size(); }
  // Replace mode: substitute at most n matches (the decision's `apply`);
  // 0 = no limit.
  void setApplyLimit(size_t n) { applyLimit_ = n; }

 private:
  void scan(std::string& out, bool atEof);

  std::string find_;
  std::string replace_;
  bool countOnly_;
  std::string carry_;
  size_t found_ = 0;
  size_t applied_ = 0;
  size_t applyLimit_ = 0;   // 0 = no limit (countOnly scans the whole file)
  bool done_ = false;       // apply limit reached: the rest copies through
};

// ---- the keyed-CSV row diff: a whole-day rewrite says what changed -----------
// RCA 2026-11-01/02 cause 15: a day delete and a from-scratch re-entry dropped
// the fries row and changed a protein figure, and the reply said only
// "replaced 1 of 1 match(es)". The find text IS what was there, so the
// row-level change is computed from the spans themselves - no second pass over
// the file, and nothing is claimed that the caller's own words do not show.

// One row the report names: its date and key column values.
struct CsvEditRow {
  std::string date;   // the date column's value (YYYY-MM-DD)
  std::string key;    // the key column's value
};

// A row that stayed (same date and key) while a figure column changed.
struct CsvEditChange {
  CsvEditRow row;
  std::string col;         // the figure column's name
  std::string was, now;    // its cells, as the spans held them
};

struct CsvEditDiff {
  bool keyed = false;   // the spans read as rows of a keyed CSV (a date column
                        // and a key column were both trusted); the caller says
                        // nothing otherwise - never a guess
  std::vector<CsvEditRow> removed;    // rows the replacement took out
  std::vector<CsvEditRow> added;      // rows the replacement put in
  std::vector<CsvEditChange> changed; // rows that stayed with a figure changed
};

// The row-level change a keyed-CSV edit makes. `headerRow` is the file's first
// row (the column names); the find text is exactly what was there. Rows that
// do not parse as the header's rows with a YYYY-MM-DD date (a fragment of a
// row, a comment, the header itself) take no part. Only figure columns are
// compared for `changed`: figures are what a day's total counts, while
// rewritten free text is a re-entry's normal churn.
CsvEditDiff diffKeyedEdit(const std::string& headerRow, const std::string& find,
                          const std::string& replace);

// The most named rows per list in renderKeyedEditDiff; the rest is "+N more".
inline constexpr size_t kEditDiffMaxRows = 8;

// The report lines ("rows removed: ...", "rows added: ...", "changed: ..."),
// "" when the diff has nothing to say.
std::string renderKeyedEditDiff(const CsvEditDiff& d);

// ---- the removed-rows note: a re-add that skips a row is named ---------------
// The caller holds the rows an edit removed (in RAM, one file, bounded); when
// rows land for the same file again - a keyed append, an edit that adds rows -
// the ones not re-added are named, so a from-scratch re-entry cannot quietly
// drop a row the owner already reported.
inline constexpr size_t kRemovedNoteMaxRows = 40;

// `remembered` minus every row `readded` holds (date plus key; duplicates pair
// in order).
std::vector<CsvEditRow> notReadded(const std::vector<CsvEditRow>& remembered,
                                   const std::vector<CsvEditRow>& readded);

// The warning for what a re-add left out: "" when nothing is missing.
std::string renderNotReadded(const std::vector<CsvEditRow>& missing);

// ---- the history rows a keyed-CSV write keeps in the file --------------------
// Lane UPSERT fix pass (review finding 2): a row that disappears or changes today
// must stay nameable from the file itself, so a disputed total can be re-read
// from the ledger's own history. A keyed-CSV edit keeps every row it removed as
// a "#removed: " row and every row it changed (same date and key, different
// text) as a "#replaced: " row holding the old text; a whole-file rewrite
// (artifact.save over a keyed CSV) keeps the old file's rows of `today` that the
// new text drops or changes the same way. "" when the spans do not read as rows
// of a keyed CSV. Capped like the ledger's history rows (food_ledger.h).
inline constexpr size_t kEditHistoryMaxRows = 12;
inline constexpr size_t kEditHistoryMaxBytes = 900;

// The history lines a keyed-CSV edit must append to the file: "#removed: <row>"
// for each row the replacement took out, "#replaced: <row>" for each row it
// changed. `headerRow` is the file's first row; the find text is exactly what
// was there. "" when the spans do not read as rows of a keyed CSV.
std::string historyRowsForKeyedEdit(const std::string& headerRow, const std::string& find,
                                    const std::string& replace);

// The streaming form of the whole-file rewrite: construct with the NEW text and
// the day to keep, feed every row of the OLD file in order (header first), then
// finish() returns the history lines to append to the new file ("" = none).
class RewriteHistoryScan {
 public:
  RewriteHistoryScan(const std::string& newText, const std::string& today);
  void feed(const std::string& row, bool complete);
  std::string finish() const { return out_; }

 private:
  void feedDataRow(const std::string& row, const std::vector<std::string>& fields);
  std::string today_;
  std::vector<std::string> header_;   // the NEW text's header: the file's columns after the save
  bool sawHeader_ = false;
  int dateCol_ = -1, keyCol_ = -1;
  std::map<std::string, std::string> newRows_;   // identity (date + key) -> the new row's raw line
  std::string out_;
  size_t rows_ = 0, bytes_ = 0;
};

// The whole-text form of the same (the host lab, the tests): the old file's rows
// of `today` that `newText` drops or changes, as history lines.
std::string historyRowsForRewrite(const std::string& oldText, const std::string& newText,
                                  const std::string& today);

}  // namespace nimbus::orch
