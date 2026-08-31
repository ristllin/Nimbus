#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>

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

static void test_state_from_str_roundtrips() {
  const State all[] = {State::Idle, State::Checking, State::UpToDate, State::Available,
                       State::Downloading, State::Verifying, State::ReadyToReboot,
                       State::Error, State::Unsupported};
  for (State s : all)
    TEST_ASSERT_EQUAL_INT((int)s, (int)stateFromStr(stateStr(s)));
  TEST_ASSERT_EQUAL_INT((int)State::Idle, (int)stateFromStr("bogus"));
  TEST_ASSERT_EQUAL_INT((int)State::Idle, (int)stateFromStr(nullptr));
}

// CUM-193: the device screen + web UI describe the update state from this one
// pure view. Locks the owner-facing copy and the affordance flags per state.
static void test_update_view() {
  UpdateView idle = updateView(State::Idle, -1, "", "", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("", idle.line);   // Idle keeps the control's default help
  TEST_ASSERT_FALSE(idle.busy);
  TEST_ASSERT_FALSE(idle.showInstall);

  UpdateView chk = updateView(State::Checking, -1, "", "", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("Checking for updates...", chk.line);
  TEST_ASSERT_TRUE(chk.busy);                // the in-progress affordance
  TEST_ASSERT_FALSE(chk.showInstall);

  UpdateView utd = updateView(State::UpToDate, -1, "", "", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("Up to date - v4.4.0", utd.line);
  TEST_ASSERT_FALSE(utd.busy);
  TEST_ASSERT_FALSE(utd.showInstall);

  UpdateView av = updateView(State::Available, -1, "v4.4.1", "", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("Update available: v4.4.1", av.line);
  TEST_ASSERT_FALSE(av.busy);
  TEST_ASSERT_TRUE(av.showInstall);          // offer Install
  UpdateView av2 = updateView(State::Available, -1, "", "", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("Update available: a new version", av2.line);
  TEST_ASSERT_TRUE(av2.showInstall);

  UpdateView dl = updateView(State::Downloading, 42, "v4.4.1", "", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("Installing... 42%", dl.line);
  TEST_ASSERT_TRUE(dl.busy);
  TEST_ASSERT_EQUAL_INT(42, dl.pct);
  UpdateView dl0 = updateView(State::Downloading, -1, "", "", "");
  TEST_ASSERT_EQUAL_STRING("Installing...", dl0.line);

  UpdateView err = updateView(State::Error, -1, "", "no-release", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("Update check failed (no-release)", err.line);  // honest reason
  TEST_ASSERT_FALSE(err.busy);
  UpdateView err2 = updateView(State::Error, -1, "", "", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("Update check failed. Try again.", err2.line);

  UpdateView un = updateView(State::Unsupported, -1, "", "", "v4.4.0");
  TEST_ASSERT_EQUAL_STRING("Updates aren't available on this build.", un.line);

  TEST_ASSERT_TRUE(updateView(State::Verifying, -1, "", "", "").busy);
  TEST_ASSERT_TRUE(updateView(State::ReadyToReboot, -1, "", "", "").busy);

  // Every line fits the device's ~48-char printable-ASCII row.
  const State all[] = {State::Idle, State::Checking, State::UpToDate, State::Available,
                       State::Downloading, State::Verifying, State::ReadyToReboot,
                       State::Error, State::Unsupported};
  for (State s : all) {
    UpdateView v = updateView(s, 100, "v4.4.1", "sha-fail", "v4.4.0");
    for (const char* p = v.line; *p; ++p)
      TEST_ASSERT_TRUE_MESSAGE(*p >= 32 && *p < 127, "update line has a non-ASCII byte");
    TEST_ASSERT_TRUE_MESSAGE(strlen(v.line) < 48, "update line overruns the 48-char row");
  }
}

// CUM-264 #1: the "latest vs available" verdict and the status token must render
// from the SAME state read, so they can never contradict. Both stateStr(s) (the
// panel's Status field) and updateView(s).line (the verdict line) are pure
// functions of ONE State - this property test fails if any State could show a
// "latest/up to date" verdict while the status says "available" (the owner's
// screenshot) or the reverse. The web panel enforces this by re-deriving fwMsg
// from d.ota (== stateStr) on every render; this locks the invariant the panel
// leans on, across EVERY enum value, so a new state cannot reintroduce the drift.
static void test_verdict_status_consistency() {
  const State all[] = {State::Idle, State::Checking, State::UpToDate, State::Available,
                       State::Downloading, State::Verifying, State::ReadyToReboot,
                       State::Error, State::Unsupported};
  for (State s : all) {
    const char* status = stateStr(s);
    UpdateView v = updateView(s, -1, "v4.4.6", "", "v4.4.2");
    std::string line = v.line;
    bool saysAvailable = line.find("available") != std::string::npos ||
                         line.find("Available") != std::string::npos;
    bool saysUpToDate  = line.find("Up to date") != std::string::npos ||
                         line.find("up to date") != std::string::npos ||
                         line.find("latest") != std::string::npos;
    if (!std::strcmp(status, "available")) {
      TEST_ASSERT_TRUE_MESSAGE(saysAvailable, "available status must show an available verdict");
      TEST_ASSERT_FALSE_MESSAGE(saysUpToDate, "available status must NOT show an up-to-date verdict");
      TEST_ASSERT_TRUE_MESSAGE(v.showInstall, "available status must offer Install");
    }
    if (!std::strcmp(status, "up-to-date")) {
      TEST_ASSERT_TRUE_MESSAGE(saysUpToDate, "up-to-date status must show an up-to-date verdict");
      TEST_ASSERT_FALSE_MESSAGE(saysAvailable, "up-to-date status must NOT show an available verdict");
      TEST_ASSERT_FALSE_MESSAGE(v.showInstall, "up-to-date status must NOT offer Install");
    }
    // No non-terminal state may claim either verdict (a stale claim next to a
    // busy/idle status is the same class of contradiction).
    if (std::strcmp(status, "available") && std::strcmp(status, "up-to-date")) {
      TEST_ASSERT_FALSE_MESSAGE(saysAvailable && v.showInstall,
                                "only the available state offers Install with an available verdict");
    }
  }
}

// CUM-197: a "Check for updates" 409 is always a LOCAL refusal - the copy names
// the real cause and NEVER blames the network.
static void test_check_refusal_copy() {
  TEST_ASSERT_EQUAL_STRING("Can't check: no Wi-Fi. Connect and try again.", checkRefusalCopy("no-wifi"));
  TEST_ASSERT_EQUAL_STRING("Can't check now: low memory. Try again.", checkRefusalCopy("low-heap"));
  TEST_ASSERT_EQUAL_STRING("This build doesn't receive updates.", checkRefusalCopy("unsupported"));
  TEST_ASSERT_EQUAL_STRING("An update is already running.", checkRefusalCopy("busy"));
  TEST_ASSERT_EQUAL_STRING("An update is already running.", checkRefusalCopy("in-progress"));
  TEST_ASSERT_EQUAL_STRING("Couldn't start the check. Try again.", checkRefusalCopy("weird"));
  TEST_ASSERT_EQUAL_STRING("Couldn't start the check. Try again.", checkRefusalCopy(nullptr));
  const char* reasons[] = {"no-wifi", "low-heap", "unsupported", "busy", "in-progress", "weird", ""};
  for (const char* w : reasons) {
    const char* c = checkRefusalCopy(w);
    TEST_ASSERT_NULL_MESSAGE(strstr(c, "network"), "refusal copy must not blame the network");
    TEST_ASSERT_TRUE_MESSAGE(strlen(c) < 48, "refusal copy overruns the 48-char device row");
  }
}

// CUM-197: a persisted "last result" that names a different version than the one
// running is stale (a later flash bypassed the OTA that wrote it).
static void test_last_result_stale() {
  // ONLY "ok <version>" claims the running image; stale iff that version differs.
  TEST_ASSERT_FALSE(lastResultStale("ok v4.4.0", "v4.4.0"));          // same image
  TEST_ASSERT_TRUE(lastResultStale("ok v4.2.0", "v4.3.0-154-g96b8bb3"));  // the owner's case
  TEST_ASSERT_TRUE(lastResultStale("ok v4.4.0", "v4.2.0"));
  TEST_ASSERT_FALSE(lastResultStale("ok v4.4.0", "v4.4.0-5-gabc"));   // same release, dev suffix
  // Records that intentionally name a NON-running version MUST be kept - a
  // cross-release rollback/dryrun/failed-install is not stale (review finding).
  TEST_ASSERT_FALSE(lastResultStale("rollback v4.4.0", "v4.3.0"));    // rolled back FROM v4.4.0
  TEST_ASSERT_FALSE(lastResultStale("rollback v4.3.0", "v4.3.0"));
  TEST_ASSERT_FALSE(lastResultStale("dryrun ok v4.4.0", "v4.3.0"));   // verified, not installed
  TEST_ASSERT_FALSE(lastResultStale("download v4.4.0", "v4.3.0"));    // failed install of v4.4.0
  TEST_ASSERT_FALSE(lastResultStale("installing v4.4.0", "v4.3.0"));
  // Empty / non-version / non-ok tokens -> leave the record alone.
  TEST_ASSERT_FALSE(lastResultStale("", "v4.4.0"));
  TEST_ASSERT_FALSE(lastResultStale(nullptr, "v4.4.0"));
  TEST_ASSERT_FALSE(lastResultStale("sim-arm app0", "v4.4.0"));
  TEST_ASSERT_FALSE(lastResultStale("rollback-lost", "v4.4.0"));
  TEST_ASSERT_FALSE(lastResultStale("aborted-preflip", "v4.4.0"));
}

// CUM-264 #2: a HIL simulation "last result" must never reach an owner. The
// class rule (not the instance) - anything carrying kSimResultPrefix is a test
// artifact - so a NEW sim seam that keeps the prefix is caught with no new guard,
// while every real OTA outcome the field can hold is left untouched.
static void test_is_sim_result() {
  using nimbus::ota::isSimResult;
  using nimbus::ota::kSimResultPrefix;
  // Every string the sim seams (simArm) can write today - incl. the owner's
  // exact residue "sim-arm crash" (OTASIM arm crash, see test_l29_release_gate).
  TEST_ASSERT_TRUE(isSimResult("sim-arm app0"));
  TEST_ASSERT_TRUE(isSimResult("sim-arm app1"));
  TEST_ASSERT_TRUE(isSimResult("sim-arm crash"));
  // The class: the prefix itself is the guard - any future "sim-*" label is
  // caught, so a new seam needs no separate rule.
  TEST_ASSERT_TRUE(isSimResult(kSimResultPrefix));
  TEST_ASSERT_TRUE(isSimResult((std::string(kSimResultPrefix) + "whatever-next").c_str()));
  // Every REAL OTA outcome the "last result" field can hold must be preserved.
  TEST_ASSERT_FALSE(isSimResult("ok v4.4.6"));
  TEST_ASSERT_FALSE(isSimResult("rollback v4.4.6"));
  TEST_ASSERT_FALSE(isSimResult("rollback-lost"));
  TEST_ASSERT_FALSE(isSimResult("dryrun ok v4.4.6"));
  TEST_ASSERT_FALSE(isSimResult("installing v4.4.6"));
  TEST_ASSERT_FALSE(isSimResult("download v4.4.6"));
  TEST_ASSERT_FALSE(isSimResult("aborted-preflip"));
  // Empty / null -> nothing to filter.
  TEST_ASSERT_FALSE(isSimResult(""));
  TEST_ASSERT_FALSE(isSimResult(nullptr));
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
  RUN_TEST(test_state_from_str_roundtrips);
  RUN_TEST(test_update_view);
  RUN_TEST(test_verdict_status_consistency);
  RUN_TEST(test_check_refusal_copy);
  RUN_TEST(test_last_result_stale);
  RUN_TEST(test_is_sim_result);
  return UNITY_END();
}
