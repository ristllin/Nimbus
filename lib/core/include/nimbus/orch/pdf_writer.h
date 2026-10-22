#pragma once
#include <cstddef>
#include <functional>
#include <string>

// pdf_writer - the device's own PDF writer (ruling R7, owner ask OA-107): a PDF for
// the owner with no OpenAI or Anthropic key, built from text on the device itself.
// It renders a light Markdown subset onto A4 pages with the two built-in PDF fonts
// (Helvetica and Helvetica-Bold, WinAnsi encoding, so nothing is embedded):
//
//   # Heading / ## Subheading / ### Minor heading
//   - bullet (or "* bullet")
//   | a | b |   table rows (a |---| separator row is skipped; the first row is bold)
//   a blank line separates paragraphs; any other line is body text, word-wrapped
//
// Text, headings and simple tables only: no images, charts or links (those need a
// provider sandbox). Characters outside WinAnsi print as '?'.
//
// The PDF is STREAMED to `sink` as it is laid out, one page at a time, so the device
// never holds the whole file: RAM use is one page's content stream (a few KB) plus
// the object offset table. Pure and host-tested (test/test_pdf_writer).
namespace nimbus {
namespace orch {

struct PdfLimits {
  size_t maxInput = 48 * 1024;   // bytes of title + text accepted
  int maxPages = 40;             // a longer document is refused, never cut silently
};

struct PdfResult {
  bool ok = false;
  int pages = 0;
  size_t bytes = 0;      // bytes handed to the sink
  std::string error;     // owner-facing reason when !ok
};

// Receives the PDF bytes in order; false aborts the write (the result says why).
using PdfSink = std::function<bool(const char* data, size_t len)>;

PdfResult writePdf(const std::string& title, const std::string& text, const PdfSink& sink,
                   const PdfLimits& limits = PdfLimits());

// The width of `text` (UTF-8) in points at `size` in Helvetica (bold when `bold`), as
// the writer measures it for wrapping. Exposed for the host test.
double pdfTextWidth(const std::string& text, double size, bool bold);

}  // namespace orch
}  // namespace nimbus
