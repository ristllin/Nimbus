#include <unity.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nimbus/logring.h"     // core::LogRing::redact  (the shared redaction)
#include "nimbus/log_sinks.h"   // core::emitRedacted     (the shared choke point)
#include "../../src/sys/errlog.h"   // the portable durable-log policy under test

using namespace nimbus::errlog;

void setUp() {}
void tearDown() {}

// ---- line formatting + inline sanitize --------------------------------------

static void test_format_line_shape() {
  std::string line = formatLine(7, 12345, cat::kProvider, "mistral 429 backing off");
  TEST_ASSERT_EQUAL_STRING("7 12345 provider mistral 429 backing off\n", line.c_str());
}

static void test_format_line_null_cat_is_dash() {
  std::string line = formatLine(0, 0, nullptr, "hi");
  TEST_ASSERT_EQUAL_STRING("0 0 - hi\n", line.c_str());
}

// A log call can never inject a second line or a terminal escape into the file:
// CR/LF/tab/control bytes collapse to spaces, and the line still ends in one '\n'.
static void test_sanitize_strips_control_and_newlines() {
  // split literal so \x01 does not greedily swallow the following 'c' as a hex digit
  std::string line = formatLine(1, 2, "net", "a\nb\r\nc\ttab" "\x01" "ctrl");
  TEST_ASSERT_EQUAL_STRING("1 2 net a b  c tab ctrl\n", line.c_str());
  // exactly one newline, at the end
  TEST_ASSERT_EQUAL_INT(1, (int)std::count(line.begin(), line.end(), '\n'));
  TEST_ASSERT_EQUAL_CHAR('\n', line.back());
}

static void test_line_length_capped() {
  std::string huge(5000, 'x');
  std::string line = formatLine(1, 1, "mem", huge);
  TEST_ASSERT_TRUE(line.size() <= kMaxLineBytes);
  TEST_ASSERT_EQUAL_CHAR('\n', line.back());
}

// ---- rotation policy --------------------------------------------------------

static void test_should_rotate() {
  TierCaps c = capsFor(false);   // flash: 24 KB/file
  TEST_ASSERT_FALSE(shouldRotate(0, 5000, c));            // empty file never rotates
  TEST_ASSERT_FALSE(shouldRotate(100, 200, c));           // well under cap
  TEST_ASSERT_TRUE(shouldRotate(c.maxFileBytes - 10, 100, c));  // would exceed
  TEST_ASSERT_TRUE(shouldRotate(c.maxFileBytes, 1, c));
}

static void test_tier_caps_bounded() {
  TEST_ASSERT_TRUE(capsFor(true).maxFileBytes > capsFor(false).maxFileBytes);   // SD generous
  TEST_ASSERT_TRUE(capsFor(false).maxFileBytes <= 32768);   // flash fallback stays small
  TEST_ASSERT_TRUE(capsFor(false).maxFiles >= 2);
}

static void test_rotated_names() {
  TEST_ASSERT_EQUAL_STRING("nimbus.log", rotatedName(0).c_str());
  TEST_ASSERT_EQUAL_STRING("nimbus.log.1", rotatedName(1).c_str());
  TEST_ASSERT_EQUAL_STRING("nimbus.log.3", rotatedName(3).c_str());
  TEST_ASSERT_EQUAL_STRING("/log/nimbus.log.2", pathFor(rotatedName(2)).c_str());
}

// ---- line-aligned tail ------------------------------------------------------

static void test_tail_short_returns_all() {
  TEST_ASSERT_EQUAL_STRING("abc\n", tailOf("abc\n", 100).c_str());
}

static void test_tail_aligns_to_line() {
  // 3 lines; ask for a byte budget that lands mid-line-1 -> should start at line 2.
  std::string content = "line-one\nline-two\nline-three\n";
  std::string t = tailOf(content, 20);   // 20 < full; must not start mid-"line-one"
  TEST_ASSERT_TRUE(t.find("line-one") == std::string::npos);   // partial first line dropped
  TEST_ASSERT_TRUE(t.find("line-three") != std::string::npos); // newest kept
  TEST_ASSERT_EQUAL_CHAR('\n', t.back());                      // ends on a line boundary
}

// ---- retrieval planning + path-traversal safety -----------------------------

static void test_known_log_names() {
  const size_t maxFiles = kSdCaps.maxFiles;   // 4 -> valid rotated indices 1..3
  TEST_ASSERT_TRUE(isKnownLogName("nimbus.log", maxFiles));
  TEST_ASSERT_TRUE(isKnownLogName("nimbus.log.1", maxFiles));
  TEST_ASSERT_TRUE(isKnownLogName("nimbus.log.3", maxFiles));
  TEST_ASSERT_FALSE(isKnownLogName("nimbus.log.0", maxFiles));   // .0 is the active alias
  TEST_ASSERT_FALSE(isKnownLogName("nimbus.log.4", maxFiles));   // beyond retention
  TEST_ASSERT_FALSE(isKnownLogName("nimbus.log.x", maxFiles));
  TEST_ASSERT_FALSE(isKnownLogName("other.log", maxFiles));
  TEST_ASSERT_FALSE(isKnownLogName("", maxFiles));
}

static void test_path_traversal_rejected() {
  const size_t maxFiles = kSdCaps.maxFiles;
  TEST_ASSERT_FALSE(isKnownLogName("../secrets", maxFiles));
  TEST_ASSERT_FALSE(isKnownLogName("/etc/passwd", maxFiles));
  TEST_ASSERT_FALSE(isKnownLogName("nimbus.log/../x", maxFiles));
  TEST_ASSERT_FALSE(isKnownLogName("..", maxFiles));
  TEST_ASSERT_FALSE(isKnownLogName("nimbus.log.1/..", maxFiles));
  TEST_ASSERT_FALSE(isKnownLogName("nimbus.log\\1", maxFiles));
}

static void test_plan_retrieval() {
  const size_t maxFiles = kSdCaps.maxFiles;
  TEST_ASSERT_EQUAL(RetrievalKind::List,    planRetrieval(true,  "", maxFiles).kind);
  TEST_ASSERT_EQUAL(RetrievalKind::Current, planRetrieval(false, "", maxFiles).kind);
  RetrievalPlan f = planRetrieval(false, "nimbus.log.2", maxFiles);
  TEST_ASSERT_EQUAL(RetrievalKind::File, f.kind);
  TEST_ASSERT_EQUAL_STRING("nimbus.log.2", f.file.c_str());
  RetrievalPlan bad = planRetrieval(false, "../secrets", maxFiles);
  TEST_ASSERT_EQUAL(RetrievalKind::Reject, bad.kind);
}

// ---- the load-bearing one: no secret can reach the durable log --------------
//
// Reproduce EXACTLY the sink composition agent_log.h uses: one redaction pass via
// core::emitRedacted, whose ring sink both stores to the RAM ring AND (in the
// device) hands the same string to errlog::append. Here the "durable" sink formats
// the line the way errlog does on disk, and we assert the on-disk line carries no
// secret. This is what proves the durable log inherits the redaction with no bypass.
static void test_durable_sink_never_sees_a_secret() {
  std::vector<std::string> secrets = {"sk-abcdef0123456789", "6789:AAtelegramBotToken"};
  std::string raw =
      "provider mistral HTTP 401: {\"error\":\"invalid key sk-abcdef0123456789\"} "
      "Authorization: Bearer eyJraw.token.sig api_key=live_9f8e7d6c5b4a";

  std::string ringLine, durableLine;
  core::emitRedacted(
      raw.c_str(), secrets,
      [](const std::string&) {},                       // serial sink (unused here)
      [&](const std::string& red) {                    // ring + durable, as agent_log does
        ringLine = red;
        durableLine = formatLine(1, 100, cat::kProvider, red);   // what errlog writes on disk
      });

  // The durable line is the redacted line (same object the ring got), formatted.
  TEST_ASSERT_TRUE(durableLine.find(ringLine) != std::string::npos);
  // None of the secrets survive into the durable line.
  TEST_ASSERT_TRUE(durableLine.find("sk-abcdef0123456789") == std::string::npos);
  TEST_ASSERT_TRUE(durableLine.find("6789:AAtelegramBotToken") == std::string::npos);
  TEST_ASSERT_TRUE(durableLine.find("eyJraw.token.sig") == std::string::npos);
  TEST_ASSERT_TRUE(durableLine.find("live_9f8e7d6c5b4a") == std::string::npos);
  // And redaction actually happened.
  TEST_ASSERT_TRUE(durableLine.find("***") != std::string::npos);
}

// ---- the write engine: self-heal, rotation/retention, size-sync -------------
//
// A fake filesystem that models the two boot-order traps the device hits: it starts
// UNMOUNTED (every op fails, like LittleFS before memory::begin()), and it refuses
// to create files under /log until the dir exists (like LittleFS/SD, which do not
// auto-create parent dirs). These are exactly the conditions that made the flash
// fallback silently never persist (the review's Finding 1).
struct FakeFs {
  bool mounted = false;
  std::map<std::string, std::string> files;
  std::set<std::string> dirs;

  long fileSize(const std::string& p) {
    if (!mounted) return -1;
    auto it = files.find(p);
    return it == files.end() ? -1 : (long)it->second.size();
  }
  size_t appendFile(const std::string& p, const char* d, size_t n) {
    if (!mounted) return 0;
    if (dirs.find(kDir) == dirs.end()) return 0;   // parent dir must exist first
    files[p].append(d, n);
    return n;
  }
  void makeDir(const std::string& p)    { if (mounted) dirs.insert(p); }
  void removeFile(const std::string& p) { files.erase(p); }
  void renameFile(const std::string& a, const std::string& b) {
    auto it = files.find(a);
    if (it != files.end()) { files[b] = it->second; files.erase(it); }
  }
};

static size_t totalBytes(const FakeFs& fs) {
  size_t t = 0;
  for (auto& kv : fs.files) t += kv.second.size();
  return t;
}

// Finding-1 regression: on the flash tier, an FS that is not mounted at the first
// write must NOT lose durability once it mounts. Early writes fail soft (RAM only);
// the first write after mount self-heals (creates /log, then the file).
static void test_selfheal_on_late_mount() {
  FakeFs fs;                       // starts unmounted
  LogWriter<FakeFs> w;
  w.setTier(&fs, false);           // flash tier
  TEST_ASSERT_FALSE(w.write("1 0 - before-mount\n"));   // FS down -> not persisted
  TEST_ASSERT_TRUE(fs.files.empty());
  fs.mounted = true;                                    // memory::begin() mounts LittleFS later
  TEST_ASSERT_TRUE(w.write("2 0 - after-mount\n"));     // self-heals with no re-init
  TEST_ASSERT_EQUAL_INT(1, (int)fs.dirs.count(kDir));   // /log was created on retry
  TEST_ASSERT_EQUAL_STRING("2 0 - after-mount\n",
                           fs.files[pathFor(kBaseName)].c_str());
}

// Fresh mounted FS with no /log yet (first-ever boot): the mkdir-then-retry path
// must create the dir and persist.
static void test_fresh_fs_creates_dir_and_persists() {
  FakeFs fs; fs.mounted = true;    // mounted, but /log absent
  LogWriter<FakeFs> w;
  w.setTier(&fs, false);
  TEST_ASSERT_TRUE(w.write("1 0 - x\n"));
  TEST_ASSERT_EQUAL_INT(1, (int)fs.dirs.count(kDir));
  TEST_ASSERT_EQUAL_STRING("1 0 - x\n", fs.files[pathFor(kBaseName)].c_str());
}

// Rotation + retention on the flash tier (24 KB x 2): total on-disk bytes stay
// bounded and no file beyond the retention count is ever created.
static void test_rotation_retention_flash() {
  FakeFs fs; fs.mounted = true; fs.dirs.insert(kDir);
  LogWriter<FakeFs> w;
  w.setTier(&fs, false);
  std::string line(1000, 'x'); line.back() = '\n';
  for (int i = 0; i < 60; ++i) w.write(line);           // 60 KB >> 24 KB cap
  TEST_ASSERT_EQUAL_INT(1, (int)fs.files.count(pathFor(kBaseName)));      // active
  TEST_ASSERT_EQUAL_INT(1, (int)fs.files.count(pathFor(rotatedName(1)))); // one rotated
  TEST_ASSERT_EQUAL_INT(0, (int)fs.files.count(pathFor(rotatedName(2)))); // retention = 2 files
  TEST_ASSERT_TRUE(totalBytes(fs) <=
                   kFlashCaps.maxFileBytes * kFlashCaps.maxFiles + 1000);
}

// SD tier keeps 4 files (active + .1 + .2 + .3); .4 is never kept.
static void test_rotation_retention_sd() {
  FakeFs fs; fs.mounted = true; fs.dirs.insert(kDir);
  LogWriter<FakeFs> w;
  w.setTier(&fs, true);
  std::string line(4096, 'q'); line.back() = '\n';
  for (int i = 0; i < 64 * 5; ++i) w.write(line);       // ~1.25 MB, several rotations
  TEST_ASSERT_EQUAL_INT(1, (int)fs.files.count(pathFor(rotatedName(3))));
  TEST_ASSERT_EQUAL_INT(0, (int)fs.files.count(pathFor(rotatedName(4))));
  TEST_ASSERT_TRUE(totalBytes(fs) <=
                   kSdCaps.maxFileBytes * kSdCaps.maxFiles + 4096);
}

// Size-sync: a file left over from a PRIOR boot must be counted, so the first write
// after boot rotates against the real size (not a stale 0). If size were 0, 20 KB +
// 6 KB would not rotate on the 24 KB flash cap; it must.
static void test_size_sync_from_existing_file() {
  FakeFs fs; fs.mounted = true; fs.dirs.insert(kDir);
  fs.files[pathFor(kBaseName)] = std::string(20000, 'y');   // prior-boot contents
  LogWriter<FakeFs> w;
  w.setTier(&fs, false);                                    // flash: 24 KB cap
  std::string line(6000, 'z'); line.back() = '\n';
  TEST_ASSERT_TRUE(w.write(line));
  TEST_ASSERT_EQUAL_UINT(20000, (unsigned)fs.files[pathFor(rotatedName(1))].size());
  TEST_ASSERT_EQUAL_UINT(6000,  (unsigned)fs.files[pathFor(kBaseName)].size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_format_line_shape);
  RUN_TEST(test_format_line_null_cat_is_dash);
  RUN_TEST(test_sanitize_strips_control_and_newlines);
  RUN_TEST(test_line_length_capped);
  RUN_TEST(test_should_rotate);
  RUN_TEST(test_tier_caps_bounded);
  RUN_TEST(test_rotated_names);
  RUN_TEST(test_tail_short_returns_all);
  RUN_TEST(test_tail_aligns_to_line);
  RUN_TEST(test_known_log_names);
  RUN_TEST(test_path_traversal_rejected);
  RUN_TEST(test_plan_retrieval);
  RUN_TEST(test_durable_sink_never_sees_a_secret);
  RUN_TEST(test_selfheal_on_late_mount);
  RUN_TEST(test_fresh_fs_creates_dir_and_persists);
  RUN_TEST(test_rotation_retention_flash);
  RUN_TEST(test_rotation_retention_sd);
  RUN_TEST(test_size_sync_from_existing_file);
  return UNITY_END();
}
