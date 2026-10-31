#include "nimbus/orch/file_append.h"

#include <cstdlib>
#include <vector>

namespace nimbus::orch {

// v4.6.3 (the 30 Oct investigation): the first row of a NEW CSV must be its
// header (the column names, no data). A data row first would grow the file up
// headerless: keyed writes refuse forever after (no key column to name), so
// every later "correction" would append a duplicate - the exact loop the
// investigation documented. "" when the first row is a header.
static std::string csvCreateHeaderRefusal(const std::string& body) {
  const size_t nl = body.find('\n');
  const std::string first = body.substr(0, nl == std::string::npos ? body.size() : nl);
  if (first.find_first_of("0123456789") == std::string::npos) return std::string();
  return "creating a CSV needs its header row first (the column names, no data - e.g. "
         "date,meal,items,kcal,protein_g for the food log): a headerless CSV cannot be "
         "updated by key later, so corrections would duplicate - send the header row "
         "first, then the data rows";
}

AppendPlan planAppend(const std::string& text, size_t fileBytes, char lastByte,
                       bool csvCreate) {
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
  if (csvCreate) {
    const std::string refuse = csvCreateHeaderRefusal(body);
    if (!refuse.empty()) {
      p.err = refuse;
      return p;
    }
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
  if (v.lastCut)
    out += "[the last row is incomplete - the file was cut mid-write; the next "
           "files.append starts on a new line]\n";
  return out;
}

namespace {
// The CSV row helpers of the keyed upsert (G1, rows 32/33). A field is the
// text between commas, surrounding spaces trimmed; a quoted field is used as
// written (quotes included): a log row, not a spreadsheet formula.
std::vector<std::string> splitCsvRow(const std::string& row) {
  std::vector<std::string> out;
  size_t p = 0;
  while (true) {
    size_t c = row.find(',', p);
    const size_t end = c == std::string::npos ? row.size() : c;
    size_t b = p, e = end;
    while (b < e && (row[b] == ' ' || row[b] == '\t')) b++;
    while (e > b && (row[e - 1] == ' ' || row[e - 1] == '\t')) e--;
    out.push_back(row.substr(b, e - b));
    if (c == std::string::npos) break;
    p = c + 1;
  }
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

// The leading number of a field ("450" or "450 kcal"); 0 when none.
double numPrefix(const std::string& f) {
  if (f.empty()) return 0;
  char* end = nullptr;
  const double v = std::strtod(f.c_str(), &end);
  return end == f.c_str() ? 0 : v;
}

int columnIndex(const std::vector<std::string>& header, const std::string& name) {
  for (size_t i = 0; i < header.size(); i++)
    if (header[i] == name) return (int)i;
  return -1;
}

// The date column ("date", then "day"), or -1.
int dateColumnOf(const std::vector<std::string>& header) {
  int c = columnIndex(header, "date");
  return c >= 0 ? c : columnIndex(header, "day");
}

void dayTotalOf(const std::vector<std::string>& rows, int dateCol, int sumCol,
                const std::string& today, CsvUpsert& out) {
  if (dateCol < 0 || sumCol < 0 || today.empty()) return;
  double total = 0;
  for (size_t r = 1; r < rows.size(); r++) {
    const std::vector<std::string> f = splitCsvRow(rows[r]);
    if (f.size() <= (size_t)dateCol || f.size() <= (size_t)sumCol) continue;
    if (f[(size_t)dateCol].rfind(today, 0) != 0) continue;
    total += numPrefix(f[(size_t)sumCol]);
  }
  out.todayTotal = total;
  out.hasTotal = true;
}
// The column indexes a keyed upsert needs; -1 when the column is absent.
struct UpsertCols {
  int key = -1, sum = -1, date = -1;
};

// The header checks' outcome: the column indexes when the headers can merge,
// else the reason they cannot.
struct UpsertHeader {
  bool        ok = false;
  UpsertCols  cols;
  std::string err;
};

// The header checks: the text's header must name the key (and the sum) column,
// and a stored file must carry the key column too (its header is the first
// row; an empty or new file adopts the text's header, and `rows` receives it).
UpsertHeader upsertColumns(const std::vector<std::string>& fresh,
                           const std::string& keyCol, const std::string& sumCol,
                           std::vector<std::string>& rows, std::vector<std::string>& header) {
  UpsertHeader h;
  const std::vector<std::string> newHeader = splitCsvRow(fresh.front());
  h.cols.key = columnIndex(newHeader, keyCol);
  if (h.cols.key < 0) {
    h.err = "the text's header has no column '" + keyCol + "'";
    return h;
  }
  h.cols.sum = sumCol.empty() ? -1 : columnIndex(newHeader, sumCol);
  if (!sumCol.empty() && h.cols.sum < 0) {
    h.err = "the text's header has no column '" + sumCol + "'";
    return h;
  }
  if (!rows.empty()) {
    header = splitCsvRow(rows.front());
    if (columnIndex(header, keyCol) < 0) {
      // v4.6.3 (the 30 Oct investigation): the stored file's "header" is often
      // a data row (the file was created headerless), so every keyed write
      // refuses and every "correction" appends a duplicate. Name the way out:
      // the rewrite tools - an append can never fix a headerless file.
      h.err = "the stored file has no column '" + keyCol + "' - its header differs (a file "
              "created without a header row reads its first data row as the header); fix the "
              "file with files.edit, or files.read it and artifact.save the whole corrected "
              "file with a header row first";
      return h;
    }
  } else {
    rows.push_back(fresh.front());   // the header lands as the file's first row
    header = newHeader;
  }
  h.cols.date = dateColumnOf(header);
  h.ok = true;
  return h;
}

// Merge each new data row by key: a row whose key exists replaces it, anything
// else appends. A stray row (no key value) is skipped.
void mergeRowsByKey(std::vector<std::string>& rows, const std::vector<std::string>& fresh,
                    int keyIdx, int& replaced, int& appended) {
  for (size_t i = 1; i < fresh.size(); i++) {
    const std::vector<std::string> f = splitCsvRow(fresh[i]);
    if (f.size() <= (size_t)keyIdx || f[(size_t)keyIdx].empty()) continue;   // a stray row: skip
    const std::string& key = f[(size_t)keyIdx];
    bool hit = false;
    for (size_t r = 1; r < rows.size() && !hit; r++) {
      const std::vector<std::string> g = splitCsvRow(rows[r]);
      if (g.size() > (size_t)keyIdx && g[(size_t)keyIdx] == key) {
        rows[r] = fresh[i];
        replaced++;
        hit = true;
      }
    }
    if (!hit) {
      rows.push_back(fresh[i]);
      appended++;
    }
  }
}


}  // namespace

CsvUpsert planCsvUpsert(const std::string& existing, const std::string& newText,
                        const std::string& keyCol, const std::string& sumCol,
                        const std::string& today) {
  CsvUpsert out;
  if (keyCol.empty()) {
    out.err = "need 'key': the column whose value identifies a row";
    return out;
  }
  std::vector<std::string> rows = splitRows(existing);
  const std::vector<std::string> fresh = splitRows(newText);
  if (fresh.empty()) {
    out.err = "need 'text': the header row and at least one data row";
    return out;
  }
  std::vector<std::string> header;
  const UpsertHeader h = upsertColumns(fresh, keyCol, sumCol, rows, header);
  if (!h.ok) {
    out.err = h.err;
    return out;
  }
  mergeRowsByKey(rows, fresh, h.cols.key, out.replaced, out.appended);
  // Join and report.
  std::string joined;
  for (const std::string& r : rows) joined += r + "\n";
  out.changed = joined != existing;
  out.out = out.changed ? joined : std::string();
  dayTotalOf(rows, h.cols.date, h.cols.sum, today, out);
  out.ok = true;
  return out;
}

bool csvDayTotal(const std::string& fileText, const std::string& sumCol,
                 const std::string& today, double& total) {
  if (sumCol.empty() || today.empty()) return false;
  const std::vector<std::string> rows = splitRows(fileText);
  if (rows.empty()) return false;
  const std::vector<std::string> header = splitCsvRow(rows.front());
  const int sumIdx = columnIndex(header, sumCol);
  const int dateIdx = dateColumnOf(header);
  if (sumIdx < 0 || dateIdx < 0) return false;
  CsvUpsert tmp;
  dayTotalOf(rows, dateIdx, sumIdx, today, tmp);
  if (!tmp.hasTotal) return false;
  total = tmp.todayTotal;
  return true;
}

}  // namespace nimbus::orch
