#include "nimbus/orch/pdf_writer.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace nimbus {
namespace orch {

namespace {

// ---- fonts -------------------------------------------------------------------
// Advance widths (1/1000 em) of the standard Type 1 Helvetica and Helvetica-Bold
// for ASCII 32..126, from their Adobe AFM files. Any other WinAnsi byte is
// measured as 556 (the font's figure width): wrapping stays close enough.
const unsigned short kHelv[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,   // 32-47
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,   // 48-63
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,  // 64-79
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,   // 80-95
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,   // 96-111
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};       // 112-126
const unsigned short kHelvBold[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,   // 32-47
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,   // 48-63
    975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,   // 64-79
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,   // 80-95
    333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,   // 96-111
    611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};       // 112-126

double byteWidth(unsigned char c, double size, bool bold) {
  const unsigned w = (c >= 32 && c <= 126) ? (bold ? kHelvBold : kHelv)[c - 32] : 556;
  return w * size / 1000.0;
}

// One UTF-8 code point to its WinAnsi byte, or '?' when WinAnsi has none.
unsigned char winAnsi(unsigned cp) {
  if (cp < 0x80) return cp >= 0x20 ? (unsigned char)cp : ' ';
  if (cp >= 0xA0 && cp <= 0xFF) return (unsigned char)cp;   // Latin-1 = WinAnsi here
  switch (cp) {
    case 0x20AC: return 0x80;   // euro sign
    case 0x2026: return 0x85;   // ellipsis
    case 0x2018: return 0x91;
    case 0x2019: return 0x92;
    case 0x201C: return 0x93;
    case 0x201D: return 0x94;
    case 0x2022: return 0x95;   // bullet
    case 0x2013: return 0x96;   // en dash
    case 0x2014: return 0x97;   // em dash (owner text may carry one)
    default:     return '?';
  }
}

// UTF-8 text to WinAnsi bytes (a broken sequence becomes '?').
std::string toWinAnsi(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) { out += '?'; i++; continue; }
    unsigned cp = n == 1 ? c : c & (0x7F >> n);
    for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    out += (char)winAnsi(cp);
    i += n;
  }
  return out;
}

double ansiWidth(const std::string& a, double size, bool bold) {
  double w = 0;
  for (unsigned char c : a) w += byteWidth(c, size, bold);
  return w;
}

// A PDF string literal: ( ) and \ escaped.
std::string pdfString(const std::string& a) {
  std::string out = "(";
  for (char c : a) {
    if (c == '(' || c == ')' || c == '\\') out += '\\';
    out += c;
  }
  return out + ")";
}

// Greedy word wrap of WinAnsi text to `width` points; a word longer than a line is
// broken by characters. Always returns at least one (possibly empty) line.
std::vector<std::string> wrap(const std::string& a, double size, bool bold, double width) {
  std::vector<std::string> lines;
  std::string line, word;
  auto flushWord = [&]() {
    if (word.empty()) return;
    const std::string cand = line.empty() ? word : line + " " + word;
    if (ansiWidth(cand, size, bold) <= width) { line = cand; word.clear(); return; }
    if (!line.empty()) { lines.push_back(line); line.clear(); }
    while (ansiWidth(word, size, bold) > width && word.size() > 1) {   // an over-long word
      size_t k = 1;
      while (k < word.size() && ansiWidth(word.substr(0, k + 1), size, bold) <= width) k++;
      lines.push_back(word.substr(0, k));
      word.erase(0, k);
    }
    line = word;
    word.clear();
  };
  for (char c : a) {
    if (c == ' ') flushWord();
    else word += c;
  }
  flushWord();
  lines.push_back(line);
  return lines;
}

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r");
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

std::string num(double v) {
  char b[24];
  std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

// ---- page geometry (A4, points) ---------------------------------------------
constexpr double kPageW = 595, kPageH = 842;
constexpr double kMarginX = 56, kTop = 782, kBottom = 64;
constexpr double kTextW = kPageW - 2 * kMarginX;
constexpr double kBody = 11, kBodyLead = 15;
constexpr double kCell = 10, kCellLead = 13, kCellPad = 4;

// The streaming document: objects are numbered as they are written, their offsets
// kept for the xref table. Object 1 is the catalog, 2 the page tree (written last,
// once every page is known), 3 and 4 the fonts, 5 the document info.
class Writer {
 public:
  Writer(const PdfSink& sink, const PdfLimits& lim) : sink_(sink), lim_(lim), offsets_(6, 0) {}

  bool begin(const std::string& title) {
    emit("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    object(1, "<</Type/Catalog/Pages 2 0 R>>");
    object(3, "<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>>");
    object(4, "<</Type/Font/Subtype/Type1/BaseFont/Helvetica-Bold/Encoding/WinAnsiEncoding>>");
    object(5, "<</Title" + pdfString(toWinAnsi(title)) + "/Producer(Nimbus)>>");
    return ok_;
  }

  // Room for `h` points on the current page; starts a new one when needed. A block
  // taller than a whole page (a table row with huge cells) is refused: drawing it
  // would put text below the page edge, a silent cut.
  bool room(double h) {
    if (y_ - h >= kBottom) return true;
    if (h > kTop - kBottom)
      return fail("a table row is taller than a page; shorten its cells or split the table");
    return newPage();
  }
  void text(double x, double size, bool bold, const std::string& ansi) {
    page_ += "BT /" + std::string(bold ? "F2 " : "F1 ") + num(size) + " Tf " + num(x) + " " +
             num(y_) + " Td " + pdfString(ansi) + " Tj ET\n";
  }
  void rule(double x1, double y1, double x2, double y2) {
    page_ += "0.6 w " + num(x1) + " " + num(y1) + " m " + num(x2) + " " + num(y2) + " l S\n";
  }
  double y() const { return y_; }
  void down(double h) { y_ -= h; }
  bool started() const { return pages_ > 0; }
  bool failed() const { return !ok_; }
  const std::string& error() const { return err_; }

  bool newPage() {
    if (pages_ > 0 && !flushPage()) return false;
    if (pages_ >= lim_.maxPages) {
      return fail("the document is longer than " + std::to_string(lim_.maxPages) +
                  " pages; split it or shorten it");
    }
    pages_++;
    page_.clear();
    y_ = kTop;
    return true;
  }

  PdfResult finish() {
    PdfResult r;
    if (ok_ && pages_ == 0) newPage();
    if (ok_) flushPage();
    if (ok_) {
      std::string kids;
      for (int id : pageIds_) kids += std::to_string(id) + " 0 R ";
      object(2, "<</Type/Pages/Kids[" + kids + "]/Count " + std::to_string(pageIds_.size()) + ">>");
      writeXref();
    }
    r.ok = ok_;
    r.pages = (int)pageIds_.size();
    r.bytes = bytes_;
    r.error = err_;
    return r;
  }

 private:
  bool fail(const std::string& why) {
    if (ok_) err_ = why;
    ok_ = false;
    return false;
  }
  void emit(const std::string& s) {
    if (!ok_) return;
    if (!sink_(s.data(), s.size())) { fail("the file could not be written"); return; }
    bytes_ += s.size();
  }
  void object(int id, const std::string& body) {
    if ((size_t)id >= offsets_.size()) offsets_.resize(id + 1, 0);
    offsets_[id] = bytes_;
    emit(std::to_string(id) + " 0 obj\n" + body + "\nendobj\n");
  }
  bool flushPage() {
    // The page number, then the content stream and the page object.
    const std::string label = "Page " + std::to_string(pages_);
    page_ += "BT /F1 9 Tf " + num((kPageW - ansiWidth(label, 9, false)) / 2) + " 36 Td " +
             pdfString(label) + " Tj ET\n";
    const int content = nextId_++, pageObj = nextId_++;
    object(content, "<</Length " + std::to_string(page_.size()) + ">>\nstream\n" + page_ +
                        "\nendstream");
    object(pageObj, "<</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Resources<</Font<</F1 3 0 R"
                    "/F2 4 0 R>>>>/Contents " + std::to_string(content) + " 0 R>>");
    pageIds_.push_back(pageObj);
    page_.clear();
    return ok_;
  }
  void writeXref() {
    const size_t at = bytes_;
    std::string x = "xref\n0 " + std::to_string(offsets_.size()) + "\n0000000000 65535 f \n";
    char e[24];
    for (size_t i = 1; i < offsets_.size(); i++) {
      std::snprintf(e, sizeof e, "%010lu 00000 n \n", (unsigned long)offsets_[i]);
      x += e;
    }
    x += "trailer\n<</Size " + std::to_string(offsets_.size()) +
         "/Root 1 0 R/Info 5 0 R>>\nstartxref\n" + std::to_string(at) + "\n%%EOF\n";
    emit(x);
  }

  const PdfSink& sink_;
  const PdfLimits& lim_;
  std::vector<size_t> offsets_;
  std::vector<int> pageIds_;
  std::string page_;
  std::string err_;
  size_t bytes_ = 0;
  int nextId_ = 6;
  int pages_ = 0;
  double y_ = kTop;
  bool ok_ = true;
};

// ---- blocks ------------------------------------------------------------------

void paragraph(Writer& w, const std::string& ansi, double size, bool bold, double lead,
               double indent) {
  for (const std::string& line : wrap(ansi, size, bold, kTextW - indent)) {
    if (!w.room(lead)) return;
    w.down(lead);
    w.text(kMarginX + indent, size, bold, line);
  }
}

void heading(Writer& w, const std::string& ansi, int level) {
  const double size = level == 1 ? 15 : level == 2 ? 13 : 11.5;
  w.down(level == 1 ? 8 : 6);
  paragraph(w, ansi, size, true, size + 5, 0);
}

void bullet(Writer& w, const std::string& ansi) {
  const std::vector<std::string> lines = wrap(ansi, kBody, false, kTextW - 14);
  for (size_t i = 0; i < lines.size(); i++) {
    if (!w.room(kBodyLead)) return;
    w.down(kBodyLead);
    if (i == 0) w.text(kMarginX + 3, kBody, false, "\x95");
    w.text(kMarginX + 14, kBody, false, lines[i]);
  }
}

// One table row's cells (WinAnsi), or false for a |---| separator row.
bool tableCells(const std::string& line, std::vector<std::string>& cells) {
  cells.clear();
  std::string s = trim(line);
  if (!s.empty() && s.front() == '|') s.erase(0, 1);
  if (!s.empty() && s.back() == '|') s.pop_back();
  bool separator = true;
  size_t start = 0;
  for (;;) {
    const size_t bar = s.find('|', start);
    const std::string cell = trim(s.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
    if (cell.find_first_not_of("-: ") != std::string::npos) separator = false;
    cells.push_back(toWinAnsi(cell));
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return !separator;
}

// Column widths: each column's widest cell, scaled down together to fit the page.
std::vector<double> columnWidths(const std::vector<std::vector<std::string>>& rows, size_t ncol) {
  std::vector<double> w(ncol, 24);
  for (size_t r = 0; r < rows.size(); r++)
    for (size_t c = 0; c < rows[r].size() && c < ncol; c++) {
      const double cw = ansiWidth(rows[r][c], kCell, r == 0) + 2 * kCellPad;
      if (cw > w[c]) w[c] = cw;
    }
  double total = 0;
  for (double v : w) total += v;
  if (total > kTextW)
    for (double& v : w) v *= kTextW / total;
  return w;
}

void tableRow(Writer& w, const std::vector<std::string>& cells, const std::vector<double>& cw,
              bool header) {
  std::vector<std::vector<std::string>> wrapped;
  size_t lines = 1;
  for (size_t c = 0; c < cw.size(); c++) {
    wrapped.push_back(wrap(c < cells.size() ? cells[c] : "", kCell, header, cw[c] - 2 * kCellPad));
    if (wrapped.back().size() > lines) lines = wrapped.back().size();
  }
  const double h = lines * kCellLead + kCellPad;
  if (!w.room(h)) return;
  const double top = w.y();
  double x = kMarginX;
  for (size_t c = 0; c < cw.size(); c++) {
    for (size_t l = 0; l < wrapped[c].size(); l++) {
      w.down(kCellLead);
      w.text(x + kCellPad, kCell, header, wrapped[c][l]);
    }
    w.down(-(double)wrapped[c].size() * kCellLead);   // back to the row top
    w.rule(x, top, x, top - h);
    x += cw[c];
  }
  w.rule(x, top, x, top - h);
  w.rule(kMarginX, top, x, top);
  w.rule(kMarginX, top - h, x, top - h);
  w.down(h);
}

void table(Writer& w, const std::vector<std::string>& lines) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> cells;
  size_t ncol = 0;
  for (const std::string& l : lines)
    if (tableCells(l, cells)) {
      rows.push_back(cells);
      if (cells.size() > ncol) ncol = cells.size();
    }
  if (rows.empty()) return;
  const std::vector<double> cw = columnWidths(rows, ncol);
  w.down(4);
  for (size_t r = 0; r < rows.size() && !w.failed(); r++) tableRow(w, rows[r], cw, r == 0);
  w.down(6);
}

int headingLevel(const std::string& line) {
  int n = 0;
  while (n < (int)line.size() && line[n] == '#') n++;
  return (n >= 1 && n <= 3 && n < (int)line.size() && line[n] == ' ') ? n : 0;
}

bool isBullet(const std::string& line) {
  return line.size() > 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ';
}

// Lay out one source line (or, for a table, the run of table lines it starts).
size_t layoutLine(Writer& w, const std::vector<std::string>& src, size_t i) {
  const std::string line = trim(src[i]);
  if (line.empty()) { w.down(6); return i + 1; }
  if (line[0] == '|') {
    size_t j = i;
    std::vector<std::string> run;
    while (j < src.size() && !trim(src[j]).empty() && trim(src[j])[0] == '|') run.push_back(src[j++]);
    table(w, run);
    return j;
  }
  const int level = headingLevel(line);
  if (level) heading(w, toWinAnsi(line.substr(level + 1)), level);
  else if (isBullet(line)) bullet(w, toWinAnsi(trim(line.substr(2))));
  else paragraph(w, toWinAnsi(line), kBody, false, kBodyLead, 0);
  return i + 1;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t nl = text.find('\n', start);
    out.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return out;
}

}  // namespace

double pdfTextWidth(const std::string& text, double size, bool bold) {
  return ansiWidth(toWinAnsi(text), size, bold);
}

PdfResult writePdf(const std::string& title, const std::string& text, const PdfSink& sink,
                   const PdfLimits& limits) {
  PdfResult bad;
  auto blank = [](const std::string& v) { return v.find_first_not_of(" \t\r\n") == std::string::npos; };
  if (blank(text) && blank(title)) {
    bad.error = "nothing to write: give the document a title or some text";
    return bad;
  }
  if (title.size() + text.size() > limits.maxInput) {
    bad.error = "the text is longer than " + std::to_string(limits.maxInput / 1024) +
                " KB; split it into several documents";
    return bad;
  }
  Writer w(sink, limits);
  if (!w.begin(title) || !w.newPage()) return w.finish();
  if (!trim(title).empty()) {
    paragraph(w, toWinAnsi(trim(title)), 18, true, 24, 0);
    w.down(8);
  }
  const std::vector<std::string> src = splitLines(text);
  for (size_t i = 0; i < src.size() && !w.failed();) i = layoutLine(w, src, i);
  return w.finish();
}

}  // namespace orch
}  // namespace nimbus
