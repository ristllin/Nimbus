#include <unity.h>

#include <cstdio>
#include <cstring>

#include "nimbus/cloud/loopback_target.h"
#include "nimbus/net/page_split.h"

#include <string>

using nimbus::cloud::kLoopbackMaxRespBody;
using nimbus::cloud::kLoopbackRespHeadroom;
using nimbus::cloud::kRelayResFrameMax;

void setUp() {}
void tearDown() {}

static long fileSize(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return -1;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fclose(f);
  return n;
}

static std::string readFile(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// CUM-173 regression guard, per response since CUM-565. The config page is the
// biggest tunneled response; a body over the loopback response cap overflows the
// parser and handleReq turns the tunneled request into a 5xx (the field white-screen).
// The page is served as two responses (nimbus/net/page_split.h): GET / (the HTML and
// a script tag) and GET app.js (the script). The webui snapshot
// (tools/webui_page.snapshot) is the byte-exact assembled page kept current by
// tools/webui_concat_check.py, which also asserts the script fragment is last and
// exactly <script>...</script>. This asserts EACH served body + headroom fits the
// cap, so the NEXT page growth fails the host battery instead of reaching the
// owner's device.
static void test_config_page_fits_loopback_cap() {
  const long page = fileSize("tools/webui_page.snapshot");
  TEST_ASSERT_TRUE_MESSAGE(page > 0,
                           "cannot read tools/webui_page.snapshot (run from repo root)");
  const std::string snap = readFile("tools/webui_page.snapshot");
  const size_t at = snap.find(nimbus::net::kPageScriptOpen);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, "the page has no <script> fragment");
  TEST_ASSERT_TRUE_MESSAGE(snap.find(nimbus::net::kPageScriptOpen, at + 1) == std::string::npos,
                           "the page has more than one <script>");
  size_t off = 0, n = 0;
  TEST_ASSERT_TRUE_MESSAGE(
      nimbus::net::pageScriptBody(snap.data() + at, snap.size() - at, off, n),
      "the script fragment must be last and exactly <script>...</script>\\n, or the device "
      "serves the page whole");
  const size_t html = nimbus::net::splitHtmlBytes(snap.size(), snap.size() - at);
  const size_t js = nimbus::net::splitScriptBytes(snap.size() - at);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)n, (uint32_t)js);
  char msg[200];
  std::snprintf(msg, sizeof msg,
                "GET / is %u B: + headroom exceeds kLoopbackMaxRespBody (a tunneled GET / "
                "would 5xx). Shrink the HTML fragments.",
                (unsigned)html);
  TEST_ASSERT_TRUE_MESSAGE(html + kLoopbackRespHeadroom <= kLoopbackMaxRespBody, msg);
  std::snprintf(msg, sizeof msg,
                "GET app.js is %u B: + headroom exceeds kLoopbackMaxRespBody (a tunneled "
                "app.js would 5xx). Shrink the script or split it again.",
                (unsigned)js);
  TEST_ASSERT_TRUE_MESSAGE(js + kLoopbackRespHeadroom <= kLoopbackMaxRespBody, msg);
}

// Counter-test for the split: a script fragment that is not exactly
// <script>...</script>\n is never split (the device then serves the page whole, and
// the guard above, which demands the split shape, fails instead of measuring a
// response the device does not send).
static void test_split_needs_the_exact_script_fragment() {
  size_t off = 0, n = 0;
  const char ok[] = "<script>a()</script>\n";
  TEST_ASSERT_TRUE(nimbus::net::pageScriptBody(ok, sizeof ok - 1, off, n));
  TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)off);
  TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)n);
  const char noClose[] = "<script>a()</script>";
  TEST_ASSERT_FALSE(nimbus::net::pageScriptBody(noClose, sizeof noClose - 1, off, n));
  const char noOpen[] = "<div>a()</script>\n";
  TEST_ASSERT_FALSE(nimbus::net::pageScriptBody(noOpen, sizeof noOpen - 1, off, n));
  TEST_ASSERT_FALSE(nimbus::net::pageScriptBody("", 0, off, n));
  // The tag is relative: the cloud serves the device page under /d/<id>/, where an
  // absolute "/app.js" would leave the tunnel.
  TEST_ASSERT_NOT_NULL(std::strstr(nimbus::net::kPageScriptTag, "src=\"app.js\""));
}

// Counter-test for the cap raise (CUM-279): whenever kLoopbackMaxRespBody grows, the
// base64-expanded frame it permits MUST still fit the relay's res-frame protocol max,
// or a full page would be truncated/rejected at the wire (the fault the cap guards).
static void test_cap_stays_base64_safe() {
  const unsigned long expanded =
      (unsigned long)kLoopbackMaxRespBody + kLoopbackMaxRespBody / 3u;  // ~4/3 base64
  TEST_ASSERT_TRUE_MESSAGE(
      expanded < kRelayResFrameMax,
      "kLoopbackMaxRespBody base64-expands past the relay res-frame max: a full page "
      "would be truncated. Lower the cap or raise the protocol frame.");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_config_page_fits_loopback_cap);
  RUN_TEST(test_cap_stays_base64_safe);
  RUN_TEST(test_split_needs_the_exact_script_fragment);
  return UNITY_END();
}
