// orch_file_aids_e1 - the old engine's files.read and files.edit on the shared aids
// (file_aids_e1.h).
#include "nimbus/orch/file_aids_e1.h"

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
