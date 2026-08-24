#include <unity.h>

#include <cstdio>
#include <cstring>

#include "nimbus/ota/ota_logic.h"

using namespace nimbus::ota;

void setUp() {}
void tearDown() {}

// --- version ----------------------------------------------------------------

static void test_parse_version() {
  int a, b, c;
  TEST_ASSERT_TRUE(parseVersion("v2.10.0", a, b, c));
  TEST_ASSERT_EQUAL_INT(2, a); TEST_ASSERT_EQUAL_INT(10, b); TEST_ASSERT_EQUAL_INT(0, c);
  TEST_ASSERT_TRUE(parseVersion("2.10.0", a, b, c));
  TEST_ASSERT_TRUE(parseVersion("v2.11.0-rc1", a, b, c));
  TEST_ASSERT_EQUAL_INT(11, b);
  TEST_ASSERT_TRUE(parseVersion("v1.2.3+g1234", a, b, c));
  TEST_ASSERT_FALSE(parseVersion("", a, b, c));
  TEST_ASSERT_FALSE(parseVersion(nullptr, a, b, c));
  TEST_ASSERT_FALSE(parseVersion("v2.9", a, b, c));
  TEST_ASSERT_FALSE(parseVersion("v2.9.0.1", a, b, c));   // trailing .1
  TEST_ASSERT_FALSE(parseVersion("abc", a, b, c));
  TEST_ASSERT_FALSE(parseVersion("v2..0", a, b, c));
  TEST_ASSERT_FALSE(parseVersion("v2.9.0rc", a, b, c));   // junk without - or +
}

static void test_compare_versions() {
  TEST_ASSERT_TRUE(compareVersions("v2.10.0", "v2.9.9") > 0);   // 10 > 9 numerically
  TEST_ASSERT_TRUE(compareVersions("v2.9.9", "v2.10.0") < 0);
  TEST_ASSERT_EQUAL_INT(0, compareVersions("v2.10.0", "2.10.0"));
  TEST_ASSERT_EQUAL_INT(0, compareVersions("v2.10.0-rc1", "v2.10.0"));  // suffix ignored
  TEST_ASSERT_TRUE(compareVersions("v3.0.0", "v2.99.99") > 0);
  TEST_ASSERT_TRUE(compareVersions("garbage", "v0.0.1") < 0);   // garbage older
  TEST_ASSERT_TRUE(compareVersions("v0.0.1", "garbage") > 0);
  TEST_ASSERT_EQUAL_INT(0, compareVersions("junk", "junk2"));
}

// --- decoders ---------------------------------------------------------------

static void test_decode_hex() {
  uint8_t out[4];
  TEST_ASSERT_TRUE(decodeHex("deadBEEF", out, 4));
  TEST_ASSERT_EQUAL_HEX8(0xde, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xef, out[3]);
  TEST_ASSERT_FALSE(decodeHex("dead", out, 4));       // too short
  TEST_ASSERT_FALSE(decodeHex("deadbeef00", out, 4)); // trailing junk
  TEST_ASSERT_FALSE(decodeHex("deadbeeg", out, 4));   // bad nibble
  TEST_ASSERT_FALSE(decodeHex(nullptr, out, 4));
}

static void test_decode_b64() {
  uint8_t out[16];
  // "Man" -> TWFu
  TEST_ASSERT_EQUAL_UINT(3, decodeB64("TWFu", out, sizeof out));
  TEST_ASSERT_EQUAL_UINT8('M', out[0]);
  TEST_ASSERT_EQUAL_UINT8('n', out[2]);
  // padding forms
  TEST_ASSERT_EQUAL_UINT(2, decodeB64("TWE=", out, sizeof out));
  TEST_ASSERT_EQUAL_UINT(1, decodeB64("TQ==", out, sizeof out));
  // malformed
  TEST_ASSERT_EQUAL_UINT(0, decodeB64("TWF", out, sizeof out));    // not %4
  TEST_ASSERT_EQUAL_UINT(0, decodeB64("TW!u", out, sizeof out));   // bad char
  TEST_ASSERT_EQUAL_UINT(0, decodeB64("TQ==TWFu", out, sizeof out)); // data after pad
  TEST_ASSERT_EQUAL_UINT(0, decodeB64("", out, sizeof out));
  TEST_ASSERT_EQUAL_UINT(0, decodeB64("TWFu", out, 2));            // overflow
}

// --- signed message ---------------------------------------------------------

static void test_sig_message_golden() {
  char buf[160];
  const char* sha =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  size_t n = buildSigMessage(buf, sizeof buf, "v4.3.0", "nimbus-tft", sha);
  // GOLDEN - tools/make_manifest.py --print-message must produce these bytes.
  // Byte-locked to tools/test_make_manifest.py::test_print_message_golden_v2.
  const char* want =
      "nimbus-ota-v2\nv4.3.0\nnimbus-tft\n"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
  TEST_ASSERT_EQUAL_UINT(strlen(want), n);
  TEST_ASSERT_EQUAL_STRING(want, buf);
  // too-small buffer refuses cleanly
  TEST_ASSERT_EQUAL_UINT(0, buildSigMessage(buf, 20, "v4.3.0", "nimbus-tft", sha));
}

static void test_type_allowed_for_board() {
  // Solide build: only nimbus-tft.
  TEST_ASSERT_TRUE(typeAllowedForBoard("nimbus-tft", false));
  TEST_ASSERT_FALSE(typeAllowedForBoard("freenove-28", false));
  // Freenove build: only the freenove sizes.
  TEST_ASSERT_TRUE(typeAllowedForBoard("freenove-28", true));
  TEST_ASSERT_TRUE(typeAllowedForBoard("freenove-35", true));
  TEST_ASSERT_TRUE(typeAllowedForBoard("freenove-40", true));
  TEST_ASSERT_FALSE(typeAllowedForBoard("nimbus-tft", true));
  // Unknown / legacy / untyped slugs never match either family.
  TEST_ASSERT_FALSE(typeAllowedForBoard("", false));
  TEST_ASSERT_FALSE(typeAllowedForBoard("", true));
  TEST_ASSERT_FALSE(typeAllowedForBoard(nullptr, false));
  TEST_ASSERT_FALSE(typeAllowedForBoard("esp32s3", false));   // legacy build tag
  TEST_ASSERT_FALSE(typeAllowedForBoard("freenove-99", true));
}

static void test_derive_device_type() {
  char b[24];
  // Solide + TFT -> nimbus-tft.
  TEST_ASSERT_EQUAL_UINT(10, deriveDeviceType(b, sizeof b, false, true));
  TEST_ASSERT_EQUAL_STRING("nimbus-tft", b);
  // Solide + e-ink -> untyped (frozen, no updates).
  TEST_ASSERT_EQUAL_UINT(0, deriveDeviceType(b, sizeof b, false, false));
  TEST_ASSERT_EQUAL_STRING("", b);
  // Freenove -> base size default.
  TEST_ASSERT_EQUAL_UINT(11, deriveDeviceType(b, sizeof b, true, true));
  TEST_ASSERT_EQUAL_STRING("freenove-28", b);
  // Whatever a derived type is, it must be one this board is allowed to run.
  deriveDeviceType(b, sizeof b, false, true);
  TEST_ASSERT_TRUE(typeAllowedForBoard(b, false));
  deriveDeviceType(b, sizeof b, true, true);
  TEST_ASSERT_TRUE(typeAllowedForBoard(b, true));
  // Too-small buffer refuses cleanly.
  TEST_ASSERT_EQUAL_UINT(0, deriveDeviceType(b, 4, false, true));
  TEST_ASSERT_EQUAL_STRING("", b);
}

// --- manifest ---------------------------------------------------------------

static const char* kSha64 =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void makeManifestJson(char* buf, size_t cap, const char* schema,
                             const char* url, const char* size,
                             const char* sha, const char* sig,
                             const char* minVersion = "") {
  snprintf(buf, cap,
           "{\"schema\":%s,\"version\":\"v2.11.0\",\"build\":\"v2.11.0-0-gabc\","
           "\"notes\":\"test release\",\"minVersion\":\"%s\",\"variants\":{"
           "\"nimbus-tft\":{\"url\":\"%s\",\"size\":%s,\"sha256\":\"%s\","
           "\"sig\":\"%s\"},"
           "\"freenove-28\":{\"url\":\"https://x/y.bin\",\"size\":3000000,"
           "\"sha256\":\"%s\",\"sig\":\"%s\"}}}",
           schema, minVersion, url, size, sha, sig, kSha64, "TWFuTWFuTWFu");
}

static void test_manifest_valid() {
  char json[900];
  makeManifestJson(json, sizeof json, "2",
                   "https://github.com/x/y/releases/download/v2.11.0/fw.bin",
                   "3014656", kSha64, "TWFuTWFuTWFu");
  ManifestInfo m;
  const char* err = nullptr;
  TEST_ASSERT_TRUE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("v2.11.0", m.version);
  TEST_ASSERT_EQUAL_STRING("test release", m.notes);
  TEST_ASSERT_EQUAL_UINT32(3014656u, m.v.size);
  TEST_ASSERT_EQUAL_STRING(kSha64, m.v.shaHex);
  TEST_ASSERT_EQUAL_HEX8(0x01, m.v.sha256[0]);
  TEST_ASSERT_EQUAL_HEX8(0xef, m.v.sha256[31]);
  TEST_ASSERT_EQUAL_UINT8(9, m.v.sigLen);  // "ManManMan"
  // the other typed variant parses independently
  ManifestInfo t;
  TEST_ASSERT_TRUE(parseManifest(json, strlen(json), "freenove-28", t, &err));
  TEST_ASSERT_EQUAL_UINT32(3000000u, t.v.size);
  // an untyped device ("") matches no variant -> "no update"
  ManifestInfo u;
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "", u, &err));
  TEST_ASSERT_EQUAL_STRING("schema", err);  // empty variant rejected up front
}

static void test_manifest_uppercase_sha_canonicalized() {
  char upper[65];
  for (int i = 0; i < 64; i++) {
    char c = kSha64[i];
    upper[i] = (c >= 'a' && c <= 'f') ? (char)(c - 32) : c;
  }
  upper[64] = '\0';
  char json[900];
  makeManifestJson(json, sizeof json, "2", "https://x/fw.bin", "3014656",
                   upper, "TWFuTWFuTWFu");
  ManifestInfo m;
  TEST_ASSERT_TRUE(parseManifest(json, strlen(json), "nimbus-tft", m));
  TEST_ASSERT_EQUAL_STRING(kSha64, m.v.shaHex);  // lowercased for the message
}

static void test_manifest_rejects() {
  char json[900];
  ManifestInfo m;
  const char* err;

  makeManifestJson(json, sizeof json, "1", "https://x/fw.bin", "3014656",
                   kSha64, "TWFuTWFuTWFu");  // legacy schema 1 now rejected
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("schema", err);

  makeManifestJson(json, sizeof json, "3", "https://x/fw.bin", "3014656",
                   kSha64, "TWFuTWFuTWFu");  // future schema also rejected
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("schema", err);

  makeManifestJson(json, sizeof json, "2", "http://x/fw.bin", "3014656",
                   kSha64, "TWFuTWFuTWFu");  // not https
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("url", err);

  makeManifestJson(json, sizeof json, "2", "https://x/fw.bin", "1000",
                   kSha64, "TWFuTWFuTWFu");  // below floor
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("size", err);

  makeManifestJson(json, sizeof json, "2", "https://x/fw.bin", "9000000",
                   kSha64, "TWFuTWFuTWFu");  // above one slot
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("size", err);

  makeManifestJson(json, sizeof json, "2", "https://x/fw.bin", "3014656",
                   "deadbeef", "TWFuTWFuTWFu");  // short sha
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("sha256", err);

  makeManifestJson(json, sizeof json, "2", "https://x/fw.bin", "3014656",
                   kSha64, "!!bad!!!");  // bad b64
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("sig", err);

  makeManifestJson(json, sizeof json, "2", "https://x/fw.bin", "3014656",
                   kSha64, "TWFuTWFuTWFu");
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "freenove-99", m, &err));
  TEST_ASSERT_EQUAL_STRING("variant", err);  // unknown/absent device type

  makeManifestJson(json, sizeof json, "2", "https://x/fw.bin", "3014656",
                   kSha64, "TWFuTWFuTWFu", "not-a-version");
  TEST_ASSERT_FALSE(parseManifest(json, strlen(json), "nimbus-tft", m, &err));
  TEST_ASSERT_EQUAL_STRING("version", err);  // bad minVersion

  TEST_ASSERT_FALSE(parseManifest("{", 1, "nimbus-tft", m, &err));  // truncated json
}

// --- state machine ----------------------------------------------------------

static void test_state_machine() {
  TEST_ASSERT_TRUE(canCheck(State::Idle));
  TEST_ASSERT_TRUE(canCheck(State::UpToDate));
  TEST_ASSERT_TRUE(canCheck(State::Available));
  TEST_ASSERT_TRUE(canCheck(State::Error));
  TEST_ASSERT_FALSE(canCheck(State::Checking));
  TEST_ASSERT_FALSE(canCheck(State::Downloading));
  TEST_ASSERT_FALSE(canCheck(State::Verifying));
  TEST_ASSERT_FALSE(canCheck(State::ReadyToReboot));
  TEST_ASSERT_FALSE(canCheck(State::Unsupported));

  TEST_ASSERT_TRUE(canInstall(State::Available));
  TEST_ASSERT_FALSE(canInstall(State::Idle));
  TEST_ASSERT_FALSE(canInstall(State::UpToDate));
  TEST_ASSERT_FALSE(canInstall(State::Downloading));
  TEST_ASSERT_FALSE(canInstall(State::Error));

  TEST_ASSERT_EQUAL_STRING("available", stateStr(State::Available));
  TEST_ASSERT_EQUAL_STRING("unsupported", stateStr(State::Unsupported));
}

// --- eligibility ------------------------------------------------------------

static ManifestInfo mkM(const char* ver, const char* minV = "") {
  ManifestInfo m;
  strncpy(m.version, ver, sizeof(m.version) - 1);
  strncpy(m.minVersion, minV, sizeof(m.minVersion) - 1);
  return m;
}

static void test_eligibility() {
  TEST_ASSERT_EQUAL_INT((int)Eligibility::Newer,
                        (int)eligibility("v2.10.0", mkM("v2.11.0")));
  TEST_ASSERT_EQUAL_INT((int)Eligibility::Same,
                        (int)eligibility("v2.11.0", mkM("v2.11.0")));
  TEST_ASSERT_EQUAL_INT((int)Eligibility::Older,
                        (int)eligibility("v2.12.0", mkM("v2.11.0")));
  // minVersion floor blocks the auto jump
  TEST_ASSERT_EQUAL_INT((int)Eligibility::BlockedMinVersion,
                        (int)eligibility("v1.9.0", mkM("v2.11.0", "v2.0.0")));
  TEST_ASSERT_EQUAL_INT((int)Eligibility::Newer,
                        (int)eligibility("v2.5.0", mkM("v2.11.0", "v2.0.0")));
  TEST_ASSERT_TRUE(autoEligible(Eligibility::Newer));
  TEST_ASSERT_FALSE(autoEligible(Eligibility::Same));
  TEST_ASSERT_FALSE(autoEligible(Eligibility::Older));
  TEST_ASSERT_FALSE(autoEligible(Eligibility::BlockedMinVersion));
}

// --- boot health / rollback -------------------------------------------------

static void test_rollback_policy() {
  TEST_ASSERT_FALSE(shouldRollback(false, 99));   // no pending flag => never
  TEST_ASSERT_FALSE(shouldRollback(true, 1));
  TEST_ASSERT_FALSE(shouldRollback(true, 3));     // third attempt still allowed
  TEST_ASSERT_TRUE(shouldRollback(true, 4));      // exhausted
}

static void test_boot_healthy() {
  TEST_ASSERT_FALSE(bootHealthy(60, true));
  TEST_ASSERT_TRUE(bootHealthy(120, true));
  TEST_ASSERT_FALSE(bootHealthy(120, false));     // no WiFi => longer bar
  TEST_ASSERT_FALSE(bootHealthy(599, false));
  TEST_ASSERT_TRUE(bootHealthy(600, false));
}

// --- auto-install idle window ------------------------------------------------

static IdleSnapshot idle() {
  IdleSnapshot s;
  s.onExternalPower = true;
  s.battPct = 100;
  s.internalFreeB = 40000;
  return s;
}

static void test_auto_install_window() {
  TEST_ASSERT_TRUE(autoInstallAllowed(idle()));
  { IdleSnapshot s = idle(); s.turnInFlight = true;  TEST_ASSERT_FALSE(autoInstallAllowed(s)); }
  { IdleSnapshot s = idle(); s.voiceActive = true;   TEST_ASSERT_FALSE(autoInstallAllowed(s)); }
  { IdleSnapshot s = idle(); s.audioPlaying = true;  TEST_ASSERT_FALSE(autoInstallAllowed(s)); }
  { IdleSnapshot s = idle(); s.internalFreeB = 20000; TEST_ASSERT_FALSE(autoInstallAllowed(s)); }
  // battery: external power excuses a low pack; on battery the floor applies
  { IdleSnapshot s = idle(); s.onExternalPower = false; s.battPct = 49;
    TEST_ASSERT_FALSE(autoInstallAllowed(s)); }
  { IdleSnapshot s = idle(); s.onExternalPower = false; s.battPct = 50;
    TEST_ASSERT_TRUE(autoInstallAllowed(s)); }
  { IdleSnapshot s = idle(); s.battPct = 5;  // charging/full/external => ok
    TEST_ASSERT_TRUE(autoInstallAllowed(s)); }
  // health floor applies on battery, is excused by external power
  { IdleSnapshot s = idle(); s.onExternalPower = false; s.battPct = 90; s.healthPct = 59;
    TEST_ASSERT_FALSE(autoInstallAllowed(s)); }
  { IdleSnapshot s = idle(); s.onExternalPower = false; s.battPct = 90; s.healthPct = 60;
    TEST_ASSERT_TRUE(autoInstallAllowed(s)); }
  { IdleSnapshot s = idle(); s.onExternalPower = true; s.healthPct = 10;  // wall power => ok
    TEST_ASSERT_TRUE(autoInstallAllowed(s)); }
}

// --- battery / health install gate ------------------------------------------

static void test_install_gate() {
  auto in = [](bool mon, bool ext, int pct, uint8_t h) {
    InstallGateInput g; g.battMonEnabled = mon; g.onExternalPower = ext;
    g.battPct = pct; g.healthPct = h; return g;
  };
  // healthy pack on battery: allowed
  TEST_ASSERT_EQUAL_INT((int)InstallGate::Allowed, (int)installGate(in(true, false, 80, 100)));
  // exactly at the floors: allowed (>=)
  TEST_ASSERT_EQUAL_INT((int)InstallGate::Allowed, (int)installGate(in(true, false, 40, 60)));
  // below level floor -> NeedPower
  TEST_ASSERT_EQUAL_INT((int)InstallGate::NeedPower, (int)installGate(in(true, false, 39, 100)));
  // healthy level but worn pack -> NeedRecalibrate
  TEST_ASSERT_EQUAL_INT((int)InstallGate::NeedRecalibrate, (int)installGate(in(true, false, 80, 59)));
  // both low: level checked first -> NeedPower
  TEST_ASSERT_EQUAL_INT((int)InstallGate::NeedPower, (int)installGate(in(true, false, 10, 10)));
  // external power excuses everything
  TEST_ASSERT_EQUAL_INT((int)InstallGate::Allowed, (int)installGate(in(true, true, 5, 10)));
  // monitoring disabled: no pack to protect, always allowed
  TEST_ASSERT_EQUAL_INT((int)InstallGate::Allowed, (int)installGate(in(false, false, 5, 10)));
  TEST_ASSERT_EQUAL_STRING("allowed", installGateStr(InstallGate::Allowed));
  TEST_ASSERT_EQUAL_STRING("need-power", installGateStr(InstallGate::NeedPower));
  TEST_ASSERT_EQUAL_STRING("need-recalibrate", installGateStr(InstallGate::NeedRecalibrate));
}

// --- definitive check result ------------------------------------------------

static void test_check_result() {
  TEST_ASSERT_EQUAL_INT((int)CheckResult::UpToDate,   (int)checkResult(State::UpToDate, true));
  TEST_ASSERT_EQUAL_INT((int)CheckResult::NewVersion, (int)checkResult(State::Available, true));
  // Error that reached the server = a bad manifest; that didn't = unreachable
  TEST_ASSERT_EQUAL_INT((int)CheckResult::Failed,      (int)checkResult(State::Error, true));
  TEST_ASSERT_EQUAL_INT((int)CheckResult::Unreachable, (int)checkResult(State::Error, false));
  // still-running / never-checked states are Pending, never a false terminal
  TEST_ASSERT_EQUAL_INT((int)CheckResult::Pending, (int)checkResult(State::Checking, true));
  TEST_ASSERT_EQUAL_INT((int)CheckResult::Pending, (int)checkResult(State::Idle, false));
  TEST_ASSERT_EQUAL_STRING("up-to-date", checkResultStr(CheckResult::UpToDate));
  TEST_ASSERT_EQUAL_STRING("new-version", checkResultStr(CheckResult::NewVersion));
  TEST_ASSERT_EQUAL_STRING("unreachable", checkResultStr(CheckResult::Unreachable));
  TEST_ASSERT_EQUAL_STRING("failed", checkResultStr(CheckResult::Failed));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_version);
  RUN_TEST(test_compare_versions);
  RUN_TEST(test_decode_hex);
  RUN_TEST(test_decode_b64);
  RUN_TEST(test_sig_message_golden);
  RUN_TEST(test_type_allowed_for_board);
  RUN_TEST(test_derive_device_type);
  RUN_TEST(test_manifest_valid);
  RUN_TEST(test_manifest_uppercase_sha_canonicalized);
  RUN_TEST(test_manifest_rejects);
  RUN_TEST(test_state_machine);
  RUN_TEST(test_eligibility);
  RUN_TEST(test_rollback_policy);
  RUN_TEST(test_boot_healthy);
  RUN_TEST(test_auto_install_window);
  RUN_TEST(test_install_gate);
  RUN_TEST(test_check_result);
  return UNITY_END();
}
