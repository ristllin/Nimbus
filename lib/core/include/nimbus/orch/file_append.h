#pragma once
#include <cstddef>
#include <cstdint>
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
// The tail view: default and maximum row counts, and the window read from the
// end of the file (it fits one tool result with room for the header lines).
inline constexpr size_t kTailDefaultRows = 20;
inline constexpr size_t kTailMaxRows = 200;
inline constexpr size_t kTailWindowBytes = 7680;

// The bytes to write for one append, or an error. A pure append on every path (owner
// ruling 41, ruling 42 item 7): whatever the file is called and whatever it holds, the
// rows go on the end as sent; no row is merged, replaced, refused or set aside, and no
// stored byte is rewritten. Rows are normalized to "\n" line ends and always end with
// one. When the file's last byte is not a line end (a row cut by a power loss or a hand
// edit) the new rows start on a fresh line and `repairedCut` says so - the last row is
// reported, never silently joined to the new one. The only errors are no text, text over
// kAppendMaxBytes, and a refused allocation (nothing is planned; the caller writes nothing).
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

AppendPlan planAppend(const std::string& text, size_t fileBytes, char lastByte);

// The one error of a files.append call that carries an argument the pure append does not
// take (an older `key`, `sum` or `replace`, of any value): it names them and nothing is
// written, so a correction meant as a replace never lands as a second row. "" when the call
// carries none. `named` lists the argument names the call carries.
std::string appendArgsError(const std::vector<std::string>& named);
inline constexpr const char* kAppendOldArgs[] = {"key", "sum", "replace"};

// A fact for the reply of an append to a CSV: how many of the appended rows have a field
// count other than the header's (a column filter cannot compare such a row). `stored` is the file's
// first line ("" for a new file: the appended text's own first line is then the header). ""
// when every row fits. The rows land either way: this only says so.
std::string csvWidthNote(const std::string& stored, const std::string& appended);
// The error a refused allocation gives (planAppend and every append binding).
inline constexpr const char kAppendNoMemory[] = "out of memory";   // short: no allocation to say it

// The tail view over the last bytes of a file. `window` is the end of the file;
// `windowStart` is its offset in the file (0 = the window is the whole file).
// A window that starts mid-file drops its first, partial row.
struct TailView {
  std::vector<std::string> rows;   // the last N complete rows, oldest first
  bool earlier = false;            // rows above these exist and are not shown
  bool lastCut = false;            // the file does not end in a line end: the
                                   // last row is incomplete (cut mid-write)
  size_t firstAt = 0;              // the file byte the first shown row starts at (RB-TR1:
                                   // the reply names bytes 0 to it as the rows not shown)
};
TailView tailRows(const std::string& window, size_t windowStart, size_t n);

// The tool text: a heading naming what is shown, the optional header row (a CSV's
// first line), the rows, and a closing note when the last row is incomplete.
std::string renderTail(const TailView& v, const std::string& name, size_t fileBytes,
                       const std::string& headerRow);


// ---- reading a CSV ---------------------------------------------------------------
// Fields follow CSV quoting: "chicken, rice" is one field, "" inside quotes is a quote; a
// comma inside a balanced pair of parentheses is also part of its field ("tea (no sugar,
// oat milk)", as files written before v5.2.0 hold such rows). A row starting with '#' is a
// comment. Nothing here totals a column (ruling 95 removed the day total with sum).

// The columns of a CSV's header row (no byte order mark, spaces trimmed).
std::vector<std::string> csvHeaderOf(const std::string& row);
// The columns of a stored row as `header` names them, read the way the row reader reads
// it. Empty when its columns cannot be trusted (a broken quote, wider than the header).
std::vector<std::string> csvStoredRow(const std::string& row, const std::vector<std::string>& header);
// A cell read as a figure: a plain number or a quoted "1,045".
bool csvFigure(const std::string& cell, double& out);

// The date column of a header given some of its rows: by name (date, day, or a
// name holding date, day or time) when that column holds YYYY-MM-DD dates in
// those rows, else the first column that does; -1 = none. The one rule the
// column filter and the files.edit row diff share, so the two can never
// disagree on which column is the date.
int csvDateColumnOf(const std::vector<std::string>& header,
                    const std::vector<std::vector<std::string>>& rows);

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
// Feed `size` bytes through `rows`. ReadFailed when the read ends short of `size`
// (a card fault is never taken for a shorter file); Stopped when `rows` stopped.
StreamEnd streamRows(size_t size, const ReadFn& read, RowSplitter& rows);

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
// owner can see which day boundary the log uses.
struct LogDay {
  std::string date;
  std::string zone;
};
LogDay logDayOf(time_t now, bool tzSet);

}  // namespace nimbus::orch
