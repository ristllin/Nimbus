#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// file_append - the portable rules behind files.append and the files.read tail
// view (host-tested; the device and nimbusd only move bytes).
//
// Logging a row used to cost a whole-file read plus a whole-file rewrite
// (files.read, then artifact.save of the entire log), and every byte of both rode
// the turn's prompt. files.append writes only the new rows at the end of the
// file; the tail view reads only the end of the file. Neither ever rewrites a byte
// that is already stored.
namespace nimbus::orch {

// One append call carries at most this much text (a few rows, never a file).
inline constexpr size_t kAppendMaxBytes = 4096;
// The largest stored log a keyed update, a day total or a files.edit streams (about
// 20,000 rows): each pass holds the store lock, so a bigger file is refused with the
// way out (start a new log) instead of stalling every other card user.
inline constexpr size_t kKeyedMaxBytes = 1024 * 1024;
// The tail view: default and maximum row counts, and the window read from the
// end of the file (it fits one tool result with room for the header lines).
inline constexpr size_t kTailDefaultRows = 20;
inline constexpr size_t kTailMaxRows = 200;
inline constexpr size_t kTailWindowBytes = 7680;

// The bytes to write for one append, or an error. Rows are normalized to "\n"
// line ends and always end with one. When the file's last byte is not a line end
// (a row cut by a power loss or a hand edit) the new rows start on a fresh line
// and `repairedCut` says so - the cut row is reported, never silently joined to
// the new one.
struct AppendPlan {
  bool ok = false;
  std::string bytes;     // exactly what to append
  std::string err;       // why not, when !ok
  bool repairedCut = false;
  int rows = 0;          // rows in `bytes`
};
// A .csv file name, in any case: the one rule the device, nimbusd and the lab share.
inline bool isCsvName(const std::string& name) {
  if (name.size() <= 4) return false;
  std::string ext = name.substr(name.size() - 4);
  for (char& c : ext)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return ext == ".csv";
}

// csvCreate: this append would CREATE a keyed-eligible CSV (.csv, no stored
// file). Its first row must then be the header (the column names) - a
// headerless CSV can never be updated by key, so every later "correction"
// would append a duplicate (v4.6.3, the 30 Oct investigation's loop).
AppendPlan planAppend(const std::string& text, size_t fileBytes, char lastByte,
                      bool csvCreate = false);

// The tail view over the last bytes of a file. `window` is the end of the file;
// `windowStart` is its offset in the file (0 = the window is the whole file).
// A window that starts mid-file drops its first, partial row.
struct TailView {
  std::vector<std::string> rows;   // the last N complete rows, oldest first
  bool earlier = false;            // rows above these exist and are not shown
  bool lastCut = false;            // the file does not end in a line end: the
                                   // last row is incomplete (cut mid-write)
};
TailView tailRows(const std::string& window, size_t windowStart, size_t n);

// The tool text: a heading naming what is shown, the optional header row (a CSV's
// first line), the rows, and a closing note when the last row is incomplete.
std::string renderTail(const TailView& v, const std::string& name, size_t fileBytes,
                       const std::string& headerRow);


// ---- the keyed CSV upsert and the day total -----------------------------------
// A keyed CSV is a log whose rows are unique per DAY and key: a row is identified
// by its date (the "date" or "day" column, YYYY-MM-DD) plus its key column (a meal,
// a ledger category). Logging the same date and key again REPLACES that row - that
// is how a correction works, so a day's total (the sum of its rows) never counts a
// corrected figure twice. A new date or key adds a row. Only the new rows cross
// the model's context; the merge happens on the device, row by row.
//
// The rules (each refusal writes nothing and names the row and the reason):
//   - an update never touches another day's row: the date is part of the identity.
//     The date column is found by its name (date, day, or a name holding date, day
//     or time) or else by its content (the column the new rows fill with
//     YYYY-MM-DD dates); a log with no date column is refused for keyed writes;
//   - a key that names two or more stored rows (key "date" on a day with several
//     rows, a log that already holds duplicates), or two rows of one call with the
//     same identity, is ambiguous and refused - unless the call says replace: "all"
//     for stored duplicates, which keeps the others as "#superseded: " rows;
//   - the text's header must equal the stored header (same columns, same order);
//   - every new row has the header's field count, a key value, a YYYY-MM-DD date
//     not after today (when today is known) and a plain number in the sum column;
//   - a stored last row with no line end is complete when it is well-formed (a file
//     saved without a final line end: it gets its line end). Only a PROVABLY cut
//     one (a quote left open, fewer fields than the header, no plain number where
//     the row before holds one, or in the sum column) is set aside: a new row with
//     its identity replaces it, otherwise it is kept as a "#cut: " comment row that
//     never matches or counts again;
//   - a stored row starting with '#' is a comment: kept, never matched or counted.
//     A replace keeps the row it replaced as a "#replaced: " comment row, so a
//     disputed day can be re-read against what it held before (lane UPSERT, 4 Nov);
//   - a keyed write whose key names a reused label (a meal slot like "dinner") never
//     replaces a stored row that describes a DIFFERENT item: item identity is decided
//     in code, by the item words of the items column (or, without one, of the row's
//     non-date, non-key, non-figure fields) - same item replaces, different item
//     appends under a unique key ("dinner-2", "dinner-3", ... avoiding the keys the
//     day already holds) and the stored row is kept. key=date (a one-row-per-day
//     ledger) has no reused label and replaces as before. A re-send of a row already
//     stored under another key of the same day (a retry of a rekeyed write under its
//     original label) adds nothing: the receipt names the key it is already logged
//     under and the day's total is unchanged;
// Fields follow CSV quoting: "chicken, rice" is one field, "" inside quotes is a
// quote; a comma inside a balanced pair of parentheses is also part of its field
// (a migrated ledger's "Boba tea (no sugar, honey tapioca)"). A stored row with more
// fields than the header (a bare comma in its free text) is read when exactly one set of
// figures fits: its free-text column holds the extra fields, its date and figures land
// where the header names them, and none of the extra fields is a bare number. Otherwise,
// like a row with no YYYY-MM-DD date, it is kept but never matched or counted, and the
// total says so. A key compares without case or surrounding spaces. A day's total counts
// meal rows and nothing else: the header pasted again is no row, a row that states a total
// is never summed (and refused as a new row), and in a food log (a kcal or protein sum) a
// row repeating an earlier row's words that day counts once, with the last figures. The replies
// never quote a stored row's free text (only dates, the model's own keys, plain
// numbers and a header of plain labels), so the tool's output stays the device's own.

// One day's total of the sum column.
struct DayTotal {
  std::string date;   // YYYY-MM-DD
  double total = 0;
  int rows = 0;       // rows dated `date` that were counted
  int unread = 0;     // rows dated `date` whose sum field is not a plain number
  int totalRows = 0;  // rows dated `date` that state a total ("total", "daily total"): never summed
  int repeats = 0;    // rows dated `date` that repeat an earlier row's food: counted once
  std::vector<std::pair<uint32_t, double>> seen;   // a food log's counted rows: their words and figure
};

// What one new row did to the file.
struct RowChange {
  std::string date;   // its date ("" = the log has no date column)
  std::string key;    // its key value (the new unique key when the stored row of the sent key was kept)
  bool replaced = false;
  std::string was;    // the replaced row's sum figure ("" = none or not a plain number)
  std::string wasItems;   // the replaced row's item words, capped ("" = none): what the replace took away
  std::string keptKey;    // non-empty when the sent key named a stored row of a DIFFERENT item: that row was
                          // kept and this row was appended under a new unique key
  std::string dupKey;     // non-empty when the row is already stored under another key of the same day (a re-send
                          // of a rekeyed row under its original label): nothing was added
  int superseded = 0; // more stored rows of this identity set aside (replace: "all")
  std::string sameFoodDate;   // the newest earlier day with the same food at a far-off figure ("" = none)
  double sameFoodFigure = 0;  // that row's figure
  double figure = 0;          // this row's
};

struct CsvUpsert {
  bool ok = false;
  std::string err;         // why not, when !ok
  std::string out;         // the whole new file ("" = nothing to write); planCsvUpsert only
  bool changed = false;    // the file differs from what was stored
  int replaced = 0;        // rows whose date and key already existed and described the same item
  int appended = 0;        // rows with a new date or key
  int rekeyed = 0;         // appended rows that reused a stored key naming a different item (a new unique key)
  int duplicates = 0;      // rows already stored under another key of the same day: not added again
  int superseded = 0;      // duplicate stored rows set aside as #superseded: (replace: "all")
  bool hasTotal = false;   // today's total is known
  double todayTotal = 0;   // the sum column over today's rows
  std::vector<RowChange> changes;   // one per new row, in the text's order
  std::vector<DayTotal> days;       // today first (when known), then each other date written
  DayTotal protein;                 // a food log with a protein column: today's protein, read the same way
  std::string proteinCol;           // that column ("" = none: no line in the reply)
  bool cut = false;                 // the stored last row was cut mid-write
  std::string cutDate;              // its date, when it has one
  bool cutReplaced = false;         // a new row replaced it (else it is now a #cut: row)
  int undated = 0;                  // stored data rows with no YYYY-MM-DD date (in no total)
  std::string noTotal;              // why there is no day total ("" = there is one, or no sum)
};

// The keyed upsert as a stream, so the device merges a log of any size through a
// few hundred bytes: begin() with the new text, feed() every stored row in order
// (the first is the header), then finish(). Each call returns false once the call
// is refused (result().err says why); the caller then writes nothing.
class CsvUpsertStream {
 public:
  CsvUpsertStream(const std::string& keyCol, const std::string& sumCol, const std::string& today);
  // The time zone `today` is in ("GMT"; "UTC, no time zone is set"), named in an
  // after-today refusal so the model can tell the owner why. Optional; before begin().
  void setZone(const std::string& zoneAt) { zone_ = zoneAt; }
  // replace: "all" - a key naming several stored rows (an older log's repeated
  // corrections) replaces the first and sets the others aside as "#superseded: "
  // rows instead of refusing. Optional; before begin().
  void setSupersede(bool on) { supersede_ = on; }
  // Parse and check the new rows.
  bool begin(const std::string& newText);
  // One stored row without its line end; `complete` = false for a final row that
  // had none. `out` receives the bytes to write for it (line end included).
  bool feed(const std::string& row, bool complete, std::string& out);
  // After the last stored row: the rows still to add (and the header of a new file).
  bool finish(std::string& out);
  const CsvUpsert& result() const { return r_; }

 private:
  struct NewRow {
    std::string raw, id, date, key;
    std::vector<std::string> fields;
    bool used = false;   // it replaced a stored row
    bool rekey = false;  // its key named a stored row of a different item: append under a new unique key
    bool dup = false;    // already stored under another key of the same day: add nothing
    std::string was;     // that row's sum figure
    std::string wasItems;   // that row's item words, capped
    std::string keptKey;    // the sent key that was kept by the stored row (rekey)
    std::string dupKey;     // the stored key this row is already logged under (dup)
    int superseded = 0;  // more stored rows of its identity set aside
    std::vector<std::string> foodWords;   // a food log: the words of its items
    std::string sameFoodDate;
    double figure = 0, sameFoodFigure = 0;
  };
  bool fail(const std::string& why);
  bool parseHeader(const std::string& line);
  void findFoodColumns();
  bool parseNewRow(size_t n, const std::string& raw, NewRow& row);
  bool findDateColumn(const std::vector<NewRow>& rows);
  bool checkNewRow(size_t n, NewRow& row);
  void planDays();
  bool feedHeader(const std::string& row, bool complete, std::string& out);
  bool feedData(const std::string& row, bool complete, std::string& out);
  void feedCut(const std::vector<std::string>& f, const std::string& row, NewRow* hit, std::string& out);
  NewRow* match(const std::string& id);
  void replaceWith(NewRow& n, const std::vector<std::string>& stored, const std::string& storedRaw, bool identical,
                   bool complete, std::string& out);
  bool secondMatch(NewRow& n, const std::string& row, std::string& out);
  void countUndated(const std::vector<std::string>& f);
  std::string identity(const std::vector<std::string>& f) const;
  std::string itemTextOf(const std::vector<std::string>& f) const;   // what a row says it holds
  std::vector<std::string> itemWords(const std::vector<std::string>& f) const;   // its item words
  bool sameWords(const std::vector<std::string>& a, const std::vector<std::string>& b) const;   // the same-item rule
  bool sameItem(const NewRow& n, const std::vector<std::string>& stored) const;
  std::string itemWordsOf(const std::vector<std::string>& f) const;   // capped, for the receipt
  bool rekeyRow(NewRow& n);   // assign a unique key; false = none free
  bool finishRow(NewRow& n, std::string& out);   // one pending row at finish(): dup/rekey/append
  void noteTakenKey(const std::vector<std::string>& f);   // record a stored row's key for the day
  bool feedRekey(NewRow& hit, const std::vector<std::string>& f, const std::string& row, bool complete,
                 std::string& out);   // a reused label naming a different item: keep the stored row
  // Lane UPSERT fix pass: a stored row's day, key and item words, remembered (capped) so a
  // re-send of a row already stored under another key of the same day is recognised.
  struct SeenRow {
    std::string date, key;
    std::vector<std::string> words;
  };
  void recordSeen(const std::vector<std::string>& f);
  void markDupAgainstStored(const std::vector<std::string>& f);   // a pending rekey row that is this stored row
  void tally(const std::vector<std::string>& f);
  void watchSameFood(const std::vector<std::string>& f);
  void noteSameFood(NewRow& n, const std::string& day, double was) const;
  std::string keyCol_, sumCol_, today_, zone_;
  std::vector<std::string> header_;
  std::string headerRaw_;
  int keyIdx_ = -1, sumIdx_ = -1, dateIdx_ = -1, itemsIdx_ = -1, proteinIdx_ = -1;
  std::vector<NewRow> rows_;
  std::vector<std::string> prev_;   // the last well-formed stored row (judges a cut last row)
  bool begun_ = false, sawHeader_ = false, failed_ = false, supersede_ = false;
  size_t storedRows_ = 0, outRows_ = 0;
  std::map<std::string, std::set<std::string>> taken_;   // keys each date already holds (capped)
  size_t takenCount_ = 0;
  std::set<std::string> assigned_;   // keys this call assigned (rekeyed rows)
  std::vector<SeenRow> seen_;   // stored rows seen so far (capped): the same-day duplicate guard
  CsvUpsert r_;
};

// The whole-text form of the upsert (nimbusd, the host lab, the tests): merge
// `newText` (one header row, then data rows) into `existing` keyed by date plus
// `keyCol`; with `sumCol`, report the day totals. `out` is the new file.
CsvUpsert planCsvUpsert(const std::string& existing, const std::string& newText,
                        const std::string& keyCol, const std::string& sumCol,
                        const std::string& today, bool supersede = false);

// The same food twice (CUM-864: one drink, 225 kcal on 29 Oct and 90 on 1 Nov, each with its own
// confident sum). A keyed food log's reply names an earlier day's row of the same food when its
// figure is off by more than a quarter. The words: lowercase, singular, no sizes or filler.
std::vector<std::string> foodWordsOf(const std::string& text);
// Enough of the words in common to be one food: at least two (or the only ones) and most of the shorter list.
bool sameFoodWords(const std::vector<std::string>& a, const std::vector<std::string>& b);
// One of the words, spelling slips allowed ("cappucino").
bool foodWordIn(const std::string& word, const std::vector<std::string>& words);

// The columns of a CSV's header row (no byte order mark, spaces trimmed).
std::vector<std::string> csvHeaderOf(const std::string& row);
// The columns of a stored row as `header` names them, read the way the day total reads
// it: a bare comma in its free text is folded back. Empty when its columns cannot be
// trusted (a broken quote, wider than the header with no single reading).
std::vector<std::string> csvStoredRow(const std::string& row, const std::vector<std::string>& header);
// A cell read as the day total reads a figure: a plain number or a quoted "1,045".
bool csvFigure(const std::string& cell, double& out);
// Does this stored row state a total instead of a thing eaten? (never counted)
bool csvStatesTotal(const std::vector<std::string>& fields);

// The date column of a header given some of its rows: by name (date, day, or a
// name holding date, day or time) when that column holds YYYY-MM-DD dates in
// those rows, else the first column that does; -1 = none. The one rule the
// keyed upsert and the files.edit row diff share, so the two can never
// disagree on which column is the date.
int csvDateColumnOf(const std::vector<std::string>& header,
                    const std::vector<std::vector<std::string>>& rows);

// One day's total as a stream (the files.read tail view): feed every row of the
// file in order, header first.
class CsvDayTally {
 public:
  CsvDayTally(const std::string& sumCol, const std::string& day);
  void feed(const std::string& row, bool complete);
  // The header names a date column and the sum column (else whyNot() says which).
  bool ok() const;
  std::string whyNot() const;
  const DayTotal& total() const { return t_; }
  int undated() const { return undated_; }   // data rows with no YYYY-MM-DD date
  // Every stored row dated `day`, counted or not, as it is read (the food ledger
  // shows today's rows with the same reading the total uses). Optional.
  void setDayRowSink(std::function<void(const std::string& row)> sink) { dayRow_ = std::move(sink); }

 private:
  void readHeader(const std::string& row);
  std::string rowDay(const std::vector<std::string>& f);
  std::function<void(const std::string& row)> dayRow_;
  std::string sumCol_, day_;
  std::vector<std::string> header_, prev_;
  int dateIdx_ = -1, sumIdx_ = -1;
  size_t width_ = 0;
  bool sawHeader_ = false;
  int undated_ = 0;
  DayTotal t_;
};

// The day total alone over a whole text: the sum of `sumCol` over the rows dated
// `today`. False when the file has no date column or no such sum column.
bool csvDayTotal(const std::string& fileText, const std::string& sumCol,
                 const std::string& today, double& out);

// One day's total as the reply states it, over a whole text (nimbusd, the lab): the same
// words as the device's files.read {tail, sum} and the upsert reply.
std::string csvDayTotalLine(const std::string& fileText, const std::string& sumCol, const std::string& today,
                            const std::string& zone);

// Rows out of bytes read in chunks: push() each chunk, then end(). Each complete
// row (its line end, and a CR before it, stripped) goes to onRow(row, true); a
// final row with no line end goes to onRow(row, false). onRow returning false, or
// a row longer than kRowMaxBytes, stops the stream (ok() false).
class RowSplitter {
 public:
  static constexpr size_t kRowMaxBytes = 16384;
  using OnRow = std::function<bool(const std::string& row, bool complete)>;
  explicit RowSplitter(OnRow onRow) : onRow_(std::move(onRow)) {}
  bool push(const char* data, size_t n);
  bool end();
  bool ok() const { return ok_; }
  bool tooLong() const { return tooLong_; }

 private:
  OnRow onRow_;
  std::string cur_;
  bool ok_ = true, tooLong_ = false;
};

// Reading a stored file of a known size in chunks (the device reads the card 512
// bytes at a time). `read` returns the bytes it read, <= 0 at a failure or the end.
enum class StreamEnd { Done, ReadFailed, Stopped };
using ReadFn = std::function<int(char* dst, size_t n)>;
using WriteFn = std::function<bool(const std::string& bytes)>;   // false = a short write
// Feed `size` bytes through `rows`. ReadFailed when the read ends short of `size`
// (a card fault is never taken for a shorter file); Stopped when `rows` stopped.
StreamEnd streamRows(size_t size, const ReadFn& read, RowSplitter& rows);

// The keyed merge end to end: the stored file (`size` bytes through `read`; none
// when !stored) through `m`, every output byte to `write`. False with `err` when
// anything failed - a short read, a refused row, a short write: the caller then
// discards what was written and the stored file stays as it was.
bool mergeKeyed(CsvUpsertStream& m, bool stored, size_t size, const ReadFn& read,
                const WriteFn& write, std::string& err);

// The YYYY-MM-DD day a date field starts with ("2026-10-25 08:30" -> "2026-10-25"),
// or "" when it does not start with one.
std::string isoDayOf(const std::string& field);

// A CSV's final row that has no line end: is it PROVABLY cut by a power loss (a
// quote left open, fewer fields than `header`, or no plain number where the
// well-formed row before it, `prev`, holds one)? A well-formed row only lacks its
// line end (a file saved without one) and is complete.
bool csvLastRowCut(const std::string& header, const std::string& prev, const std::string& row);

// The log's day: the device's local date ("YYYY-MM-DD", "" while the clock is not
// set) in the process time zone, which the device sets from its time-zone
// setting, and the zone's name for the reply ("BST", "GMT"). `tzSet` false = no
// time zone is set and the device runs on UTC: the zone then says so, so the
// owner can see which day boundary a total uses.
struct LogDay {
  std::string date;
  std::string zone;
};
LogDay logDayOf(time_t now, bool tzSet);

// The reply lines. `zone` names the day's time zone ("GMT", "UTC, no time zone is
// set"); `where` is "project/name".
std::string renderDayTotal(const DayTotal& d, const std::string& sumCol, bool isToday,
                           const std::string& zone);
std::string renderUpsertReply(const CsvUpsert& up, const std::string& where,
                              const std::string& sumCol, const std::string& zone);
// "[N rows have no YYYY-MM-DD date and count toward no day's total]"
std::string renderUndated(int rows);

}  // namespace nimbus::orch
