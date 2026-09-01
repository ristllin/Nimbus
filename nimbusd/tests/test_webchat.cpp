// test_webchat - offline (T1) proof of the web surface's building blocks: the
// reply ring buffer (bounds, sequence numbers, JSON escaping) and the assembled
// web app nimbusd serves (the device's real page, byte-parity with the blessed
// fragments, plus the tunnel token seed). No sockets, no engine, no keys.
#include <string>

#include "reply_buffer.h"
#include "test_util.h"
#include "web_ui.h"
#include "webui_page.h"

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

// The assembled page is the device's REAL web app (the five-destination shell,
// every pane), so a Virtual Nimbus is recognizably the same product - and it is
// served with the tunnel token seed injected ahead of the app script, with no em
// dash anywhere (project-wide rule).
static void checkPage(ndtest::Ctx& c) {
  const std::string page = buildWebUiPage("tok-abc-123");
  c.ok(has(page, "<!doctype html"), "the page is an HTML document");
  // Structural invariants (the same the device-side webui_concat_check asserts):
  // the nav bar, the five destinations, and the Home pane.
  c.ok(has(page, "class=tabs>"), "the app nav bar is present");
  for (const char* d : {"data-p=home", "data-p=chat", "data-p=memory",
                        "data-p=assistant", "data-p=device"})
    c.ok(has(page, d), std::string("destination present: ") + d);
  c.ok(has(page, "id=pane-dash") && has(page, "id=pane-chat") && has(page, "id=pane-set"),
       "the Home / Chat / Device panes are present");
  c.ok(has(page, "id=ringsim"), "the ring simulator markup is present (full app, not a stub)");

  // Tunnel auto sign-in: the token is seeded into localStorage ahead of the app
  // script that reads it, so the app is signed in without a second sign-in.
  c.ok(has(page, "setItem('nimbusTok','tok-abc-123')"),
       "the served page seeds the given web token");
  c.ok(page.find("setItem('nimbusTok'") < page.find("function nimbusTok"),
       "the seed runs before the app's nimbusTok() is defined/used");
  // Honest-UI flag (CUM-279): the served page marks itself hosted, synchronously
  // before the app script, so device-only chrome is hidden with no flash. The shared
  // page reads window.NIMBUS_HOSTED; the device page never sets it (so it is falsy).
  c.ok(has(page, "window.NIMBUS_HOSTED=true"),
       "the served page sets the hosted honest-UI flag");
  c.ok(page.find("window.NIMBUS_HOSTED=true") < page.find("let HOSTED="),
       "the hosted flag is set before the app script reads it");
  // An empty token (dev / ungated) still seeds a sentinel so the client gate is
  // skipped and the ungated API accepts the request.
  c.ok(has(buildWebUiPage(""), "setItem('nimbusTok','tunnel')"),
       "an ungated instance seeds a sentinel token (gate still skipped)");

  bool emdash = false;
  for (size_t i = 0; i + 2 < page.size(); i++)
    if ((unsigned char)page[i] == 0xE2 && (unsigned char)page[i + 1] == 0x80 &&
        (unsigned char)page[i + 2] == 0x94)
      emdash = true;
  c.ok(!emdash, "the served page contains no em dash");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== web surface (T1, offline) ===\n");
  checkBuffer(c);
  checkPage(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
