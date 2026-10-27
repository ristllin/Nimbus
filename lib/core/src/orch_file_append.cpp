#include "nimbus/orch/file_append.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

#include "nimbus/mem_cap.h"   // utf8CapLen - a quoted row is cut on a character boundary

namespace nimbus::orch {

AppendPlan planAppend(const std::string& text, size_t fileBytes, char lastByte) {
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
// ---- the CSV helpers of the keyed upsert and the day total -------------------

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

// One field from `p` up to (not including) the next separator. False when a
// quote never closes or text follows a closing quote.
bool readField(const std::string& row, size_t& p, std::string& field) {
  while (p < row.size() && isBlank(row[p])) p++;
  if (p < row.size() && row[p] == '"') {
    p++;
    if (!readQuoted(row, p, field)) return false;
    while (p < row.size() && isBlank(row[p])) p++;
    return p >= row.size() || row[p] == ',';
  }
  const size_t b = p;
  while (p < row.size() && row[p] != ',') p++;
  size_t e = p;
  while (e > b && isBlank(row[e - 1])) e--;
  field.assign(row, b, e - b);
  return true;
}

// The fields of one CSV row: commas separate, a field in double quotes may hold
// commas, spaces around a field are trimmed. `ok` false when a quote is left open
// or text follows a closing quote (the rest of the row is then one field). At
// most `maxFields` + 1 fields come back (a row of thousands of commas costs no
// more): the extra one stands for "wider than that", the rest of the row.
std::vector<std::string> csvFields(const std::string& row, bool* ok = nullptr,
                                   size_t maxFields = kMaxColumns) {
  std::vector<std::string> out;
  bool good = true;
  size_t p = 0;
  while (true) {
    if (out.size() == maxFields) {   // wider than the cap: the rest as one field
      out.emplace_back(row, p < row.size() ? p : row.size(), std::string::npos);
      break;
    }
    std::string field;
    if (!readField(row, p, field)) {
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

std::vector<std::string> splitRows(const std::string& text) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= text.size()) {
    const size_t nl = text.find('\n', p);
    const size_t end = nl == std::string::npos ? text.size() : nl;
    std::string row = text.substr(p, end - p);
    if (!row.empty() && row.back() == '\r') row.pop_back();
    out.push_back(row);
    if (nl == std::string::npos) break;
    p = nl + 1;
  }
  if (!out.empty() && out.back().empty()) out.pop_back();   // the final line end
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

// What may follow a number: nothing, or spaces then a unit in letters ("kcal").
bool unitTail(const std::string& f, size_t p) {
  while (p < f.size() && isBlank(f[p])) p++;
  for (; p < f.size(); p++)
    if (!isLetter(f[p])) return false;
  return true;
}

// A plain number ("450", "-2.5", "450 kcal"): an optional sign, digits with at
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
// name holds date, day or time (timestamp, datetime, logged_at, Day of meal) or is
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

// The date column of a header given some of its rows: by name when that column
// holds dates in those rows, else the first column that does; -1 = none.
int dateColumnOf(const std::vector<std::string>& header, const std::vector<std::vector<std::string>>& rows) {
  const int byName = dateColumnByName(header);
  if (byName >= 0 && allIsoAt(rows, (size_t)byName)) return byName;
  for (size_t c = 0; c < header.size(); c++)
    if (allIsoAt(rows, c)) return (int)c;
  return rows.empty() ? byName : -1;
}

// A final row that had no line end: is it PROVABLY cut by a power loss? Only
// then: a quote left open, fewer fields than the header, or no plain number in
// the sum column or in a column that holds one in the row before. A well-formed
// row only lacks its line end (a file saved without one): it is complete.
bool provablyCut(const std::vector<std::string>& f, bool quotesOk, size_t width,
                 const std::vector<std::string>& prev, int sumIdx) {
  if (!quotesOk || f.size() < width) return true;
  double v = 0;
  if (sumIdx >= 0 && (size_t)sumIdx < f.size() && !plainNumber(f[(size_t)sumIdx], v)) return true;
  for (size_t i = 0; i < prev.size() && i < f.size(); i++)
    if (plainNumber(prev[i], v) && !plainNumber(f[i], v)) return true;
  return false;
}

// A stored header quoted back in a refusal, when every name is a plain label
// (letters, digits, spaces, _ ( ) -): the retry is then one call. "" = not plain.
std::string plainHeader(const std::vector<std::string>& h) {
  std::string out;
  for (const std::string& n : h) {
    for (char c : n)
      if (!(isLetter(c) || isDigit(c) || c == ' ' || c == '_' || c == '(' || c == ')' || c == '-')) return std::string();
    out += (out.empty() ? "" : ",") + n;
  }
  return out.size() <= 160 ? out : std::string();
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

// The model's own header as quoted back in a refusal: about 160 bytes at most.
std::string quoteRow(const std::string& row) {
  if (row.size() <= 160) return row;
  return row.substr(0, (size_t)nimbus::utf8CapLen(row.c_str(), (int)row.size(), 157)) + "...";
}

std::string rowNo(size_t n) { return "row " + std::to_string((unsigned)n); }

// Count one row into `d` when it is dated d.date: its sum field must be a plain
// number in a row no wider than the header (a wider row has a shifted column).
void tallyInto(DayTotal& d, const std::vector<std::string>& f, size_t width, int dateIdx,
               int sumIdx) {
  if (dateIdx < 0 || sumIdx < 0 || f.size() <= (size_t)dateIdx) return;
  if (isoDay(f[(size_t)dateIdx]) != d.date) return;
  double v = 0;
  if (f.size() <= width && f.size() > (size_t)sumIdx && plainNumber(f[(size_t)sumIdx], v)) {
    d.total += v;
    d.rows++;
  } else {
    d.unread++;
  }
}

std::string numText(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

}  // namespace

// ---- CsvUpsertStream ----------------------------------------------------------
// The replies never quote a stored row's free text: only the identity the model
// itself sent, a stored date, a stored figure that is a plain number, and a
// stored header made of plain labels. What a log holds may once have come from
// outside (files.read, which returns it, is an External tool); files.append stays
// a tool whose output is the device's own.

CsvUpsertStream::CsvUpsertStream(const std::string& keyCol, const std::string& sumCol,
                                 const std::string& today)
    : keyCol_(keyCol), sumCol_(sumCol), today_(today) {}

bool CsvUpsertStream::fail(const std::string& why) {
  failed_ = true;
  r_.ok = false;
  r_.err = why;
  return false;
}

// Date plus key (the date alone when the key IS the date column). "" = the row
// has no date or no key (never matched).
std::string CsvUpsertStream::identity(const std::vector<std::string>& f) const {
  // A row wider than the header has a shifted column (an unquoted comma): its
  // date and key cannot be trusted, so it is never matched (nor counted).
  if (keyIdx_ < 0 || dateIdx_ < 0 || f.size() <= (size_t)keyIdx_ || f.size() <= (size_t)dateIdx_ ||
      f.size() > header_.size())
    return std::string();
  const std::string key = keyText(f[(size_t)keyIdx_]);
  const std::string day = isoDay(f[(size_t)dateIdx_]);
  if (day.empty() || key.empty()) return std::string();
  return keyIdx_ == dateIdx_ ? day : day + "\x1f" + key;
}

// The shape of one new row: no comment, closed quotes, the header's width, a key.
bool CsvUpsertStream::parseNewRow(size_t n, const std::string& raw, NewRow& row) {
  if (isCommentRow(raw)) return fail(rowNo(n) + " starts with # (a comment row) - start it with its first column");
  bool quotes = true;
  row.raw = raw;
  row.fields = csvFields(raw, &quotes, header_.size());
  if (!quotes)
    return fail(rowNo(n) + " has a broken quote (an unclosed quote, or text after a closing one) - put a "
                "field with a comma in double quotes and write a quote inside it as \"\"");
  if (row.fields.size() != header_.size())
    return fail(rowNo(n) + " has " + std::to_string((unsigned)row.fields.size()) +
                " fields but the header has " + std::to_string((unsigned)header_.size()) +
                " - put a field that holds a comma in double quotes");
  row.key = row.fields[(size_t)keyIdx_];
  if (keyText(row.key).empty()) return fail(rowNo(n) + " has no value in '" + keyCol_ + "'");
  return true;
}

// The date, the sum and the identity of one new row (the date column is known).
bool CsvUpsertStream::checkNewRow(size_t n, NewRow& row) {
  row.date = isoDay(row.fields[(size_t)dateIdx_]);
  if (row.date.empty())
    return fail(rowNo(n) + "'s date '" + row.fields[(size_t)dateIdx_] + "' is not a YYYY-MM-DD date");
  if (!today_.empty() && row.date > today_)
    return fail(rowNo(n) + " is dated " + row.date + ", after today (" + today_ +
                (zone_.empty() ? std::string() : ", " + zone_) + ") - log the owner's local date" +
                (zone_.find("no time zone") != std::string::npos
                     ? std::string(" (the device's today is the UTC date until a time zone is set in Settings)")
                     : std::string()));
  double v = 0;
  if (sumIdx_ >= 0 && !plainNumber(row.fields[(size_t)sumIdx_], v))
    return fail(rowNo(n) + "'s '" + sumCol_ + "' value '" + row.fields[(size_t)sumIdx_] +
                "' is not a plain number - write one number, like 450");
  row.id = identity(row.fields);
  for (size_t i = 0; i < rows_.size(); i++)
    if (rows_[i].id == row.id)
      return fail(rowNo(i + 1) + " and " + rowNo(n) + " both name " + row.date + " '" + row.key +
                  "' - send one row per date and key");
  return true;
}

// The date column: by its name or, failing that, the column the new rows date. A
// keyed write with no date column would let one day's row replace another's: refused.
bool CsvUpsertStream::findDateColumn(const std::vector<NewRow>& rows) {
  std::vector<std::vector<std::string>> fields;
  for (const NewRow& r : rows) fields.push_back(r.fields);
  dateIdx_ = dateColumnOf(header_, fields);
  if (dateIdx_ >= 0) return true;
  const int named = dateColumnByName(header_);
  for (size_t i = 0; named >= 0 && i < rows.size(); i++)
    if (isoDay(rows[i].fields[(size_t)named]).empty())   // every new row has the header's width
      return fail("the date column '" + header_[(size_t)named] + "' must hold YYYY-MM-DD dates (" + rowNo(i + 1) +
                  ": '" + rows[i].fields[(size_t)named] + "')");
  return fail("this log has no date column, and a keyed write without one would replace other days' rows - "
              "start the log with a date column (date,meal,...; dates as YYYY-MM-DD) and log there");
}

// The text's header: within the column cap, naming the key (and the sum) column.
bool CsvUpsertStream::parseHeader(const std::string& line) {
  headerRaw_ = withoutBom(line);
  header_ = csvFields(headerRaw_);
  if (header_.size() > kMaxColumns)
    return fail("the header has more than " + std::to_string((unsigned)kMaxColumns) + " columns - this is not a log");
  keyIdx_ = columnIndex(header_, keyCol_);
  if (keyIdx_ < 0) return fail("the text's header has no column '" + keyCol_ + "'");
  sumIdx_ = sumCol_.empty() ? -1 : columnIndex(header_, sumCol_);
  if (!sumCol_.empty() && sumIdx_ < 0) return fail("the text's header has no column '" + sumCol_ + "'");
  return true;
}

bool CsvUpsertStream::begin(const std::string& newText) {
  begun_ = true;
  if (keyCol_.empty()) return fail("need 'key': the column whose value identifies a row");
  if (newText.size() > kAppendMaxBytes)
    return fail("text too large for one call (max " + std::to_string((unsigned)kAppendMaxBytes) +
                " B) - send fewer rows per call");
  std::vector<std::string> lines;
  for (const std::string& l : splitRows(newText))
    if (!isBlankRow(l)) lines.push_back(l);
  if (lines.size() < 2) return fail("need 'text': the header row, then at least one data row");
  if (!parseHeader(lines.front())) return false;
  std::vector<NewRow> parsed;
  for (size_t i = 1; i < lines.size(); i++) {
    NewRow row;
    if (!parseNewRow(i, lines[i], row)) return false;
    parsed.push_back(std::move(row));
  }
  if (!findDateColumn(parsed)) return false;
  for (size_t i = 0; i < parsed.size(); i++) {
    if (!checkNewRow(i + 1, parsed[i])) return false;
    rows_.push_back(std::move(parsed[i]));
  }
  planDays();
  return true;
}

// The days the reply totals: today first, then every other date the text writes.
void CsvUpsertStream::planDays() {
  if (sumIdx_ < 0) return;
  if (today_.empty()) r_.noTotal = "the device clock is not set";
  else r_.days.push_back(DayTotal{today_});
  for (const NewRow& n : rows_) {
    bool seen = false;
    for (const DayTotal& d : r_.days) seen = seen || d.date == n.date;
    if (!seen) r_.days.push_back(DayTotal{n.date});
  }
}

bool CsvUpsertStream::feedHeader(const std::string& row, bool complete, std::string& out) {
  outRows_++;
  out = row + "\n";
  if (!complete) r_.changed = true;      // the line end the row was missing
  if (isBlankRow(row)) return true;      // blank lines above the header are kept as they are
  sawHeader_ = true;
  const std::vector<std::string> stored = csvFields(withoutBom(row));
  if (sameHeader(stored, header_)) return true;
  const std::string plain = stored.size() <= kMaxColumns ? plainHeader(stored) : std::string();
  return fail("the text's header (" + quoteRow(headerRaw_) + ") differs from the stored header (" +
              (!plain.empty() ? plain
                              : (stored.size() > kMaxColumns ? std::string("too many")
                                                             : std::to_string((unsigned)stored.size())) +
                                    " columns; read it with files.read {project, name, tail: 1}") +
              ") - send the stored header and its column order");
}

CsvUpsertStream::NewRow* CsvUpsertStream::match(const std::string& id) {
  if (id.empty()) return nullptr;
  for (NewRow& n : rows_)
    if (n.id == id) return &n;
  return nullptr;
}

void CsvUpsertStream::replaceWith(NewRow& n, const std::vector<std::string>& stored, bool same,
                                  std::string& out) {
  double v = 0;
  n.used = true;
  if (sumIdx_ >= 0 && stored.size() > (size_t)sumIdx_ && plainNumber(stored[(size_t)sumIdx_], v))
    n.was = numText(v);   // the replaced figure, a plain number: never stored free text
  r_.replaced++;
  if (!same) r_.changed = true;
  out = n.raw + "\n";
  tally(n.fields);
}

// A last row provably cut mid-write: the new row with its identity replaces it,
// or it is kept as a comment row that never matches or counts again.
void CsvUpsertStream::feedCut(const std::vector<std::string>& f, const std::string& row, NewRow* hit,
                              std::string& out) {
  r_.cut = true;
  r_.cutDate = f.size() > (size_t)dateIdx_ ? isoDay(f[(size_t)dateIdx_]) : std::string();
  r_.changed = true;
  if (hit && !hit->used) {
    replaceWith(*hit, f, false, out);
    r_.cutReplaced = true;
    return;
  }
  out = "#cut: " + row + "\n";
}

// A second stored row answering to one identity: set aside (replace: "all"), or
// refused with the way out.
bool CsvUpsertStream::secondMatch(NewRow& n, const std::string& row, std::string& out) {
  if (supersede_) {
    out = "#superseded: " + row + "\n";   // kept, never counted or matched again
    n.superseded++;
    r_.superseded++;
    r_.changed = true;
    return true;
  }
  if (keyIdx_ == dateIdx_)
    return fail(n.date + " has 2 or more rows - key '" + keyCol_ +
                "' cannot tell them apart; nothing was written. In a food log pass the column that tells "
                "one day's rows apart (like meal) as the key; if these rows are one day's running totals "
                "(a daily ledger), send the same call with replace: \"all\" to keep only this row");
  return fail("2 or more stored rows are " + n.date + " '" + n.key +
              "' - the key cannot tell them apart; nothing was written. If they are one meal logged more "
              "than once (an older log's corrections), send the same call with replace: \"all\": this row "
              "replaces them, the others are kept as #superseded: rows and not counted. If they are separate "
              "items, log the new one under its own '" + keyCol_ + "' value");
}

void CsvUpsertStream::countUndated(const std::vector<std::string>& f) {
  if (sumIdx_ >= 0 && (f.size() <= (size_t)dateIdx_ || isoDay(f[(size_t)dateIdx_]).empty())) r_.undated++;
}

bool CsvUpsertStream::feedData(const std::string& row, bool complete, std::string& out) {
  outRows_++;
  if (isCommentRow(row) || isBlankRow(row)) {   // kept as written, never matched or counted
    out = row + "\n";
    if (!complete) r_.changed = true;
    return true;
  }
  bool quotes = true;
  const std::vector<std::string> f = csvFields(row, &quotes, header_.size());
  NewRow* hit = match(identity(f));
  if (!complete && provablyCut(f, quotes, header_.size(), prev_, sumIdx_)) {
    feedCut(f, row, hit, out);
    return true;
  }
  if (!complete) r_.changed = true;   // a well-formed last row: only its line end was missing
  if (f.size() == header_.size()) prev_ = f;   // the row a cut last row is judged against
  if (!hit) {
    out = row + "\n";
    countUndated(f);
    tally(f);
    return true;
  }
  if (hit->used) return secondMatch(*hit, row, out);
  replaceWith(*hit, f, hit->raw == row && complete, out);
  return true;
}

bool CsvUpsertStream::feed(const std::string& row, bool complete, std::string& out) {
  out.clear();
  if (failed_ || !begun_) return false;
  storedRows_++;
  return sawHeader_ ? feedData(row, complete, out) : feedHeader(row, complete, out);
}

void CsvUpsertStream::tally(const std::vector<std::string>& f) {
  for (DayTotal& d : r_.days) tallyInto(d, f, header_.size(), dateIdx_, sumIdx_);
}

bool CsvUpsertStream::finish(std::string& out) {
  out.clear();
  if (failed_ || !begun_) return false;
  if (!sawHeader_) {
    out += headerRaw_ + "\n";   // a new (or blank) file adopts the text's header
    outRows_++;
    r_.changed = true;
  }
  for (NewRow& n : rows_) {
    r_.changes.push_back({n.date, n.key, n.used, n.was, n.superseded});
    if (n.used) continue;
    out += n.raw + "\n";
    outRows_++;
    r_.appended++;
    r_.changed = true;
    tally(n.fields);
  }
  // Every stored row is written back (replaced in place, set aside, or as it
  // was), plus the appended rows: no update can drop a row.
  const size_t expect = storedRows_ + (size_t)r_.appended + (sawHeader_ ? 0 : 1);
  if (outRows_ != expect) return fail("internal row count mismatch - nothing was written");
  if (!today_.empty() && !r_.days.empty() && r_.days.front().date == today_) {
    r_.hasTotal = true;
    r_.todayTotal = r_.days.front().total;
  }
  r_.ok = true;
  return true;
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

CsvUpsert planCsvUpsert(const std::string& existing, const std::string& newText,
                        const std::string& keyCol, const std::string& sumCol,
                        const std::string& today, bool supersede) {
  // The device's own path (mergeKeyed) over a text instead of the card.
  CsvUpsertStream m(keyCol, sumCol, today);
  m.setSupersede(supersede);
  if (!m.begin(newText)) return m.result();
  size_t pos = 0;
  std::string joined, err;
  const bool ok = mergeKeyed(
      m, true, existing.size(),
      [&](char* dst, size_t n) {
        const size_t k = n < existing.size() - pos ? n : existing.size() - pos;
        existing.copy(dst, k, pos);
        pos += k;
        return (int)k;
      },
      [&](const std::string& bytes) {
        joined += bytes;
        return true;
      },
      err);
  CsvUpsert r = m.result();
  if (!ok) {
    r.ok = false;
    r.err = err;
    return r;
  }
  r.changed = r.changed && joined != existing;
  r.out = r.changed ? joined : std::string();
  return r;
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

void CsvDayTally::feed(const std::string& row, bool complete) {
  if (!sawHeader_) return readHeader(row);
  if (sumIdx_ < 0 || day_.empty() || isCommentRow(row) || isBlankRow(row)) return;
  bool quotes = true;
  const std::vector<std::string> f = csvFields(row, &quotes, width_);
  if (!complete && provablyCut(f, quotes, width_, prev_, sumIdx_)) return;   // a cut row never counts
  if (f.size() == width_) prev_ = f;
  if (dateIdx_ < 0) dateIdx_ = dateColumnOf(header_, {f});   // the first data row settles it
  const std::string day = dateIdx_ >= 0 && f.size() > (size_t)dateIdx_ ? isoDay(f[(size_t)dateIdx_]) : std::string();
  if (day.empty()) {
    undated_++;
    return;
  }
  tallyInto(t_, f, width_, dateIdx_, sumIdx_);
  if (dayRow_ && day == day_) dayRow_(row);
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

bool csvDayTotal(const std::string& fileText, const std::string& sumCol,
                 const std::string& today, double& total) {
  if (sumCol.empty() || today.empty()) return false;
  CsvDayTally tally(sumCol, today);
  RowSplitter rows([&](const std::string& row, bool complete) {
    tally.feed(row, complete);
    return true;
  });
  rows.push(fileText.data(), fileText.size());
  rows.end();
  if (!tally.ok()) return false;
  total = tally.total().total;
  return true;
}

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

bool mergeKeyed(CsvUpsertStream& m, bool stored, size_t size, const ReadFn& read,
                const WriteFn& write, std::string& err) {
  std::string piece;
  bool wrote = true;
  RowSplitter rows([&](const std::string& row, bool complete) {
    if (!m.feed(row, complete, piece)) return false;
    wrote = write(piece);
    return wrote;
  });
  const StreamEnd end = stored ? streamRows(size, read, rows) : StreamEnd::Done;
  if (end == StreamEnd::Done && m.finish(piece) && (wrote = write(piece))) return true;
  if (end == StreamEnd::ReadFailed) err = "the card read failed - the log was not changed; try again";
  else if (!m.result().err.empty()) err = m.result().err;
  else if (!wrote) err = "SD short write - the log was not changed";
  else if (rows.tooLong()) err = "a stored row is longer than 16 KB - this is not a log; nothing was changed";
  else err = "the merge stopped - nothing was changed";
  return false;
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
                           const std::string& zone) {
  std::string out = isToday ? "today's " + sumCol + " (" + d.date + (zone.empty() ? "" : ", " + zone) + "): "
                            : sumCol + " on " + d.date + ": ";
  out += numText(d.total);
  if (d.rows == 0) out += " - no rows are dated " + d.date;
  else out += " from " + std::to_string(d.rows) + (d.rows == 1 ? " row" : " rows");
  if (d.unread > 0)
    out += "; " + std::to_string(d.unread) + (d.unread == 1 ? " row" : " rows") + " dated " + d.date +
           (d.unread == 1 ? " is not counted: its '" : " are not counted: their '") + sumCol +
           "' field could not be read (not a plain number, or an unquoted comma shifted it)";
  return out;
}

namespace {
// What one new row did, as reply lines.
std::string renderChange(const RowChange& c, const std::string& sumCol) {
  const std::string what = (c.date.empty() ? std::string() : c.date + " ") + "'" + c.key + "'";
  if (!c.replaced) return "- added " + what + "\n";
  std::string out = "- replaced " + what + (c.was.empty() ? std::string() : " (was " + c.was + " " + sumCol + ")") + "\n";
  if (c.superseded > 0)
    out += "- set aside " + std::to_string(c.superseded) + " more " + (c.superseded == 1 ? "row" : "rows") + " of " +
           what + " as #superseded: (not counted)\n";
  return out;
}
}  // namespace

std::string renderUpsertReply(const CsvUpsert& up, const std::string& where,
                              const std::string& sumCol, const std::string& zone) {
  std::string out = "replaced " + std::to_string(up.replaced) + (up.replaced == 1 ? " row" : " rows") +
                    ", added " + std::to_string(up.appended) + (up.appended == 1 ? " row" : " rows") +
                    " in " + where + (up.changed ? "" : " (no change: the rows were already as sent)") + "\n";
  for (const RowChange& c : up.changes) out += renderChange(c, sumCol);
  if (up.cut) {
    const std::string which = up.cutDate.empty() ? std::string() : " (dated " + up.cutDate + ")";
    out += up.cutReplaced ? "- the stored last row" + which + " was cut mid-write; a new row replaced it\n"
                          : "- the stored last row" + which +
                                " was cut mid-write; it is kept as a #cut: row and not counted - log it again in full\n";
  }
  for (size_t i = 0; i < up.days.size(); i++)
    out += renderDayTotal(up.days[i], sumCol, i == 0 && up.hasTotal, zone) + "\n";
  if (up.undated > 0) out += renderUndated(up.undated) + "\n";
  if (!up.noTotal.empty()) out += "[no day total: " + up.noTotal + "]\n";
  return out;
}

std::string renderUndated(int rows) {
  return "[" + std::to_string(rows) + (rows == 1 ? " row has" : " rows have") +
         " no YYYY-MM-DD date and count toward no day's total]";
}

}  // namespace nimbus::orch
