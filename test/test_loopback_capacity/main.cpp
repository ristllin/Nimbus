#include <unity.h>

#include <cstdio>

#include "nimbus/cloud/loopback_target.h"

using nimbus::cloud::kLoopbackMaxRespBody;
using nimbus::cloud::kLoopbackRespHeadroom;

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

// CUM-173 regression guard. The assembled config page (served at GET /) is the biggest
// tunneled response body; if it exceeds the loopback response cap the parser overflows
// and handleReq turns every tunneled GET / into a 5xx (the field white-screen). The
// webui snapshot (tools/webui_page.snapshot) is the byte-exact assembled page kept
// current by tools/webui_concat_check.py. This asserts page + headroom fits the cap, so
// the NEXT page growth fails the host battery instead of reaching the owner's device.
static void test_config_page_fits_loopback_cap() {
  const long page = fileSize("tools/webui_page.snapshot");
  TEST_ASSERT_TRUE_MESSAGE(page > 0,
                           "cannot read tools/webui_page.snapshot (run from repo root)");
  TEST_ASSERT_TRUE_MESSAGE(
      (unsigned long)page + kLoopbackRespHeadroom <= kLoopbackMaxRespBody,
      "config page + headroom exceeds kLoopbackMaxRespBody: a tunneled GET / would 5xx. "
      "Raise the cap (PSRAM-backed, must stay under the 512KB res-frame max even after "
      "base64) or shrink the page.");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_config_page_fits_loopback_cap);
  return UNITY_END();
}
