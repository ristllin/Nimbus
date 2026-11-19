#include "nimbus/orch/file_append.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <vector>

#include "nimbus/mem_cap.h"   // utf8CapLen - a quoted row is cut on a character boundary

namespace nimbus::orch {

namespace {
AppendPlan planAppendOrThrow(const std::string& text, size_t fileBytes, char lastByte) {
  AppendPlan p;
  std::string body;
  body.reserve(text.size() + 2);
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '\r') {
      body += '\n';
      if (i + 1 < text.size() && text[i + 1] == '\n') i++;
    } else {
      body += text[i];
    }
  }
  if (body.find_first_not_of(" \t\n") == std::string::npos) {
    p.err = "need 'text': one or more rows to add";
    return p;
  }
  if (body.back() != '\n') body += '\n';
  if (body.size() > kAppendMaxBytes) {   // counted as written, final line end included
    p.err = "text too large for one append (max " + std::to_string((unsigned)kAppendMaxBytes) +
            " B) - add fewer rows per call";
    return p;
  }
  if (fileBytes > 0 && lastByte != '\n') {
    body.insert(body.begin(), '\n');
    p.repairedCut = true;
  }
  for (char c : body)
    if (c == '\n') p.rows++;
  if (p.repairedCut) p.rows--;   // the leading line end closes the cut row
  p.bytes = std::move(body);
  p.ok = true;
  return p;
}
}  // namespace

// A refused allocation (ruling 30) is a designed state: nothing is planned, so the caller
// writes nothing and the stored file keeps every byte.
AppendPlan planAppend(const std::string& text, size_t fileBytes, char lastByte) {
  try {
    return planAppendOrThrow(text, fileBytes, lastByte);
  } catch (const std::bad_alloc&) {
    AppendPlan p;
    p.err.assign(kAppendNoMemory);   // fits the small-string buffer on every library: no allocation
    return p;
  }
}

TailView tailRows(const std::string& window, size_t windowStart, size_t n) {
  TailView v;
  if (n == 0) n = kTailDefaultRows;
  if (n > kTailMaxRows) n = kTailMaxRows;
  size_t begin = 0;
  if (windowStart > 0) {
    const size_t nl = window.find('\n');
    begin = nl == std::string::npos ? window.size() : nl + 1;   // drop the partial row
    v.earlier = true;
  }
  std::vector<std::string> all;
  size_t pos = begin;
  while (pos < window.size()) {
    size_t nl = window.find('\n', pos);
    if (nl == std::string::npos) {
      all.push_back(window.substr(pos));
      v.lastCut = true;
      break;
    }
    all.push_back(window.substr(pos, nl - pos));
    pos = nl + 1;
  }
  if (all.size() > n) {
    v.earlier = true;
    all.erase(all.begin(), all.end() - (long)n);
  }
  v.rows = std::move(all);
  return v;
}

std::string renderTail(const TailView& v, const std::string& name, size_t fileBytes,
                       const std::string& headerRow) {
  std::string out = "last " + std::to_string(v.rows.size()) + " row" + (v.rows.size() == 1 ? "" : "s") +
                    " of " + name + " (" + std::to_string((unsigned)fileBytes) + " B" +
                    (v.earlier ? "; earlier rows not shown" : "; the whole file") + ")\n";
  if (!headerRow.empty() && v.earlier) out += "header: " + headerRow + "\n";
  for (const auto& r : v.rows) out += r + "\n";
  // A CSV's last row without a line end is complete when it is well-formed (a
  // file saved without a final line end); only a provably cut one is flagged.
  const bool cut = v.lastCut && (headerRow.empty() || v.rows.empty() ||
                                 csvLastRowCut(headerRow, v.rows.size() > 1 ? v.rows[v.rows.size() - 2] : "",
                                               v.rows.back()));
  if (cut)
    out += "[the last row is incomplete - the file was cut mid-write; log that row again in full]\n";
  return out;
}

namespace {
// ---- the CSV helpers of the day total and the row reader ---------------------

constexpr size_t kMaxColumns = 64;   // a log's header; a wider one is not a log

bool isBlank(char c) { return c == ' ' || c == '\t'; }

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

std::string lowered(const std::string& s) {
  std::string out(s);
  for (char& c : out) c = lowerAscii(c);
  return out;
}

bool sameText(const std::string& a, const std::string& b) { return lowered(a) == lowered(b); }

// A key value as identity compares it: no case, no spaces around it (a quoted
// " lunch " and lunch are one key).
std::string keyText(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
  return lowered(s.substr(b, e - b));
}

// One quoted field from `p` (just past the opening quote) to just past the
// closing one; false when it never closes.
bool readQuoted(const std::string& row, size_t& p, std::string& field) {
  while (p < row.size()) {
    const char c = row[p++];
    if (c != '"') {
      field += c;
    } else if (p < row.size() && row[p] == '"') {
      field += '"';   // "" inside quotes is one quote
      p++;
    } else {
      return true;
    }
  }
  return false;
}

// The ')' closing the '(' at `open`, within this row; npos when it never closes.
size_t closingParen(const std::string& row, size_t open) {
  int depth = 0;
  for (size_t i = open; i < row.size(); i++) {
    if (row[i] == '(') depth++;
    else if (row[i] == ')' && --depth == 0) return i;
  }
  return std::string::npos;
}

// An unquoted field from `p` up to the next separator. A comma inside a balanced pair of
// parentheses stays in the field: files written before v5.2.0 hold rows like "tea (no sugar,
// oat milk)", and a split there would leave that row out of every total. An unbalanced '(' shields nothing, so a cut row still reads short, and `parens`
// goes false: the row scans for a close at most once in vain, so a row of open parentheses
// costs linear time.
void readPlainField(const std::string& row, size_t& p, std::string& field, bool& parens) {
  const size_t b = p;
  while (p < row.size() && row[p] != ',') {
    if (row[p] == '(' && parens) {
      const size_t close = closingParen(row, p);
      if (close != std::string::npos) p = close;
      else parens = false;
    }
    p++;
  }
  size_t e = p;
  while (e > b && isBlank(row[e - 1])) e--;
  field.assign(row, b, e - b);
}

// One field from `p` up to (not including) the next separator. False when a quote never
// closes or text follows a closing quote.
bool readField(const std::string& row, size_t& p, std::string& field, bool& parens) {
  while (p < row.size() && isBlank(row[p])) p++;
  if (p < row.size() && row[p] == '"') {
    p++;
    if (!readQuoted(row, p, field)) return false;
    while (p < row.size() && isBlank(row[p])) p++;
    return p >= row.size() || row[p] == ',';
  }
  readPlainField(row, p, field, parens);
  return true;
}

// The fields of one CSV row: commas separate, a field in double quotes or a balanced
// pair of parentheses may hold commas, spaces around a field are trimmed. `ok` false when a quote is left open
// or text follows a closing quote (the rest of the row is then one field). At
// most `maxFields` + 1 fields come back (a row of thousands of commas costs no
// more): the extra one stands for "wider than that", the rest of the row.
std::vector<std::string> csvFields(const std::string& row, bool* ok = nullptr,
                                   size_t maxFields = kMaxColumns) {
  std::vector<std::string> out;
  bool good = true, parens = true;
  size_t p = 0;
  while (true) {
    if (out.size() == maxFields) {   // wider than the cap: the rest as one field
      out.emplace_back(row, p < row.size() ? p : row.size(), std::string::npos);
      break;
    }
    std::string field;
    if (!readField(row, p, field, parens)) {
      good = false;
      field.assign(row, p < row.size() ? p : row.size(), std::string::npos);
      p = row.size();
    }
    out.push_back(field);
    if (p >= row.size()) break;
    p++;   // past the comma
    if (p == row.size()) {
      out.emplace_back();   // a trailing comma: one more, empty field
      break;
    }
  }
  if (ok) *ok = good;
  return out;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The YYYY-MM-DD day a date field starts with ("2026-10-25" or
// "2026-10-25 08:30"), or "" when it does not start with one.
std::string isoDay(const std::string& f) {
  if (f.size() < 10) return std::string();
  for (size_t i = 0; i < 10; i++) {
    const bool dash = i == 4 || i == 7;
    if (dash ? f[i] != '-' : !isDigit(f[i])) return std::string();
  }
  if (f.size() > 10 && f[10] != ' ' && f[10] != 'T') return std::string();
  const int month = (f[5] - '0') * 10 + (f[6] - '0'), day = (f[8] - '0') * 10 + (f[9] - '0');
  if (month < 1 || month > 12 || day < 1 || day > 31) return std::string();
  return f.substr(0, 10);
}

bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// What may follow a number: nothing, or spaces then a unit in letters ("kg").
bool unitTail(const std::string& f, size_t p) {
  while (p < f.size() && isBlank(f[p])) p++;
  for (; p < f.size(); p++)
    if (!isLetter(f[p])) return false;
  return true;
}

// A plain number ("450", "-2.5", "450 kg"): an optional sign, digits with at
// most one decimal point (12 digits at most), then nothing or a unit in letters.
// "~450", "450-500", "1,046", "1e5", "inf" and "0x1A" are not plain numbers.
bool plainNumber(const std::string& f, double& v) {
  size_t p = (!f.empty() && (f[0] == '-' || f[0] == '+')) ? 1 : 0;
  int digits = 0, dots = 0;
  for (; p < f.size() && (isDigit(f[p]) || f[p] == '.'); p++) (f[p] == '.' ? dots : digits)++;
  if (digits == 0 || digits > 12 || dots > 1 || !unitTail(f, p)) return false;
  v = std::strtod(f.substr(0, p).c_str(), nullptr);
  return true;
}

int columnIndex(const std::vector<std::string>& header, const std::string& name) {
  for (size_t i = 0; i < header.size(); i++)
    if (sameText(header[i], name)) return (int)i;
  return -1;
}

// The date column by its name: "date", then "day", then the first column whose
// name holds date, day or time (timestamp, datetime, logged_at, Day of entry) or is
// "when" / "ts"; -1 = none by name.
int dateColumnByName(const std::vector<std::string>& header) {
  int c = columnIndex(header, "date");
  if (c < 0) c = columnIndex(header, "day");
  for (size_t i = 0; c < 0 && i < header.size(); i++) {
    const std::string n = lowered(header[i]);
    if (n.find("date") != std::string::npos || n.find("day") != std::string::npos ||
        n.find("time") != std::string::npos || n == "when" || n == "ts")
      c = (int)i;
  }
  return c;
}

// Does every row of `rows` hold a YYYY-MM-DD day in column `c`?
bool allIsoAt(const std::vector<std::vector<std::string>>& rows, size_t c) {
  for (const auto& f : rows)
    if (f.size() <= c || isoDay(f[c]).empty()) return false;
  return !rows.empty();
}

// A figure as a stored row may hold it: a plain number, or a quoted one with thousands
// separators ("1,045": a bare comma never reaches a single field). New rows stay plain.
bool figureOf(const std::string& f, double& v) {
  if (plainNumber(f, v)) return true;
  std::string joined;
  size_t digits = 0, groups = 0;
  size_t p = (!f.empty() && (f[0] == '-' || f[0] == '+')) ? 1 : 0;
  if (p) joined += f[0];
  for (; p < f.size() && (isDigit(f[p]) || f[p] == ','); p++) {
    if (f[p] != ',') {
      joined += f[p];
      digits++;
      continue;
    }
    if (groups == 0 ? (digits == 0 || digits > 3) : digits != 3) return false;
    digits = 0;
    groups++;
  }
  if (groups == 0 || digits != 3) return false;
  return plainNumber(joined + f.substr(p), v);
}

// A stored row's fields as its header names them. A row wider than its header keeps one
// more field (the rest of the row), so it never reads as the header's width.
std::vector<std::string> rowFields(const std::string& row, bool* quotes, const std::vector<std::string>& header) {
  return csvFields(row, quotes, header.size());
}

// A row that states a total ("daily total") instead of an entry.
bool statesTotal(const std::vector<std::string>& f, int dateIdx, int sumIdx) {
  for (size_t c = 0; c < f.size(); c++) {
    if ((int)c == dateIdx || (int)c == sumIdx) continue;
    const std::string cell = keyText(f[c]);
    for (const char* word : {"total", "totals", "subtotal", "day total", "daily total", "grand total", "running total",
                             "total so far", "today total", "todays total", "today's total"})
      if (cell == word) return true;
  }
  return false;
}

// A final row that had no line end: is it PROVABLY cut by a power loss? Only
// then: a quote left open, fewer fields than the header, or no plain number in
// the sum column or in a column that holds one in the row before. A well-formed
// row only lacks its line end (a file saved without one): it is complete.
bool provablyCut(const std::vector<std::string>& f, bool quotesOk, size_t width,
                 const std::vector<std::string>& prev, int sumIdx) {
  if (!quotesOk || f.size() < width) return true;
  double v = 0;
  if (sumIdx >= 0 && (size_t)sumIdx < f.size() && !figureOf(f[(size_t)sumIdx], v)) return true;
  for (size_t i = 0; i < prev.size() && i < f.size(); i++)
    if (figureOf(prev[i], v) && !figureOf(f[i], v)) return true;
  return false;
}

bool isCommentRow(const std::string& row) {
  size_t p = 0;
  while (p < row.size() && isBlank(row[p])) p++;
  return p < row.size() && row[p] == '#';
}

bool isBlankRow(const std::string& row) {
  for (char c : row)
    if (!isBlank(c)) return false;
  return true;
}

// The header as written, without a UTF-8 byte order mark (a hand-edited file).
std::string withoutBom(const std::string& row) {
  return row.compare(0, 3, "\xEF\xBB\xBF") == 0 ? row.substr(3) : row;
}

bool sameHeader(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (!sameText(a[i], b[i])) return false;
  return true;
}

// Count one row into `d` when it is dated d.date. Counted: a row of the header's width whose
// sum field reads as a number. Not counted, and said: a row whose columns cannot be trusted
// (`unread`), a row that states a total (`totalRows`).
void tallyInto(DayTotal& d, const std::vector<std::string>& f, const std::vector<std::string>& header, int dateIdx,
               int sumIdx) {
  if (dateIdx < 0 || sumIdx < 0 || f.size() <= (size_t)dateIdx) return;
  if (isoDay(f[(size_t)dateIdx]) != d.date) return;
  if (statesTotal(f, dateIdx, sumIdx)) {
    d.totalRows++;
    return;
  }
  double v = 0;
  if (f.size() != header.size() || !figureOf(f[(size_t)sumIdx], v)) {
    d.unread++;
    return;
  }
  d.total += v;
  d.rows++;
}

std::string numText(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

}  // namespace

// The date column of a header given some of its rows: by name when that column
// holds dates in those rows, else the first column that does; -1 = none.
int csvDateColumnOf(const std::vector<std::string>& header, const std::vector<std::vector<std::string>>& rows) {
  const int byName = dateColumnByName(header);
  if (byName >= 0 && allIsoAt(rows, (size_t)byName)) return byName;
  for (size_t c = 0; c < header.size(); c++)
    if (allIsoAt(rows, c)) return (int)c;
  return rows.empty() ? byName : -1;
}


// ---- rows from chunks, and the whole-text forms -------------------------------

bool RowSplitter::push(const char* data, size_t n) {
  for (size_t i = 0; i < n && ok_; i++) {
    const char c = data[i];
    if (c != '\n') {
      if (cur_.size() >= kRowMaxBytes) {
        ok_ = false;
        tooLong_ = true;
        break;
      }
      cur_ += c;
      continue;
    }
    if (!cur_.empty() && cur_.back() == '\r') cur_.pop_back();
    ok_ = onRow_(cur_, true);
    cur_.clear();
  }
  return ok_;
}

bool RowSplitter::end() {
  if (ok_ && !cur_.empty()) {
    if (cur_.back() == '\r') cur_.pop_back();
    ok_ = onRow_(cur_, false);
  }
  cur_.clear();
  return ok_;
}

CsvDayTally::CsvDayTally(const std::string& sumCol, const std::string& day)
    : sumCol_(sumCol), day_(day) {
  t_.date = day;
}

void CsvDayTally::readHeader(const std::string& row) {
  if (isBlankRow(row)) return;   // blank lines above the header
  sawHeader_ = true;
  header_ = csvFields(withoutBom(row));
  width_ = header_.size();
  sumIdx_ = sumCol_.empty() ? -1 : columnIndex(header_, sumCol_);
}

// The row's day; the first data row settles which column holds it.
std::string CsvDayTally::rowDay(const std::vector<std::string>& f) {
  if (dateIdx_ < 0) dateIdx_ = csvDateColumnOf(header_, {f});
  return dateIdx_ >= 0 && f.size() > (size_t)dateIdx_ ? isoDay(f[(size_t)dateIdx_]) : std::string();
}

void CsvDayTally::feed(const std::string& row, bool complete) {
  if (!sawHeader_) return readHeader(row);
  if (sumIdx_ < 0 || day_.empty() || isCommentRow(row) || isBlankRow(row)) return;
  bool quotes = true;
  const std::vector<std::string> f = rowFields(row, &quotes, header_);
  if (!complete && provablyCut(f, quotes, width_, prev_, sumIdx_)) return;   // a cut row never counts
  if (sameHeader(f, header_)) return;   // the header pasted again mid-file: never a row
  if (f.size() == width_) prev_ = f;
  const std::string day = rowDay(f);
  if (day.empty()) {
    undated_++;
    return;
  }
  tallyInto(t_, f, header_, dateIdx_, sumIdx_);
}

bool CsvDayTally::ok() const {
  // A file with no data rows yet still has a known zero total when its header
  // could date rows; once rows are read, a date column was found in them.
  return sawHeader_ && sumIdx_ >= 0 && !day_.empty() && (dateIdx_ >= 0 || dateColumnByName(header_) >= 0);
}

std::string CsvDayTally::whyNot() const {
  if (day_.empty()) return "the device clock is not set";
  if (sumIdx_ < 0) return "the file has no column '" + sumCol_ + "'";
  if (dateIdx_ < 0 && dateColumnByName(header_) < 0) return "the file has no date column";
  return std::string();
}

std::string csvDayTotalLine(const std::string& fileText, const std::string& sumCol, const std::string& today,
                            const std::string& zone) {
  CsvDayTally tally(sumCol, today);
  RowSplitter rows([&](const std::string& row, bool complete) {
    tally.feed(row, complete);
    return true;
  });
  rows.push(fileText.data(), fileText.size());
  rows.end();
  if (!tally.ok()) return "[no day total: " + tally.whyNot() + "]\n";
  std::string line = renderDayTotal(tally.total(), sumCol, true, zone) + "\n";
  if (tally.undated() > 0) line += renderUndated(tally.undated()) + "\n";
  return line;
}

std::vector<std::string> csvHeaderOf(const std::string& row) { return csvFields(withoutBom(row)); }

std::vector<std::string> csvStoredRow(const std::string& row, const std::vector<std::string>& header) {
  bool quotes = true;
  std::vector<std::string> f = rowFields(row, &quotes, header);
  if (!quotes || f.size() > header.size()) f.clear();
  return f;
}

bool csvFigure(const std::string& cell, double& out) { return figureOf(cell, out); }

StreamEnd streamRows(size_t size, const ReadFn& read, RowSplitter& rows) {
  char buf[512];
  size_t got = 0;
  while (got < size && rows.ok()) {
    const int n = read(buf, size - got < sizeof buf ? size - got : sizeof buf);
    if (n <= 0) break;
    got += (size_t)n;
    rows.push(buf, (size_t)n);
  }
  if (!rows.ok()) return StreamEnd::Stopped;
  if (got != size) return StreamEnd::ReadFailed;
  return rows.end() ? StreamEnd::Done : StreamEnd::Stopped;
}

std::string isoDayOf(const std::string& field) { return isoDay(field); }

bool csvLastRowCut(const std::string& header, const std::string& prev, const std::string& row) {
  bool quotes = true;
  const std::vector<std::string> h = csvFields(withoutBom(header));
  const std::vector<std::string> f = csvFields(row, &quotes, h.size());
  std::vector<std::string> p = csvFields(prev, nullptr, h.size());
  if (p.size() != h.size()) p.clear();   // only a well-formed row before it says which columns hold numbers
  return provablyCut(f, quotes, h.size(), p, -1);
}

// ---- the log's day ----------------------------------------------------------------

LogDay logDayOf(time_t now, bool tzSet) {
  LogDay d;
  struct tm lt;
  if (now < 1600000000 || !localtime_r(&now, &lt)) return d;   // never synced: no day
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
  d.date = buf;
  if (!tzSet) {
    d.zone = "UTC, no time zone is set";
    return d;
  }
  char zone[16] = "";
  std::strftime(zone, sizeof zone, "%Z", &lt);
  d.zone = zone;
  return d;
}

// ---- the reply ------------------------------------------------------------------

std::string renderDayTotal(const DayTotal& d, const std::string& sumCol, bool isToday,
                           const std::string& zoneAt) {
  std::string out = isToday ? "today's " + sumCol + " (" + d.date + (zoneAt.empty() ? "" : ", " + zoneAt) + "): "
                            : sumCol + " on " + d.date + ": ";
  const std::string rowsWord = std::to_string(d.rows) + (d.rows == 1 ? " row" : " rows");
  if (d.unread > 0) {
    // No figure that looks like the day's: what was read, and that the rest was not.
    out += "not known - " + numText(d.total) + " counted from " + rowsWord + ", but " + std::to_string(d.unread) +
           (d.unread == 1 ? " more row" : " more rows") + " dated " + d.date + (d.unread == 1 ? " has" : " have") +
           " no readable '" + sumCol + "' (not a number, or its columns are shifted); fix " +
           (d.unread == 1 ? "it" : "them") + " with files.edit before stating a total";
  } else {
    out += numText(d.total);
    if (d.rows == 0) out += " - no rows are dated " + d.date;
    else out += " from " + rowsWord;
  }
  if (d.totalRows > 0)
    out += "; " + std::to_string(d.totalRows) + (d.totalRows == 1 ? " total row is" : " total rows are") +
           " not counted (the device adds up the rows itself)";
  return out;
}

std::string renderUndated(int rows) {
  return "[" + std::to_string(rows) + (rows == 1 ? " row has" : " rows have") +
         " no YYYY-MM-DD date and count toward no day's total]";
}

}  // namespace nimbus::orch
