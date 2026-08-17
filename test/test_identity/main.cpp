// Host tests for nimbus::identity - the device-name sanitizer, mDNS label
// derivation, and the first-boot sibling auto-numbering (P2 of the agent-3
// revamp plan). Pure string logic; the NVS/WiFi glue is device-side.
#include <unity.h>

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
  RUN_TEST(test_setup_pass_shape);
  RUN_TEST(test_setup_pass_alphabet_unambiguous);
  RUN_TEST(test_wifi_qr_payload_basic);
  RUN_TEST(test_wifi_qr_payload_open_and_empty);
  RUN_TEST(test_wifi_qr_payload_escapes);
  return UNITY_END();
}
