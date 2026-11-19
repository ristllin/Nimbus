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
constexpr const char* kNoHeader = "sum and column need a CSV header line; line 1 is: \"";
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

std::string num(uint64_t v) { return std::to_string(v); }

// At most n bytes of s, cut before a UTF-8 continuation byte, with " ..." when cut.
std::string quoteCut(std::string_view s, size_t n) {
  if (s.size() <= n) return std::string(s);
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return std::string(s.substr(0, n)) + " ...";
}

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

}  // namespace

// ---- exact decimals ---------------------------------------------------------------
NumParse parseDec3(std::string_view cell, Dec3& out) {
  out.milli = 0;
  const std::string_view s = trimmed(cell);
  if (s.empty()) return NumParse::Blank;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') neg = s[i++] == '-';
  uint64_t v = 0;
  size_t digits = 0, frac = 0;
  bool point = false, roundUp = false, rounded = false;
  constexpr uint64_t kMax = uint64_t(INT64_MAX);
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '.' && !point) {
      point = true;
      continue;
    }
    if (!isDigit(c)) return NumParse::NotNumber;
    ++digits;
    if (point && frac >= 3) {
      if (frac == 3) roundUp = c >= '5';
      rounded = true;
      ++frac;
      continue;
    }
    frac += point;
    if (v > (kMax - uint64_t(c - '0')) / 10) return NumParse::NotNumber;
    v = v * 10 + uint64_t(c - '0');
  }
  if (!digits) return NumParse::NotNumber;
  for (; frac < 3; ++frac) {
    if (v > kMax / 10) return NumParse::NotNumber;
    v *= 10;
  }
  if (roundUp) {
    if (v == kMax) return NumParse::NotNumber;
    ++v;
  }
  out.milli = neg ? -int64_t(v) : int64_t(v);
  return rounded ? NumParse::Rounded : NumParse::Ok;
}

std::string renderDec3(Dec3 d) {
  const bool neg = d.milli < 0;
  const uint64_t v = neg ? uint64_t(0) - uint64_t(d.milli) : uint64_t(d.milli);
  std::string out = neg ? "-" : "";
  out += num(v / 1000);
  unsigned f = unsigned(v % 1000);
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
  if (q.column.empty() && q.sum.empty()) return "column or sum is needed";
  if (!q.column.empty() && q.from.empty()) return "column needs from";
  if (q.column.empty() && (!q.from.empty() || !q.to.empty())) return "from and to need column";
  if (q.sum.size() > kCsvSumMax) return "sum takes at most " + num(kCsvSumMax) + " columns";
  for (size_t i = 0; i < q.sum.size(); ++i) {
    if (trimmed(q.sum[i]).empty()) return "sum has an empty column name";
    for (size_t j = 0; j < i; ++j)
      if (trimmed(q.sum[j]) == trimmed(q.sum[i])) return "sum names " + q.sum[i] + " twice";
  }
  if (q.limit < 16) return "limit is at least 16";
  return "";
}

// ---- the scanner --------------------------------------------------------------------
struct CsvScanner::Impl {
  CsvQuery q;
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
  bool headerDone = false, showFull = false;
  char last = '\n';

  int keyIdx = -1;
  int sumIdx[kCsvSumMax];
  size_t sums = 0;

  uint16_t* buckets = nullptr;
  DupBlock* blocks[kDupBlocks] = {};
  size_t dupCount = 0;

  ~Impl() {
    workingFree(rec);
    workingFree(cell);
    workingFree(body);
    workingFree(buckets);
    for (DupBlock* b : blocks) workingFree(b);
  }
};

CsvScanner::CsvScanner(const CsvQuery& q, const std::string& path) {
  err_ = checkCsvQuery(q);
  if (!err_.empty()) return;
  void* mem = workingAlloc(sizeof(Impl));
  if (!mem) {
    err_ = kAidNoMemory;
    return;
  }
  impl_ = new (mem) Impl();
  Impl& m = *impl_;
  m.q = q;
  m.path = path;
  m.to = m.q.to.empty() ? std::string_view(m.q.from) : std::string_view(m.q.to);
  m.numericRange = !m.q.column.empty() && parseDec3(m.q.from, m.fromNum) <= NumParse::Rounded &&
                   parseDec3(m.to, m.toNum) <= NumParse::Rounded;
  m.bodyCap = std::min<size_t>(q.limit, 1u << 20) + 2 * (kCsvRecordMax + 32);
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

namespace {

// The fields of a record (raw bytes), decoded and trimmed, by the documented rule. fn(i,
// value) for each; returns the field count. cell is scratch for the decoded values.
template <class Fn>
size_t eachField(const char* r, size_t n, char* cell, Fn&& fn) {
  size_t count = 0, start = 0, len = 0;
  bool inQ = false, fieldStart = true;
  auto done = [&]() {
    fn(count, trimmed(std::string_view(cell + start, len)));
    ++count;
    start += len;
    len = 0;
  };
  for (size_t i = 0; i < n; ++i) {
    const char c = r[i];
    if (inQ) {
      if (c == '"') {
        if (i + 1 < n && r[i + 1] == '"') {
          cell[start + len++] = '"';
          ++i;
          continue;
        }
        inQ = false;
        continue;
      }
      cell[start + len++] = c;
      continue;
    }
    if (c == ',') {
      done();
      fieldStart = true;
      continue;
    }
    if (c == '"' && fieldStart) {
      inQ = true;
      fieldStart = false;
      continue;
    }
    fieldStart = false;
    cell[start + len++] = c;
  }
  done();
  return count;
}

}  // namespace

bool CsvScanner::feed(const char* data, size_t len) {
  if (!err_.empty() || !impl_) return false;
  Impl& m = *impl_;
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    ++res_.bytes;
    m.last = c;
    if (c == '\0') {
      err_ = m.path + T::kNul + num(m.physLine) + ")";
      return false;
    }
    if (!m.inRecord) {
      m.inRecord = true;
      m.recLine = uint32_t(m.physLine);
      m.recLen = 0;
      m.inQ = m.pendingQ = m.tooLong = false;
      m.fieldStart = true;
    }
    if (m.tooLong) {
      if (c == '\n') {
        ++m.physLine;
        if (!endRecord()) return false;
      }
      continue;
    }
    const bool closing = m.inQ && m.pendingQ && c != '"';
    if (c == '\n' && (!m.inQ || closing)) {
      ++m.physLine;
      if (!endRecord()) return false;
      continue;
    }
    if (m.recLen >= kCsvRecordMax) {
      m.tooLong = true;
      m.physLine += c == '\n';
      continue;
    }
    m.rec[m.recLen++] = c;
    if (m.inQ) {
      if (m.pendingQ) {
        m.pendingQ = false;
        if (c == '"') continue;
        m.inQ = false;
      } else {
        if (c == '"') m.pendingQ = true;
        m.physLine += c == '\n';
        continue;
      }
    }
    if (c == ',') {
      m.fieldStart = true;
    } else {
      if (c == '"' && m.fieldStart) m.inQ = true;
      m.fieldStart = false;
    }
  }
  return true;
}

bool CsvScanner::finish() {
  if (!err_.empty() || !impl_) return false;
  Impl& m = *impl_;
  if (m.inRecord && !endRecord()) return false;
  if (!m.headerDone) {
    err_ = std::string(T::kNoHeader) + "\"";
    return false;
  }
  res_.lines = m.physLine - 1 + (res_.bytes > 0 && m.last != '\n' ? 1 : 0);
  if (res_.bytes > 0 && m.last != '\n') res_.tornLine = uint32_t(res_.lines);
  collectGroups();
  res_.body.assign(m.body, m.bodyLen);
  return true;
}

bool CsvScanner::header() {
  Impl& m = *impl_;
  m.headerDone = true;
  std::string_view h(m.rec, m.recLen);
  if (h.size() >= 3 && h.compare(0, 3, "\xEF\xBB\xBF") == 0) h.remove_prefix(3);
  if (h.empty() || m.tooLong) {
    err_ = T::kNoHeader + quoteCut(h, kAidLineOneQuoteMax) + "\"";
    return false;
  }
  const std::string_view col = trimmed(m.q.column);
  int found[kWanted];
  int seen[kWanted] = {};
  for (int& f : found) f = -1;
  const size_t n = eachField(h.data(), h.size(), m.cell, [&](size_t i, std::string_view name) {
    if (!col.empty() && name == col) {
      found[0] = int(i);
      ++seen[0];
    }
    for (size_t s = 0; s < m.q.sum.size(); ++s)
      if (name == trimmed(m.q.sum[s])) {
        found[s + 1] = int(i);
        ++seen[s + 1];
      }
  });
  res_.headerFields = n;
  const std::string quote = quoteCut(h, kAidHeaderQuoteMax);
  for (size_t w = 0; w <= m.q.sum.size(); ++w) {
    const std::string_view name = w == 0 ? col : trimmed(m.q.sum[w - 1]);
    if (w == 0 && col.empty()) continue;
    if (seen[w] == 0) err_ = "column " + std::string(name) + T::kNotInHeader + quote;
    if (seen[w] > 1) err_ = "column " + std::string(name) + T::kTwice + quote;
    if (!err_.empty()) return false;
  }
  m.keyIdx = found[0];
  m.sums = m.q.sum.size();
  for (size_t s = 0; s < m.sums; ++s) m.sumIdx[s] = found[s + 1];
  // The header leads every page.
  const std::string lead = "L1 ";
  std::memcpy(m.body, lead.data(), lead.size());
  std::memcpy(m.body + lead.size(), h.data(), h.size());
  m.bodyLen = lead.size() + h.size();
  m.body[m.bodyLen++] = '\n';
  return true;
}

bool CsvScanner::endRecord() {
  Impl& m = *impl_;
  m.inRecord = false;
  if (!m.tooLong && m.recLen && m.rec[m.recLen - 1] == '\r') --m.recLen;
  if (!m.headerDone) return header();
  if (!m.tooLong && m.recLen == 0) {
    ++res_.emptyLines;
    return true;
  }
  ++res_.records;
  if (m.tooLong) {
    listAdd(res_.tooLong, m.recLine);
    return true;
  }
  // The wanted fields: the key at slot 0, the sums after it.
  std::string_view want[kWanted];
  const size_t n = eachField(m.rec, m.recLen, m.cell, [&](size_t i, std::string_view v) {
    if (int(i) == m.keyIdx) want[0] = v;
    for (size_t s = 0; s < m.sums; ++s)
      if (int(i) == m.sumIdx[s]) want[s + 1] = v;
  });
  if (n != res_.headerFields) {
    listAdd(res_.fieldCount, m.recLine);
    return true;
  }
  if (m.keyIdx >= 0) {
    const int r = inRange(want[0]);
    if (r < 0) {
      listAdd(res_.notCompared, m.recLine);
      return true;
    }
    if (r == 0) {
      ++res_.outOfRange;
      return true;
    }
  }
  ++res_.matched;
  show();
  if (!remember()) return false;
  return addSums(want + 1);
}

int CsvScanner::inRange(std::string_view key) const {
  const Impl& m = *impl_;
  if (key.empty()) return -1;
  Dec3 k;
  if (m.numericRange && parseDec3(key, k) <= NumParse::Rounded)
    return (m.fromNum.milli <= k.milli && k.milli <= m.toNum.milli) ? 1 : 0;
  const std::string_view from = m.q.from;
  if (!comparable(key, from) || !comparable(key, m.to)) return -1;
  return (key.substr(0, from.size()) >= from && key.substr(0, m.to.size()) <= m.to) ? 1 : 0;
}

void CsvScanner::show() {
  Impl& m = *impl_;
  if (res_.matched <= m.q.skipRows || m.showFull) return;
  char lead[16];
  const int ln = std::snprintf(lead, sizeof lead, "L%u ", unsigned(m.recLine));
  const size_t need = size_t(ln) + m.recLen + 1;
  if (res_.shownFirst && m.bodyLen + need > m.q.limit) {
    m.showFull = true;
    return;
  }
  std::memcpy(m.body + m.bodyLen, lead, size_t(ln));
  std::memcpy(m.body + m.bodyLen + size_t(ln), m.rec, m.recLen);
  m.bodyLen += need;
  m.body[m.bodyLen - 1] = '\n';
  if (!res_.shownFirst) res_.shownFirst = res_.matched;
  res_.shownLast = res_.matched;
}

bool CsvScanner::remember() {
  Impl& m = *impl_;
  if (m.dupCount >= kCsvIdenticalMax) return true;
  if (!m.buckets) {
    m.buckets = static_cast<uint16_t*>(workingAlloc(kDupBuckets * sizeof(uint16_t)));
    if (!m.buckets) {
      err_ = kAidNoMemory;
      return false;
    }
    for (size_t i = 0; i < kDupBuckets; ++i) m.buckets[i] = kNone;
  }
  const size_t at = m.dupCount;
  DupBlock*& blk = m.blocks[at / kDupBlock];
  if (!blk) {
    blk = static_cast<DupBlock*>(workingAlloc(sizeof(DupBlock)));
    if (!blk) {
      err_ = kAidNoMemory;
      return false;
    }
  }
  const uint64_t h = fnv64(kFnv64Basis, std::string_view(m.rec, m.recLen));
  uint16_t& head = m.buckets[h % kDupBuckets];
  uint16_t group = uint16_t(at);
  for (uint16_t e = head; e != kNone;) {
    const DupEntry& d = m.blocks[e / kDupBlock]->e[e % kDupBlock];
    if (d.hash == h) {
      group = d.group;
      break;
    }
    e = d.next;
  }
  DupEntry& me = blk->e[at % kDupBlock];
  me = {h, m.recLine, head, group};
  head = uint16_t(at);
  blk->n[at % kDupBlock] = 1;
  if (group != at) ++m.blocks[group / kDupBlock]->n[group % kDupBlock];
  ++m.dupCount;
  res_.identicalChecked = m.dupCount;
  return true;
}

bool CsvScanner::addSums(const std::string_view* cells) {
  Impl& m = *impl_;
  for (size_t s = 0; s < m.sums; ++s) {
    CsvColumnSum& col = res_.sums[s];
    Dec3 v;
    const NumParse p = parseDec3(cells[s], v);
    if (p == NumParse::Blank) {
      ++col.blank;
      listAdd(res_.blankCells, m.recLine, int(s));
      continue;
    }
    if (p == NumParse::NotNumber) {
      ++col.notNumber;
      listAdd(res_.notNumberCells, m.recLine, int(s));
      continue;
    }
    if (p == NumParse::Rounded) {
      ++col.rounded;
      listAdd(res_.roundedCells, m.recLine, int(s));
    }
    ++col.numbers;
    if (__builtin_add_overflow(col.total.milli, v.milli, &col.total.milli) || col.total.milli == INT64_MIN) {
      err_ = "the total of " + col.name + T::kPastRange;
      return false;
    }
  }
  return true;
}

void CsvScanner::collectGroups() {
  Impl& m = *impl_;
  auto entry = [&](size_t i) -> DupEntry& { return m.blocks[i / kDupBlock]->e[i % kDupBlock]; };
  auto size = [&](size_t i) { return m.blocks[i / kDupBlock]->n[i % kDupBlock]; };
  for (size_t i = 0; i < m.dupCount; ++i) {
    if (entry(i).group != i || size(i) < 2) continue;
    ++res_.identicalGroups;
    if (res_.identical.size() >= kAidListMax) continue;
    LineGroup g;
    g.count = size(i);
    for (size_t j = i; j < m.dupCount && g.lines.size() < kAidGroupLinesMax; ++j)
      if (entry(j).group == i) g.lines.push_back(entry(j).line);
    res_.identical.push_back(std::move(g));
  }
}

// ---- the driver ---------------------------------------------------------------------
CsvScanOutcome scanCsv(AidSource& src, uint64_t size, const std::string& path, const CsvQuery& q) {
  (void)size;
  CsvScanOutcome o;
  CsvScanner s(q, path);
  char* chunk = s.error().empty() ? static_cast<char*>(workingAlloc(kAidChunk)) : nullptr;
  if (!s.error().empty() || !chunk) {
    o.error = s.error().empty() ? kAidNoMemory : s.error();
    return o;
  }
  bool ok = true;
  for (uint64_t off = 0;;) {
    size_t got = 0;
    if (!src.readAt(off, kAidChunk, chunk, got)) {
      o.error = T::kCardRead;
      ok = false;
      break;
    }
    if (got && !s.feed(chunk, got)) break;
    off += got;
    if (got < kAidChunk) break;
  }
  workingFree(chunk);
  if (ok && s.finish()) {
    o.ok = true;
    o.result = std::move(s.result());
  } else if (o.error.empty()) {
    o.error = s.error();
  }
  return o;
}

// ---- rendering ----------------------------------------------------------------------
namespace {

std::string cellList(const LineList& l, const CsvScanResult& r) {
  std::string out;
  for (size_t i = 0; i < l.lines.size(); ++i) {
    if (i) out += ", ";
    out += r.sums[l.columns[i]].name + " line " + num(l.lines[i]);
  }
  if (l.count > l.lines.size()) out += " +" + num(l.count - l.lines.size());
  return out;
}

std::string groupList(const CsvScanResult& r) {
  std::string out;
  for (size_t i = 0; i < r.identical.size(); ++i) {
    const LineGroup& g = r.identical[i];
    if (i) out += ", ";
    for (size_t k = 0; k < g.lines.size(); ++k) out += (k ? "=" : "") + num(g.lines[k]);
    if (g.count > g.lines.size()) out += "+" + num(g.count - g.lines.size());
  }
  if (r.identicalGroups > r.identical.size()) out += " +" + num(r.identicalGroups - r.identical.size());
  return out;
}

}  // namespace

std::string renderLines(const LineList& l) {
  if (!l.count) return "";
  std::string out = l.count == 1 ? "line " : "lines ";
  for (size_t i = 0; i < l.lines.size(); ++i) out += (i ? ", " : "") + num(l.lines[i]);
  if (l.count > l.lines.size()) out += " +" + num(l.count - l.lines.size());
  return out;
}

std::string renderCsvTail(const CsvQuery& q, const CsvScanResult& r) {
  std::string out = "rows " + num(r.matched) + " of " + num(r.records);
  const std::string& to = q.to.empty() ? q.from : q.to;
  if (!q.column.empty()) out += " where " + q.column + " " + q.from + ".." + to;
  if (!r.shownFirst && r.matched) out += "; shown none";
  else if (r.shownFirst > 1 || r.shownLast < r.matched) out += "; shown " + num(r.shownFirst) + "-" + num(r.shownLast);
  if (!r.sums.empty()) {
    out += "; sum";
    for (const auto& s : r.sums) out += " " + s.name + "=" + renderDec3(s.total);
  }
  if (r.blankCells.count) out += "; blank " + num(r.blankCells.count) + " (" + cellList(r.blankCells, r) + ")";
  if (r.notNumberCells.count)
    out += "; not a number " + num(r.notNumberCells.count) + " (" + cellList(r.notNumberCells, r) + ")";
  if (r.roundedCells.count)
    out += "; rounded to 3 decimals " + num(r.roundedCells.count) + " (" + cellList(r.roundedCells, r) + ")";
  if (r.notCompared.count)
    out += "; not compared " + num(r.notCompared.count) + " (" + renderLines(r.notCompared) + ": " + q.column +
           " blank or not like " + q.from + ")";
  if (r.fieldCount.count)
    out += "; skipped " + num(r.fieldCount.count) + " (" + renderLines(r.fieldCount) +
           ": field count is not the header's " + num(r.headerFields) + ")";
  if (r.tooLong.count)
    out += "; too long " + num(r.tooLong.count) + " (" + renderLines(r.tooLong) + ": over " + num(kCsvRecordMax) + " B)";
  if (r.emptyLines) out += "; empty lines " + num(r.emptyLines);
  if (r.identicalGroups)
    out += "; identical lines " + num(r.identicalGroups) + (r.identicalGroups == 1 ? " group (" : " groups (") +
           groupList(r) + ")";
  if (r.identicalChecked < r.matched)
    out += "; identical lines checked in the first " + num(r.identicalChecked) + " matching rows only";
  if (r.tornLine) out += "; last line " + num(r.tornLine) + " has no newline (a write may have been cut short)";
  return out;
}

std::string renderCsvRead(const CsvQuery& q, const CsvScanResult& r) {
  std::string out = "\n" + r.body + renderCsvTail(q, r);
  if (r.shownFirst && r.shownLast < r.matched) out += "\nnext_offset=" + num(r.shownLast);
  return out;
}

}  // namespace nimbus::orch::aids
