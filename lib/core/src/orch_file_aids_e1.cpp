// orch_file_aids_e1 - the old engine's files.read and files.edit on the shared aids
// (file_aids_e1.h).
#include "nimbus/orch/file_aids_e1.h"

#include <algorithm>
#include <string_view>

#include "nimbus/orch/day_start.h"
#include "nimbus/orch/file_append.h"   // csvHeaderOf, csvStoredRow, csvDateColumnOf

namespace nimbus::orch::aids::e1 {

namespace {

using detail::join;
using detail::Num;

// The file's header and its first data rows (comment and empty lines left out), from its
// first read: what csvDateColumnOf needs to name the date column.
bool firstRows(AidSource& src, std::vector<std::string>& header, std::vector<std::vector<std::string>>& rows) {
  std::string head(kAidChunk, '\0');
  size_t got = 0;
  if (!src.readAt(0, head.size(), &head[0], got)) return false;
  head.resize(got);
  size_t at = 0;
  while (at < head.size() && rows.size() < 8) {
    const size_t nl = head.find('\n', at);
    if (nl == std::string::npos) break;
    std::string line = head.substr(at, nl - at);
    at = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') continue;
    if (header.empty()) header = csvHeaderOf(line);
    else if (std::vector<std::string> r = csvStoredRow(line, header); !r.empty()) rows.push_back(std::move(r));
  }
  return true;
}

// The keyed-CSV row report of files.edit (unchanged rules, file_edit.h): rows removed, added
// or changed, the history rows kept, and the removals a later re-add skipped.
std::string keyedReport(const EditCall& c, const std::string& hist, RemovedRows& memory) {
  if (c.header.empty()) return std::string();
  const CsvEditDiff diff = diffKeyedEdit(c.header, c.find, c.replace);
  if (!diff.keyed) return std::string();
  std::string out;
  if (const std::string lines = renderKeyedEditDiff(diff); !lines.empty()) out += "\n" + lines;
  if (!hist.empty()) out += "\nthe removed and changed rows are kept in the file as history rows (#removed: / #replaced:)";
  if (memory.project == c.project && memory.name == c.name) {
    memory.rows = notReadded(memory.rows, diff.added);
    if (const std::string warn = renderNotReadded(memory.rows); !warn.empty()) out += "\n" + warn;
  }
  if (!diff.removed.empty() && (memory.project != c.project || memory.name != c.name))
    memory = RemovedRows{c.project, c.name, {}};
  for (const CsvEditRow& r : diff.removed)
    if (memory.rows.size() < kRemovedNoteMaxRows) memory.rows.push_back(r);
  return out;
}

// files.edit's own caps before the binding's quota: the decision on the counted matches.
struct CapSink : AidSink {
  AidSink& inner;
  const EditCall& call;
  std::string err;
  CapSink(AidSink& i, const EditCall& c) : inner(i), call(c) {}
  bool partWrite(std::string_view b, bool first) override { return inner.partWrite(b, first); }
  bool partSwap() override { return inner.partSwap(); }
  void partDiscard() override { inner.partDiscard(); }
  bool admit(uint64_t newSize, size_t matches) override {
    EditPolicy p = call.pol;
    if (call.line) p = EditPolicy{};
    const EditDecision d = decideEdit(call.find.size(), call.replace.size(), p, matches);
    if (!d.ok && matches) return (err = d.err).empty();
    return inner.admit(newSize, matches);
  }
  std::string refusal() const override { return err.empty() ? inner.refusal() : err; }
};

}  // namespace

std::string runningDate(uint64_t epoch, int32_t offsetMin, uint16_t startMin) {
  char d[12];
  if (!day::clockSet(epoch) || !day::formatDate(day::runningDay(epoch, offsetMin, startMin), d, sizeof d))
    return std::string();
  return d;
}

std::string dayTotal(AidSource& src, uint64_t size, const std::string& path, const std::vector<std::string>& sums,
                     const std::string& day) {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;
  if (!firstRows(src, header, rows)) return "[no day total: card read failed]\n";
  const int dc = csvDateColumnOf(header, rows);
  if (dc < 0) return "[no day total: the header names no date column]\n";
  CsvQuery q;
  q.column = header[size_t(dc)];
  q.from = day;
  q.sum = sums;
  q.showRows = false;
  if (const std::string bad = checkCsvQuery(q); !bad.empty()) return "[no day total: " + bad + "]\n";
  const CsvScanOutcome o = scanCsv(src, size, path, q);
  detail::Txt t;
  if (o.ok) t << std::string_view(renderCsvTail(q, o.result)) << "\n";
  else t << "[no day total: " << std::string_view(o.error) << "]\n";
  return std::move(t.s);
}

ReadForm readForm(ArduinoJson::JsonObjectConst a) {
  if (!a["column"].isNull() || !a["from"].isNull() || !a["to"].isNull()) return ReadForm::Table;
  if (!a["sum"].isNull()) return ReadForm::DayTotal;
  const ArduinoJson::JsonVariantConst tail = a["tail"];
  return tail.is<float>() && tail.as<double>() >= 1 ? ReadForm::Tail : ReadForm::Plain;
}

// sum: a list of column names, or one string of names separated by commas. False with err.
static bool sumArgs(ArduinoJson::JsonObjectConst a, std::vector<std::string>& sums, std::string& err) {
  const ArduinoJson::JsonVariantConst v = a["sum"];
  if (v.is<ArduinoJson::JsonArrayConst>()) {
    for (ArduinoJson::JsonVariantConst x : v.as<ArduinoJson::JsonArrayConst>()) {
      if (!x.is<const char*>()) return (err = "sum is not a list of column names").empty();
      sums.emplace_back(x.as<const char*>());
    }
    return true;
  }
  if (v.isNull()) return true;
  if (!v.is<const char*>()) return (err = "sum is not a list of column names").empty();
  const std::string names = v.as<const char*>();   // one string: names separated by commas
  for (size_t at = 0; at <= names.size();) {
    const size_t comma = std::min(names.find(',', at), names.size());
    sums.push_back(names.substr(at, comma - at));
    at = comma + 1;
  }
  return true;
}

namespace {

size_t digits(uint64_t v) {
  size_t d = 1;
  while (v >= 10) {
    v /= 10;
    ++d;
  }
  return d;
}

// The longest head line readPart writes for a file of `size` bytes: seven numbers (a, b, n,
// x, y, m and the next offset) of at most digits(size + 1) each.
size_t headBound(uint64_t size) {
  static constexpr char kLongest[] = "bytes - of , lines - of ; next part: \"offset\":\n";
  return sizeof(kLongest) - 1 + 7 * digits(size + 1);
}

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// One pass over the file: its line ends in all and before `from`, and the bytes from `from`
// (at most `want`). False on a card fault or a read short of `size`.
struct PartScan {
  uint64_t newlines = 0, newlinesBefore = 0;
  char last = '\n';
  std::string window;
};
bool scanForPart(AidSource& src, uint64_t size, uint64_t from, size_t want, PartScan& p) {
  std::string chunk(kAidChunk, '\0');
  uint64_t off = 0;
  for (;;) {
    size_t got = 0;
    if (!src.readAt(off, chunk.size(), &chunk[0], got)) return false;
    for (size_t i = 0; i < got; ++i) {
      const uint64_t at = off + i;
      if (chunk[i] == '\n') {
        ++p.newlines;
        if (at < from) ++p.newlinesBefore;
      }
      if (at >= from && p.window.size() < want) p.window.push_back(chunk[i]);
    }
    if (got) p.last = chunk[got - 1];
    off += got;
    if (got < chunk.size()) break;
  }
  return off >= size;
}

uint64_t lineEnds(std::string_view s) {
  uint64_t k = 0;
  for (char c : s) k += c == '\n';
  return k;
}

}  // namespace

bool readPart(AidSource& src, uint64_t size, uint64_t offset, size_t cap, std::string& out) {
  if (cap < kPartMinCap) cap = kPartMinCap;
  const uint64_t from0 = std::min(offset, size);
  const size_t room = cap - headBound(size);
  PartScan p;
  // Four bytes more: an offset inside a character moves past its continuation bytes, and the
  // byte after the part tells whether the cut splits a character.
  if (!scanForPart(src, size, from0, room + 4, p)) {
    out = "the card read failed; nothing was shown";
    return false;
  }
  const std::string_view w(p.window);
  size_t skip = 0;
  while (skip < 3 && skip < w.size() && isContinuation(w[skip])) ++skip;
  const uint64_t from = from0 + skip;
  const size_t full = size_t(std::min<uint64_t>(room, size - from));
  size_t len = full;
  if (from + len < size) {
    const size_t nl = w.rfind('\n', skip + len - 1);
    if (nl != std::string_view::npos && nl >= skip + len / 2) len = nl + 1 - skip;
    else
      while (len > 0 && isContinuation(w[skip + len])) --len;
    // Bytes that are no character at all (a run of continuation bytes): the part takes them
    // as they are (the wire repairs what is not UTF-8), so a read always reaches the end.
    if (len == 0) len = full;
  }
  const std::string_view part = w.substr(skip, len);
  const uint64_t to = from + len;
  detail::Txt t;
  t << "bytes " << from << "-" << to << " of " << size;
  if (len) {
    const uint64_t first = 1 + p.newlinesBefore;
    t << ", lines " << first << "-" << first + lineEnds(part.substr(0, len - 1)) << " of "
      << p.newlines + (p.last != '\n' ? 1 : 0);
  }
  if (to < size) t << "; next part: \"offset\":" << to << "\n";
  else t << "; end of file\n";
  t << part;
  out = std::move(t.s);
  return true;
}

uint64_t offsetArg(ArduinoJson::JsonObjectConst a) {
  const ArduinoJson::JsonVariantConst v = a["offset"];
  if (!v.is<float>()) return 0;
  const double d = v.as<double>();
  constexpr double kMax = 9007199254740992.0;   // 2^53: every integer below it is exact
  return d > 0 ? uint64_t(d < kMax ? d : kMax) : 0;
}

bool queryArgs(ArduinoJson::JsonObjectConst a, size_t offset, CsvQuery& q, std::string& err) {
  std::string* fields[] = {&q.column, &q.from, &q.to};
  const char* keys[] = {"column", "from", "to"};
  for (size_t i = 0; i < 3; ++i) {
    const ArduinoJson::JsonVariantConst v = a[keys[i]];
    if (!v.isNull() && !v.is<const char*>()) return (err = std::string(keys[i]) + " is not a string").empty();
    if (!v.isNull()) *fields[i] = v.as<const char*>();
  }
  if (!sumArgs(a, q.sum, err)) return false;
  q.skipRows = offset;
  q.limit = kE1PageBytes;
  err = checkCsvQuery(q);
  return err.empty();
}

bool tableRead(AidSource& src, uint64_t size, const std::string& path, const CsvQuery& q, std::string& out) {
  out = checkCsvQuery(q);
  if (!out.empty()) return false;
  const CsvScanOutcome o = scanCsv(src, size, path, q);
  if (!o.ok) {
    out = o.error;
    return false;
  }
  detail::Txt t;
  t << std::string_view(path) << " size=" << o.result.bytes << " lines=" << o.result.lines
    << std::string_view(renderCsvRead(q, o.result));
  out = std::move(t.s);
  return true;
}

bool edit(AidSource& src, AidSink& sink, uint64_t size, const EditCall& c, RemovedRows& memory, std::string& reply) {
  if (const std::string big = editSizeRefusal(size_t(size)); !big.empty()) return (reply = big).empty();
  EditRequest r;
  r.oldText = c.find;
  r.newText = c.replace;
  r.all = c.pol.all;
  r.line = c.line;
  r.count = c.pol.all || c.line ? 0 : uint32_t(c.pol.count > 0 ? c.pol.count : 1);
  r.oldName = "find";
  r.trailer = c.header.empty() ? std::string() : historyRowsForKeyedEdit(c.header, c.find, c.replace);   // fenced by editFile
  CapSink capped(sink, c);
  const std::string where = c.project + "/" + c.name;
  const EditOutcome o = editFile(src, capped, size, 0, where, r);
  if (!o.ok) return (reply = o.error).empty();
  EditDecision d;
  d.ok = true;
  d.matches = o.found;
  d.apply = int(o.replaced);
  reply = renderEditReply(where, d, size_t(o.newSize));
  reply += renderEditLines(o);
  reply += keyedReport(c, r.trailer, memory);
  return true;
}

}  // namespace nimbus::orch::aids::e1
