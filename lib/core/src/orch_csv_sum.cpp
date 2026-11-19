// orch_csv_sum - the CSV scan behind read_file column/from/to/sum (file_aids.h).
//
// One pass, byte by byte, over chunks of any size: the record splitter keeps at most one
// record (kCsvRecordMax) and the shown page; the identical-line table grows in fixed
// blocks up to kCsvIdenticalMax rows. Every block comes from workingAlloc and is checked.
#include <cstdio>
#include <cstring>
#include <new>

#include "nimbus/orch/file_aids.h"
#include "nimbus/orch/psram_alloc.h"

namespace nimbus::orch::aids {

namespace {

// Model-facing templates of the scan (tool results; ruling 42's read_file aids).
namespace T {
constexpr const char* kNoHeader = "sum and column need a CSV header line; line ";
constexpr const char* kNoHeaderIs = " is: \"";
constexpr const char* kOnlyComments = "sum and column need a CSV header line; every line is a comment";
constexpr const char* kNotInHeader = " is not in the header: ";
constexpr const char* kTwice = " is in the header twice: ";
constexpr const char* kNul = " is not text (a NUL byte on line ";
constexpr const char* kPastRange = " is past the exact range; nothing summed";
constexpr const char* kCardRead = "card read failed";
}  // namespace T

constexpr size_t kDupBlock = 512;
constexpr size_t kDupBlocks = kCsvIdenticalMax / kDupBlock;
constexpr size_t kDupBuckets = 1024;
constexpr uint16_t kNone = 0xFFFF;
constexpr size_t kWanted = kCsvSumMax + 1;  // the key and the sums

struct DupEntry {
  uint64_t hash;
  uint32_t line;
  uint16_t next;   // the next entry in this bucket
  uint16_t group;  // the group's first entry
};
struct DupBlock {
  DupEntry e[kDupBlock];
  uint16_t n[kDupBlock];  // a group's size, kept on its first entry
};

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

void listAdd(LineList& l, uint32_t line, int column = -1) {
  ++l.count;
  if (l.lines.size() >= kAidListMax) return;
  l.lines.push_back(line);
  if (column >= 0) l.columns.push_back(static_cast<uint8_t>(column));
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
namespace {

std::string checkSumNames(const std::vector<std::string>& sum) {
  if (sum.size() > kCsvSumMax) return "sum takes at most " + num(kCsvSumMax) + " columns";
  for (size_t i = 0; i < sum.size(); ++i) {
    if (trimmed(sum[i]).empty()) return "sum has an empty column name";
    for (size_t j = 0; j < i; ++j)
      if (trimmed(sum[j]) == trimmed(sum[i])) return "sum names " + sum[i] + " twice";
  }
  return "";
}

}  // namespace

std::string checkCsvQuery(const CsvQuery& q) {
  if (q.column.empty() && q.sum.empty()) return "column or sum is needed";
  if (!q.column.empty() && q.from.empty()) return "column needs from";
  if (q.column.empty() && (!q.from.empty() || !q.to.empty())) return "from and to need column";
  const std::string names = checkSumNames(q.sum);
  if (!names.empty()) return names;
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
  uint64_t physLine = 1;
  uint32_t recLine = 1;
  bool inRecord = false, inQ = false, pendingQ = false, fieldStart = true, tooLong = false;
  bool leadBlank = true, inComment = false;   // only blanks so far; a '#' line being skipped
  uint32_t lastComment = 0;
  bool headerDone = false, showFull = false;
  char last = '\n';

  int keyIdx = -1;
  int sumIdx[kCsvSumMax];
  size_t sums = 0;

  uint16_t* buckets = nullptr;
  DupBlock* blocks[kDupBlocks] = {};
  size_t dupCount = 0;

  Impl(CsvScanResult& r, std::string& e, const CsvQuery& query) : res(r), err(e), q(query) {}
  ~Impl() {
    workingFree(rec);
    workingFree(cell);
    workingFree(body);
    workingFree(buckets);
    for (DupBlock* b : blocks) workingFree(b);
  }

  bool fail(std::string e) {
    err = std::move(e);
    return false;
  }

  // ---- bytes to records ----
  void startRecord() {
    inRecord = true;
    recLine = uint32_t(physLine);
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
  void locate(std::string_view h, int* found, int* seen) {
    const std::string_view col = trimmed(q.column);
    res.headerFields = eachField(h.data(), h.size(), cell, [&](size_t i, std::string_view name) {
      for (size_t w = 0; w <= q.sum.size(); ++w) {
        const std::string_view want = w == 0 ? col : trimmed(q.sum[w - 1]);
        if (want.empty() || name != want) continue;
        found[w] = int(i);
        ++seen[w];
      }
    });
  }
  bool checkNames(std::string_view h, const int* seen) {
    const std::string quote = quoteCut(h, kAidHeaderQuoteMax);
    for (size_t w = 0; w <= q.sum.size(); ++w) {
      const std::string_view name = w == 0 ? trimmed(q.column) : trimmed(q.sum[w - 1]);
      if (name.empty()) continue;
      if (seen[w] != 1) return fail(join({"column ", name, seen[w] ? T::kTwice : T::kNotInHeader, quote}));
    }
    return true;
  }
  bool header() {
    headerDone = true;
    const std::string_view h = withoutBom(rec, recLen);
    if (h.empty() || tooLong)
      return fail(join({T::kNoHeader, Num(recLine), T::kNoHeaderIs, quoteCut(h, kAidLineOneQuoteMax), "\""}));
    int found[kWanted], seen[kWanted] = {};
    for (int& f : found) f = -1;
    locate(h, found, seen);
    if (!checkNames(h, seen)) return false;
    keyIdx = found[0];
    sums = q.sum.size();
    for (size_t s = 0; s < sums; ++s) sumIdx[s] = found[s + 1];
    // The header leads every page.
    const int ln = std::snprintf(body, 16, "L%u ", unsigned(recLine));
    std::memcpy(body + ln, h.data(), h.size());
    bodyLen = size_t(ln) + h.size();
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
  // Put the wanted fields in want (key at 0, sums after); false: field count differs.
  bool fields(std::string_view* want) {
    const size_t n = eachField(rec, recLen, cell, [&](size_t i, std::string_view v) {
      if (int(i) == keyIdx) want[0] = v;
      for (size_t s = 0; s < sums; ++s)
        if (int(i) == sumIdx[s]) want[s + 1] = v;
    });
    return n == res.headerFields;
  }
  bool endRecord(uint32_t lastLine) {
    inRecord = false;
    if (!tooLong && recLen && rec[recLen - 1] == '\r') --recLen;
    if (!headerDone) return header();
    if (!tooLong && recLen == 0) return ++res.emptyLines, true;
    ++res.records;
    if (tooLong) return spanAdd(res.tooLong, recLine, lastLine), true;
    std::string_view want[kWanted];
    const bool counted = fields(want);
    const int r = keyIdx >= 0 ? inRange(want[0]) : 1;
    if (!counted) return badCount(r, lastLine);
    if (r < 0) spanAdd(res.notCompared, recLine, lastLine);
    if (r == 0) ++res.outOfRange;
    if (r <= 0) return true;
    ++res.matched;
    show();
    return remember() && addSums(want + 1);
  }
  // A row whose field count differs from the header's. Another day's (its key plainly out of
  // range) is counted only; one in range, not comparable or spanning lines (it may hold other
  // rows) is named by line.
  bool badCount(int r, uint32_t lastLine) {
    if (r == 0 && lastLine == recLine) ++res.fieldCountOutside;
    else spanAdd(res.fieldCount, recLine, lastLine);
    return true;
  }

  void show() {
    if (!q.showRows || res.matched <= q.skipRows || showFull) return;
    char lead[16];
    const int ln = std::snprintf(lead, sizeof lead, "L%u ", unsigned(recLine));
    const size_t need = size_t(ln) + recLen + 1;
    if (res.shownFirst && bodyLen + need > limit) {
      showFull = true;
      return;
    }
    std::memcpy(body + bodyLen, lead, size_t(ln));
    std::memcpy(body + bodyLen + size_t(ln), rec, recLen);
    bodyLen += need;
    body[bodyLen - 1] = '\n';
    if (!res.shownFirst) res.shownFirst = res.matched;
    res.shownLast = res.matched;
  }

  // ---- identical lines ----
  DupEntry& entry(size_t i) { return blocks[i / kDupBlock]->e[i % kDupBlock]; }
  uint16_t& groupSize(size_t i) { return blocks[i / kDupBlock]->n[i % kDupBlock]; }
  bool tableRoom() {
    if (!buckets) {
      buckets = static_cast<uint16_t*>(workingAlloc(kDupBuckets * sizeof(uint16_t)));
      if (!buckets) return false;
      for (size_t i = 0; i < kDupBuckets; ++i) buckets[i] = kNone;
    }
    DupBlock*& blk = blocks[dupCount / kDupBlock];
    if (!blk) blk = static_cast<DupBlock*>(workingAlloc(sizeof(DupBlock)));
    return blk != nullptr;
  }
  bool remember() {
    if (dupCount >= kCsvIdenticalMax) return true;
    if (!tableRoom()) return fail(kAidNoMemory);
    const uint64_t h = fnv64(kFnv64Basis, std::string_view(rec, recLen));
    uint16_t& head = buckets[h % kDupBuckets];
    const uint16_t at = uint16_t(dupCount);
    uint16_t group = at;
    for (uint16_t e = head; e != kNone && group == at; e = entry(e).next)
      if (entry(e).hash == h) group = entry(e).group;
    entry(at) = {h, recLine, head, group};
    head = at;
    groupSize(at) = 1;
    if (group != at) ++groupSize(group);
    res.identicalChecked = ++dupCount;
    return true;
  }
  void collectGroups() {
    for (size_t i = 0; i < dupCount; ++i) {
      if (entry(i).group != i || groupSize(i) < 2) continue;
      ++res.identicalGroups;
      if (res.identical.size() >= kAidListMax) continue;
      LineGroup g;
      g.count = groupSize(i);
      for (size_t j = i; j < dupCount && g.lines.size() < kAidGroupLinesMax; ++j)
        if (entry(j).group == i) g.lines.push_back(entry(j).line);
      res.identical.push_back(std::move(g));
    }
  }

  // ---- totals ----
  bool addSums(const std::string_view* cells) {
    for (size_t s = 0; s < sums; ++s) {
      CsvColumnSum& col = res.sums[s];
      Dec3 v;
      const NumParse p = parseDec3(cells[s], v);
      if (p == NumParse::Blank || p == NumParse::NotNumber) {
        ++(p == NumParse::Blank ? col.blank : col.notNumber);
        listAdd(p == NumParse::Blank ? res.blankCells : res.notNumberCells, recLine, int(s));
        continue;
      }
      if (p == NumParse::Rounded) listAdd(res.roundedCells, recLine, int(s));
      col.rounded += p == NumParse::Rounded;
      ++col.numbers;
      if (__builtin_add_overflow(col.total.milli, v.milli, &col.total.milli) || col.total.milli == INT64_MIN)
        return fail("the total of " + col.name + T::kPastRange);
    }
    return true;
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
    collectGroups();
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
  for (const auto& s : q.sum) {
    res_.sums.emplace_back();
    res_.sums.back().name = std::string(trimmed(s));
  }
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

void cellsInto(Txt& t, const LineList& l, const CsvScanResult& r) {
  for (size_t i = 0; i < l.lines.size(); ++i)
    t << (i ? ", " : "") << std::string_view(r.sums[l.columns[i]].name) << " line " << uint64_t(l.lines[i]);
  more(t, l.count, l.lines.size());
}

void groupsInto(Txt& t, const CsvScanResult& r) {
  for (size_t i = 0; i < r.identical.size(); ++i) {
    const LineGroup& g = r.identical[i];
    for (size_t k = 0; k < g.lines.size(); ++k) t << (k ? "=" : (i ? ", " : "")) << uint64_t(g.lines[k]);
    if (g.count > g.lines.size()) t << "+" << uint64_t(g.count - g.lines.size());
  }
  more(t, r.identicalGroups, r.identical.size());
}

// One column's cells of a reason (blank, not a number) as a line list: the lines of that
// column the list names, and that column's full count.
LineList cellsOf(const LineList& l, size_t col, size_t count) {
  LineList out;
  out.count = count;
  for (size_t i = 0; i < l.lines.size(); ++i)
    if (i < l.columns.size() && l.columns[i] == col) out.lines.push_back(l.lines[i]);
  return out;
}

// ", " before every item but the first of a "not in it" list.
void item(Txt& t, bool& first, const LineList& l) {
  t << (first ? "; not in it: " : ", ");
  first = false;
  linesInto(t, l);
}

// Ruling 88: a figure and what it covers are one statement of fact:
//   "; sum <name>=<total> (<k> of <n> rows[; not in it: <lines> (<why>), ...])"
// k is the cells that entered the total; n the matching rows plus every row the range may
// hold that the scan could not place or read (a wrong field count, a key not compared, a row
// too long), each of them named here with its reason, as are the column's blank and
// not-a-number cells.
void sumInto(Txt& t, const CsvQuery& q, const CsvScanResult& r, size_t i) {
  const CsvColumnSum& s = r.sums[i];
  const uint64_t n = r.matched + r.fieldCount.count + r.notCompared.count + r.tooLong.count;
  t << "; sum " << std::string_view(s.name) << "=" << std::string_view(renderDec3(s.total)) << " ("
    << uint64_t(s.numbers) << " of " << n << (n == 1 ? " row" : " rows");
  bool first = true;
  if (r.fieldCount.count) {
    item(t, first, r.fieldCount);
    t << " (field count is not the header's " << uint64_t(r.headerFields) << ")";
  }
  if (r.notCompared.count) {
    item(t, first, r.notCompared);
    t << " (" << std::string_view(q.column) << " blank or not like " << std::string_view(q.from) << ")";
  }
  if (r.tooLong.count) {
    item(t, first, r.tooLong);
    t << " (over " << uint64_t(kCsvRecordMax) << " B)";
  }
  if (s.blank) {
    item(t, first, cellsOf(r.blankCells, i, s.blank));
    t << " (blank)";
  }
  if (s.notNumber) {
    item(t, first, cellsOf(r.notNumberCells, i, s.notNumber));
    t << " (not a number)";
  }
  t << ")";
}

// The row counts: rows, the range, what is shown, the sums.
void countsInto(Txt& t, const CsvQuery& q, const CsvScanResult& r) {
  t << "rows " << uint64_t(r.matched) << " of " << uint64_t(r.records);
  if (!q.column.empty())
    t << " where " << std::string_view(q.column) << " " << std::string_view(q.from) << ".."
      << std::string_view(q.to.empty() ? q.from : q.to);
  const bool partial = r.shownFirst > 1 || r.shownLast < r.matched;
  if (q.showRows && !r.shownFirst && r.matched) t << "; shown none";
  else if (q.showRows && partial) t << "; shown " << uint64_t(r.shownFirst) << "-" << uint64_t(r.shownLast);
  for (size_t i = 0; i < r.sums.size(); ++i) sumInto(t, q, r, i);
}

// "; <what> <k> (" when the list has rows; false when it has none.
bool open(Txt& t, const char* what, size_t count) {
  if (count) t << "; " << what << " " << uint64_t(count) << " (";
  return count > 0;
}

// The lists of rows and cells left out of a total, by reason.
void listsInto(Txt& t, const CsvQuery& q, const CsvScanResult& r) {
  if (r.commentLines) {
    t << "; " << uint64_t(r.commentLines) << (r.commentLines == 1 ? " comment line" : " comment lines") << " skipped (";
    linesInto(t, r.commentSpans);
    t << ")";
  }
  const bool summed = !r.sums.empty();   // then blank, not a number, not compared, skipped and too long are in each sum
  const struct {
    const char* what;
    const LineList& l;
  } cells[] = {{"blank", summed ? LineList() : r.blankCells},
               {"not a number", summed ? LineList() : r.notNumberCells},
               {"rounded to 3 decimals", r.roundedCells}};
  for (const auto& c : cells)
    if (open(t, c.what, c.l.count)) cellsInto(t, c.l, r), t << ")";
  if (summed) return;
  if (open(t, "not compared", r.notCompared.count)) {
    linesInto(t, r.notCompared);
    t << ": " << std::string_view(q.column) << " blank or not like " << std::string_view(q.from) << ")";
  }
  if (open(t, "skipped", r.fieldCount.count)) {
    linesInto(t, r.fieldCount);
    t << ": field count is not the header's " << uint64_t(r.headerFields) << ")";
  }
}

// Every other fact a total depends on.
void factsInto(Txt& t, const CsvScanResult& r) {
  if (r.fieldCountOutside)
    t << "; " << uint64_t(r.fieldCountOutside)
      << (r.fieldCountOutside == 1 ? " other row outside the range has" : " other rows outside the range have")
      << " a wrong field count";
  if (r.sums.empty() && open(t, "too long", r.tooLong.count)) {
    linesInto(t, r.tooLong);
    t << ": over " << uint64_t(kCsvRecordMax) << " B)";
  }
  if (r.emptyLines) t << "; empty lines " << uint64_t(r.emptyLines);
  if (r.identicalGroups) {
    t << "; identical lines " << uint64_t(r.identicalGroups) << (r.identicalGroups == 1 ? " group (" : " groups (");
    groupsInto(t, r);
    t << ")";
  }
  if (r.identicalChecked < r.matched)
    t << "; identical lines checked in the first " << uint64_t(r.identicalChecked) << " matching rows only";
  if (r.tornLine) t << "; last line " << uint64_t(r.tornLine) << " has no newline (a write may have been cut short)";
  if (r.openQuoteLine)
    t << "; the row at line " << uint64_t(r.openQuoteLine) << " ends inside a quoted field (a write may have been cut short)";
}

}  // namespace

std::string renderLines(const LineList& l) {
  if (!l.count) return "";
  Txt t;
  linesInto(t, l);
  return std::move(t.s);
}

std::string renderCsvTail(const CsvQuery& q, const CsvScanResult& r) {
  Txt t;
  countsInto(t, q, r);
  listsInto(t, q, r);
  factsInto(t, r);
  return std::move(t.s);
}

std::string renderCsvRead(const CsvQuery& q, const CsvScanResult& r) {
  Txt t;
  t << "\n" << std::string_view(r.body);
  countsInto(t, q, r);
  listsInto(t, q, r);
  factsInto(t, r);
  if (r.shownFirst && r.shownLast < r.matched) t << "\nnext_offset=" << uint64_t(r.shownLast);
  return std::move(t.s);
}

}  // namespace nimbus::orch::aids
