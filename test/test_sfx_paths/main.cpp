#include <unity.h>

#include <cstdio>
#include <cstring>

#include "nimbus/sfx_map.h"
#include "nimbus/sfx_paths.h"

using nimbus::sfx::Ev;
using nimbus::sfx::parseClipFilename;
using nimbus::sfx::safeRepoPath;
using nimbus::sfx::slug;

void setUp() {}
void tearDown() {}

// The sync manifest is hostile input: only clean sd/<pool>/ paths pass, and
// the owner's sd/custom/ pool is NEVER writable by a synced manifest.
static void test_safe_repo_path_accepts_clean_sd_paths() {
  TEST_ASSERT_TRUE(safeRepoPath("sd/general/boot-0.wav"));
  TEST_ASSERT_TRUE(safeRepoPath("sd/pulse/agent_done-2.wav"));
  TEST_ASSERT_TRUE(safeRepoPath("sd/terran/needs_you-1.wav"));  // legacy theme pool
}

static void test_safe_repo_path_rejects_custom_pool() {
  TEST_ASSERT_FALSE(safeRepoPath("sd/custom/x.wav"));
  TEST_ASSERT_FALSE(safeRepoPath("sd/custom/boot-0.wav"));
  // ...but a pool merely STARTING with "custom" is a different (allowed) name.
  TEST_ASSERT_TRUE(safeRepoPath("sd/customx/boot-0.wav"));
}

static void test_safe_repo_path_rejects_hostile() {
  TEST_ASSERT_FALSE(safeRepoPath(nullptr));
  TEST_ASSERT_FALSE(safeRepoPath(""));
  TEST_ASSERT_FALSE(safeRepoPath("sd/../mem/x"));       // traversal out of /sfx
  TEST_ASSERT_FALSE(safeRepoPath("sd/a b.wav"));        // space corrupts the GET line
  TEST_ASSERT_FALSE(safeRepoPath("sd/a\r\nHost: x"));   // CRLF injection
  TEST_ASSERT_FALSE(safeRepoPath("basic/boot.wav"));    // embedded tier is not synced
  TEST_ASSERT_FALSE(safeRepoPath("/sfx/general/x.wav"));  // absolute, not sd/-relative
  TEST_ASSERT_FALSE(safeRepoPath("mem/battery/d1.jsonl"));
}

// Every enum slug round-trips through "<slug>-<n>.wav" -> parseClipFilename.
static void test_parse_clip_filename_roundtrip_all_slugs() {
  for (unsigned i = 0; i < (unsigned)Ev::COUNT; i++) {
    const char* s = slug((Ev)i);
    TEST_ASSERT_NOT_NULL(s);
    static const unsigned kNs[] = {0u, 7u, 12u};
    for (unsigned n : kNs) {
      char name[48];
      snprintf(name, sizeof(name), "%s-%u.wav", s, n);
      char slugBuf[24];
      unsigned back = 999;
      TEST_ASSERT_TRUE_MESSAGE(parseClipFilename(name, slugBuf, sizeof(slugBuf), &back), name);
      TEST_ASSERT_EQUAL_STRING(s, slugBuf);
      TEST_ASSERT_EQUAL_UINT(n, back);
      Ev e;
      TEST_ASSERT_TRUE(nimbus::sfx::parseSlug(slugBuf, e));
      TEST_ASSERT_EQUAL_INT((int)i, (int)e);
    }
  }
}

static void test_parse_clip_filename_rejects_malformed() {
  char s[24];
  unsigned n = 0;
  TEST_ASSERT_FALSE(parseClipFilename(nullptr, s, sizeof(s), &n));
  TEST_ASSERT_FALSE(parseClipFilename("boot-0.wav", nullptr, 0, &n));
  TEST_ASSERT_FALSE(parseClipFilename("boot-0.wav", s, sizeof(s), nullptr));
  TEST_ASSERT_FALSE(parseClipFilename("", s, sizeof(s), &n));
  TEST_ASSERT_FALSE(parseClipFilename("boot.wav", s, sizeof(s), &n));      // no -<n>
  TEST_ASSERT_FALSE(parseClipFilename("boot-.wav", s, sizeof(s), &n));     // empty digits
  TEST_ASSERT_FALSE(parseClipFilename("boot-x.wav", s, sizeof(s), &n));    // non-digit
  TEST_ASSERT_FALSE(parseClipFilename("boot-0.mp3", s, sizeof(s), &n));    // wrong ext
  TEST_ASSERT_FALSE(parseClipFilename("boot-0", s, sizeof(s), &n));        // no ext
  TEST_ASSERT_FALSE(parseClipFilename("-0.wav", s, sizeof(s), &n));        // empty slug
  TEST_ASSERT_FALSE(parseClipFilename("boot-99999.wav", s, sizeof(s), &n));  // n cap
  // A slug that will not fit the out buffer FAILS (no truncated partials).
  char tiny[4];
  TEST_ASSERT_FALSE(parseClipFilename("boot-0.wav", tiny, sizeof(tiny), &n));
  TEST_ASSERT_TRUE(parseClipFilename("boo-0.wav", tiny, sizeof(tiny), &n));
  TEST_ASSERT_EQUAL_STRING("boo", tiny);
}

// Multi-dash names split on the LAST dash (slugs themselves never carry '-',
// but an owner-dropped custom file might; the parse must stay well-defined).
static void test_parse_clip_filename_last_dash() {
  char s[24];
  unsigned n = 0;
  TEST_ASSERT_TRUE(parseClipFilename("my-cool-3.wav", s, sizeof(s), &n));
  TEST_ASSERT_EQUAL_STRING("my-cool", s);
  TEST_ASSERT_EQUAL_UINT(3u, n);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_safe_repo_path_accepts_clean_sd_paths);
  RUN_TEST(test_safe_repo_path_rejects_custom_pool);
  RUN_TEST(test_safe_repo_path_rejects_hostile);
  RUN_TEST(test_parse_clip_filename_roundtrip_all_slugs);
  RUN_TEST(test_parse_clip_filename_rejects_malformed);
  RUN_TEST(test_parse_clip_filename_last_dash);
  UNITY_END();
  return 0;
}
