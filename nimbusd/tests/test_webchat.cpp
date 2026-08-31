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

// The ReplyBuffer: a bounded ring with monotonic sequence numbers, JSON output,
// and no clear-on-read (so multiple browser tabs stay consistent).
static void checkBuffer(ndtest::Ctx& c) {
  ReplyBuffer b(3);
  c.eqi((long)b.lastSeq(), 0, "empty buffer: lastSeq 0");
  for (int i = 1; i <= 5; i++) b.push("assistant", "m" + std::to_string(i));
  c.eqi((long)b.lastSeq(), 5, "lastSeq is the count of pushes");
  c.eqi((long)b.size(), 3, "buffer is bounded to its cap (3), oldest evicted");

  const std::string all = b.sinceJsonArray(0);
  c.ok(!has(all, "\"m1\"") && !has(all, "\"m2\""), "evicted entries (m1,m2) are gone");
  c.ok(has(all, "\"m3\"") && has(all, "\"m5\""), "the last cap entries are retained");
  c.ok(has(all, "\"seq\":3") && has(all, "\"seq\":5"), "entries carry their sequence numbers");
  c.ok(has(all, "\"role\":\"assistant\"") && has(all, "\"ts\":"), "entries carry role and timestamp");

  const std::string tail = b.sinceJsonArray(4);
  c.ok(has(tail, "\"seq\":5") && !has(tail, "\"seq\":4"),
       "sinceJsonArray(after) returns only entries with a higher seq");
  c.ok(b.sinceJsonArray(4) == tail, "reads never clear the buffer (multi-tab safe)");

  ReplyBuffer e(4);
  e.push("assistant", "a\"b\\c\nd\te");
  c.ok(has(e.sinceJsonArray(0), "a\\\"b\\\\c\\nd\\te"),
       "text is JSON-escaped (quote, backslash, newline, tab)");
}

// The static chat page is fully self-contained: no external resources, printable
// ASCII only, no em dash, and it carries the honest keyless state.
static void checkPage(ndtest::Ctx& c) {
  const std::string page = kChatPageHtml;
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
    // U+2014 EM DASH as UTF-8 (0xE2 0x80 0x94) is banned project-wide.
    if (ch == 0xE2 && i + 2 < page.size() &&
        (unsigned char)page[i + 1] == 0x80 && (unsigned char)page[i + 2] == 0x94)
      emdash = true;
  }
  c.ok(ascii, "the page is printable ASCII only (surface constraint)");
  c.ok(!emdash, "the page contains no em dash");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== web chat surface (T1, offline) ===\n");
  checkBuffer(c);
  checkPage(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
