// orch_csv_sum - the CSV scan behind read_file column/from/to (file_aids.h). It totals nothing
// (ruling 95) and its result is a plain read of the selected rows (ruling 97): the rows as
// stored, in file order, and nothing said about the others.
//
// One pass, byte by byte, over chunks of any size: the record splitter keeps at most one
// record (kCsvRecordMax) and the shown part. Every block comes from workingAlloc and is checked.
#include <cstdio>
#include <cstring>
#include <new>

#include "nimbus/orch/file_aids.h"
#include "nimbus/orch/psram_alloc.h"

namespace nimbus::orch::aids {

namespace {

// Model-facing templates of the scan (tool results; ruling 42's read_file aids).
namespace T {
constexpr const char* kNoHeader = "column needs a CSV header line; line ";
constexpr const char* kNoHeaderIs = " is: \"";
constexpr const char* kOnlyComments = "column needs a CSV header line; every line is a comment";
constexpr const char* kNotInHeader = " is not in the header: ";
constexpr const char* kTwice = " is in the header twice: ";
constexpr const char* kNul = " is not text (a NUL byte on line ";
constexpr const char* kCardRead = "card read failed";
}  // namespace T

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string_view trimmed(std::string_view s) {
  while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
  return s;
}

std::string num(uint64_t v) { return std::string(detail::Num(v)); }
using detail::join;
using detail::Num;
using detail::quoteCut;

int shapeOf(char c) {
  if (isDigit(c)) return 256;
  if (isLetter(c)) return 257;
  return static_cast<unsigned char>(c);
}

// The key can be compared with bound b (file_aids.h's prefix rule).
bool comparable(std::string_view k, std::string_view b) {
  if (k.size() < b.size()) return false;
  for (size_t i = 0; i < b.size(); ++i)
    if (shapeOf(k[i]) != shapeOf(b[i])) return false;
  return !(!b.empty() && isDigit(b.back()) && k.size() > b.size() && isDigit(k[b.size()]));
}

void listAdd(LineList& l, uint32_t line) {
  ++l.count;
  if (l.lines.size() < kAidListMax) l.lines.push_back(line);
}
void spanAdd(LineList& l, uint32_t first, uint32_t last) {
  if (l.lines.size() < kAidListMax) l.ends.push_back(last);
  listAdd(l, first);
}

}  // namespace

// ---- exact decimals ---------------------------------------------------------------
namespace {

constexpr uint64_t kDecMax = uint64_t(INT64_MAX);

// The digits of a number as they come: thousandths, the fourth decimal for rounding.
struct DecDigits {
  uint64_t v = 0;
  size_t digits = 0, frac = 0;
  bool point = false, roundUp = false, rounded = false;

  bool mulAdd(uint64_t d) {
    if (v > (kDecMax - d) / 10) return false;
    v = v * 10 + d;
    return true;
  }
  // False: not a number.
  bool take(char c) {
    if (c == '.' && !point) return point = true;
    if (!isDigit(c)) return false;
    ++digits;
    if (point && frac >= 3) {
      if (frac++ == 3) roundUp = c >= '5';
      rounded = true;
      return true;
    }
    frac += point;
    return mulAdd(uint64_t(c - '0'));
  }
  bool finish() {
    for (; frac < 3; ++frac)
      if (!mulAdd(0)) return false;
    if (roundUp && v == kDecMax) return false;
    v += roundUp;
    return digits > 0;
  }
};

}  // namespace

NumParse parseDec3(std::string_view cell, Dec3& out) {
  out.milli = 0;
  std::string_view s = trimmed(cell);
  if (s.empty()) return NumParse::Blank;
  const bool neg = s[0] == '-';
  if (s[0] == '+' || s[0] == '-') s.remove_prefix(1);
  DecDigits d;
  for (char c : s)
    if (!d.take(c)) return NumParse::NotNumber;
  if (!d.finish()) return NumParse::NotNumber;
  out.milli = neg ? -int64_t(d.v) : int64_t(d.v);
  return d.rounded ? NumParse::Rounded : NumParse::Ok;
}

std::string renderDec3(Dec3 d) {
  const bool neg = d.milli < 0;
  const uint64_t v = neg ? uint64_t(0) - uint64_t(d.milli) : uint64_t(d.milli);
  std::string out = neg ? "-" : "";
  out += num(v / 1000);
  const unsigned f = unsigned(v % 1000);
  if (f) {
    char b[8];
    std::snprintf(b, sizeof b, ".%03u", f);
    size_t n = 4;
    while (b[n - 1] == '0') --n;
    out.append(b, n);
  }
  return out;
}

// ---- the query ----------------------------------------------------------------------
std::string checkCsvQuery(const CsvQuery& q) {
  if (q.column.empty()) return "column is needed";
  if (q.from.empty()) return "column needs from";
  if (q.limit < 16) return "limit is at least 16";
  Dec3 d;
  if (parseDec3(q.from, d) == NumParse::Rounded || parseDec3(q.to, d) == NumParse::Rounded)
    return "from and to take at most 3 decimals";
  return "";
}

namespace {

// The fields of a record (raw bytes), decoded and trimmed, by the documented rule: fn(i,
// value) for each; returns the field count. cell is scratch for the decoded values.
template <class Fn>
size_t eachField(const char* r, size_t n, char* cell, Fn&& fn) {
  size_t count = 0, start = 0, len = 0;
  bool inQ = false, fieldStart = true;
  for (size_t i = 0; i < n; ++i) {
    const char c = r[i];
    if (inQ && c == '"') {
      inQ = i + 1 < n && r[i + 1] == '"';
      if (inQ) cell[start + len++] = r[i++];
      continue;
    }
    if (!inQ && c == ',') {
      fn(count++, trimmed(std::string_view(cell + start, len)));
      start += len;
      len = 0;
      fieldStart = true;
      continue;
    }
    if (!inQ && c == '"' && fieldStart) inQ = true;
    else cell[start + len++] = c;
    fieldStart = false;
  }
  fn(count++, trimmed(std::string_view(cell + start, len)));
  return count;
}

std::string_view withoutBom(const char* p, size_t n) {
  std::string_view h(p, n);
  if (h.size() >= 3 && h.compare(0, 3, "\xEF\xBB\xBF") == 0) h.remove_prefix(3);
  return h;
}

}  // namespace

// ---- the scanner --------------------------------------------------------------------
struct CsvScanner::Impl {
  CsvScanResult& res;
  std::string& err;
  const CsvQuery& q;   // the caller's, alive for the scan (file_aids.h)
  size_t limit = 0;    // q.limit held to kCsvShowMax
  std::string path;
  std::string_view to;
  bool numericRange = false;
  Dec3 fromNum, toNum;

  char* rec = nullptr;   // the current record (kCsvRecordMax)
  char* cell = nullptr;  // decoded wanted fields (kCsvRecordMax)
  char* body = nullptr;
  size_t recLen = 0, bodyLen = 0, bodyCap = 0;
  uint64_t recStart = 0;   // the current record's first byte
  bool partOpen = false;   // a record at or past q.fromByte has started
  uint64_t physLine = 1;
  uint32_t recLine = 1;
  bool inRecord = false, inQ = false, pendingQ = false, fieldStart = true, tooLong = false;
  bool leadBlank = true, inComment = false;   // only blanks so far; a '#' line being skipped
  uint32_t lastComment = 0;
  bool headerDone = false, showFull = false, shownRow = false;
  char last = '\n';

  int keyIdx = -1;

  Impl(CsvScanResult& r, std::string& e, const CsvQuery& query) : res(r), err(e), q(query) {}
  ~Impl() {
    workingFree(rec);
    workingFree(cell);
    workingFree(body);
  }

  bool fail(std::string e) {
    err = std::move(e);
    return false;
  }

  // ---- bytes to records ----
  void startRecord() {
    inRecord = true;
    recLine = uint32_t(physLine);
    recStart = res.bytes - 1;
    if (!partOpen && recStart >= q.fromByte) {   // the part starts at the first line at or past the offset
      partOpen = true;
      res.partFrom = recStart;
      res.partFromLine = recLine;
    }
    recLen = 0;
    inQ = pendingQ = tooLong = inComment = false;
    fieldStart = leadBlank = true;
  }
  // A comment line (first non-blank byte '#'): never a row; counted, its runs named.
  void comment(uint32_t line) {
    inRecord = false;
    ++res.commentLines;
    LineList& l = res.commentSpans;
    if (l.count && lastComment + 1 == line) {
      if (l.lines.size() == l.count) l.ends.back() = line;
    } else {
      spanAdd(l, line, line);
    }
    lastComment = line;
  }
  bool lineEnd() {
    ++physLine;
    return endRecord(uint32_t(physLine - 1));
  }
  // The quote state after c (a byte kept in the record).
  void track(char c) {
    if (inQ && !pendingQ) {
      pendingQ = c == '"';
      physLine += c == '\n';
      return;
    }
    if (inQ) {
      pendingQ = false;
      if (c == '"') return;
      inQ = false;
    }
    if (c == ',') {
      fieldStart = true;
      return;
    }
    if (c == '"' && fieldStart) inQ = true;
    fieldStart = false;
  }
  // A byte of a UTF-8 byte order mark at the very start of the file (blank for the comment test).
  bool bomByte(char c) const {
    static const char kBom[] = "\xEF\xBB\xBF";
    return recLine == 1 && recLen < 3 && res.bytes == recLen + 1 && c == kBom[recLen];
  }
  // True when c belongs to a comment line (and is consumed by it).
  bool commentByte(char c) {
    if (inComment) {
      if (c == '\n') comment(uint32_t(physLine++));
      return true;
    }
    if (!leadBlank || c == ' ' || c == '\t' || c == '\n' || bomByte(c)) return false;
    leadBlank = false;
    return inComment = c == '#';
  }
  bool step(char c) {
    ++res.bytes;
    last = c;
    if (c == '\0') return fail(join({path, T::kNul, Num(physLine), ")"}));
    if (!inRecord) startRecord();
    if (commentByte(c)) return true;
    const bool closing = inQ && pendingQ && c != '"';
    if (c == '\n' && (!inQ || closing)) return lineEnd();
    // Past kCsvRecordMax the bytes are not kept, but the quotes still decide the end.
    tooLong = tooLong || recLen >= kCsvRecordMax;
    if (!tooLong) rec[recLen++] = c;
    track(c);
    return true;
  }

  // ---- the header ----
  // The key column's index in the header, and how many times the header names it.
  void locate(std::string_view h, int& found, int& seen) {
    const std::string_view col = trimmed(q.column);
    res.headerFields = eachField(h.data(), h.size(), cell, [&](size_t i, std::string_view name) {
      if (name != col) return;
      found = int(i);
      ++seen;
    });
  }
  bool checkName(std::string_view h, int seen) {
    if (seen == 1) return true;
    const std::string quote = quoteCut(h, kAidHeaderQuoteMax);
    return fail(join({"column ", trimmed(q.column), seen ? T::kTwice : T::kNotInHeader, quote}));
  }
  bool header() {
    headerDone = true;
    const std::string_view h = withoutBom(rec, recLen);
    if (h.empty() || tooLong)
      return fail(join({T::kNoHeader, Num(recLine), T::kNoHeaderIs, quoteCut(h, kAidLineOneQuoteMax), "\""}));
    int found = -1, seen = 0;
    locate(h, found, seen);
    if (!checkName(h, seen)) return false;
    keyIdx = found;
    if (recStart < q.fromByte) return true;   // the header is shown in the part that holds it
    std::memcpy(body, h.data(), h.size());
    bodyLen = h.size();
    body[bodyLen++] = '\n';
    return true;
  }

  // ---- one data record ----
  // 1 match, 0 out of range, -1 not compared.
  int inRange(std::string_view key) const {
    if (key.empty()) return -1;
    Dec3 k;
    const NumParse p = numericRange ? parseDec3(key, k) : NumParse::NotNumber;
    if (p == NumParse::Rounded) return -1;   // more decimals than the comparison holds
    if (p == NumParse::Ok) return (fromNum.milli <= k.milli && k.milli <= toNum.milli) ? 1 : 0;
    const std::string_view from = q.from;
    if (!comparable(key, from) || !comparable(key, to)) return -1;
    return (key.substr(0, from.size()) >= from && key.substr(0, to.size()) <= to) ? 1 : 0;
  }
  // Put the key field in key; false: field count differs.
  bool fields(std::string_view& key) {
    const size_t n = eachField(rec, recLen, cell, [&](size_t i, std::string_view v) {
      if (int(i) == keyIdx) key = v;
    });
    return n == res.headerFields;
  }
  bool endRecord(uint32_t lastLine) {
    inRecord = false;
    if (!tooLong && recLen && rec[recLen - 1] == '\r') --recLen;
    if (!headerDone) return header();
    if (!tooLong && recLen == 0) return ++res.emptyLines, true;
    ++res.records;
    if (tooLong) return spanAdd(res.tooLong, recLine, lastLine), show(), true;
    std::string_view key;
    const bool counted = fields(key);
    const int r = keyIdx >= 0 ? inRange(key) : 1;
    if (!counted) return badCount(r, lastLine);
    if (r < 0) spanAdd(res.notCompared, recLine, lastLine), show();
    if (r == 0) ++res.outOfRange;
    if (r <= 0) return true;
    ++res.matched;
    show();
    return true;
  }
  // A row whose field count differs from the header's: placed by its key like any row. Plainly
  // out of range on one line it is left out like any row of another day; in range, not
  // comparable, or spanning lines (it may hold other rows) it is kept (ruling 97).
  bool badCount(int r, uint32_t lastLine) {
    if (r == 0 && lastLine == recLine) ++res.fieldCountOutside;
    else spanAdd(res.fieldCount, recLine, lastLine), show();
    return true;
  }

  // A kept record into the part, as stored (its first kCsvRecordMax bytes when it is longer).
  // The part's first row is always shown; a row that does not fit ends the part before it.
  void show() {
    if (recStart < q.fromByte || showFull) return;
    if (shownRow && bodyLen + recLen + 1 > limit) {
      showFull = res.more = true;
      res.partTo = recStart;
      res.partToLine = recLine - 1;
      return;
    }
    std::memcpy(body + bodyLen, rec, recLen);
    bodyLen += recLen;
    body[bodyLen++] = '\n';
    shownRow = true;
    ++res.shown;
  }

  bool finish() {
    if (inRecord && inComment) comment(recLine);
    if (inRecord && inQ && !pendingQ) res.openQuoteLine = recLine;
    if (inRecord && !endRecord(uint32_t(physLine - (last == '\n' ? 1 : 0)))) return false;   // the last byte's line
    if (!headerDone && res.commentLines) return fail(T::kOnlyComments);
    if (!headerDone) return fail(join({T::kNoHeader, "1", T::kNoHeaderIs, "\""}));
    const bool torn = res.bytes > 0 && last != '\n';
    res.lines = physLine - 1 + (torn ? 1 : 0);
    if (torn) res.tornLine = uint32_t(res.lines);
    if (!partOpen) res.partFrom = res.bytes, res.partFromLine = uint32_t(res.lines + 1);
    if (!res.more) res.partTo = res.bytes, res.partToLine = uint32_t(res.lines);
    res.body.assign(body, bodyLen);
    return true;
  }
};

CsvScanner::CsvScanner(const CsvQuery& q, const std::string& path, CsvScanResult* into)
    : res_(into ? *into : own_) {
  err_ = checkCsvQuery(q);
  if (!err_.empty()) return;
  void* mem = workingAlloc(sizeof(Impl));
  if (!mem) {
    err_ = kAidNoMemory;
    return;
  }
  impl_ = new (mem) Impl(res_, err_, q);
  Impl& m = *impl_;
  m.path = path;
  m.to = q.to.empty() ? std::string_view(q.from) : std::string_view(q.to);
  m.numericRange = !q.column.empty() && parseDec3(q.from, m.fromNum) == NumParse::Ok &&
                   parseDec3(m.to, m.toNum) == NumParse::Ok;
  m.limit = std::min(q.limit, kCsvShowMax);
  m.bodyCap = m.limit + 2 * (kCsvRecordMax + 32);
  m.rec = static_cast<char*>(workingAlloc(kCsvRecordMax));
  m.cell = m.rec ? static_cast<char*>(workingAlloc(kCsvRecordMax)) : nullptr;
  m.body = m.cell ? static_cast<char*>(workingAlloc(m.bodyCap)) : nullptr;
  if (!m.body) err_ = kAidNoMemory;
}

CsvScanner::~CsvScanner() {
  if (!impl_) return;
  impl_->~Impl();
  workingFree(impl_);
}

bool CsvScanner::feed(const char* data, size_t len) {
  if (!err_.empty() || !impl_) return false;
  for (size_t i = 0; i < len; ++i)
    if (!impl_->step(data[i])) return false;
  return true;
}

bool CsvScanner::finish() { return err_.empty() && impl_ && impl_->finish(); }

// ---- the driver ---------------------------------------------------------------------
CsvScanOutcome scanCsv(AidSource& src, uint64_t size, const std::string& path, const CsvQuery& q) {
  CsvScanOutcome o;
  CsvScanner s(q, path, &o.result);
  char* chunk = s.error().empty() ? static_cast<char*>(workingAlloc(kAidChunk)) : nullptr;
  if (!s.error().empty() || !chunk) {
    o.error = s.error().empty() ? kAidNoMemory : s.error();
    return o;
  }
  bool ok = true;
  uint64_t off = 0;
  for (;;) {
    size_t got = 0;
    if (!src.readAt(off, kAidChunk, chunk, got)) {
      o.error = T::kCardRead;
      ok = false;
      break;
    }
    if (got && !s.feed(chunk, got)) {
      ok = false;   // the scanner's own error is said below
      break;
    }
    off += got;
    if (got < kAidChunk) break;
  }
  // A read that stopped short of the size the store gave is a card fault, never a shorter file.
  if (ok && off < size) {
    o.error = T::kCardRead;
    ok = false;
  }
  workingFree(chunk);
  if (ok && s.finish()) {
    o.ok = true;
  } else if (o.error.empty()) {
    o.error = s.error();
  }
  return o;
}

// ---- rendering ----------------------------------------------------------------------
namespace detail {

std::string join(std::initializer_list<std::string_view> parts) {
  Txt t;
  for (auto p : parts) t << p;
  return std::move(t.s);
}

Num::Num(uint64_t v) {
  char t[24];
  size_t k = 0;
  do {
    t[k++] = char('0' + v % 10);
    v /= 10;
  } while (v);
  for (n = 0; n < k; ++n) b[n] = t[k - 1 - n];
}

Txt& Txt::operator<<(const char* p) {
  s += p;
  return *this;
}
Txt& Txt::operator<<(std::string_view v) {
  s.append(v.data(), v.size());
  return *this;
}
Txt& Txt::operator<<(uint64_t v) {
  const Num n(v);
  s.append(n.b, n.n);
  return *this;
}

std::string quoteCut(std::string_view s, size_t n) {
  if (s.size() <= n) return std::string(s);
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return join({s.substr(0, n), " ..."});
}

}  // namespace detail

namespace {

using detail::Txt;

// " +N" when a list names fewer than it counts.
void more(Txt& t, size_t count, size_t named) {
  if (count > named) t << " +" << uint64_t(count - named);
}

void linesInto(Txt& t, const LineList& l) {
  const bool span = l.ends.size() == l.lines.size() && !l.ends.empty() && l.ends[0] > l.lines[0];
  t << (l.count == 1 && !span ? "line " : "lines ");
  for (size_t i = 0; i < l.lines.size(); ++i) {
    t << (i ? ", " : "") << uint64_t(l.lines[i]);
    if (i < l.ends.size() && l.ends[i] > l.lines[i]) t << "-" << uint64_t(l.ends[i]);
  }
  more(t, l.count, l.lines.size());
}

}  // namespace

std::string renderLines(const LineList& l) {
  if (!l.count) return "";
  Txt t;
  linesInto(t, l);
  return std::move(t.s);
}

}  // namespace nimbus::orch::aids
