#include <unity.h>

#include <algorithm>
#include <cstdio>
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

// Tier-correct caps (the retrieval route passes capsFor(onSdTier()).maxFiles): on the
// flash tier (2 files) only nimbus.log.1 is valid; .2/.3 are rejected as bad names, not
// served as 404s. On the SD tier (4 files) .1..3 are valid.
static void test_plan_retrieval_respects_active_tier() {
  const size_t flash = capsFor(false).maxFiles;   // 2 -> valid rotated index is just 1
  const size_t sd    = capsFor(true).maxFiles;     // 4 -> valid rotated indices 1..3
  TEST_ASSERT_EQUAL(RetrievalKind::File,   planRetrieval(false, "nimbus.log.1", flash).kind);
  TEST_ASSERT_EQUAL(RetrievalKind::Reject, planRetrieval(false, "nimbus.log.2", flash).kind);
  TEST_ASSERT_EQUAL(RetrievalKind::Reject, planRetrieval(false, "nimbus.log.3", flash).kind);
  TEST_ASSERT_EQUAL(RetrievalKind::File,   planRetrieval(false, "nimbus.log.2", sd).kind);
  TEST_ASSERT_EQUAL(RetrievalKind::File,   planRetrieval(false, "nimbus.log.3", sd).kind);
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
      "Authorization: Bearer eyJraw.token.sig api_key=live_9f8e7d6c5b4a";  // gitleaks:allow - audited fake fixture; this test asserts emitRedacted MASKS it

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

// ---- the write engine: one held handle, rotation, degrade ---------------------
//
// A fake filesystem that models what the device hits: it starts UNMOUNTED (every op
// fails, like LittleFS before memory::begin()), it refuses to create files under /log
// until the dir exists (neither LittleFS nor SD auto-creates parent dirs), and it
// counts every open of the held log handle so the one-handle contract is measured.
struct FakeFs {
  bool mounted = false;
  bool failOpen = false;    // openAppend fails (card gone, FS full)
  bool failWrite = false;   // writes on the held handle fail (stale handle)
  std::map<std::string, std::string> files;
  std::set<std::string> dirs;
  bool held = false;
  std::string heldPath;
  int opens = 0;            // successful openAppend calls
  int openCalls = 0;        // every openAppend call
  int heldMisuse = 0;       // rename/remove of the file while it is held open
  int flushes = 0;          // fsyncs of the held file
  int writesLeft = -1;      // writes that succeed before every write fails (-1 = no limit)

  bool openAppend(const std::string& p) {
    ++openCalls;
    if (!mounted || failOpen || dirs.find(kDir) == dirs.end()) return false;
    held = true;
    heldPath = p;
    files[p];   // create if absent
    ++opens;
    return true;
  }
  long openSize() { return held ? (long)files[heldPath].size() : -1; }
  size_t writeOpen(const char* d, size_t n) {
    if (!held || !mounted || failWrite || writesLeft == 0) return 0;
    if (writesLeft > 0) --writesLeft;
    files[heldPath].append(d, n);
    return n;
  }
  void flushOpen() { if (held) ++flushes; }
  void closeOpen() { held = false; }
  void makeDir(const std::string& p) { if (mounted) dirs.insert(p); }
  void removeFile(const std::string& p) {
    if (held && p == heldPath) ++heldMisuse;
    files.erase(p);
  }
  void renameFile(const std::string& a, const std::string& b) {
    if (held && (a == heldPath || b == heldPath)) ++heldMisuse;
    auto it = files.find(a);
    if (it != files.end()) { files[b] = it->second; files.erase(it); }
  }
  std::string readSmall(const std::string& p, size_t maxBytes) {
    if (!mounted) return {};
    auto it = files.find(p);
    if (it == files.end()) return {};
    return it->second.substr(0, maxBytes);
  }
  bool writeSmall(const std::string& p, const std::string& data) {
    if (!mounted || dirs.find(kDir) == dirs.end()) return false;
    files[p] = data;
    return true;
  }
  const std::string& log() { return files[pathFor(kBaseName)]; }
};

static size_t totalLogBytes(FakeFs& fs) {
  size_t t = 0;
  for (auto& kv : fs.files)
    if (kv.first.find(std::string(kDir) + "/" + kBaseName) == 0) t += kv.second.size();
  return t;
}

static std::vector<std::string> linesOf(const std::string& s) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) nl = s.size();
    out.push_back(s.substr(pos, nl - pos));
    pos = nl + 1;
  }
  return out;
}

// Exact matches of a formatted line (with its '\n') among the file's lines.
static size_t countExact(const std::vector<std::string>& lines, const std::string& formatted) {
  size_t n = 0;
  for (const auto& l : lines)
    if (l + "\n" == formatted) ++n;
  return n;
}

static size_t countLinesWith(const std::string& s, const std::string& needle) {
  size_t n = 0;
  for (const auto& l : linesOf(s))
    if (l.find(needle) != std::string::npos) ++n;
  return n;
}

// The LineStamp source the durable log uses for the lines it formats itself (the tier
// line, markers). Monotonic like the device's g_seq.
static uint32_t g_testSeq = 0;
static LineStamp testStamp() { return LineStamp{g_testSeq++, 0}; }

static std::string numbered(uint32_t i, const char* tag = "line") {
  return formatLine(g_testSeq++, i, "-", std::string(tag) + "-" + std::to_string(i));
}

// The inverted Finding-1 pin (crash forensics F1): a line emitted before the FS is
// mounted is NOT lost - it waits in the early backlog and lands on disk, in order,
// before the first after-mount line.
static void test_selfheal_on_late_mount() {
  FakeFs fs;                       // starts unmounted
  DurableLog<FakeFs> d(testStamp);
  TEST_ASSERT_TRUE(d.append("1 0 - before-mount\n", 0));   // queued, not dropped
  TEST_ASSERT_TRUE(fs.files.empty());
  fs.mounted = true;                                        // memory::begin() mounts LittleFS
  d.attach(&fs, false, 0);
  TEST_ASSERT_TRUE(d.append("2 0 - after-mount\n", 0));
  TEST_ASSERT_EQUAL_INT(1, (int)fs.dirs.count(kDir));       // /log was created
  const std::string log = fs.log();
  const size_t before = log.find("before-mount"), after = log.find("after-mount");
  TEST_ASSERT_TRUE(before != std::string::npos);
  TEST_ASSERT_TRUE(after != std::string::npos);
  TEST_ASSERT_TRUE(before < after);
}

// One case of the property below: k lines before the attach, n - k after.
static void checkNoLineLost(bool sd, uint32_t n, uint32_t k) {
  FakeFs fs;
  g_testSeq = 0;
  DurableLog<FakeFs> d(testStamp);
  std::vector<std::string> pre, post;
  for (uint32_t i = 0; i < k; ++i) {
    pre.push_back(numbered(i, "pre"));
    d.append(pre.back(), 0);
  }
  fs.mounted = true;
  d.attach(&fs, sd, 0);
  for (uint32_t i = k; i < n; ++i) {
    post.push_back(numbered(i, "post"));
    d.append(post.back(), 0);
  }
  const std::vector<std::string> lines = linesOf(fs.log());
  const size_t kept = k < EarlyBacklog::kMaxLines ? k : EarlyBacklog::kMaxLines;
  // Exactly the first `kept` pre lines, in order, first in the file; the rest dropped.
  TEST_ASSERT_TRUE(lines.size() >= kept);
  for (size_t i = 0; i < pre.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT(i < kept ? 1 : 0, (unsigned)countExact(lines, pre[i]));
    if (i < kept) TEST_ASSERT_EQUAL_STRING(pre[i].substr(0, pre[i].size() - 1).c_str(), lines[i].c_str());
  }
  // Past the cap: exactly one marker, right after the kept lines, with the exact count.
  const size_t markers = countLinesWith(fs.log(), "early line(s) dropped");
  TEST_ASSERT_EQUAL_UINT(k > EarlyBacklog::kMaxLines ? 1 : 0, (unsigned)markers);
  if (markers) {
    const std::string want = std::to_string(k - EarlyBacklog::kMaxLines) + " early line(s)";
    TEST_ASSERT_TRUE(lines[kept].find(want) != std::string::npos);
  }
  // Every post line exactly once, after every pre line.
  for (const auto& p : post) {
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)countExact(lines, p));
    for (size_t i = 0; i < kept && i < lines.size(); ++i) TEST_ASSERT_TRUE(lines[i] + "\n" != p);
  }
  // The backlog is freed once flushed: no resident memory after boot.
  TEST_ASSERT_TRUE(d.backlog().closed());
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)d.backlog().bytes());
}

// The class (T1): over N pre-attach lines x every attach point k x both tiers, every
// pre-attach line lands exactly once, ascending, before any post-attach line; past the
// cap, exactly the cap plus ONE marker carrying the exact drop count.
static void test_no_line_lost_before_mount() {
  for (int sd = 0; sd < 2; ++sd)
    for (uint32_t n = 0; n <= 30; ++n)
      for (uint32_t k = 0; k <= n; ++k) checkNoLineLost(sd == 1, n, k);
}

// The byte bound holds as well as the line bound: a few long lines fill 4 KB first.
static void test_backlog_byte_bound() {
  EarlyBacklog b;
  const std::string big(1000, 'x');
  size_t queued = 0;
  for (int i = 0; i < 10; ++i) queued += b.push(big) ? 1 : 0;
  TEST_ASSERT_EQUAL_UINT(4, (unsigned)queued);           // 4 x 1000 <= 4096 < 5 x 1000
  TEST_ASSERT_EQUAL_UINT(6, b.dropped());
  TEST_ASSERT_TRUE(b.bytes() <= EarlyBacklog::kMaxBytes);
  b.release();
  TEST_ASSERT_TRUE(b.closed());
  TEST_ASSERT_FALSE(b.push("late\n"));                   // closed: refused
}

// Notifier without a card: storage never attaches, the backlog is freed at the end of
// setup and later lines are refused (they live in the RAM tail only).
static void test_unattached_backlog_released() {
  DurableLog<FakeFs> d(testStamp);
  for (int i = 0; i < 5; ++i) d.append(numbered(i), 0);
  TEST_ASSERT_TRUE(d.backlog().bytes() > 0);
  d.releaseBacklog();
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)d.backlog().bytes());
  TEST_ASSERT_FALSE(d.append(numbered(9), 0));
}

// A normal boot attaches the tier once (SD at its mount, or flash at memory::begin()),
// and a later re-attach of the SAME tier is a remount, never a flip: no spurious
// "SD available" line (the old seq 7 line that every boot printed).
static void test_no_spurious_retier_on_normal_boot() {
  for (int sd = 0; sd < 2; ++sd) {
    FakeFs fs; fs.mounted = true;
    DurableLog<FakeFs> d(testStamp);
    d.append(numbered(0), 0);
    d.attach(&fs, sd == 1, 0);
    d.attach(&fs, sd == 1, 0);   // memory::begin() resolving the same tier
    d.append(numbered(1), 0);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)countLinesWith(fs.log(), "SD available"));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)countLinesWith(fs.log(), "SD lost"));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(fs.log(), sd ? "durable log on SD" : "durable log on flash"));
  }
}

// Counter-test for the guard above: a REAL mid-run flip still logs both lines.
static void test_real_sd_flip_logs_both_lines() {
  FakeFs card; card.mounted = true;
  FakeFs flash; flash.mounted = true;
  DurableLog<FakeFs> d(testStamp);
  d.attach(&card, true, 0);
  d.append(numbered(0), 0);
  d.attach(&flash, false, 0);   // card demoted
  d.append(numbered(1), 0);
  d.attach(&card, true, 0);     // card back
  d.append(numbered(2), 0);
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(flash.log(), "SD lost: durable log now on flash"));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(card.log(), "SD available: durable log now on SD"));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(flash.log(), "line-1"));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(card.log(), "line-2"));
}

// Fresh device (AGENTS.md section 3): no /log at all. The attach creates it, writes the
// boot counter and persists every queued line.
static void test_fresh_device_without_log_dir() {
  FakeFs fs; fs.mounted = true;   // mounted, /log absent
  DurableLog<FakeFs> d(testStamp);
  d.append(formatLine(0, 0, "boot", "boot: n=? fw=v reason=power-on(0x01)"), 0);
  d.attach(&fs, true, 0);
  TEST_ASSERT_EQUAL_INT(1, (int)fs.dirs.count(kDir));
  TEST_ASSERT_EQUAL_UINT(1, d.bootNo());
  TEST_ASSERT_EQUAL_STRING("1\n", fs.files[pathFor(kBootCounterName)].c_str());
  TEST_ASSERT_TRUE(fs.log().find("boot: n=1 fw=v") != std::string::npos);
  TEST_ASSERT_TRUE(fs.log().find("n=?") == std::string::npos);
}

// The boot counter climbs one per boot, survives garbage, and is stamped only into the
// boot record (the placeholder in any other line is left alone).
static void test_boot_counter_per_boot() {
  FakeFs fs; fs.mounted = true;
  for (uint32_t boot = 1; boot <= 3; ++boot) {
    DurableLog<FakeFs> d(testStamp);
    d.append(formatLine(0, 0, "boot", "boot: n=? fw=v"), 0);
    d.attach(&fs, false, 0);
    TEST_ASSERT_EQUAL_UINT(boot, d.bootNo());
  }
  TEST_ASSERT_EQUAL_UINT(3, (unsigned)countLinesWith(fs.log(), "boot: n="));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(fs.log(), "boot: n=3 "));
  fs.files[pathFor(kBootCounterName)] = "garbage";
  DurableLog<FakeFs> d(testStamp);
  d.attach(&fs, false, 0);
  TEST_ASSERT_EQUAL_UINT(1, d.bootNo());
}

// Rotation + retention on the flash tier (24 KB x 2): total on-disk bytes stay
// bounded, no file beyond the retention count is created, and the held file is never
// renamed or removed while open.
static void test_rotation_retention_flash() {
  FakeFs fs; fs.mounted = true;
  LogWriter<FakeFs> w;
  w.setTier(&fs, false);
  std::string line(1000, 'x'); line.back() = '\n';
  for (int i = 0; i < 60; ++i) w.write(line, 0);         // 60 KB >> 24 KB cap
  TEST_ASSERT_EQUAL_INT(1, (int)fs.files.count(pathFor(kBaseName)));      // active
  TEST_ASSERT_EQUAL_INT(1, (int)fs.files.count(pathFor(rotatedName(1)))); // one rotated
  TEST_ASSERT_EQUAL_INT(0, (int)fs.files.count(pathFor(rotatedName(2)))); // retention = 2 files
  TEST_ASSERT_TRUE(totalLogBytes(fs) <= kFlashCaps.maxFileBytes * kFlashCaps.maxFiles + 1000);
  TEST_ASSERT_EQUAL_INT(0, fs.heldMisuse);
}

// SD tier keeps 4 files (active + .1 + .2 + .3); .4 is never kept.
static void test_rotation_retention_sd() {
  FakeFs fs; fs.mounted = true;
  LogWriter<FakeFs> w;
  w.setTier(&fs, true);
  std::string line(4096, 'q'); line.back() = '\n';
  for (int i = 0; i < 64 * 5; ++i) w.write(line, 0);     // ~1.25 MB, several rotations
  TEST_ASSERT_EQUAL_INT(1, (int)fs.files.count(pathFor(rotatedName(3))));
  TEST_ASSERT_EQUAL_INT(0, (int)fs.files.count(pathFor(rotatedName(4))));
  TEST_ASSERT_TRUE(totalLogBytes(fs) <= kSdCaps.maxFileBytes * kSdCaps.maxFiles + 4096);
  TEST_ASSERT_EQUAL_INT(0, fs.heldMisuse);
}

// Size-sync: a file left over from a PRIOR boot is counted (read from the held handle
// on open), so the first write after boot rotates against the real size.
static void test_size_sync_from_existing_file() {
  FakeFs fs; fs.mounted = true; fs.dirs.insert(kDir);
  fs.files[pathFor(kBaseName)] = std::string(20000, 'y');   // prior-boot contents
  LogWriter<FakeFs> w;
  w.setTier(&fs, false);                                    // flash: 24 KB cap
  std::string line(6000, 'z'); line.back() = '\n';
  TEST_ASSERT_TRUE(w.write(line, 0));
  TEST_ASSERT_EQUAL_UINT(20000, (unsigned)fs.files[pathFor(rotatedName(1))].size());
  TEST_ASSERT_EQUAL_UINT(6000,  (unsigned)fs.files[pathFor(kBaseName)].size());
}

// The one-handle contract (panic F1.1), measured over random runs: lines of random
// size, random remounts (re-attach of the same tier) and the rotations they cause.
// Opens never exceed rotations + remounts + 1 - never one per line.
static void test_opens_bounded_by_rotations_and_remounts() {
  uint32_t rng = 12345;
  auto next = [&rng]() { rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xFFFFFF; };
  for (int run = 0; run < 40; ++run) {
    FakeFs fs; fs.mounted = true;
    DurableLog<FakeFs> d(testStamp);
    const bool sd = (run & 1) != 0;
    d.attach(&fs, sd, 0);
    const int opensAtAttach = fs.opens;   // boot attach: tier line opens the file once
    int remounts = 0;
    const int lines = 1 + (int)(next() % 5000);
    for (int i = 0; i < lines; ++i) {
      if (next() % 997 == 0) { d.attach(&fs, sd, 0); ++remounts; }
      std::string msg((size_t)(1 + next() % 300), 'm');
      d.append(formatLine((uint32_t)i, 0, "-", msg), (uint32_t)i);
    }
    const int rotations = (int)d.writer().rotations();
    TEST_ASSERT_EQUAL_INT(1, opensAtAttach);
    TEST_ASSERT_TRUE(fs.opens <= rotations + remounts + 1);
    TEST_ASSERT_TRUE(fs.opens >= 1);
    TEST_ASSERT_EQUAL_INT(fs.opens, (int)d.writer().opens());
    TEST_ASSERT_EQUAL_INT(0, fs.heldMisuse);
  }
}

// Degrade, not per-line retry: when the file cannot be opened the engine tries at most
// once per kReopenRetryMs, the lines in between are counted, and the first line after
// recovery is followed by one marker carrying the exact missed count.
static void test_open_failure_degrades_then_recovers() {
  FakeFs fs; fs.mounted = true; fs.failOpen = true;
  DurableLog<FakeFs> d(testStamp);
  d.attach(&fs, true, 0);   // the tier line's open fails
  const uint32_t step = 100;
  uint32_t t = 0;
  for (int i = 0; i < 200; ++i, t += step) d.append(numbered((uint32_t)i), t);   // 20 s of lines
  const uint32_t window = LogWriter<FakeFs>::kReopenRetryMs;
  // One attempt at attach, then at most one per retry window.
  TEST_ASSERT_TRUE(fs.openCalls <= 1 + (int)(t / window) + 1);
  TEST_ASSERT_TRUE(fs.openCalls < 10);
  fs.failOpen = false;
  t += window;
  d.append(numbered(999, "back"), t);
  const std::string log = fs.log();
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(log, "back-999"));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(log, "while the log file could not be opened"));
  // 1 tier line + 200 lines were missed while the file could not be opened.
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(log, "missed 201 line(s)"));
}

// The missed-lines marker itself can fail (the card flakes right after a line landed):
// the count is put back and reported by the next marker, never lost.
static void test_missed_marker_failure_keeps_the_count() {
  FakeFs fs; fs.mounted = true; fs.failOpen = true;
  DurableLog<FakeFs> d(testStamp);
  d.attach(&fs, true, 0);                                       // the tier line is missed (1)
  for (uint32_t i = 0; i < 4; ++i) d.append(numbered(i), 0);    // 4 more missed
  fs.failOpen = false;
  fs.writesLeft = 1;                     // the next line lands, then every write fails
  const uint32_t t = LogWriter<FakeFs>::kReopenRetryMs;
  d.append(numbered(100, "lands"), t);
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(fs.log(), "lands-100"));
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)countLinesWith(fs.log(), "missed"));   // the marker failed
  fs.writesLeft = -1;                    // the card recovers
  d.append(numbered(101, "back"), 2 * t + 1);
  // The five early misses survive the failed marker; nothing is double counted.
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)countLinesWith(fs.log(), "missed 5 line(s)"));
}

// A write error (a stale handle after a remount) reopens once and retries the line, so a
// single error costs one open, not a lost line.
static void test_write_error_reopens_once() {
  FakeFs fs; fs.mounted = true;
  LogWriter<FakeFs> w;
  w.setTier(&fs, true);
  TEST_ASSERT_TRUE(w.write("1 0 - a\n", 0));
  TEST_ASSERT_EQUAL_INT(1, fs.opens);
  fs.failWrite = true;
  TEST_ASSERT_FALSE(w.write("2 0 - b\n", 1));   // reopen + retry also fail -> degraded
  TEST_ASSERT_EQUAL_INT(2, fs.opens);
  fs.failWrite = false;
  TEST_ASSERT_FALSE(w.write("3 0 - c\n", 2));   // inside the retry window: no open at all
  TEST_ASSERT_EQUAL_INT(2, fs.openCalls);
  TEST_ASSERT_TRUE(w.write("4 0 - d\n", 2 + LogWriter<FakeFs>::kReopenRetryMs));
  TEST_ASSERT_EQUAL_INT(3, fs.opens);
  TEST_ASSERT_EQUAL_UINT(2, w.takeUnwritten());
}

// The per-line path never opens a file (panic F1.1): the device adapter's writeOpen
// body in errlog_fs.cpp must contain no open( call. Source guard, so an edit that
// reintroduces open-per-line fails here, not as a field panic.
static std::string readSource(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static std::string methodBody(const std::string& src, const char* signature) {
  const size_t at = src.find(signature);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, signature);
  const size_t open = src.find('{', at);
  const size_t end = src.find("\n  }", open);
  TEST_ASSERT_TRUE(open != std::string::npos && end != std::string::npos);
  return src.substr(open, end - open);
}

static void test_source_per_line_path_has_no_open() {
  const std::string src = readSource("src/sys/errlog_fs.cpp");
  const std::string write = methodBody(src, "size_t writeOpen(");
  const std::string flush = methodBody(src, "void flushOpen(");
  TEST_ASSERT_TRUE_MESSAGE(write.find("open(") == std::string::npos, "the per-line write opens a file");
  TEST_ASSERT_TRUE_MESSAGE(flush.find("open(") == std::string::npos, "the per-line flush opens a file");
  TEST_ASSERT_TRUE(flush.find("flush()") != std::string::npos);   // fsync per line stays
  // The tier is followed only once the memory subsystem resolved it (no spurious flip).
  TEST_ASSERT_TRUE(src.find("storageResolved()") != std::string::npos);
}

// Every line written outside a batch is fsynced (a line before a panic is on the card);
// the boot backlog is written with ONE fsync, so the attach adds no per-line syncs.
static void test_flush_per_line_and_one_for_the_backlog() {
  FakeFs fs; fs.mounted = true;
  DurableLog<FakeFs> d(testStamp);
  for (uint32_t i = 0; i < 10; ++i) d.append(numbered(i, "early"), 0);
  d.attach(&fs, true, 0);
  // 10 backlog lines in one batch (1 flush) + the tier line (1 flush).
  TEST_ASSERT_EQUAL_INT(2, fs.flushes);
  for (uint32_t i = 0; i < 5; ++i) d.append(numbered(i, "late"), 0);
  TEST_ASSERT_EQUAL_INT(7, fs.flushes);
}

// ---- CUM-409: durable persistence is scoped to warn/error + key events ------
//
// The predicate agent_log.h's logPersist() gates the durable write on. Info stays
// RAM-only; warn/error persist; a category tag is a key event that persists even at
// info level.
static void test_persists_durably_scopes_to_warn_error_and_tags() {
  TEST_ASSERT_FALSE(persistsDurably(Level::Info, nullptr));   // routine info -> RAM only
  TEST_ASSERT_FALSE(persistsDurably(Level::Info, ""));        // empty tag is no tag
  TEST_ASSERT_TRUE(persistsDurably(Level::Info, cat::kNvs));  // info + key-event tag -> durable
  TEST_ASSERT_TRUE(persistsDurably(Level::Warn, nullptr));    // warn -> durable
  TEST_ASSERT_TRUE(persistsDurably(Level::Error, nullptr));   // error -> durable
}

// The class test TASK.md names: a BURST of info lines produces ZERO durable writes;
// a warn line produces EXACTLY ONE. Routed through the same predicate the device uses,
// against the fake FS, so the count is real (not a point assertion on one call).
static void test_info_burst_zero_durable_warn_one() {
  FakeFs fs; fs.mounted = true; fs.dirs.insert(kDir);
  LogWriter<FakeFs> w; w.setTier(&fs, false);
  int durableWrites = 0;
  auto route = [&](Level lvl, const char* cat, const std::string& msg) {
    if (persistsDurably(lvl, cat) && w.write(formatLine(0, 0, cat, msg), 0)) ++durableWrites;
  };
  for (int i = 0; i < 64; ++i) route(Level::Info, nullptr, "routine info line");
  TEST_ASSERT_EQUAL_INT(0, durableWrites);              // burst of info -> nothing durable
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)w.bytesWritten());// and zero flash wear
  route(Level::Warn, nullptr, "a warning");
  TEST_ASSERT_EQUAL_INT(1, durableWrites);              // one warn -> exactly one durable write
  TEST_ASSERT_TRUE(w.bytesWritten() > 0);
  route(Level::Error, nullptr, "an error");
  route(Level::Info, cat::kProvider, "provider fell back");  // key event, durable even at info
  TEST_ASSERT_EQUAL_INT(3, durableWrites);
}

// CUM-409 flash-wear counter: bytes written to durable equals the sum of the lines
// actually persisted (a rotation renames, it never re-appends).
static void test_wear_counter_tracks_bytes_written() {
  FakeFs fs; fs.mounted = true; fs.dirs.insert(kDir);
  LogWriter<FakeFs> w; w.setTier(&fs, false);
  const std::string a = "1 0 - alpha\n", b = "2 0 - bravo\n";
  TEST_ASSERT_TRUE(w.write(a, 0));
  TEST_ASSERT_TRUE(w.write(b, 0));
  TEST_ASSERT_EQUAL_UINT((unsigned)(a.size() + b.size()), (unsigned)w.bytesWritten());
  // A failed write (FS down) adds nothing to the wear count.
  FakeFs down;  // unmounted
  LogWriter<FakeFs> w2; w2.setTier(&down, false);
  TEST_ASSERT_FALSE(w2.write("3 0 - charlie\n", 0));
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)w2.bytesWritten());
}

// CUM-407: the drop is never silent - the count is persisted in a bounded marker line
// at the next successful write, and no marker is emitted when nothing was dropped (so an
// ordinary write leaves no seq gap a reader would misread as a drop).
static void test_drop_marker_only_when_dropped() {
  TEST_ASSERT_TRUE(dropMarkerLine(5, 100, 0).empty());   // nothing dropped -> no marker
  std::string m = dropMarkerLine(5, 100, 3);
  TEST_ASSERT_FALSE(m.empty());
  TEST_ASSERT_TRUE(m.find("dropped 3 line(s)") != std::string::npos);
  TEST_ASSERT_TRUE(m.find("storage") != std::string::npos);   // tagged storage-class
  TEST_ASSERT_EQUAL_CHAR('\n', m.back());
  // The marker is itself a well-formed, capped, single line.
  TEST_ASSERT_TRUE(dropMarkerLine(1, 1, 4294967295u).size() <= kMaxLineBytes);
  TEST_ASSERT_EQUAL_INT(1, (int)std::count(m.begin(), m.end(), '\n'));
}

// The 2026-10-29 Lumi panic pin (fw v4.6.1, the dream-end row write): a durable line
// refused by the FILE-write context guard - errlog_fs skips the FILE touch when the
// CPU interrupt level is raised, because newlib's lock_acquire_generic ABORTS on a
// recursive FILE lock acquired in that state - rides the SAME bounded marker as a
// card-lock contention drop: one accounting, one wording, the exact count, so the
// next successful write always records what was missed, whatever refused it.
static void test_context_guard_drop_rides_the_contention_marker() {
  const std::string m = dropMarkerLine(9, 200, 2);
  TEST_ASSERT_FALSE(m.empty());
  TEST_ASSERT_TRUE(m.find("dropped 2 line(s)") != std::string::npos);
  TEST_ASSERT_TRUE(m.find("card-lock contention or an unsafe write context") != std::string::npos);
  TEST_ASSERT_EQUAL_CHAR('\n', m.back());
  TEST_ASSERT_EQUAL_INT(1, (int)std::count(m.begin(), m.end(), '\n'));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_persists_durably_scopes_to_warn_error_and_tags);
  RUN_TEST(test_info_burst_zero_durable_warn_one);
  RUN_TEST(test_wear_counter_tracks_bytes_written);
  RUN_TEST(test_drop_marker_only_when_dropped);
  RUN_TEST(test_context_guard_drop_rides_the_contention_marker);
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
  RUN_TEST(test_plan_retrieval_respects_active_tier);
  RUN_TEST(test_durable_sink_never_sees_a_secret);
  RUN_TEST(test_selfheal_on_late_mount);
  RUN_TEST(test_no_line_lost_before_mount);
  RUN_TEST(test_backlog_byte_bound);
  RUN_TEST(test_unattached_backlog_released);
  RUN_TEST(test_no_spurious_retier_on_normal_boot);
  RUN_TEST(test_real_sd_flip_logs_both_lines);
  RUN_TEST(test_fresh_device_without_log_dir);
  RUN_TEST(test_boot_counter_per_boot);
  RUN_TEST(test_rotation_retention_flash);
  RUN_TEST(test_rotation_retention_sd);
  RUN_TEST(test_size_sync_from_existing_file);
  RUN_TEST(test_opens_bounded_by_rotations_and_remounts);
  RUN_TEST(test_open_failure_degrades_then_recovers);
  RUN_TEST(test_write_error_reopens_once);
  RUN_TEST(test_missed_marker_failure_keeps_the_count);
  RUN_TEST(test_source_per_line_path_has_no_open);
  RUN_TEST(test_flush_per_line_and_one_for_the_backlog);
  return UNITY_END();
}
