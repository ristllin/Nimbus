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

std::string appendArgsError(const std::vector<std::string>& named) {
  std::string list;
  for (const std::string& a : named) list += (list.empty() ? "" : ", ") + a;
  if (list.empty()) return std::string();
  return "files.append takes project, name and text; " + list + (named.size() == 1 ? " is" : " are") +
         " not an argument of it, and nothing was added";
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
  std::vector<size_t> starts;
  size_t pos = begin;
  while (pos < window.size()) {
    size_t nl = window.find('\n', pos);
    starts.push_back(pos);
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
  v.firstAt = windowStart + (all.empty() ? std::min(begin, window.size()) : starts[starts.size() - all.size()]);
  v.rows = std::move(all);
  return v;
}

std::string renderTail(const TailView& v, const std::string& name, size_t fileBytes,
                       const std::string& headerRow) {
  std::string out = "last " + std::to_string(v.rows.size()) + " row" + (v.rows.size() == 1 ? "" : "s") +
                    " of " + name + " (" + std::to_string((unsigned)fileBytes) + " B" +
                    (v.earlier ? "; earlier rows not shown: bytes 0-" + std::to_string((unsigned)v.firstAt) +
                                     ", read them with \"offset\":0"
                               : std::string("; the whole file")) +
                    ")\n";
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
// ---- the CSV helpers of the row reader -------------------------------------------

constexpr size_t kMaxColumns = 64;   // a log's header; a wider one is not a log

bool isBlank(char c) { return c == ' ' || c == '\t'; }

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

std::string lowered(const std::string& s) {
  std::string out(s);
  for (char& c : out) c = lowerAscii(c);
  return out;
}

bool sameText(const std::string& a, const std::string& b) { return lowered(a) == lowered(b); }

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
// oat milk)", and a split there would misread that row. An unbalanced '(' shields nothing, so a cut row still reads short, and `parens`
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

std::vector<std::string> csvHeaderOf(const std::string& row) { return csvFields(withoutBom(row)); }

std::string csvWidthNote(const std::string& stored, const std::string& appended) {
  // A new file: its own first line is the header.
  size_t p = 0;
  std::string header = stored;
  if (header.empty()) {
    p = appended.find('\n');
    if (p == std::string::npos) return std::string();
    header = appended.substr(0, p++);
  }
  if (isBlankRow(header)) return std::string();
  const size_t width = csvHeaderOf(header).size();
  int off = 0;
  while (p < appended.size()) {
    size_t nl = appended.find('\n', p);
    if (nl == std::string::npos) nl = appended.size();
    const std::string row = appended.substr(p, nl - p);
    p = nl + 1;
    if (isBlankRow(row) || isCommentRow(row)) continue;
    if (csvFields(row, nullptr, width).size() != width) off++;
  }
  if (off == 0) return std::string();
  return "; " + std::to_string(off) + (off == 1 ? " row has" : " rows have") + " a field count other than the header's " +
         std::to_string((unsigned)width);
}

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

}  // namespace nimbus::orch
