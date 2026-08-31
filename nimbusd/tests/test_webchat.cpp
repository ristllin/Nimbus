// test_webchat - offline (T1) proof of the web chat surface's building blocks:
// the reply ring buffer (bounds, sequence numbers, JSON escaping) and the static
// chat page's self-containment (no external resources, ASCII-only, wired to the
// right relative endpoints). No sockets, no engine, no keys.
#include <string>

#include "chat_page.h"
#include "reply_buffer.h"
#include "test_util.h"

using namespace nimbusd;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

int main() {
  ndtest::Ctx c;
  std::printf("=== web chat surface (T1, offline) ===\n");

  // ---- ReplyBuffer: bounded ring, monotonic seq, no clear-on-read -----------
  {
    ReplyBuffer b(3);
    c.eqi((long)b.lastSeq(), 0, "empty buffer: lastSeq 0");
    for (int i = 1; i <= 5; i++) b.push("assistant", "m" + std::to_string(i));
    c.eqi((long)b.lastSeq(), 5, "lastSeq is the count of pushes");
    c.eqi((long)b.size(), 3, "buffer is bounded to its cap (3), oldest evicted");

    const std::string all = b.sinceJsonArray(0);
    c.ok(!has(all, "\"m1\"") && !has(all, "\"m2\""), "evicted entries (m1,m2) are gone");
    c.ok(has(all, "\"m3\"") && has(all, "\"m4\"") && has(all, "\"m5\""),
         "the last cap entries (m3,m4,m5) are retained");
    c.ok(has(all, "\"seq\":3") && has(all, "\"seq\":5"), "entries carry their sequence numbers");
    c.ok(has(all, "\"role\":\"assistant\"") && has(all, "\"ts\":"), "entries carry role and timestamp");

    // Sequence filter: after=4 yields only seq 5.
    const std::string tail = b.sinceJsonArray(4);
    c.ok(has(tail, "\"seq\":5") && !has(tail, "\"seq\":4") && !has(tail, "\"seq\":3"),
         "sinceJsonArray(after) returns only entries with a higher seq");

    // A read does not clear: a second read at the same watermark repeats.
    c.ok(b.sinceJsonArray(4) == tail, "reads never clear the buffer (multi-tab safe)");
  }

  // ---- JSON escaping: quotes, backslash, newline, control bytes -------------
  {
    ReplyBuffer b(4);
    b.push("assistant", "a\"b\\c\nd\te");
    const std::string j = b.sinceJsonArray(0);
    c.ok(has(j, "a\\\"b\\\\c\\nd\\te"), "text is JSON-escaped (quote, backslash, newline, tab)");
  }

  // ---- the static chat page is fully self-contained -------------------------
  {
    const std::string page = chatPageHtml();
    c.ok(has(page, "<!doctype html"), "the page is an HTML document");
    c.ok(has(page, "api/message") && has(page, "api/replies"),
         "the page talks to the relative api/message + api/replies endpoints");
    c.ok(!has(page, "://"), "no absolute URL scheme anywhere (no http:// or https://)");
    c.ok(!has(page, "src=\"//") && !has(page, "href=\"//"), "no protocol-relative external refs");
    c.ok(!has(page, "<script src") && !has(page, "<link "),
         "no external scripts or stylesheets (inline only)");
    c.ok(!has(page, "@import"), "no CSS @import of an external sheet");
    c.ok(has(page, "No provider key configured"),
         "the page carries the honest keyless state (CUM-211)");

    bool ascii = true, emdash = false;
    for (size_t i = 0; i < page.size(); i++) {
      unsigned char ch = (unsigned char)page[i];
      if (ch >= 0x80) ascii = false;
      // U+2014 EM DASH as UTF-8: 0xE2 0x80 0x94 - banned project-wide.
      if (ch == 0xE2 && i + 2 < page.size() &&
          (unsigned char)page[i + 1] == 0x80 && (unsigned char)page[i + 2] == 0x94)
        emdash = true;
    }
    c.ok(ascii, "the page is printable ASCII only (surface constraint)");
    c.ok(!emdash, "the page contains no em dash");
  }

  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
