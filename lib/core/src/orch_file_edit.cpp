#include "nimbus/orch/file_edit.h"

#include <algorithm>

#include "nimbus/orch/file_append.h"   // kKeyedMaxBytes

namespace nimbus::orch {

// The caps and the policy shape, before anything is opened: "" when the edit
// may proceed to the scan.
std::string validateEdit(size_t findLen, size_t replaceLen, const EditPolicy& pol) {
  if (findLen == 0 || findLen > kEditFindMaxBytes)
    return "the find text is empty or longer than " +
           std::to_string((unsigned)kEditFindMaxBytes) + " B - edit a smaller span";
  if (replaceLen > kEditReplaceMaxBytes)
    return "the replacement is longer than " +
           std::to_string((unsigned)kEditReplaceMaxBytes) + " B - split the edit";
  if (pol.count < 1 || pol.count > kEditMaxMatches)
    return "count must be between 1 and " + std::to_string(kEditMaxMatches);
  return std::string();
}

EditDecision decideEdit(size_t findLen, size_t replaceLen, const EditPolicy& pol,
                        size_t matches) {
  EditDecision d;
  d.matches = matches;
  if (const std::string bad = validateEdit(findLen, replaceLen, pol); !bad.empty()) {
    d.err = bad;
    return d;
  }
  if (matches == 0) {
    d.err = "no match for the find text - nothing changed";
    return d;
  }
  if (pol.all) {
    if ((int)matches > kEditMaxMatches) {
      d.err = "too many matches (" + std::to_string(matches) + ", the cap is " +
              std::to_string(kEditMaxMatches) + " per call) - make the find text longer";
      return d;
    }
    d.apply = (int)matches;
  } else {
    d.apply = std::min(pol.count, (int)matches);
  }
  d.sizeDelta = ((int64_t)replaceLen - (int64_t)findLen) * (int64_t)d.apply;
  d.ok = true;
  return d;
}

std::string editSizeRefusal(size_t fileBytes) {
  if (fileBytes <= kKeyedMaxBytes) return std::string();
  return "the file is over 1 MB, too large to edit in place - start a new file for new text "
         "(like notes-2.md); nothing was changed";
}

bool editChangesNothing(const std::string& find, const std::string& replace) { return find == replace; }

std::string renderEditNoChange(const std::string& where, const EditDecision& d) {
  return "edited " + where + ": found " + std::to_string(d.matches) + " match" + (d.matches == 1 ? "" : "es") +
         " (no change: the replacement is the same text, the file was not touched)";
}

std::string renderEditReply(const std::string& where, const EditDecision& d, size_t newSize) {
  std::string msg = "edited " + where + ": replaced " + std::to_string(d.apply) + " of " +
                    std::to_string(d.matches) + " match" + (d.matches == 1 ? "" : "es") + " (now " +
                    std::to_string(newSize) + " bytes)";
  if (d.matches > (size_t)d.apply)
    msg += "; " + std::to_string(d.matches - d.apply) + " more remain (all: true replaces them)";
  return msg;
}

TextEditStream::TextEditStream(const std::string& find, const std::string& replace,
                               bool countOnly)
    : find_(find), replace_(replace), countOnly_(countOnly) {
  if (find_.empty()) find_ = "\n";   // never matches an empty span: the tool refuses it earlier
}

void TextEditStream::scan(std::string& out, bool atEof) {
  if (done_) return;
  for (;;) {
    const size_t pos = carry_.find(find_);
    if (pos == std::string::npos) break;
    if (countOnly_) {
      // The scan pass: count and skip past the span (matches never overlap).
      found_++;
      carry_.erase(0, pos + find_.size());
      continue;
    }
    if (applied_ >= applyLimit_) {
      // The limit is reached: everything left - the pre-match bytes, this
      // match and all that follows - copies through unchanged, and nothing
      // is matched again.
      done_ = true;
      out += carry_;
      carry_.clear();
      return;
    }
    out.append(carry_, 0, pos);
    out += replace_;
    applied_++;
    found_++;
    carry_.erase(0, pos + find_.size());   // the scan resumes after the span
  }
  // No match in the carry: everything but the last (find_.size() - 1) bytes is
  // settled - unless the file has ended, when all of it is. The scan pass
  // emits nothing at all.
  if (countOnly_) {
    if (carry_.size() > find_.size() - 1) carry_.erase(0, carry_.size() - (find_.size() - 1));
    return;
  }
  const size_t keep = atEof ? 0 : find_.size() - 1;
  if (carry_.size() > keep) {
    out.append(carry_, 0, carry_.size() - keep);
    carry_.erase(0, carry_.size() - keep);
  }
}

void TextEditStream::feed(const char* data, size_t len, std::string& out) {
  if (done_) {
    if (!countOnly_) out.append(data, len);
    return;
  }
  carry_.append(data, len);
  scan(out, false);
}

void TextEditStream::finish(std::string& out) {
  if (done_) {
    if (!countOnly_) out += carry_;
    carry_.clear();
    return;
  }
  scan(out, true);
  // A partial span at the end of the file is not a match: emit it as it is -
  // and the scan pass emits nothing at all, its unsettled tail included (the
  // header's contract).
  if (!countOnly_) out += carry_;
  carry_.clear();
}

// ---- the keyed-CSV row diff ---------------------------------------------------

namespace {

std::string diffTrim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
  return s.substr(a, b - a);
}

std::string diffKey(const std::string& s) {
  std::string out = diffTrim(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return out;
}

// The span's lines, CR ends dropped, a final line without its end included: the
// parse below tells a whole row from a fragment, so the line split never guesses.
std::vector<std::string> spanLines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      if (start < text.size()) out.push_back(text.substr(start));
      return out;
    }
    std::string line = text.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
    start = nl + 1;
  }
}

// One parsed row of a span: its cells by the header's columns.
struct SpanRow {
  std::vector<std::string> cells;
  std::string date, key;   // its identity, once the columns are known
};

// The span's rows that read as the header's rows (the same reader the day total
// uses; a fragment, a comment or the header itself reads as none).
std::vector<SpanRow> spanRows(const std::string& text, const std::vector<std::string>& header) {
  std::vector<SpanRow> out;
  for (const std::string& line : spanLines(text)) {
    SpanRow r;
    r.cells = csvStoredRow(line, header);
    if (r.cells.size() == header.size()) out.push_back(r);
  }
  return out;
}

// The key column: the leftmost column other than the date that no parsed row
// reads as a plain number in (a meal, a category), or a column named "key"
// when every other column is numeric; -1 = none, and the caller says nothing.
int diffKeyColumn(const std::vector<std::string>& header, int dateCol,
                  const std::vector<SpanRow>& rows) {
  for (size_t c = 0; c < header.size(); c++) {
    if ((int)c == dateCol) continue;
    double v;
    bool anyFigure = false;
    for (const SpanRow& r : rows)
      if (c < r.cells.size() && csvFigure(r.cells[c], v)) {
        anyFigure = true;
        break;
      }
    if (!anyFigure) return (int)c;
  }
  for (size_t c = 0; c < header.size(); c++) {
    if ((int)c != dateCol && diffKey(header[c]) == "key") return (int)c;
  }
  return -1;
}

bool sameRowIdentity(const CsvEditRow& a, const CsvEditRow& b) {
  return a.date == b.date && diffKey(a.key) == diffKey(b.key);
}

// How the report names a row: the key column's own value when it already
// carries the date, else the date and the key.
std::string rowLabel(const CsvEditRow& r) {
  if (!r.date.empty() && r.key.rfind(r.date, 0) == 0) return r.key;
  if (r.date.empty()) return r.key;
  return r.date + " " + r.key;
}

}  // namespace

CsvEditDiff diffKeyedEdit(const std::string& headerRow, const std::string& find,
                          const std::string& replace) {
  CsvEditDiff d;
  const std::vector<std::string> header = csvHeaderOf(headerRow);
  if (header.size() < 2) return d;   // a one-column file keys nothing
  std::vector<SpanRow> before = spanRows(find, header);
  std::vector<SpanRow> after = spanRows(replace, header);
  // The date column: the one rule the keyed upsert uses, over both spans' rows,
  // so the diff and the upsert can never disagree on which column is the date.
  std::vector<std::vector<std::string>> all;
  for (const SpanRow& r : before) all.push_back(r.cells);
  for (const SpanRow& r : after) all.push_back(r.cells);
  const int dateCol = csvDateColumnOf(header, all);
  if (dateCol < 0) return d;
  const auto dated = [dateCol](std::vector<SpanRow>& rows) {
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [dateCol](const SpanRow& r) {
                                return isoDayOf(r.cells[(size_t)dateCol]).empty();
                              }),
               rows.end());
  };
  dated(before);
  dated(after);
  const int keyCol = diffKeyColumn(header, dateCol, before);
  if (keyCol < 0) return d;
  const auto identify = [dateCol, keyCol](std::vector<SpanRow>& rows) {
    for (SpanRow& r : rows) {
      r.date = isoDayOf(r.cells[(size_t)dateCol]);
      r.key = diffTrim(r.cells[(size_t)keyCol]);
    }
  };
  identify(before);
  identify(after);
  // No dated row parsed on either side (a mid-row edit, a span of fragments):
  // there is no row-level change to report, and the caller says nothing.
  d.keyed = !before.empty() || !after.empty();
  // Pair the rows by identity, in order (duplicates pair one to one): what
  // stayed may have changed, what did not is removed or added.
  std::vector<bool> used(after.size(), false);
  for (const SpanRow& b : before) {
    size_t hit = after.size();
    for (size_t i = 0; i < after.size(); i++)
      if (!used[i] && b.date == after[i].date && diffKey(b.key) == diffKey(after[i].key)) {
        used[i] = true;
        hit = i;
        break;
      }
    if (hit == after.size()) {
      d.removed.push_back({b.date, b.key});
      continue;
    }
    const SpanRow& a = after[hit];
    for (size_t c = 0; c < header.size(); c++) {   // the figure columns only
      if ((int)c == dateCol || (int)c == keyCol) continue;
      double was = 0, now = 0;
      if (csvFigure(b.cells[c], was) && csvFigure(a.cells[c], now) && was != now)
        d.changed.push_back({{b.date, b.key}, header[c], diffTrim(b.cells[c]), diffTrim(a.cells[c])});
    }
  }
  for (size_t i = 0; i < after.size(); i++)
    if (!used[i]) d.added.push_back({after[i].date, after[i].key});
  return d;
}

std::string renderKeyedEditDiff(const CsvEditDiff& d) {
  if (!d.keyed || (d.removed.empty() && d.added.empty() && d.changed.empty()))
    return std::string();
  std::string out;
  const auto rowsLine = [&out](const char* what, const std::vector<CsvEditRow>& rows) {
    if (rows.empty()) return;
    if (!out.empty()) out += '\n';
    out += what;
    const size_t named = std::min(rows.size(), kEditDiffMaxRows);
    for (size_t i = 0; i < named; i++) out += (i ? ", " : "") + rowLabel(rows[i]);
    if (rows.size() > named)
      out += " (+" + std::to_string(rows.size() - named) + " more)";
  };
  rowsLine("rows removed: ", d.removed);
  rowsLine("rows added: ", d.added);
  if (!d.changed.empty()) {
    if (!out.empty()) out += '\n';
    out += "changed: ";
    const size_t named = std::min(d.changed.size(), kEditDiffMaxRows);
    for (size_t i = 0; i < named; i++) {
      const CsvEditChange& c = d.changed[i];
      out += (i ? "; " : "") + rowLabel(c.row) + " " + c.col + " " + c.was + " -> " + c.now;
    }
    if (d.changed.size() > named)
      out += " (+" + std::to_string(d.changed.size() - named) + " more)";
  }
  return out;
}

std::vector<CsvEditRow> notReadded(const std::vector<CsvEditRow>& remembered,
                                   const std::vector<CsvEditRow>& readded) {
  std::vector<bool> used(readded.size(), false);
  std::vector<CsvEditRow> out;
  for (const CsvEditRow& r : remembered) {
    bool hit = false;
    for (size_t i = 0; i < readded.size(); i++)
      if (!used[i] && sameRowIdentity(readded[i], r)) {
        used[i] = true;
        hit = true;
        break;
      }
    if (!hit) out.push_back(r);
  }
  return out;
}

std::string renderNotReadded(const std::vector<CsvEditRow>& missing) {
  if (missing.empty()) return std::string();
  std::string out = "note: removed by an earlier edit and not logged again: ";
  const size_t named = std::min(missing.size(), kEditDiffMaxRows);
  for (size_t i = 0; i < named; i++) out += (i ? ", " : "") + rowLabel(missing[i]);
  if (missing.size() > named)
    out += " (+" + std::to_string(missing.size() - named) + " more)";
  return out;
}

}  // namespace nimbus::orch