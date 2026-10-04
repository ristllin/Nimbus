// Host tests for nimbus::identity - the device-name sanitizer, mDNS label
// derivation, the first-boot sibling auto-numbering (P2 of the agent-3 revamp
// plan) and its scan-retry loop (CUM-468). Pure logic; the NVS/WiFi glue is
// device-side.
#include <unity.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/device_identity.h"

using namespace nimbus::identity;
using V = std::vector<std::string>;

void setUp() {}
void tearDown() {}

// ---- sanitizeName -----------------------------------------------------------
static void test_sanitize_passthrough() {
  TEST_ASSERT_EQUAL_STRING("Nimbus", sanitizeName("Nimbus").c_str());
  TEST_ASSERT_EQUAL_STRING("Desk Buddy-2", sanitizeName("Desk Buddy-2").c_str());
}

static void test_sanitize_strips_junk() {
  TEST_ASSERT_EQUAL_STRING("Nimbus", sanitizeName("  Nimbus!  ").c_str());
  TEST_ASSERT_EQUAL_STRING("caf", sanitizeName("caf\xC3\xA9").c_str());  // UTF-8 dropped
  TEST_ASSERT_EQUAL_STRING("a b", sanitizeName("a   \t b").c_str());     // blanks collapse
  TEST_ASSERT_EQUAL_STRING("", sanitizeName("!!! \xF0\x9F\x98\x80").c_str());
}

static void test_sanitize_caps_length() {
  const std::string longName(64, 'x');
  TEST_ASSERT_EQUAL(24, (int)sanitizeName(longName).size());
}

// ---- mdnsLabel ---------------------------------------------------------------
static void test_mdns_label_basic() {
  TEST_ASSERT_EQUAL_STRING("nimbus", mdnsLabel("Nimbus").c_str());   // historical default
  TEST_ASSERT_EQUAL_STRING("nimbus-2", mdnsLabel("Nimbus-2").c_str());
  TEST_ASSERT_EQUAL_STRING("desk-buddy", mdnsLabel("Desk Buddy").c_str());
}

static void test_mdns_label_trims_dashes() {
  TEST_ASSERT_EQUAL_STRING("a-b", mdnsLabel("--a__b--").c_str());
  TEST_ASSERT_EQUAL_STRING("", mdnsLabel("!!!").c_str());
}

// ---- pickSiblingName ---------------------------------------------------------
static void test_pick_no_siblings() {
  TEST_ASSERT_EQUAL_STRING("Nimbus",
      pickSiblingName("Nimbus", V{"HomeWiFi", "CoffeeShop"}).c_str());
}

static void test_pick_second_device() {
  // A sibling's setup AP is visible -> we are the second device.
  TEST_ASSERT_EQUAL_STRING("Nimbus-2",
      pickSiblingName("Nimbus", V{"Nimbus-setup", "HomeWiFi"}).c_str());
}

static void test_pick_lowest_free_gap() {
  // Nimbus + Nimbus-3 visible -> the free slot is 2.
  TEST_ASSERT_EQUAL_STRING("Nimbus-2",
      pickSiblingName("Nimbus", V{"Nimbus-setup", "Nimbus-3-setup"}).c_str());
}

static void test_pick_third_device() {
  TEST_ASSERT_EQUAL_STRING("Nimbus-3",
      pickSiblingName("Nimbus", V{"Nimbus-setup", "Nimbus-2-setup"}).c_str());
}

static void test_pick_matches_bare_names_too() {
  // Non-"-setup" sibling SSIDs (future forms) also count as occupied.
  TEST_ASSERT_EQUAL_STRING("Nimbus-3",
      pickSiblingName("Nimbus", V{"Nimbus", "Nimbus-2"}).c_str());
}

static void test_pick_ignores_lookalikes() {
  // Names that merely START with the base are not siblings.
  TEST_ASSERT_EQUAL_STRING("Nimbus",
      pickSiblingName("Nimbus", V{"NimbusCloud", "Nimbus-abc", "Nimbus-abc-setup",
                                  "Nimbus-2x-setup", "nimbus-setup"}).c_str());
}

static void test_pick_dedupes_and_bounds() {
  // Duplicate sightings collapse; absurd numbers (>4 digits) are ignored.
  TEST_ASSERT_EQUAL_STRING("Nimbus-2",
      pickSiblingName("Nimbus", V{"Nimbus-setup", "Nimbus-setup",
                                  "Nimbus-99999-setup"}).c_str());
}

// ---- first-boot sibling scan retry (CUM-468) ---------------------------------
// The 2026-10-04 bench: two fresh units side by side both named themselves
// "Nimbus" because the scan came back in <200 ms with nothing in it. These pin
// the class rule "a refused scan is not an empty neighborhood" - in BOTH shapes a
// refusal takes (a negative code, or an aborted scan reported as a fast 0) - plus
// the first-boot budget.
static constexpr int kScanFailed  = -2;   // WIFI_SCAN_FAILED
static constexpr int kScanRunning = -1;   // WIFI_SCAN_RUNNING
static constexpr uint32_t kRealScanMs = 2100;   // a completed 2.4 GHz sweep
static constexpr uint32_t kLastRetryAt = kSiblingScanWindowMs - kSiblingScanRetryGapMs;

static void test_scan_retry_real_result_is_final() {
  // Any count ends the loop at every elapsed time - 0 included when the scan took
  // as long as a real sweep: that is a genuinely empty neighborhood, and the
  // device correctly names itself "Nimbus".
  const uint32_t times[] = {0u, 150u, kLastRetryAt, kLastRetryAt + 1, 5000u, UINT32_MAX};
  for (uint32_t t : times) {
    for (int n : {1, 7, 40}) {
      TEST_ASSERT_FALSE(retrySiblingScan(n, 0, t));   // even a fast non-empty list is data
      TEST_ASSERT_FALSE(retrySiblingScan(n, kRealScanMs, t));
    }
    TEST_ASSERT_FALSE(retrySiblingScan(0, kSiblingScanMinRealMs, t));
    TEST_ASSERT_FALSE(retrySiblingScan(0, kRealScanMs, t));
  }
}

static void test_scan_retry_refusal_inside_window() {
  // Both refusal shapes: a negative code (never ran) at any attempt length, and an
  // empty list faster than any completed sweep (aborted, reported as 0).
  for (uint32_t t : {0u, 150u, kLastRetryAt}) {
    for (int rc : {kScanFailed, kScanRunning, -100}) {
      TEST_ASSERT_TRUE(retrySiblingScan(rc, 0, t));
      TEST_ASSERT_TRUE(retrySiblingScan(rc, 150, t));
    }
    TEST_ASSERT_TRUE(retrySiblingScan(0, 0, t));
    TEST_ASSERT_TRUE(retrySiblingScan(0, 150, t));
    TEST_ASSERT_TRUE(retrySiblingScan(0, kSiblingScanMinRealMs - 1, t));
  }
}

static void test_scan_retry_refusal_outside_window() {
  for (uint32_t t : {kLastRetryAt + 1, 60000u, UINT32_MAX}) {   // UINT32_MAX: no wrap back in
    for (int rc : {kScanFailed, kScanRunning}) TEST_ASSERT_FALSE(retrySiblingScan(rc, 150, t));
    TEST_ASSERT_FALSE(retrySiblingScan(0, 150, t));
  }
}

// A scripted radio on a fake clock: attempt i takes durMs[i] and returns rc[i]
// (the last entry repeats). Drives the SAME runSiblingScan loop the device runs.
// A loop that stopped honoring the window would spin forever; past kRunaway calls
// the fake answers a real list (final under any policy) to force an exit, so the
// attempt-count asserts FAIL instead of hanging the suite.
struct FakeRadio {
  static constexpr int kRunaway = 1000;
  std::vector<int>      rc;
  std::vector<uint32_t> durMs;
  uint32_t clock = 5000;   // arbitrary non-zero boot time
  uint32_t lastStart = 0;
  int      calls = 0;
  SiblingScanOutcome run() {
    return runSiblingScan(
        [this] {
          if (calls >= kRunaway) return 1;
          const size_t i = std::min<size_t>(size_t(calls), rc.size() - 1);
          lastStart = clock;
          clock += durMs[std::min<size_t>(size_t(calls), durMs.size() - 1)];
          calls++;
          return rc[i];
        },
        [this] { return clock; }, [this](uint32_t ms) { clock += ms; });
  }
};

static void test_scan_loop_bench_repro_refused_then_real() {
  // The CUM-468 shape, in both refusal forms: the first attempt is refused fast,
  // the driver is up a beat later, and a real ~2 s scan sees the sibling. Must be
  // ONE retry, with the real list reaching pickSiblingName.
  for (int refusal : {kScanFailed, 0}) {
    FakeRadio r{{refusal, 9}, {150, kRealScanMs}};
    const SiblingScanOutcome o = r.run();
    TEST_ASSERT_EQUAL(9, o.result);
    TEST_ASSERT_EQUAL(refusal, o.firstRc);
    TEST_ASSERT_EQUAL(2, o.attempts);
    TEST_ASSERT_EQUAL_UINT32(150 + kSiblingScanRetryGapMs + kRealScanMs, o.ms);
    TEST_ASSERT_TRUE(o.ms <= 3000);   // inside the ~3 s first-boot budget
  }
}

static void test_scan_loop_first_attempt_ok_never_retries() {
  // A real sweep - including an empty one - is taken at its word, first time.
  for (int n : {0, 3}) {
    FakeRadio r{{n}, {kRealScanMs}};
    const SiblingScanOutcome o = r.run();
    TEST_ASSERT_EQUAL(n, o.result);
    TEST_ASSERT_EQUAL(n, o.firstRc);
    TEST_ASSERT_EQUAL(1, o.attempts);
    TEST_ASSERT_EQUAL(1, r.calls);
  }
}

static void test_scan_loop_always_refused_terminates_in_window() {
  // Class property over both refusal shapes and every per-attempt duration a
  // refusal could take: the loop always ends, never STARTS an attempt past the
  // window, stays inside window + one attempt, and hands back the refusal (so the
  // boot log shows the name was picked without seeing siblings).
  for (int refusal : {kScanFailed, kScanRunning, 0}) {
    const uint32_t maxD = refusal == 0 ? kSiblingScanMinRealMs - 1 : 3000;
    for (uint32_t d = 0; d <= maxD; d += 7) {
      FakeRadio r{{refusal}, {d}};
      const uint32_t t0 = r.clock;
      const SiblingScanOutcome o = r.run();
      TEST_ASSERT_EQUAL(refusal, o.result);
      TEST_ASSERT_TRUE(o.attempts >= 1);
      TEST_ASSERT_TRUE(r.lastStart - t0 <= kSiblingScanWindowMs);
      TEST_ASSERT_TRUE(o.ms <= kSiblingScanWindowMs + d);
      // An instant refusal can retry at most window/gap more times.
      TEST_ASSERT_TRUE(o.attempts <= 1 + int(kSiblingScanWindowMs / kSiblingScanRetryGapMs));
    }
  }
}

static void test_scan_loop_slow_failure_is_not_retried() {
  // A scan the core gave up on after its own long timeout is not repeated: first
  // boot must not stall for another full timeout.
  FakeRadio r{{kScanFailed}, {60000}};
  const SiblingScanOutcome o = r.run();
  TEST_ASSERT_EQUAL(1, o.attempts);
  TEST_ASSERT_EQUAL(kScanFailed, o.result);
}

// ---- makeSetupPass -----------------------------------------------------------
static uint32_t s_seq;
static uint32_t seqRnd() { return s_seq++; }

static void test_setup_pass_shape() {
  s_seq = 0;
  const std::string p = makeSetupPass(seqRnd);
  TEST_ASSERT_EQUAL(kSetupPassLen, (int)p.size());
  TEST_ASSERT_TRUE(kSetupPassLen >= 8);  // WPA2 floor - softAP silently opens below it
  // Sequential rnd 0..9 walks the alphabet head: proves the mapping is the
  // documented alphabet, not some accidental reordering.
  TEST_ASSERT_EQUAL_STRING("abcdefghij", p.c_str());
}

static void test_setup_pass_alphabet_unambiguous() {
  // Sweep every symbol the generator can emit; none may be ambiguous (0/o/1/l)
  // and all must be lowercase alphanumeric.
  s_seq = 0;
  const std::string all = makeSetupPass(seqRnd) + makeSetupPass(seqRnd) +
                          makeSetupPass(seqRnd) + makeSetupPass(seqRnd);  // rnd 0..39 > 32
  for (char c : all) {
    TEST_ASSERT_TRUE(std::isalnum((unsigned char)c));
    TEST_ASSERT_TRUE(!std::isupper((unsigned char)c));
    TEST_ASSERT_TRUE(c != '0' && c != 'o' && c != '1' && c != 'l');
  }
}

// ---- wifiQrPayload -----------------------------------------------------------
static void test_wifi_qr_payload_basic() {
  TEST_ASSERT_EQUAL_STRING("WIFI:S:Nimbus-setup;T:WPA;P:abcdef2345;;",
      wifiQrPayload("Nimbus-setup", "abcdef2345").c_str());
}

static void test_wifi_qr_payload_open_and_empty() {
  TEST_ASSERT_EQUAL_STRING("WIFI:S:Nimbus-setup;T:nopass;;",
      wifiQrPayload("Nimbus-setup", "").c_str());
  TEST_ASSERT_EQUAL_STRING("", wifiQrPayload("", "whatever").c_str());
}

static void test_wifi_qr_payload_escapes() {
  // The de-facto WIFI: spec backslash-escapes \ ; , : "
  TEST_ASSERT_EQUAL_STRING("WIFI:S:a\\;b\\:c\\,d;T:WPA;P:p\\\\q\\\"r;;",
      wifiQrPayload("a;b:c,d", "p\\q\"r").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sanitize_passthrough);
  RUN_TEST(test_sanitize_strips_junk);
  RUN_TEST(test_sanitize_caps_length);
  RUN_TEST(test_mdns_label_basic);
  RUN_TEST(test_mdns_label_trims_dashes);
  RUN_TEST(test_pick_no_siblings);
  RUN_TEST(test_pick_second_device);
  RUN_TEST(test_pick_lowest_free_gap);
  RUN_TEST(test_pick_third_device);
  RUN_TEST(test_pick_matches_bare_names_too);
  RUN_TEST(test_pick_ignores_lookalikes);
  RUN_TEST(test_pick_dedupes_and_bounds);
  RUN_TEST(test_scan_retry_real_result_is_final);
  RUN_TEST(test_scan_retry_refusal_inside_window);
  RUN_TEST(test_scan_retry_refusal_outside_window);
  RUN_TEST(test_scan_loop_bench_repro_refused_then_real);
  RUN_TEST(test_scan_loop_first_attempt_ok_never_retries);
  RUN_TEST(test_scan_loop_always_refused_terminates_in_window);
  RUN_TEST(test_scan_loop_slow_failure_is_not_retried);
  RUN_TEST(test_setup_pass_shape);
  RUN_TEST(test_setup_pass_alphabet_unambiguous);
  RUN_TEST(test_wifi_qr_payload_basic);
  RUN_TEST(test_wifi_qr_payload_open_and_empty);
  RUN_TEST(test_wifi_qr_payload_escapes);
  return UNITY_END();
}
