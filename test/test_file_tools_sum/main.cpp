// read_file column/from/to/sum (file_aids.h): the CSV scan and its totals.
//
// Class rules proven here, each over generated classes rather than one instance:
//   - the scan agrees with an independent whole-file reference (its own RFC 4180 parser,
//     its own range rule, __int128 totals) on generated files of three equal domains (a
//     food log, an expense log, a training log) mixed with hostile rows: quoted commas and
//     line ends, blank and missing fields, a mismatched field count, mixed date forms,
//     repeated identical lines, a torn last line, over-long lines, NUL-free junk;
//   - every data record is accounted for exactly once (matched, out of range, not
//     compared, skipped, too long): nothing is dropped silently;
//   - the result never depends on how the file is cut into chunks (1 B to 4 KB);
//   - totals are exact decimals (int64 thousandths, half away from zero past 3 places),
//     never float, checked against the __int128 reference; past int64 is an error;
//   - paging the shown rows (offset, limit) never changes the sums, and following
//     next_offset shows every matching row exactly once;
//   - a several-megabyte file is read in 4 KB reads with bounded working memory;
//   - a refused allocation at any point is the no-memory error, never a partial sum.
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_aid_card.h"
#include "nimbus/orch/file_aids.h"

using namespace nimbus::orch::aids;
using aidfake::Card;
using aidfake::Rng;

void setUp() {}
void tearDown() { aidfake::disarm(); }

namespace {

// ---- the reference --------------------------------------------------------------
using i128 = __int128;

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

// A number per the documented grammar, as exact thousandths with half-away rounding;
// false when it is not one. rounded: a fourth decimal or more.
bool refSplit(const std::string& s, bool& neg, std::string& ip, std::string& fp) {
  size_t i = 0;
  neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
  while (i < s.size() && isdigit((unsigned char)s[i])) ip += s[i++];
  if (i < s.size() && s[i] == '.')
    for (++i; i < s.size() && isdigit((unsigned char)s[i]);) fp += s[i++];
  return i == s.size() && !(ip.empty() && fp.empty());
}
bool refNumber(const std::string& raw, i128& milli, bool& rounded) {
  bool neg;
  std::string ip, fp;
  if (!refSplit(trim(raw), neg, ip, fp)) return false;
  i128 v = 0;
  for (char c : ip) v = std::min<i128>(v * 10 + (c - '0'), (i128)INT64_MAX + 1);
  for (size_t k = 0; k < 3; ++k) v = v * 10 + (k < fp.size() ? fp[k] - '0' : 0);
  rounded = fp.size() > 3;
  v += rounded && fp[3] >= '5';
  if (v > (i128)INT64_MAX) return false;
  milli = neg ? -v : v;
  return true;
}

std::string refRender(i128 v) {
  const bool neg = v < 0;
  if (neg) v = -v;
  std::string ip = std::to_string((long long)(v / 1000));
  int f = int(v % 1000);
  std::string out = (neg && v != 0) ? "-" : "";
  out += ip;
  if (f) {
    char b[8];
    std::snprintf(b, sizeof b, "%03d", f);
    std::string fs = b;
    while (fs.back() == '0') fs.pop_back();
    out += "." + fs;
  }
  return out;
}

struct RefRecord {
  uint32_t line = 0, endLine = 0;
  bool openQuote = false;
  std::string raw;   // record bytes (line end and a trailing CR left out)
  bool tooLong = false;
};

// Records by the documented rule: a quote opens a quoted field only at a field's start;
// inside, "" is a quote and " closes; a line end outside quotes ends the record; a record
// longer than kCsvRecordMax ends at the next line end, quotes or not.
struct RefSplitter {
  const std::string& f;
  size_t i = 0;
  uint32_t line = 1;
  bool inQ = false, fieldStart = true, pendingQ = false;

  // Quote state after a kept byte c.
  void keep(char c) {
    if (inQ && !pendingQ) {
      pendingQ = c == '"';
      line += c == '\n';
      return;
    }
    if (inQ) {
      pendingQ = false;
      if (c == '"') return;
      inQ = false;
    }
    if (c == ',') { fieldStart = true; return; }
    if (c == '"' && fieldStart) inQ = true;
    fieldStart = false;
  }
  // The next record; i moves past its line end.
  RefRecord next() {
    RefRecord r;
    r.line = line;
    inQ = pendingQ = false;
    fieldStart = true;
    const size_t start = i;
    size_t end = f.size();
    r.endLine = 0;
    for (; i < f.size(); ++i) {
      const char c = f[i];
      if (c == '\n' && (!inQ || pendingQ)) { end = i++; r.endLine = line++; break; }
      if (i - start >= kCsvRecordMax) r.tooLong = true;   // the quotes still decide the end
      keep(c);
    }
    if (!r.endLine) {
      r.endLine = line - (f.back() == '\n' ? 1 : 0);   // the line of the file's last byte
      r.openQuote = inQ && !pendingQ;
    }
    r.raw = f.substr(start, end - start);
    if (!r.raw.empty() && r.raw.back() == '\r') r.raw.pop_back();
    return r;
  }
};

std::vector<RefRecord> refRecords(const std::string& f, uint64_t& physLines, size_t& emptyLines) {
  std::vector<RefRecord> out;
  physLines = std::count(f.begin(), f.end(), '\n') + ((!f.empty() && f.back() != '\n') ? 1 : 0);
  emptyLines = 0;
  RefSplitter sp{f};
  while (sp.i < f.size()) {
    RefRecord r = sp.next();
    const bool empty = r.raw.empty() && !r.tooLong;
    if (empty && !out.empty()) ++emptyLines;
    else out.push_back(r);   // an empty line 1 is the header slot
  }
  return out;
}

std::vector<std::string> refFields(const std::string& raw) {
  std::vector<std::string> out(1);
  bool inQ = false, fieldStart = true;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (inQ) {
      if (c == '"') {
        if (i + 1 < raw.size() && raw[i + 1] == '"') { out.back() += '"'; ++i; continue; }
        inQ = false;
        continue;
      }
      out.back() += c;
      continue;
    }
    if (c == ',') { out.emplace_back(); fieldStart = true; continue; }
    if (c == '"' && fieldStart) { inQ = true; fieldStart = false; continue; }
    fieldStart = false;
    out.back() += c;
  }
  for (auto& s : out) s = trim(s);
  return out;
}

int charClass(char c) {
  if (c >= '0' && c <= '9') return 256;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 257;
  return (unsigned char)c;
}
bool refComparable(const std::string& k, const std::string& b) {
  if (k.size() < b.size()) return false;
  for (size_t i = 0; i < b.size(); ++i)
    if (charClass(k[i]) != charClass(b[i])) return false;
  if (!b.empty() && isdigit((unsigned char)b.back()) && k.size() > b.size() && isdigit((unsigned char)k[b.size()]))
    return false;
  return true;
}
// 1 match, 0 out of range, -1 not compared.
int refRange(const std::string& key, const std::string& from, const std::string& to) {
  if (key.empty()) return -1;
  i128 kf, kk, kt;
  bool rf = false, rt = false, rk = false;
  if (refNumber(from, kf, rf) && refNumber(to, kt, rt) && !rf && !rt && refNumber(key, kk, rk))
    return rk ? -1 : (kf <= kk && kk <= kt) ? 1 : 0;
  if (!refComparable(key, from) || !refComparable(key, to)) return -1;
  return (key.compare(0, from.size(), from) >= 0 && key.compare(0, to.size(), to) <= 0) ? 1 : 0;
}

struct RefSum {
  i128 total = 0;
  size_t numbers = 0, rounded = 0, blank = 0, notNumber = 0;
};
struct Ref {
  bool ok = true;
  std::string errorStart;
  uint64_t lines = 0;
  size_t records = 0, matched = 0, outOfRange = 0, notCompared = 0, fieldCount = 0, tooLong = 0, empty = 0;
  std::vector<RefSum> sums;
  std::vector<uint32_t> matchedLines;
  std::vector<std::string> matchedRaw;
  std::map<std::string, std::vector<uint32_t>> groups;
  uint32_t torn = 0, openQuote = 0;
  std::vector<std::pair<uint32_t, uint32_t>> notComparedL, fieldCountL, tooLongL;
};

// The header's index of name: -1 missing, -2 twice.
int refIndex(const std::vector<std::string>& names, const std::string& n) {
  int at = -1;
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] != n) continue;
    if (at >= 0) return -2;
    at = int(i);
  }
  return at;
}

// Resolve the key and sum columns; false with ref's error set.
bool refColumns(Ref& ref, const std::vector<std::string>& names, const CsvQuery& q, int& key, std::vector<int>& cols) {
  key = q.column.empty() ? -1 : refIndex(names, q.column);
  if (!q.column.empty() && key < 0) { ref.errorStart = "column " + q.column + " is"; return ref.ok = false; }
  for (const auto& s : q.sum) {
    cols.push_back(refIndex(names, s));
    if (cols.back() < 0) { ref.errorStart = "column " + s + " is"; return ref.ok = false; }
  }
  return true;
}

void refCell(RefSum& s, const std::string& cell) {
  i128 v;
  bool rounded = false;
  if (cell.empty()) ++s.blank;
  else if (!refNumber(cell, v, rounded)) ++s.notNumber;
  else { s.total += v; ++s.numbers; s.rounded += rounded; }
}

// One data record into its bucket.
void refRecord(Ref& ref, const RefRecord& r, size_t nfields, int key, const std::vector<int>& cols, const CsvQuery& q) {
  ++ref.records;
  const auto span = std::make_pair(r.line, r.endLine);
  if (r.openQuote) ref.openQuote = r.line;
  if (r.tooLong) { ++ref.tooLong; ref.tooLongL.push_back(span); return; }
  const auto fs = refFields(r.raw);
  if (fs.size() != nfields) { ++ref.fieldCount; ref.fieldCountL.push_back(span); return; }
  const int m = key >= 0 ? refRange(fs[size_t(key)], q.from, q.to.empty() ? q.from : q.to) : 1;
  if (m < 0) { ++ref.notCompared; ref.notComparedL.push_back(span); }
  if (m == 0) ++ref.outOfRange;
  if (m <= 0) return;
  ++ref.matched;
  ref.matchedLines.push_back(r.line);
  ref.matchedRaw.push_back(r.raw);
  ref.groups[r.raw].push_back(r.line);
  for (size_t c = 0; c < cols.size(); ++c) refCell(ref.sums[c], fs[size_t(cols[c])]);
}

Ref reference(const std::string& f, const CsvQuery& q) {
  Ref ref;
  auto recs = refRecords(f, ref.lines, ref.empty);
  if (!f.empty() && f.back() != '\n') ref.torn = uint32_t(ref.lines);
  if (recs.empty() || recs[0].raw.empty() || recs[0].tooLong) {
    ref.ok = false;
    ref.errorStart = "sum and column need a CSV header line";
    return ref;
  }
  std::string hdr = recs[0].raw;
  if (hdr.compare(0, 3, "\xEF\xBB\xBF") == 0) hdr = hdr.substr(3);
  const auto names = refFields(hdr);
  int key = -1;
  std::vector<int> cols;
  if (!refColumns(ref, names, q, key, cols)) return ref;
  ref.sums.resize(cols.size());
  for (size_t i = 1; i < recs.size(); ++i) refRecord(ref, recs[i], names.size(), key, cols, q);
  for (const auto& s : ref.sums)
    if (s.total > (i128)INT64_MAX || s.total < -(i128)INT64_MAX) { ref.ok = false; ref.errorStart = "the total of"; }
  return ref;
}

// ---- fixtures: three equal domains and hostile rows -------------------------------
struct Domain {
  const char* name;
  const char* header;
  std::vector<std::string> sumCols;
  std::function<std::string(Rng&, const std::string& date)> row;
};

std::string dec(Rng& g, int maxInt, int dp) {
  std::string s = std::to_string(g.below(size_t(maxInt) + 1));
  if (dp > 0) {
    s += '.';
    for (int i = 0; i < dp; ++i) s += char('0' + g.below(10));
  }
  return s;
}

const std::vector<Domain>& domains() {
  static const std::vector<Domain> d = {
      {"food log", "date,time,item,grams,kcal,protein",
       {"grams", "kcal", "protein"},
       [](Rng& g, const std::string& date) {
         static const char* items[] = {"oats", "\"yogurt, plain\"", "apple", "\"rice\nbowl\"", "eggs"};
         return date + ",0" + std::to_string(g.below(10)) + ":1" + std::to_string(g.below(6)) + "," +
                items[g.below(5)] + "," + dec(g, 400, int(g.below(2))) + "," + dec(g, 900, 0) + "," +
                dec(g, 60, int(g.below(3)));
       }},
      {"expense log", "date,payee,category,amount",
       {"amount"},
       [](Rng& g, const std::string& date) {
         static const char* payees[] = {"grocer", "\"Cafe \"\"Blue\"\"\"", "rail", "\"Books, Ltd\""};
         std::string amt = dec(g, 250, 2);
         if (g.chance(10)) amt = "-" + amt;   // a refund
         return date + "," + payees[g.below(4)] + ",misc," + amt;
       }},
      {"training log", "date,exercise,sets,reps,kg",
       {"sets", "reps", "kg"},
       [](Rng& g, const std::string& date) {
         static const char* ex[] = {"squat", "bench", "\"row, cable\"", "deadlift"};
         return date + "," + ex[g.below(4)] + "," + dec(g, 5, 0) + "," + dec(g, 12, 0) + "," + dec(g, 180, int(g.below(5)));
       }},
  };
  return d;
}

std::string dateOf(int dayOfYear) {
  static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int m = 0;
  while (dayOfYear >= mdays[m]) dayOfYear -= mdays[m++];
  char b[16];
  std::snprintf(b, sizeof b, "2026-%02d-%02d", m + 1, dayOfYear + 1);
  return b;
}

std::string hostileRow(Rng& g, const Domain& d, const std::string& date) {
  switch (g.below(14)) {
    case 11: return date + ",\"" + std::string(kCsvRecordMax + 9, 'q') + "\n\",1";   // over-long, quoted line end
    case 12: return date + ",\"stray quote,1";                                        // a quote that never closes
    case 13: { std::string r = d.row(g, date); return r + "4"; }                        // a fourth decimal or more
    case 0: return "";                                         // an empty line
    case 1: return d.row(g, date) + ",extra";                  // too many fields
    case 2: return date + ",only";                             // too few
    case 3: { std::string r = d.row(g, date); return "19/11/2026" + r.substr(10); }   // another date form
    case 4: { std::string r = d.row(g, date); return r.substr(10); }                  // a blank date
    case 5: { std::string r = d.row(g, date); return "2026-1-5" + r.substr(10); }     // not zero padded
    case 6: { std::string r = d.row(g, date); return r + "\r"; }                      // a CRLF row
    case 7: { std::string r = d.row(g, date); size_t c = r.rfind(','); return r.substr(0, c + 1); }   // blank last cell
    case 8: { std::string r = d.row(g, date); size_t c = r.rfind(','); return r.substr(0, c + 1) + "n/a"; }
    case 9: return date + ",\"" + std::string(kCsvRecordMax + 50, 'x') + "\"";   // over-long
    default: { std::string r = d.row(g, date); return date + "T08:10" + r.substr(10); }   // a date-time
  }
}

std::string makeFile(Rng& g, const Domain& d, int days, int rowsPerDay, unsigned hostilePct, bool torn) {
  std::string f = std::string(d.header) + "\n";
  std::vector<std::string> recent;
  for (int day = 0; day < days; ++day) {
    const std::string date = dateOf(300 + day);
    for (int r = 0; r < rowsPerDay; ++r) {
      std::string row;
      if (!recent.empty() && g.chance(8)) row = recent[g.below(recent.size())];   // an identical re-log
      else if (g.chance(hostilePct)) row = hostileRow(g, d, date);
      else row = d.row(g, date);
      recent.push_back(row);
      if (recent.size() > 20) recent.erase(recent.begin());
      f += row + "\n";
    }
  }
  if (torn && f.size() > 10) f.resize(f.size() - 1 - g.below(5));
  return f;
}

CsvScanOutcome scanWith(const std::string& file, const CsvQuery& q, size_t chunk) {
  CsvScanOutcome o;
  CsvScanner s(q, "p/t.csv");
  bool ok = true;
  for (size_t i = 0; ok && i < file.size(); i += chunk) ok = s.feed(file.data() + i, std::min(chunk, file.size() - i));
  if (ok) ok = s.finish();
  o.ok = ok;
  if (!ok) o.error = s.error();
  else o.result = s.result();
  return o;
}

// The listed records are the first ones of the bucket, each with its line span.
void checkSpans(const std::vector<std::pair<uint32_t, uint32_t>>& want, const LineList& got, const char* what) {
  TEST_ASSERT_EQUAL_MESSAGE(want.size(), got.count, what);
  TEST_ASSERT_EQUAL_MESSAGE(std::min(want.size(), kAidListMax), got.lines.size(), what);
  TEST_ASSERT_EQUAL_MESSAGE(got.lines.size(), got.ends.size(), what);
  for (size_t i = 0; i < got.lines.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i].first, got.lines[i], what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i].second, got.ends[i], what);
  }
}

void checkAgainstReference(const std::string& file, const CsvQuery& q, const CsvScanOutcome& o, const char* what) {
  const Ref ref = reference(file, q);
  if (!ref.ok) {
    TEST_ASSERT_FALSE_MESSAGE(o.ok, what);
    TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE(ref.errorStart.c_str(), o.error.c_str(), ref.errorStart.size(), what);
    return;
  }
  TEST_ASSERT_TRUE_MESSAGE(o.ok, (std::string(what) + ": " + o.error).c_str());
  const CsvScanResult& r = o.result;
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(ref.lines, r.lines, what);
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(file.size(), r.bytes, what);
  TEST_ASSERT_EQUAL_MESSAGE(ref.records, r.records, what);
  TEST_ASSERT_EQUAL_MESSAGE(ref.matched, r.matched, what);
  TEST_ASSERT_EQUAL_MESSAGE(ref.outOfRange, r.outOfRange, what);
  TEST_ASSERT_EQUAL_MESSAGE(ref.notCompared, r.notCompared.count, what);
  TEST_ASSERT_EQUAL_MESSAGE(ref.fieldCount, r.fieldCount.count, what);
  TEST_ASSERT_EQUAL_MESSAGE(ref.tooLong, r.tooLong.count, what);
  TEST_ASSERT_EQUAL_MESSAGE(ref.empty, r.emptyLines, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.torn, r.tornLine, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.openQuote, r.openQuoteLine, what);
  checkSpans(ref.notComparedL, r.notCompared, what);
  checkSpans(ref.fieldCountL, r.fieldCount, what);
  checkSpans(ref.tooLongL, r.tooLong, what);
  // Nothing is dropped silently: every record is in exactly one bucket.
  TEST_ASSERT_EQUAL_MESSAGE(r.records, r.matched + r.outOfRange + r.notCompared.count + r.fieldCount.count + r.tooLong.count,
                            what);
  TEST_ASSERT_EQUAL_MESSAGE(ref.sums.size(), r.sums.size(), what);
  for (size_t c = 0; c < ref.sums.size(); ++c) {
    TEST_ASSERT_TRUE_MESSAGE(ref.sums[c].total == (i128)r.sums[c].total.milli, what);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(refRender(ref.sums[c].total).c_str(), renderDec3(r.sums[c].total).c_str(), what);
    TEST_ASSERT_EQUAL_MESSAGE(ref.sums[c].numbers, r.sums[c].numbers, what);
    TEST_ASSERT_EQUAL_MESSAGE(ref.sums[c].blank, r.sums[c].blank, what);
    TEST_ASSERT_EQUAL_MESSAGE(ref.sums[c].notNumber, r.sums[c].notNumber, what);
    TEST_ASSERT_EQUAL_MESSAGE(ref.sums[c].rounded, r.sums[c].rounded, what);
    TEST_ASSERT_EQUAL_MESSAGE(ref.sums[c].numbers + ref.sums[c].blank + ref.sums[c].notNumber, r.matched, what);
  }
  size_t groups = 0;
  for (const auto& kv : ref.groups) groups += kv.second.size() > 1;
  if (r.identicalChecked == r.matched) TEST_ASSERT_EQUAL_MESSAGE(groups, r.identicalGroups, what);
  // The groups named are real: their lines hold the same text.
  for (const auto& gr : r.identical) {
    TEST_ASSERT_TRUE_MESSAGE(gr.count >= 2, what);
    const auto it = std::find(ref.matchedLines.begin(), ref.matchedLines.end(), gr.lines[0]);
    TEST_ASSERT_TRUE_MESSAGE(it != ref.matchedLines.end(), what);
    const std::string& text = ref.matchedRaw[size_t(it - ref.matchedLines.begin())];
    TEST_ASSERT_EQUAL_MESSAGE(ref.groups.at(text).size(), gr.count, what);
    for (size_t k = 0; k < gr.lines.size(); ++k) TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.groups.at(text)[k], gr.lines[k], what);
  }
}

CsvQuery dayQuery(const Domain& d, const std::string& from, const std::string& to) {
  CsvQuery q;
  q.column = "date";
  q.from = from;
  q.to = to;
  q.sum = d.sumCols;
  return q;
}

}  // namespace

// ---- the property of record --------------------------------------------------------
static void test_scan_agrees_with_the_reference_over_three_domains_and_hostile_rows() {
  size_t runs = 0;
  for (uint64_t seed = 1; seed <= 60; ++seed) {
    Rng g(seed * 7919);
    for (const Domain& d : domains()) {
      const std::string file = makeFile(g, d, 1 + int(g.below(20)), 15, unsigned(g.below(30)), g.chance(30));
      const int day = int(g.below(20));
      std::vector<CsvQuery> qs = {
          dayQuery(d, dateOf(300 + day), ""),                          // one day
          dayQuery(d, dateOf(300 + day), dateOf(300 + day + 6)),       // a week
          dayQuery(d, dateOf(300 + day).substr(0, 7), ""),             // a month
          dayQuery(d, "2026", ""),                                     // a year
      };
      CsvQuery all;
      all.sum = d.sumCols;   // every data row
      qs.push_back(all);
      for (const auto& q : qs) {
        const size_t chunks[] = {1, 3, 64, 4096, 1 + g.below(5000)};
        for (size_t c : chunks) {
          const auto o = scanWith(file, q, c);
          char what[160];
          std::snprintf(what, sizeof what, "seed %llu %s chunk %zu from %s", (unsigned long long)seed, d.name, c,
                        q.from.c_str());
          checkAgainstReference(file, q, o, what);
          ++runs;
        }
      }
    }
  }
  TEST_ASSERT_TRUE(runs >= 4000);
}

static void test_the_result_never_depends_on_the_chunking() {
  Rng g(4242);
  for (const Domain& d : domains()) {
    const std::string file = makeFile(g, d, 10, 15, 25, true);
    const CsvQuery q = dayQuery(d, dateOf(303), dateOf(309));
    const auto base = scanWith(file, q, file.size() + 1);
    TEST_ASSERT_TRUE(base.ok);
    const std::string want = renderCsvRead(q, base.result);
    for (size_t c = 1; c < 600; c += 1 + c / 8) {
      const auto o = scanWith(file, q, c);
      TEST_ASSERT_TRUE(o.ok);
      TEST_ASSERT_EQUAL_STRING(want.c_str(), renderCsvRead(q, o.result).c_str());
    }
  }
}

// ---- exact formats -------------------------------------------------------------------
static void test_the_filtered_read_has_its_exact_format() {
  const std::string file =
      "date,time,item,qty,amount\n"
      "2026-11-18,20:00,a,1,5\n"
      "2026-11-19,08:10,\"b, c\",2,10.5\n"
      "2026-11-19,12:00,d,,3.25\n"
      "2026-11-19,12:00,d,,3.25\n"
      "19/11/2026,13:00,e,1,4\n"
      "2026-11-19,14:00,f,x,1\n"
      "2026-11-19,15:00,g\n"
      "\n"
      "2026-11-20,09:00,h,1,7\n";
  CsvQuery q;
  q.column = "date";
  q.from = "2026-11-19";
  q.sum = {"amount", "qty"};
  const auto o = scanWith(file, q, 4096);
  TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
  TEST_ASSERT_EQUAL_STRING(
      "\nL1 date,time,item,qty,amount\n"
      "L3 2026-11-19,08:10,\"b, c\",2,10.5\n"
      "L4 2026-11-19,12:00,d,,3.25\n"
      "L5 2026-11-19,12:00,d,,3.25\n"
      "L7 2026-11-19,14:00,f,x,1\n"
      "rows 4 of 8 where date 2026-11-19..2026-11-19; sum amount=18 qty=2; blank 2 (qty line 4, qty line 5); "
      "not a number 1 (qty line 7); not compared 1 (line 6: date blank or not like 2026-11-19); "
      "skipped 1 (line 8: field count is not the header's 5); empty lines 1; identical lines 1 group (4=5)",
      renderCsvRead(q, o.result).c_str());
}

static void test_paging_never_changes_the_sums_and_next_offset_shows_every_row_once() {
  Rng g(77);
  for (const Domain& d : domains()) {
    const std::string file = makeFile(g, d, 30, 15, 10, false);
    CsvQuery q = dayQuery(d, dateOf(305).substr(0, 7), "");
    const auto whole = scanWith(file, q, 4096);
    TEST_ASSERT_TRUE(whole.ok);
    const std::string tailWant = renderCsvTail(q, whole.result);
    const std::string sumsWant = tailWant.substr(tailWant.find("; sum "));
    for (size_t limit : {16u, 100u, 700u, 4096u}) {
      std::set<std::string> seen;
      size_t shown = 0;
      q.skipRows = 0;
      q.limit = limit;
      for (int page = 0; page < 100000; ++page) {
        const auto o = scanWith(file, q, 4096);
        TEST_ASSERT_TRUE(o.ok);
        const std::string text = renderCsvRead(q, o.result);
        const std::string tail = renderCsvTail(q, o.result);
        // The sums are the same on every page.
        TEST_ASSERT_TRUE(tail.find(sumsWant.substr(0, sumsWant.find(';', 2))) != std::string::npos);
        TEST_ASSERT_TRUE(o.result.shownLast >= o.result.shownFirst);
        if (o.result.shownFirst) shown += o.result.shownLast - o.result.shownFirst + 1;
        const size_t at = text.find("\nnext_offset=");
        if (at == std::string::npos) break;
        const size_t next = std::stoul(text.substr(at + 13));
        TEST_ASSERT_TRUE(next > q.skipRows);   // progress on every page
        q.skipRows = next;
      }
      TEST_ASSERT_EQUAL(whole.result.matched, shown);
    }
  }
}

static void test_decimals_are_exact_and_round_half_away_from_zero() {
  struct Case {
    const char* in;
    NumParse want;
    int64_t milli;
  } cases[] = {
      {"0", NumParse::Ok, 0},           {"12", NumParse::Ok, 12000},      {"-0.5", NumParse::Ok, -500},
      {"+3.25", NumParse::Ok, 3250},    {".5", NumParse::Ok, 500},        {"5.", NumParse::Ok, 5000},
      {" 7 ", NumParse::Ok, 7000},      {"1.2345", NumParse::Rounded, 1235}, {"-1.2345", NumParse::Rounded, -1235},
      {"1.23449", NumParse::Rounded, 1234}, {"0.0005", NumParse::Rounded, 1}, {"-0.0004", NumParse::Rounded, 0},
      {"", NumParse::Blank, 0},         {"   ", NumParse::Blank, 0},     {"1e3", NumParse::NotNumber, 0},
      {"1,000", NumParse::NotNumber, 0}, {"12 kg", NumParse::NotNumber, 0}, {".", NumParse::NotNumber, 0},
      {"-", NumParse::NotNumber, 0},    {"--1", NumParse::NotNumber, 0}, {"0x10", NumParse::NotNumber, 0},
      {"9223372036854775", NumParse::Ok, 9223372036854775000LL},
      {"9223372036854775.807", NumParse::Ok, INT64_MAX},
      {"9223372036854775.808", NumParse::NotNumber, 0},
      {"99999999999999999999", NumParse::NotNumber, 0},
  };
  for (const auto& c : cases) {
    Dec3 v;
    const NumParse p = parseDec3(c.in, v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(int(c.want), int(p), c.in);
    if (p == NumParse::Ok || p == NumParse::Rounded) TEST_ASSERT_EQUAL_INT64_MESSAGE(c.milli, v.milli, c.in);
  }
  TEST_ASSERT_EQUAL_STRING("24.3", renderDec3({24300}).c_str());
  TEST_ASSERT_EQUAL_STRING("310", renderDec3({310000}).c_str());
  TEST_ASSERT_EQUAL_STRING("-0.5", renderDec3({-500}).c_str());
  TEST_ASSERT_EQUAL_STRING("0.125", renderDec3({125}).c_str());
  TEST_ASSERT_EQUAL_STRING("0", renderDec3({0}).c_str());
  TEST_ASSERT_EQUAL_STRING("-0.001", renderDec3({-1}).c_str());
  TEST_ASSERT_EQUAL_STRING("-9223372036854775.808", renderDec3({INT64_MIN}).c_str());
  // A float sum of these drifts; the exact one does not.
  std::string f = "date,amount\n";
  for (int i = 0; i < 1000; ++i) f += "2026-11-19,0.1\n";
  CsvQuery q;
  q.sum = {"amount"};
  const auto o = scanWith(f, q, 4096);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("100", renderDec3(o.result.sums[0].total).c_str());
}

static void test_a_total_past_int64_is_an_error_never_a_partial_sum() {
  std::string f = "date,amount\n2026-11-19,9223372036854775\n2026-11-19,9223372036854775\n";
  CsvQuery q;
  q.sum = {"amount"};
  const auto o = scanWith(f, q, 7);
  TEST_ASSERT_FALSE(o.ok);
  TEST_ASSERT_EQUAL_STRING("the total of amount is past the exact range; nothing summed", o.error.c_str());
}

// ---- errors ----------------------------------------------------------------------
static void test_query_and_file_errors_state_the_fact() {
  CsvQuery q;
  q.column = "day";
  q.from = "2026-11-19";
  q.sum = {"amount"};
  auto o = scanWith("date,amount\n2026-11-19,1\n", q, 4096);
  TEST_ASSERT_EQUAL_STRING("column day is not in the header: date,amount", o.error.c_str());
  q.column = "date";
  q.sum = {"kg"};
  o = scanWith("date,amount\n2026-11-19,1\n", q, 4096);
  TEST_ASSERT_EQUAL_STRING("column kg is not in the header: date,amount", o.error.c_str());
  q.sum = {"amount"};
  o = scanWith("date,amount,amount\n2026-11-19,1,2\n", q, 4096);
  TEST_ASSERT_EQUAL_STRING("column amount is in the header twice: date,amount,amount", o.error.c_str());
  o = scanWith("", q, 4096);
  TEST_ASSERT_EQUAL_STRING("sum and column need a CSV header line; line 1 is: \"\"", o.error.c_str());
  o = scanWith("\n2026-11-19,1\n", q, 4096);
  TEST_ASSERT_EQUAL_STRING("sum and column need a CSV header line; line 1 is: \"\"", o.error.c_str());
  // A missing header: line 1 is data, and the error shows it.
  o = scanWith("2026-11-19,lunch,5\n2026-11-19,tea,1\n", q, 4096);
  TEST_ASSERT_EQUAL_STRING("column date is not in the header: 2026-11-19,lunch,5", o.error.c_str());
  // Not CSV: prose, and bytes that are not text.
  o = scanWith("# Notes\nsome words, here\n", q, 4096);
  TEST_ASSERT_EQUAL_STRING("column date is not in the header: # Notes", o.error.c_str());
  std::string bin = "date,amount\n2026-11-19,1\n";
  bin += std::string("\x89PNG\0\0\x01", 7);
  o = scanWith(bin, q, 4096);
  TEST_ASSERT_EQUAL_STRING("p/t.csv is not text (a NUL byte on line 3)", o.error.c_str());
  // The header quote is cut at 200 B.
  std::string wide;
  for (int i = 0; i < 100; ++i) wide += "col" + std::to_string(i) + ",";
  wide += "date";
  q.sum = {"nope"};
  o = scanWith(wide + "\n1\n", q, 4096);
  TEST_ASSERT_EQUAL_STRING(("column nope is not in the header: " + wide.substr(0, kAidHeaderQuoteMax) + " ...").c_str(),
                           o.error.c_str());

  // The query itself.
  CsvQuery b;
  TEST_ASSERT_EQUAL_STRING("column or sum is needed", checkCsvQuery(b).c_str());
  b.column = "date";
  TEST_ASSERT_EQUAL_STRING("column needs from", checkCsvQuery(b).c_str());
  b = CsvQuery();
  b.from = "2026";
  b.sum = {"a"};
  TEST_ASSERT_EQUAL_STRING("from and to need column", checkCsvQuery(b).c_str());
  b = CsvQuery();
  b.sum = {"a", "a"};
  TEST_ASSERT_EQUAL_STRING("sum names a twice", checkCsvQuery(b).c_str());
  b.sum = {""};
  TEST_ASSERT_EQUAL_STRING("sum has an empty column name", checkCsvQuery(b).c_str());
  b.sum.clear();
  for (int i = 0; i < 17; ++i) b.sum.push_back("c" + std::to_string(i));
  TEST_ASSERT_EQUAL_STRING("sum takes at most 16 columns", checkCsvQuery(b).c_str());
  b.sum = {"a"};
  b.limit = 15;
  TEST_ASSERT_EQUAL_STRING("limit is at least 16", checkCsvQuery(b).c_str());
  b.limit = 16;
  TEST_ASSERT_EQUAL_STRING("", checkCsvQuery(b).c_str());
}

// ---- the range rule ----------------------------------------------------------------
static void test_day_week_month_ranges_and_incomparable_keys_are_counted() {
  const std::string file =
      "date,amount\n"
      "2026-10-31,1\n2026-11-01,2\n2026-11-13,4\n2026-11-19,8\n2026-11-19 23:59,16\n2026-11-20,32\n"
      "2026-12-01,64\n19/11/2026,128\n2026-11-1,256\n,512\nNov 19,1024\n2026-11-19T00:30,2048\n";
  CsvQuery q;
  q.column = "date";
  q.sum = {"amount"};
  struct Case {
    const char* from;
    const char* to;
    const char* total;
    size_t notCompared;
  } cases[] = {
      {"2026-11-19", "", "2072", 4},            // a day: 8 + 16 + 2048; 19/11, 2026-11-1, blank, Nov 19
      {"2026-11-13", "2026-11-19", "2076", 4},  // a week
      {"2026-11", "", "2366", 3},               // a month: 2026-11-1 is a month row here
      {"2026", "", "2431", 3},
  };
  for (const auto& c : cases) {
    q.from = c.from;
    q.to = c.to;
    const auto o = scanWith(file, q, 5);
    TEST_ASSERT_TRUE(o.ok);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.total, renderDec3(o.result.sums[0].total).c_str(), c.from);
    TEST_ASSERT_EQUAL_MESSAGE(c.notCompared, o.result.notCompared.count, c.from);
  }
  // Numbers compare as numbers when both bounds and the key are numbers.
  const std::string nums = "id,amount\n5,1\n7,2\n12.5,4\n20,8\n100,16\nabc,32\n";
  q.column = "amount";
  q.from = "2";
  q.to = "8";
  q.sum = {"amount"};
  const auto o = scanWith(nums, q, 3);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("14", renderDec3(o.result.sums[0].total).c_str());
  q.column = "id";
  q.from = "5";
  q.to = "20";
  const auto o2 = scanWith(nums, q, 3);
  TEST_ASSERT_EQUAL_STRING("15", renderDec3(o2.result.sums[0].total).c_str());   // 5, 7, 12.5, 20
  TEST_ASSERT_EQUAL(1, o2.result.notCompared.count);                              // abc
}

// ---- size and memory ----------------------------------------------------------------
static void test_a_several_megabyte_file_is_read_in_4_kb_reads_with_bounded_memory() {
  Rng g(9);
  const Domain& d = domains()[1];
  std::string file = std::string(d.header) + "\n";
  while (file.size() < 6u * 1024 * 1024) file += d.row(g, dateOf(300 + int(g.below(60)))) + "\n";
  Card card;
  card.file = file;
  CsvQuery q = dayQuery(d, dateOf(310), dateOf(316));
  aidfake::arm(-1);
  const auto o = scanCsv(card, file.size(), "p/t.csv", q);
  const size_t peak = aidfake::gPeak;
  aidfake::disarm();
  TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
  TEST_ASSERT_EQUAL(kAidChunk, card.maxRead);
  TEST_ASSERT_TRUE(card.reads >= file.size() / kAidChunk);
  TEST_ASSERT_TRUE_MESSAGE(peak < 256 * 1024, std::to_string(peak).c_str());   // bounded, whatever the size
  checkAgainstReference(file, q, o, "6 MB");
  TEST_ASSERT_EQUAL(0, aidfake::gLiveBlocks);
}

static void test_a_refused_allocation_at_any_point_is_the_no_memory_error() {
  Rng g(31);
  size_t refusals = 0;
  for (const Domain& d : domains()) {
    // Enough rows for the identical-line table to grow several blocks.
    const std::string file = makeFile(g, d, 60, 15, 10, true);
    const CsvQuery q = dayQuery(d, "2026", "");
    long calls = 0;
    {
      Card c;
      c.file = file;
      aidfake::arm(-1);
      TEST_ASSERT_TRUE(scanCsv(c, file.size(), "p/t.csv", q).ok);
      calls = aidfake::gAllocCalls;
      aidfake::disarm();
    }
    TEST_ASSERT_TRUE(calls >= 3);
    for (long k = 1; k <= calls; ++k) {
      Card c;
      c.file = file;
      aidfake::arm(k);
      const auto o = scanCsv(c, file.size(), "p/t.csv", q);
      aidfake::disarm();
      TEST_ASSERT_FALSE(o.ok);
      TEST_ASSERT_EQUAL_STRING(kAidNoMemory, o.error.c_str());
      TEST_ASSERT_EQUAL(0, aidfake::gLiveBlocks);
      TEST_ASSERT_EQUAL_STRING(file.c_str(), c.file.c_str());
      ++refusals;
    }
    // And by bytes: every budget below the need refuses cleanly.
    for (size_t budget = 0; budget < 40000; budget += 1999) {
      Card c;
      c.file = file;
      aidfake::arm(-1, budget);
      const auto o = scanCsv(c, file.size(), "p/t.csv", q);
      aidfake::disarm();
      if (!o.ok) TEST_ASSERT_EQUAL_STRING(kAidNoMemory, o.error.c_str());
      TEST_ASSERT_EQUAL(0, aidfake::gLiveBlocks);
    }
  }
  TEST_ASSERT_TRUE(refusals >= 9);
}

static void test_a_card_fault_is_an_error_never_a_partial_sum() {
  Rng g(5);
  const Domain& d = domains()[2];
  const std::string file = makeFile(g, d, 20, 15, 5, false);
  const CsvQuery q = dayQuery(d, "2026-11", "");
  long calls = 0;
  {
    Card c;
    c.file = file;
    TEST_ASSERT_TRUE(scanCsv(c, file.size(), "p/t.csv", q).ok);
    calls = c.calls;
  }
  TEST_ASSERT_TRUE(calls >= 3);
  for (long k = 1; k <= calls; ++k) {
    Card c;
    c.file = file;
    c.failAt = k;
    const auto o = scanCsv(c, file.size(), "p/t.csv", q);
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_EQUAL_STRING("card read failed", o.error.c_str());
  }
}

// ---- the 180 KB year file and 15-row days, each domain --------------------------------
static void test_day_week_and_month_totals_are_exact_on_a_year_file_per_domain() {
  for (const Domain& d : domains()) {
    Rng g(1000 + d.header[0]);
    std::string file = std::string(d.header) + "\n";
    for (int day = 0; day < 365 && file.size() < 180 * 1024; ++day)
      for (int r = 0; r < 15; ++r) file += d.row(g, dateOf(day)) + "\n";
    TEST_ASSERT_TRUE(file.size() >= 100 * 1024);
    Card c;
    c.file = file;
    for (const auto& q : {dayQuery(d, dateOf(40), ""), dayQuery(d, dateOf(40), dateOf(46)), dayQuery(d, "2026-02", "")}) {
      const auto o = scanCsv(c, file.size(), "p/t.csv", q);
      checkAgainstReference(file, q, o, d.name);
      TEST_ASSERT_TRUE(o.result.matched >= 15);
    }
  }
}

static void test_identical_groups_are_named_once_each_and_the_table_limit_is_stated() {
  std::string f = "date,amount\n";
  for (int i = 0; i < 3; ++i) f += "2026-11-19,5\n";
  f += "2026-11-19,6\n2026-11-19,6\n";
  CsvQuery q;
  q.sum = {"amount"};
  auto o = scanWith(f, q, 4096);
  TEST_ASSERT_EQUAL(2, o.result.identicalGroups);
  TEST_ASSERT_EQUAL_STRING("rows 5 of 5; sum amount=27; identical lines 2 groups (2=3=4, 5=6)",
                           renderCsvTail(q, o.result).c_str());
  // Past the table, the result says how far it checked.
  std::string big = "id,amount\n";
  for (size_t i = 0; i < kCsvIdenticalMax + 10; ++i) big += std::to_string(i) + ",1\n";
  big += "0,1\n";
  o = scanWith(big, q, 4096);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL(kCsvIdenticalMax, o.result.identicalChecked);
  const std::string tail = renderCsvTail(q, o.result);
  TEST_ASSERT_TRUE(tail.find("; identical lines checked in the first 8192 matching rows only") != std::string::npos);
}

static void test_a_torn_last_line_is_named_in_the_tail() {
  CsvQuery q;
  q.sum = {"amount"};
  const auto o = scanWith("date,amount\n2026-11-19,5\n2026-11-19,3", q, 4096);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("rows 2 of 2; sum amount=8; last line 3 has no newline (a write may have been cut short)",
                           renderCsvTail(q, o.result).c_str());
  TEST_ASSERT_TRUE(endsTorn(3, '3'));
  TEST_ASSERT_FALSE(endsTorn(3, '\n'));
  TEST_ASSERT_FALSE(endsTorn(0, 'x'));
}

// ---- the hostile review's cases (each was a silent loss or an overflow before the fix) ----
static void test_review_cases_are_named_not_lost() {
  CsvQuery q;
  q.column = "date";
  q.from = "2026-11-19";
  q.sum = {"amt"};
  // A valid quoted field over 8 KB with a line end in it: only that row is too long.
  std::string f = "date,note,amt\n2026-11-18,\"" + std::string(9000, 'x') + "\n\",5\n";
  for (int i = 0; i < 5; ++i) f += "2026-11-19,lunch,10\n";
  f += "2026-11-20,x,3\n";
  auto o = scanWith(f, q, 4096);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("rows 5 of 7 where date 2026-11-19..2026-11-19; sum amt=50; too long 1 (lines 2-3: over 8192 B); "
                           "identical lines 1 group (4=5=6=7+1)",
                           renderCsvTail(q, o.result).c_str());
  // A quote that never closes: the span it swallowed is named, to the end of the file.
  f = "date,note,amt\n2026-11-18,\"big one,450\n";
  for (int i = 0; i < 20; ++i) f += "2026-11-19,lunch,10\n";
  o = scanWith(f, q, 64);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING(
      "rows 0 of 1 where date 2026-11-19..2026-11-19; sum amt=0; skipped 1 (lines 2-22: field count is not the header's 3); "
      "the row at line 2 ends inside a quoted field (a write may have been cut short)",
      renderCsvTail(q, o.result).c_str());
  // A write cut inside a quoted field at the very end.
  f = "date,amt,note\n2026-11-19,5,ok\n2026-11-19,7,\"cut\n";
  q.sum = {"amt"};
  o = scanWith(f, q, 4096);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING(
      "rows 2 of 2 where date 2026-11-19..2026-11-19; sum amt=12; the row at line 3 ends inside a quoted field (a write "
      "may have been cut short)",
      renderCsvTail(q, o.result).c_str());
  // A numeric key with more decimals than the comparison holds is not compared.
  CsvQuery n;
  n.column = "w";
  n.from = "1";
  n.to = "1.0004";
  n.sum = {"n"};
  TEST_ASSERT_EQUAL_STRING("from and to take at most 3 decimals", checkCsvQuery(n).c_str());
  n.to = "1.001";
  o = scanWith("w,n\n1.00041,1\n1.0004,1\n1.001,1\n", n, 4096);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("rows 1 of 3 where w 1..1.001; sum n=1; not compared 2 (lines 2, 3: w blank or not like 1)",
                           renderCsvTail(n, o.result).c_str());
  // A limit past kCsvShowMax is held to it: the page stays inside its block.
  CsvQuery big;
  big.sum = {"amt"};
  big.limit = 4u << 20;
  std::string rows = "date,amt\n";
  while (rows.size() < (3u << 20)) rows += "2026-11-19,10\n";
  o = scanWith(rows, big, 4096);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_TRUE(o.result.body.size() <= kCsvShowMax + 64);
  TEST_ASSERT_TRUE(o.result.shownLast < o.result.matched);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_scan_agrees_with_the_reference_over_three_domains_and_hostile_rows);
  RUN_TEST(test_the_result_never_depends_on_the_chunking);
  RUN_TEST(test_the_filtered_read_has_its_exact_format);
  RUN_TEST(test_paging_never_changes_the_sums_and_next_offset_shows_every_row_once);
  RUN_TEST(test_decimals_are_exact_and_round_half_away_from_zero);
  RUN_TEST(test_a_total_past_int64_is_an_error_never_a_partial_sum);
  RUN_TEST(test_query_and_file_errors_state_the_fact);
  RUN_TEST(test_day_week_month_ranges_and_incomparable_keys_are_counted);
  RUN_TEST(test_a_several_megabyte_file_is_read_in_4_kb_reads_with_bounded_memory);
  RUN_TEST(test_a_refused_allocation_at_any_point_is_the_no_memory_error);
  RUN_TEST(test_a_card_fault_is_an_error_never_a_partial_sum);
  RUN_TEST(test_day_week_and_month_totals_are_exact_on_a_year_file_per_domain);
  RUN_TEST(test_identical_groups_are_named_once_each_and_the_table_limit_is_stated);
  RUN_TEST(test_a_torn_last_line_is_named_in_the_tail);
  RUN_TEST(test_review_cases_are_named_not_lost);
  return UNITY_END();
}
