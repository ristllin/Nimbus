#pragma once
#include <cstddef>
#include <ctime>
#include <functional>
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
// The largest stored log a keyed update or a day total streams (about 20,000
// rows): each pass holds the store lock, so a bigger file is refused with the
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
AppendPlan planAppend(const std::string& text, size_t fileBytes, char lastByte);

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
//   - an update never touches another day's row: the date is part of the identity;
//     a log with no date column is keyed by the key column alone;
//   - a key that names two or more stored rows (key "date" on a day with several
//     rows, a log that already holds duplicates), or two rows of one call with the
//     same identity, is ambiguous and refused;
//   - the text's header must equal the stored header (same columns, same order);
//   - every new row has the header's field count, a key value, a YYYY-MM-DD date
//     not after today (when the log has a date column and today is known) and a
//     plain number in the sum column;
//   - a stored last row with no line end (cut by a power loss) is never taken as
//     complete: a new row with its identity replaces it, otherwise it is kept as a
//     "#cut: " comment row that never matches or counts again;
//   - a stored row starting with '#' is a comment: kept, never matched or counted.
// Fields follow CSV quoting: "chicken, rice" is one field, "" inside quotes is a
// quote. A stored row with more fields than the header (an unquoted comma) is kept
// but never matched or counted, and the total says so; so is a row with no
// YYYY-MM-DD date. A key compares without case or surrounding spaces. The replies
// never quote a stored row's free text (only dates, the model's own keys and plain
// numbers), so the tool's output stays the device's own.

// One day's total of the sum column.
struct DayTotal {
  std::string date;   // YYYY-MM-DD
  double total = 0;
  int rows = 0;       // rows dated `date` that were counted
  int unread = 0;     // rows dated `date` whose sum field is not a plain number
};

// What one new row did to the file.
struct RowChange {
  std::string date;   // its date ("" = the log has no date column)
  std::string key;    // its key value
  bool replaced = false;
  std::string was;    // the replaced row's sum figure ("" = none or not a plain number)
};

struct CsvUpsert {
  bool ok = false;
  std::string err;         // why not, when !ok
  std::string out;         // the whole new file ("" = nothing to write); planCsvUpsert only
  bool changed = false;    // the file differs from what was stored
  int replaced = 0;        // rows whose date and key already existed
  int appended = 0;        // rows with a new date or key
  bool hasTotal = false;   // today's total is known
  double todayTotal = 0;   // the sum column over today's rows
  std::vector<RowChange> changes;   // one per new row, in the text's order
  std::vector<DayTotal> days;       // today first (when known), then each other date written
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
  void setZone(const std::string& zone) { zone_ = zone; }
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
    std::string was;     // that row's sum figure
  };
  bool fail(const std::string& why);
  bool checkNewRow(size_t n, const std::string& raw, NewRow& row);
  void planDays();
  bool feedHeader(const std::string& row, bool complete, std::string& out);
  bool feedData(const std::string& row, bool complete, std::string& out);
  void feedCut(const std::vector<std::string>& f, const std::string& row, NewRow* hit, std::string& out);
  NewRow* match(const std::string& id);
  void replaceWith(NewRow& n, const std::vector<std::string>& stored, bool same, std::string& out);
  bool ambiguous(const NewRow& n);
  void countUndated(const std::vector<std::string>& f);
  std::string identity(const std::vector<std::string>& f) const;
  void tally(const std::vector<std::string>& f);
  std::string keyCol_, sumCol_, today_, zone_;
  std::vector<std::string> header_;
  std::string headerRaw_;
  int keyIdx_ = -1, sumIdx_ = -1, dateIdx_ = -1;
  std::vector<NewRow> rows_;
  bool begun_ = false, sawHeader_ = false, failed_ = false;
  size_t storedRows_ = 0, outRows_ = 0;
  CsvUpsert r_;
};

// The whole-text form of the upsert (nimbusd, the host lab, the tests): merge
// `newText` (one header row, then data rows) into `existing` keyed by date plus
// `keyCol`; with `sumCol`, report the day totals. `out` is the new file.
CsvUpsert planCsvUpsert(const std::string& existing, const std::string& newText,
                        const std::string& keyCol, const std::string& sumCol,
                        const std::string& today);

// One day's total as a stream (the files.read tail view): feed every row of the
// file in order, header first.
class CsvDayTally {
 public:
  CsvDayTally(const std::string& sumCol, const std::string& day);
  void feed(const std::string& row, bool complete);
  // The header names a date column and the sum column (else whyNot() says which).
  bool ok() const { return dateIdx_ >= 0 && sumIdx_ >= 0 && !day_.empty(); }
  std::string whyNot() const;
  const DayTotal& total() const { return t_; }
  int undated() const { return undated_; }   // data rows with no YYYY-MM-DD date

 private:
  std::string sumCol_, day_;
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
