#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// file_aids - the general aids that make tracking in plain CSV files reliable on small
// models (ruling 42): a read that filters rows by a column range (it totals nothing: ruling
// 95 removed sum), an edit that picks one of several matches by line number, the
// facts an append states, and the torn-tail note. Shared by both engines and nimbusd: the
// bindings move bytes, this library decides and renders, so every host gives
// byte-identical results on the same file.
//
// No size cap: the scan and the edit stream the file in chunks of kAidChunk bytes through
// AidSource (one card-lock hold per call), so a file of any size is read in bounded
// memory. Every working block comes from workingAlloc (psram_alloc.h) and is checked: a
// refused allocation, at any point, ends the call with kAidNoMemory and changes nothing.
// Nothing is dropped silently: every row the scan read is matched, out of range, or
// counted under a named reason with its line numbers.
//
// Portable, Arduino-free, host-tested (test_file_tools_csv, test_file_edit_line,
// test_file_append_facts). Engine-2 only until phase 2 links it into both engines.
namespace nimbus::orch::aids {

// ---- limits -------------------------------------------------------------------------
// Bytes per AidSource read (and per AidSink part write): one card-lock hold each.
inline constexpr size_t kAidChunk = 4096;
// The longest CSV record the scan keeps (bytes, line end excluded). A longer record is
// counted "too long" with its line and takes no part in the match.
inline constexpr size_t kCsvRecordMax = 8192;
// The most bytes of shown rows one read returns (a larger limit is this).
inline constexpr size_t kCsvShowMax = 1u << 20;
// Line numbers a list in a result names before "+N".
inline constexpr size_t kAidListMax = 8;
// Line numbers one identical-line group names before "+N".
inline constexpr size_t kAidGroupLinesMax = 4;
// Matching rows checked for identical lines; past this the result says so.
inline constexpr size_t kCsvIdenticalMax = 8192;
// The text of a line quoted in an edit error (bytes, cut on a UTF-8 boundary).
inline constexpr size_t kAidQuoteMax = 120;
// The header quoted in an unknown-column error, and line 1 in a no-header error.
inline constexpr size_t kAidHeaderQuoteMax = 200;
inline constexpr size_t kAidLineOneQuoteMax = 80;
// append: the file's last bytes read for the identical-line fact, and the lines compared.
inline constexpr size_t kAppendTailBytes = 8192;
inline constexpr size_t kAppendTailLines = 64;

// The error every call returns when a working block is refused (nothing changed).
inline constexpr const char* kAidNoMemory = "not enough memory for this call; nothing changed";

// ---- the card seam ----------------------------------------------------------------
// A binding adapts its card (FileToolsIo, files.read, nimbusd's POSIX file) to these.
// Each call is one card-lock hold and holds no lock across calls.
class AidSource {
 public:
  virtual ~AidSource() = default;
  // Read up to len bytes at offset into out; got is the count, less than len only at the
  // end of the file (the drivers read until a short read, so growth is seen). False is a
  // card fault.
  virtual bool readAt(uint64_t offset, size_t len, char* out, size_t& got) = 0;
};
// The part file an edit writes, then swaps in place of the file (file_swap.h): until
// partSwap returns true the stored file is exactly as it was.
class AidSink {
 public:
  virtual ~AidSink() = default;
  // Write bytes to the part file (first: start it empty). At most kAidChunk per call.
  virtual bool partWrite(std::string_view bytes, bool first) = 0;
  // Put the finished part file in place of the file. False: the file is as it was.
  virtual bool partSwap() = 0;
  // Remove the part file (after any failure).
  virtual void partDiscard() = 0;
  // Before any part write: may the edit leave a file of newSize bytes (the exact size pass
  // 1 computed)? A binding checks ownership, quota and room here. False: refusal() says why.
  virtual bool admit(uint64_t newSize, size_t matches) {
    (void)newSize;
    (void)matches;
    return true;
  }
  // Why the last admit or partSwap refused, when it was not a card fault (a binding's
  // ownership, quota or room check); "" means "card write failed; nothing changed".
  virtual std::string refusal() const { return std::string(); }
};

// ---- exact decimals ---------------------------------------------------------------
// A number is optional spaces, an optional sign, digits with an optional point (".5",
// "5." and "5" are numbers), optional spaces; nothing else (no exponent, no grouping, no
// unit). It is held as int64 thousandths; a fourth decimal or more rounds half away from
// zero and the cell counts as rounded. Over the int64 range is not a number.
struct Dec3 {
  int64_t milli = 0;
};
enum class NumParse : uint8_t { Ok, Rounded, Blank, NotNumber };
NumParse parseDec3(std::string_view cell, Dec3& out);
// "24.3", "310", "-0.5", "0.125": trailing zeros and a bare point dropped; never "-0".
std::string renderDec3(Dec3 v);

// ---- read_file column/from/to ------------------------------------------------------
// The query. column names a header field; a row matches when its field is in from..to
// (to defaults to from). skipRows and limit page the rows shown. A selected row that does
// not parse (a wrong field count, a key not compared) is shown beside the matching rows in
// file order and named in the tail; it is no matching row and does not move the paging.
//
// The range rule, per row key k (the field with spaces trimmed):
//   - numeric: when from, to and k are all numbers, from <= k <= to as numbers (a bound
//     with more than 3 decimals is a query error; a key with more is "not compared");
//   - else by prefix: k is comparable with a bound b when it is at least as long as b,
//     has b's shape over b's length (a digit where b has a digit, a letter where b has a
//     letter, the same byte elsewhere) and, when b ends in a digit, does not go on with a
//     digit; it matches when cut to the length of from it is >= from and cut to the length
//     of to it is <= to (bytes). So from=to="2026-11-19" is one day, "2026-11" a month,
//     2026-11-13..2026-11-19 a week, and "19/11/2026", "2026-11-1" against a day, or a
//     blank key, is "not compared" (counted with its line), never quietly out of range.
struct CsvQuery {
  std::string column;            // "" = no filter
  std::string from, to;          // to "" = from
  size_t skipRows = 0;           // matching rows not shown (read_file offset)
  size_t limit = 32768;          // bytes of shown lines (read_file limit past the page floor, at most kCsvShowMax); the first row of a page is always shown
};
// "" when the query is well formed, else the error (no file is read).
std::string checkCsvQuery(const CsvQuery& q);

// The line numbers a reason names (the first kAidListMax) and the full count.
struct LineList {
  size_t count = 0;
  std::vector<uint32_t> lines;  // at most kAidListMax
  // For records only: the line each listed record ends on, parallel to lines (a quoted
  // field with line ends, or a quote that never closes, spans several lines).
  std::vector<uint32_t> ends;
};
// One group of matching rows with identical text.
struct LineGroup {
  size_t count = 0;
  std::vector<uint32_t> lines;  // the first kAidGroupLinesMax
};

// What a scan found. A line whose first non-blank byte is '#' is a COMMENT line wherever it
// stands (ruling 49: a file-format convention, not a domain rule): never a row,
// never "not compared", counted once with its runs. The header is the first record that is
// not a comment. Every data record (a CSV record after the header that is not an empty line)
// is in exactly one of: matched, outOfRange, notCompared, fieldCount, fieldCountOutside,
// tooLong. A row whose field count differs from the header's is still placed by its key (the
// field at the header's key index): plainly out of range, it is only counted
// (fieldCountOutside, no line list) unless it spans several lines (a quote that swallowed
// rows); in range or not comparable, it is named by line
// (fieldCount), so a day's result never lists bad rows of other days.
struct CsvScanResult {
  uint64_t bytes = 0;          // bytes scanned (the file's size)
  uint64_t lines = 0;          // physical lines in the file
  size_t records = 0;          // data records
  size_t matched = 0;
  size_t outOfRange = 0;
  LineList notCompared;        // key blank or not comparable with from/to
  LineList fieldCount;         // field count differs from the header's
  size_t headerFields = 0;
  LineList tooLong;            // over kCsvRecordMax
  size_t emptyLines = 0;       // empty physical lines outside a quoted field
  size_t fieldCountOutside = 0;  // rows with a wrong field count whose key is plainly out of range
  size_t commentLines = 0;     // lines whose first non-blank byte is '#' (never a row)
  LineList commentSpans;       // their runs of consecutive lines (count = runs; ends = each run's last line)
  std::vector<LineGroup> identical;  // the first kAidListMax groups, in first-line order
  size_t identicalGroups = 0;  // all groups found
  size_t identicalChecked = 0; // matching rows checked (< matched: the table was full)
  uint32_t tornLine = 0;       // the last line when the file does not end in a line end
  uint32_t openQuoteLine = 0;  // the last record's first line when the file ends inside its quoted field
  size_t shownFirst = 0;       // 1-based index of the first matching row shown (0 = none)
  size_t shownLast = 0;
  // header then the shown rows (matching rows and the selected rows that do not parse, in file
  // order): "L<n> <record>\n" each, record bytes as stored (a quoted
  // field's line ends kept, the record's own line end and a header BOM left out)
  std::string body;
};

// The outcome of a scan: ok and the result, or the error (with !ok nothing is shown).
struct CsvScanOutcome {
  bool ok = false;
  std::string error;
  CsvScanResult result;
};

// Scan the file of `size` bytes from src in kAidChunk reads (until a short read: a file that
// grew is read whole; one that reads short of `size` is a card fault). path is the stored path
// the errors name. A query that fails checkCsvQuery is returned as its error.
CsvScanOutcome scanCsv(AidSource& src, uint64_t size, const std::string& path, const CsvQuery& q);

// The streaming scanner the driver uses (exposed so the property tests feed arbitrary
// chunkings and prove the result never depends on them).
class CsvScanner {
 public:
  // q must outlive the scanner (it is read, not copied). into: where the result is built
  // (nullptr: the scanner's own); it means nothing after a false from feed or finish.
  explicit CsvScanner(const CsvQuery& q, const std::string& path, CsvScanResult* into = nullptr);
  ~CsvScanner();
  CsvScanner(const CsvScanner&) = delete;
  CsvScanner& operator=(const CsvScanner&) = delete;
  // False with error() set: a refused block, a bad query, no header, an unknown column,
  // a NUL byte. Stop feeding after false.
  bool feed(const char* data, size_t len);
  bool finish();
  const std::string& error() const { return err_; }
  CsvScanResult& result() { return res_; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  std::string err_;
  CsvScanResult own_;
  CsvScanResult& res_;
};

// The tail read_file prints after the shown rows, one line (no line end):
//   rows <m> of <n>[ where <column> <from>..<to>]
//   [; shown <a>-<b> | ; shown none]                       (only when not every row is shown)
//   [; <k> comment line(s) skipped (lines <a>-<b>, <c>, ... +N runs)]
//   [; not compared <k> (lines <l>, ... +N: <column> blank or not like <from>)]
//   [; skipped <k> (lines <l>, ... +N: field count is not the header's <h>)]
//   [; <k> other row(s) outside the range have a wrong field count]
//   [; too long <k> (lines <l>, ... +N: over 8192 B)]
//   [; empty lines <k>]
//   [; identical lines <g> groups (<l>=<l>=<l>+N, <l>=<l>, ... +N)]
//   [; identical lines checked in the first <c> matching rows only]
//   [; last line <l> has no newline (a write may have been cut short)]
//   [; the row at line <l> ends inside a quoted field (a write may have been cut short)]
// A record list names a record spanning lines as "<first>-<last>" ("lines 2-22").
std::string renderCsvTail(const CsvQuery& q, const CsvScanResult& r);
// The whole filtered read result after the binding's head line:
//   "\n" + body (the header line is "L<n> <header>", n its line) + tail + ("\nnext_offset=<k>" when matching rows remain unshown).
std::string renderCsvRead(const CsvQuery& q, const CsvScanResult& r);

// ---- edit_file with line ----------------------------------------------------------
// Exact find and replace, streamed. Matches are found left to right without overlap.
//   - all=false, no line: exactly one match is replaced; several are an error naming
//     their lines;
//   - line: the match that starts at the start of that line and covers whole lines
//     (it ends at a line end or the end of the file) is replaced; else, for an old with no
//     line end in it, the one match that starts on that line, an exact part of it (ruling
//     88; there twice or more is an error that says so). Any other row, even one holding
//     old as part of its text, is never touched: a stale number fails with the current
//     match lines;
//   - all=true: every match (line with all is an error).
// A remove (new "") of whole lines takes the line end after them with it. ifSha8, when
// set, must be the file's current sha8 or nothing is written.
struct EditRequest {
  std::string oldText, newText;
  bool all = false;
  uint32_t line = 0;      // 0 = no pick
  std::string ifSha8;     // "" = no check; else 8 lowercase hex (sha8Of)
  uint64_t maxSize = 0;   // 0 = none; else the largest file the edit may leave (checked before any write)
  // Engine 1's files.edit (file_aids_e1.h): count > 0 with no all and no line replaces the
  // first `count` matches (its documented default is the first match, never an error).
  uint32_t count = 0;
  std::string trailer;            // bytes the new file gains at its end (engine 1's history rows)
  const char* oldName = "old";    // the argument's name in the texts ("find" on engine 1)
};
// "" when the request is well formed (old not empty, old != new, line with no all).
std::string checkEdit(const EditRequest& e);

struct EditOutcome {
  bool ok = false;
  std::string error;          // the error text when !ok (the file is unchanged)
  size_t replaced = 0;
  uint64_t oldSize = 0, newSize = 0;
  uint64_t newLines = 0;      // physical lines in the new file
  uint64_t newHash = 0;       // FNV-1a 64 of the new file (blob_store.h's hash)
  std::vector<uint32_t> at;   // new-file line where each replacement starts; for a removal
                              // (new ""), the line it was removed from (the first kAidListMax)
  int64_t shift = 0;          // one replacement: lines after it moved by this many
  uint32_t shiftFrom = 0;     // the first OLD line number that moved (0 = none)
  LineList stillAt;           // matches of old in the new file, new line numbers
  size_t found = 0;           // matches in the file before the edit
  const char* oldName = "old";
};

// Edit the file of `size` bytes with hash `hash` (FNV-1a 64; 0 unknown): pass 1 reads
// and decides, pass 2 streams the new bytes to the part file and swaps it in. Each pass
// reads src in kAidChunk calls; pass 2 recounts and abandons the part when the file
// changed between the passes. A fault or refusal at any step leaves the old file.
EditOutcome editFile(AidSource& src, AidSink& sink, uint64_t size, uint64_t hash, const std::string& path,
                     const EditRequest& e);

// The line facts of an edit that landed (engine 1 appends them to its own reply head):
//   "; at line(s) <l>, ...; <n> lines[; lines that were <s> and later are now <d> lower|higher]
//   [; <old> is still at line(s) <l>, ...]"
std::string renderEditLines(const EditOutcome& o);

// The non-overlapping matches of old in the file (the same finder the edit uses). False: a
// card fault or a refused block.
bool countMatches(AidSource& src, std::string_view old, size_t& matches);

// The edit result, one line:
//   replaced <k> in <path> at line <l>[, <l>... +N]; file is <M> B (was <N> B), <n> lines,
//   sha8=<hex>[; lines that were <s> and later are now <d> lower|higher]
//   [; old is still at line(s) <l>, ... +N]
// Errors (nothing written):
//   old is not in <path> (<N> B)
//   old is in <path> <k> times, at lines <l>, ... +N; line picks one, all=true replaces all
//   no match on line <l>; matches at line(s) <l>, ... +N
//   old is on line <l> <k> times; line <l> is: <text>
//   old is part of line <l>, not the whole line; line <l> is: <text>   (an old with a line end, not at a line start)
//   <path> changed since sha8=<asked> (now <hex>); nothing written
//   <path> changed during the edit; nothing written
//   <path> would be <N> B; an edit here leaves files up to <M> B
//   card read failed | card write failed; nothing changed | kAidNoMemory
std::string renderEdit(const std::string& path, const EditOutcome& o);

// ---- append_file facts ------------------------------------------------------------
// What an append states besides the bytes it wrote. tail is the file's last bytes (up to
// kAppendTailBytes, all of it when the file is smaller: tailIsWhole), fileLines the
// file's physical line count (0 unknown, past the binding's count limit).
struct AppendFacts {
  bool fence = false;        // write "\n" before the text: the file ends without one
  std::string note;          // "" or the result's suffix: "; added a newline before the text"
                             // then "; identical to line <l>" (one line appended) or
                             // "; line <a> is identical to line <l>, ... +N" (several);
                             // with fileLines unknown: "the line <k> above it"
};
// Pure: decide the fence and the identical-line facts. Each appended line (not empty) is
// compared with the file's last kAppendTailLines lines and the lines appended before it;
// the nearest identical line is named. An identical line is still appended.
AppendFacts appendFacts(std::string_view tail, bool tailIsWhole, uint64_t fileSize, uint64_t fileLines,
                        std::string_view text);

// ---- the torn-tail note -----------------------------------------------------------
// A file whose last byte is not a line end may hold a write cut short.
inline constexpr const char* kTornTail = "last line has no newline (a write may have been cut short)";
inline bool endsTorn(uint64_t size, char lastByte) { return size > 0 && lastByte != '\n'; }

// ---- small shared rules -----------------------------------------------------------
// "lines 7, 9, 12 +3" style list ("line 7" for one).
std::string renderLines(const LineList& l);
// FNV-1a 64 over bytes, continued (blob_store.h's content hash).
uint64_t fnv64(uint64_t hash, std::string_view bytes);
inline constexpr uint64_t kFnv64Basis = 1469598103934665603ULL;
std::string sha8Hex(uint64_t hash);  // 8 lowercase hex of the high 32 bits, as sha8Of ("unknown" for 0)

// ---- text building (shared by the renderers; out of line to keep call sites small) ---
namespace detail {
// The parts, one after the other.
std::string join(std::initializer_list<std::string_view> parts);
// A number as text, held in place for one expression.
struct Num {
  explicit Num(uint64_t v);
  operator std::string_view() const { return std::string_view(b, n); }
  char b[24];
  size_t n;
};
// At most n bytes of s, cut before a UTF-8 continuation byte, with " ..." when cut.
std::string quoteCut(std::string_view s, size_t n);
// Text built by appending (out of line, so each call site stays one call per piece).
struct Txt {
  std::string s;
  Txt& operator<<(const char* p);
  Txt& operator<<(std::string_view v);
  Txt& operator<<(uint64_t v);
};
}  // namespace detail

}  // namespace nimbus::orch::aids
