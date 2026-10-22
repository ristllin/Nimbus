// test_pdf_writer - the device's own PDF writer (ruling R7, OA-107).
//
// A PDF built on the device is the one route that needs no OpenAI or Anthropic key.
// These cases check the CLASS a reader depends on, for every document shape: the
// cross-reference table points at every object, each stream's declared length is its
// real length, the page tree counts the pages written, and the text stays inside the
// margins. The limits refuse a document instead of cutting it silently.
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/pdf_writer.h"

using nimbus::orch::PdfLimits;
using nimbus::orch::PdfResult;
using nimbus::orch::writePdf;

void setUp() {}
void tearDown() {}

static PdfResult render(const std::string& title, const std::string& text, std::string& out,
                        const PdfLimits& lim = PdfLimits()) {
  out.clear();
  return writePdf(title, text, [&](const char* d, size_t n) { out.append(d, n); return true; }, lim);
}

static size_t count(const std::string& s, const std::string& needle) {
  size_t n = 0;
  for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1)) n++;
  return n;
}

// The structural contract a PDF reader relies on.
static void checkStructure(const std::string& pdf, const PdfResult& r) {
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL((int)pdf.size(), (int)r.bytes);
  TEST_ASSERT_EQUAL(0, (int)pdf.find("%PDF-1.4\n"));
  TEST_ASSERT_TRUE(pdf.size() > 6 && pdf.compare(pdf.size() - 6, 6, "%%EOF\n") == 0);
  // startxref points at the xref table, whose entries point at each object.
  const size_t sx = pdf.rfind("startxref\n");
  TEST_ASSERT_TRUE(sx != std::string::npos);
  const size_t xref = std::stoul(pdf.substr(sx + 10));
  TEST_ASSERT_EQUAL(0, (int)pdf.compare(xref, 5, "xref\n"));
  unsigned first = 0, n = 0;
  TEST_ASSERT_EQUAL(2, std::sscanf(pdf.c_str() + xref + 5, "%u %u", &first, &n));
  const size_t entries = pdf.find('\n', xref + 5) + 1;
  for (unsigned i = 1; i < n; i++) {
    const std::string e = pdf.substr(entries + 20 * i, 20);
    TEST_ASSERT_EQUAL_MESSAGE(20, (int)e.size(), "xref entries are 20 bytes");
    const size_t off = std::stoul(e.substr(0, 10));
    const std::string head = std::to_string(i) + " 0 obj\n";
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)pdf.compare(off, head.size(), head), head.c_str());
  }
  // Every stream's /Length is its real length.
  for (size_t p = pdf.find("<</Length "); p != std::string::npos; p = pdf.find("<</Length ", p + 1)) {
    const size_t len = std::stoul(pdf.substr(p + 10));
    const size_t data = pdf.find(">>\nstream\n", p) + 10;
    TEST_ASSERT_EQUAL(0, (int)pdf.compare(data + len, 10, "\nendstream"));
  }
  // The page tree counts the pages written; each carries its number.
  TEST_ASSERT_TRUE(pdf.find("/Count " + std::to_string(r.pages) + ">>") != std::string::npos);
  TEST_ASSERT_EQUAL(r.pages, (int)count(pdf, "/Type/Page/"));
  for (int i = 1; i <= r.pages; i++)
    TEST_ASSERT_TRUE(pdf.find("(Page " + std::to_string(i) + ") Tj") != std::string::npos);
}

// Every text run starts inside the left margin and, measured with the writer's own
// font metrics, ends inside the right one.
static void checkMargins(const std::string& pdf) {
  int runs = 0;
  for (size_t p = pdf.find("BT /F"); p != std::string::npos; p = pdf.find("BT /F", p + 1)) {
    const bool bold = pdf[p + 5] == '2';
    double size = 0, x = 0, y = 0;
    TEST_ASSERT_EQUAL(3, std::sscanf(pdf.c_str() + p + 7, "%lf Tf %lf %lf Td", &size, &x, &y));
    const size_t open = pdf.find('(', p), close = pdf.find(") Tj ET", p);
    std::string s = pdf.substr(open + 1, close - open - 1);
    std::string plain;
    for (size_t i = 0; i < s.size(); i++) plain += (s[i] == '\\' && i + 1 < s.size()) ? s[++i] : s[i];
    TEST_ASSERT_TRUE(x >= 56 - 0.01 || y < 60);   // the page number sits in the footer
    TEST_ASSERT_TRUE_MESSAGE(x + nimbus::orch::pdfTextWidth(plain, size, bold) <= 595 - 56 + 0.5,
                             plain.c_str());
    TEST_ASSERT_TRUE(y >= 30 && y <= 842);
    runs++;
  }
  TEST_ASSERT_TRUE(runs > 0);
}

static const char* kReport =
    "# Weekly summary\n"
    "Revenue rose this week, driven by two large orders (see the table below).\n"
    "\n"
    "## Highlights\n"
    "- Orders shipped on time: 98%\n"
    "- A very long bullet that has to wrap onto a second line because it carries far more words "
    "than one line of an A4 page can hold at eleven points\n"
    "\n"
    "| Item | Qty | Price |\n"
    "|------|----:|------:|\n"
    "| Widget (blue) | 12 | 4.50 |\n"
    "| Gadget with a deliberately long name that must wrap inside its cell | 3 | 19.99 |\n"
    "\n"
    "### Notes\n"
    "Backslash \\ and parentheses ( ) are escaped. Supercalifragilisticexpialidociousword"
    "thatisfarlongerthananylineonthepageandmustbebrokenbycharactersinsteadofoverflowing.\n";

static void test_report_is_a_valid_pdf() {
  std::string pdf;
  const PdfResult r = render("Nimbus report", kReport, pdf);
  checkStructure(pdf, r);
  checkMargins(pdf);
  TEST_ASSERT_EQUAL(1, r.pages);
  TEST_ASSERT_TRUE(pdf.find("(Nimbus report) Tj") != std::string::npos);    // title
  TEST_ASSERT_TRUE(pdf.find("/F2 15.00 Tf") != std::string::npos);           // # heading, bold
  TEST_ASSERT_TRUE(pdf.find("(Item) Tj") != std::string::npos);
  TEST_ASSERT_TRUE(pdf.find("|---") == std::string::npos);                    // separator skipped
  TEST_ASSERT_TRUE(pdf.find("\\\\ and parentheses \\( \\) are escaped") != std::string::npos);
  TEST_ASSERT_TRUE(pdf.find("/Title(Nimbus report)") != std::string::npos);
  TEST_ASSERT_TRUE(count(pdf, " l S\n") >= 12);                               // table rules
}

// A long document flows onto further pages, each valid and numbered.
static void test_long_document_flows_onto_pages() {
  std::string text;
  for (int i = 0; i < 150; i++)
    text += "Paragraph " + std::to_string(i) + ": the quick brown fox jumps over the lazy dog, "
            "again and again, so that every line wraps at least once on the page.\n\n";
  std::string pdf;
  const PdfResult r = render("Long", text, pdf);
  checkStructure(pdf, r);
  checkMargins(pdf);
  TEST_ASSERT_TRUE(r.pages >= 5);
}

// Owner text in UTF-8: Latin-1 letters and the common typographic marks map to
// WinAnsi; anything else prints as '?'.
static void test_utf8_maps_to_winansi() {
  std::string pdf;
  const PdfResult r = render("", "Caf\xc3\xa9 \xe2\x80\x93 na\xc3\xafve \xe2\x82\xac" "5 \xe6\x97\xa5", pdf);
  checkStructure(pdf, r);
  TEST_ASSERT_TRUE(pdf.find("(Caf\xe9 \x96 na\xefve \x80" "5 ?) Tj") != std::string::npos);
}

// Refusals, never a silent cut: empty, too long to accept, too many pages, a sink
// that stops accepting bytes.
static void test_limits_refuse_instead_of_cutting() {
  std::string pdf;
  PdfResult r = render(" ", "  \n ", pdf);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.error.find("nothing to write") != std::string::npos);
  TEST_ASSERT_EQUAL(0, (int)pdf.size());

  PdfLimits small;
  small.maxInput = 100;
  r = render("t", std::string(200, 'a'), pdf, small);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.error.find("longer than") != std::string::npos);

  PdfLimits pages;
  pages.maxPages = 2;
  std::string text;
  for (int i = 0; i < 400; i++) text += "line " + std::to_string(i) + "\n";
  r = render("t", text, pdf, pages);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.error.find("longer than 2 pages") != std::string::npos);

  size_t budget = 600;
  std::string partial;
  r = writePdf("t", kReport, [&](const char* d, size_t n) {
    if (partial.size() + n > budget) return false;
    partial.append(d, n);
    return true;
  });
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.error.find("could not be written") != std::string::npos);
}

// A table row taller than a whole page is refused, never drawn below the page edge.
static void test_row_taller_than_a_page_is_refused() {
  std::string cell;
  for (int i = 0; i < 700; i++) cell += "word ";
  std::string pdf;
  const PdfResult r = render("t", "| a | b |\n|---|---|\n| " + cell + " | " + cell + " |\n", pdf);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.error.find("taller than a page") != std::string::npos);
  // A long table of normal rows still flows across pages, every run on the page.
  std::string rows = "| item | note |\n|---|---|\n";
  for (int i = 0; i < 120; i++) rows += "| row " + std::to_string(i) + " | a short note about it |\n";
  const PdfResult ok = render("t", rows, pdf);
  checkStructure(pdf, ok);
  checkMargins(pdf);
  TEST_ASSERT_TRUE(ok.pages >= 2);
}

// A title alone is a document; an empty line is just a gap.
static void test_title_only_and_blank_lines() {
  std::string pdf;
  PdfResult r = render("Only a title", "", pdf);
  checkStructure(pdf, r);
  TEST_ASSERT_EQUAL(1, r.pages);
  r = render("", "\n\n\nText after gaps\n", pdf);
  checkStructure(pdf, r);
  TEST_ASSERT_TRUE(pdf.find("(Text after gaps) Tj") != std::string::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_report_is_a_valid_pdf);
  RUN_TEST(test_long_document_flows_onto_pages);
  RUN_TEST(test_utf8_maps_to_winansi);
  RUN_TEST(test_limits_refuse_instead_of_cutting);
  RUN_TEST(test_title_only_and_blank_lines);
  RUN_TEST(test_row_taller_than_a_page_is_refused);
  return UNITY_END();
}
