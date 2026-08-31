#include <unity.h>

#include <cstdio>

#include <string>

#include "nimbus/wifi/copy.h"

using nimbus::wifi::buildScanRows;
using nimbus::wifi::deviceUrl;
using nimbus::wifi::forgetRowLabel;
using nimbus::wifi::KnownNet;
using nimbus::wifi::LinkState;
using nimbus::wifi::LinkView;
using nimbus::wifi::netStatusLine;
using nimbus::wifi::ScanHit;
using nimbus::wifi::scanRowLabel;
using nimbus::wifi::wifiRowLabel;

void setUp() {}
void tearDown() {}

static bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}
static bool printableAscii(const std::string& s) {
  for (char c : s)
    if (c < 0x20 || c >= 0x7F) return false;
  return true;
}

static LinkView view(LinkState st) {
  LinkView v;
  v.state = st;
  v.apUp = true;
  v.apSsid = "Nimbus-3-setup";
  v.mdnsName = "nimbus-3.local";
  v.apIp = "192.168.4.1";
  v.knownCount = 1;
  return v;
}

// ---- deviceUrl: the QR-to-nowhere bug ---------------------------------------

// A failed access point reports 0.0.0.0, which encodes into a perfectly valid QR
// that resolves to nothing. Showing nothing is honest; showing a confident QR is not.
static void test_device_url_refuses_0_0_0_0() {
  TEST_ASSERT_EQUAL_STRING("", deviceUrl("0.0.0.0", "tok").c_str());
}

static void test_device_url_refuses_empty_ip() {
  TEST_ASSERT_EQUAL_STRING("", deviceUrl("", "tok").c_str());
}

static void test_device_url_valid() {
  // CUM-45: the QR now carries a sign-in CODE as ?c=, never a bearer token as ?t=.
  TEST_ASSERT_EQUAL_STRING("http://192.168.4.1/?c=abc",
                           deviceUrl("192.168.4.1", "abc").c_str());
}

static void test_device_url_without_code() {
  TEST_ASSERT_EQUAL_STRING("http://10.0.0.5/", deviceUrl("10.0.0.5", "").c_str());
}

// ---- netStatusLine: the wrong-network-name bug ------------------------------

// The status line used to print the compile-time NIMBUS_AP_SSID ("Nimbus-setup"),
// so a board named Nimbus-3 told you to join a network that does not exist - on the
// one screen you look at when you are locked out.
static void test_net_status_uses_the_live_ap_ssid_not_the_macro() {
  LinkView v = view(LinkState::Unreachable);
  const std::string s = netStatusLine(v);
  TEST_ASSERT_TRUE(has(s, "Nimbus-3-setup"));
  TEST_ASSERT_FALSE(s == "Nimbus-setup");
  TEST_ASSERT_FALSE(has(s, "Nimbus-setup "));   // never the bare compile-time default
}

static void test_net_status_spells_wi_fi_with_a_hyphen() {
  for (LinkState st : {LinkState::Online, LinkState::Joining, LinkState::Scanning,
                       LinkState::Unreachable, LinkState::Idle}) {
    LinkView v = view(st);
    v.ssid = "Home";
    const std::string s = netStatusLine(v);
    TEST_ASSERT_FALSE(has(s, "WiFi"));    // style guide: never "WiFi"
    TEST_ASSERT_FALSE(has(s, "wifi"));
  }
}

static void test_net_status_is_ascii_and_bounded_in_every_state() {
  for (LinkState st : {LinkState::Online, LinkState::Joining, LinkState::Scanning,
                       LinkState::Unreachable, LinkState::Idle}) {
    for (int known : {0, 1}) {
      for (int apUp : {0, 1}) {
        LinkView v = view(st);
        v.knownCount = known;
        v.apUp = apUp != 0;
        v.ssid = "A-Very-Long-Network-Name-That-Goes-On-And-On";
        v.apSsid = "An-Absurdly-Long-Device-Name-setup";
        v.mdnsName = "an-absurdly-long-device-name.local";
        v.rssi = -52;
        const std::string s = netStatusLine(v);
        TEST_ASSERT_TRUE(s.size() <= 48);
        TEST_ASSERT_TRUE(printableAscii(s));
      }
    }
  }
}

// CUM-190: the setup AP self-recovers, so the AP-down line must announce the
// recovery and the one next step - never instruct a physical restart of the device
// (an onboarding wall on the flagship first-run flow). It states what is happening,
// then the next step.
static void test_net_status_ap_down_announces_recovery_not_a_restart() {
  LinkView v = view(LinkState::Unreachable);
  v.apUp = false;
  const std::string s = netStatusLine(v);
  TEST_ASSERT_FALSE(has(s, "Restart the device"));
  TEST_ASSERT_FALSE(has(s, "restart the device"));
  TEST_ASSERT_TRUE(has(s, "restarting"));   // says what is happening
  TEST_ASSERT_TRUE(has(s, "Reconnect"));    // the one next step
}

// A TFT intentionally drops its temporary setup hotspot after joining the LAN.
// That healthy state must never be rendered as a failure.
static void test_online_outranks_expected_ap_shutdown() {
  LinkView v = view(LinkState::Online);
  v.apUp = false;
  v.staIp = "192.0.2.10";
  v.rssi = -40;
  const std::string s = netStatusLine(v);
  TEST_ASSERT_TRUE(has(s, "Home Wi-Fi connected"));
  TEST_ASSERT_TRUE(has(s, "192.0.2.10"));
  TEST_ASSERT_FALSE(has(s, "down"));
}

static void test_net_status_unset_vs_unreachable_differ() {
  LinkView unset = view(LinkState::Unreachable);
  unset.knownCount = 0;
  LinkView unreach = view(LinkState::Unreachable);
  unreach.knownCount = 2;
  TEST_ASSERT_TRUE(has(netStatusLine(unset), "not set up"));
  TEST_ASSERT_TRUE(has(netStatusLine(unreach), "No known Wi-Fi"));
}

static void test_net_status_hold_reports_remaining_time() {
  LinkView v = view(LinkState::Unreachable);
  v.apHoldSecLeft = 840;                       // 14 min
  const std::string s = netStatusLine(v);
  TEST_ASSERT_TRUE(has(s, "Setup network on"));
  TEST_ASSERT_TRUE(has(s, "14 min"));
}

static void test_net_status_online_reports_reachable_address() {
  LinkView v = view(LinkState::Online);
  v.staIp = "192.0.2.10";
  v.rssi = -52;
  const std::string s = netStatusLine(v);
  TEST_ASSERT_TRUE(has(s, "Home Wi-Fi connected"));
  TEST_ASSERT_TRUE(has(s, "192.0.2.10"));
}

// ---- row labels --------------------------------------------------------------

static void test_wifi_row_label_per_state() {
  LinkView on = view(LinkState::Online);
  on.staIp = "192.0.2.10";
  TEST_ASSERT_TRUE(has(wifiRowLabel(on), "192.0.2.10"));

  LinkView join = view(LinkState::Joining);
  join.ssid = "TestNet";
  TEST_ASSERT_TRUE(has(wifiRowLabel(join), "joining TestNet"));

  TEST_ASSERT_TRUE(has(wifiRowLabel(view(LinkState::Scanning)), "scanning"));

  LinkView unset = view(LinkState::Unreachable);
  unset.knownCount = 0;
  TEST_ASSERT_TRUE(has(wifiRowLabel(unset), "not set up"));

  LinkView held = view(LinkState::Unreachable);
  held.apHoldSecLeft = 600;
  TEST_ASSERT_TRUE(has(wifiRowLabel(held), "setup network published"));
}

// Every row is a submenu entry, so it must carry the affordance.
static void test_wifi_row_label_always_marks_a_submenu() {
  for (LinkState st : {LinkState::Online, LinkState::Joining, LinkState::Scanning,
                       LinkState::Unreachable}) {
    const std::string s = wifiRowLabel(view(st));
    TEST_ASSERT_TRUE(s.size() >= 1 && s[s.size() - 1] == '>');
    TEST_ASSERT_TRUE(s.size() <= 44);
  }
}

static void test_scan_row_shows_signal_and_saved_marker() {
  ScanHit h;
  h.ssid = "TestNet";
  h.rssi = -45;
  h.locked = true;
  TEST_ASSERT_TRUE(has(scanRowLabel(h, /*known=*/true), "saved"));
  TEST_ASSERT_TRUE(has(scanRowLabel(h, true), "-45"));
  TEST_ASSERT_TRUE(has(scanRowLabel(h, /*known=*/false), "lock"));
  h.locked = false;
  const std::string open = scanRowLabel(h, false);
  TEST_ASSERT_FALSE(has(open, "lock"));
  TEST_ASSERT_FALSE(has(open, "saved"));
}

// The renderer clips silently, so a long SSID must be truncated HERE or it would
// push the signal strength off the panel with no indication anything was lost.
static void test_scan_row_truncates_a_long_ssid() {
  ScanHit h;
  h.ssid = "This-Network-Name-Is-Far-Too-Long-To-Fit-On-The-Panel";
  h.rssi = -80;
  const std::string s = scanRowLabel(h, false);
  TEST_ASSERT_TRUE(s.size() <= 42);
  TEST_ASSERT_TRUE(has(s, "-80"));           // the signal survived the truncation
  TEST_ASSERT_TRUE(printableAscii(s));
}

// SSIDs are arbitrary bytes off the air; the panel font is ASCII only.
static void test_non_ascii_ssid_is_sanitised_not_passed_through() {
  ScanHit h;
  h.ssid = "Caf\xC3\xA9 \xF0\x9F\x93\xB6";
  h.rssi = -60;
  const std::string s = scanRowLabel(h, false);
  TEST_ASSERT_TRUE(printableAscii(s));
  LinkView v = view(LinkState::Joining);
  v.ssid = h.ssid;
  TEST_ASSERT_TRUE(printableAscii(netStatusLine(v)));
}

static void test_forget_row_marks_the_network_in_use() {
  KnownNet n;
  n.ssid = "Home";
  TEST_ASSERT_TRUE(has(forgetRowLabel(n, /*current=*/true), "in use"));
  TEST_ASSERT_FALSE(has(forgetRowLabel(n, /*current=*/false), "in use"));
  TEST_ASSERT_TRUE(forgetRowLabel(n, true).size() <= 42);
}

// The row label is COMPLETE - it carries its own "Wi-Fi: " and its own chevron.
// A caller that prefixes it again produces "Wi-Fi: Wi-Fi: 192.168.1.5 >", which
// is exactly what shipped on the Connectivity screen of every device until
// prism caught it: no golden covered the composed row, so CI stayed green while
// the panel was wrong. This pins the contract so the next caller cannot guess.
static void test_row_label_is_complete_not_a_fragment() {
  LinkView v;
  v.state = LinkState::Online;
  v.staIp = "192.168.1.5";
  const std::string row = wifiRowLabel(v);

  // Exactly ONE label, and it is at the very start.
  TEST_ASSERT_EQUAL_STRING("Wi-Fi: ", row.substr(0, 7).c_str());
  TEST_ASSERT_TRUE(row.find("Wi-Fi: ", 7) == std::string::npos);
  // ...and it already ends in the chevron, so a caller must not add one.
  TEST_ASSERT_TRUE(row.size() >= 2 && row.compare(row.size() - 2, 2, " >") == 0);

  // Every other state carries the label too - a caller cannot special-case one.
  const LinkState others[] = {LinkState::Joining, LinkState::Scanning,
                              LinkState::Idle, LinkState::Unreachable};
  for (LinkState st : others) {
    LinkView w; w.state = st; w.ssid = "Home";
    const std::string r = wifiRowLabel(w);
    TEST_ASSERT_EQUAL_STRING("Wi-Fi: ", r.substr(0, 7).c_str());
    TEST_ASSERT_TRUE(r.find("Wi-Fi: ", 7) == std::string::npos);
  }
}

// ---- buildScanRows: the on-device picker ordering (CUM-48) ------------------
static ScanHit hit(const char* ssid, int rssi, bool locked = true) {
  ScanHit h; h.ssid = ssid; h.rssi = (int8_t)rssi; h.locked = locked; return h;
}
static KnownNet saved(const char* ssid) { KnownNet n; n.ssid = ssid; return n; }
static bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

static void test_scan_rows_saved_first_then_by_signal() {
  // Two saved, three unsaved, mixed signal. Saved must lead (each block strongest-first).
  std::vector<ScanHit> scan = { hit("Weak", -80), hit("Strong", -40), hit("HomeA", -70),
                                hit("HomeB", -55), hit("Guest", -50) };
  std::vector<KnownNet> known = { saved("HomeA"), saved("HomeB") };
  auto rows = buildScanRows(scan, known);
  TEST_ASSERT_EQUAL_INT(5, (int)rows.size());
  TEST_ASSERT_TRUE(starts(rows[0], "HomeB"));   // saved, stronger of the two saved
  TEST_ASSERT_TRUE(starts(rows[1], "HomeA"));   // saved
  TEST_ASSERT_TRUE(starts(rows[2], "Strong"));  // unsaved, strongest
  TEST_ASSERT_TRUE(starts(rows[3], "Guest"));
  TEST_ASSERT_TRUE(starts(rows[4], "Weak"));
  // saved rows carry the "saved" cue, unsaved do not.
  TEST_ASSERT_TRUE(rows[0].find("saved") != std::string::npos);
  TEST_ASSERT_TRUE(rows[2].find("saved") == std::string::npos);
}

static void test_scan_rows_collapse_duplicate_ssids() {
  // One network on two bands -> one row, at its strongest signal.
  std::vector<ScanHit> scan = { hit("Dual", -70), hit("Dual", -45), hit("Other", -60) };
  auto rows = buildScanRows(scan, {});
  TEST_ASSERT_EQUAL_INT(2, (int)rows.size());
  TEST_ASSERT_TRUE(starts(rows[0], "Dual"));           // strongest sighting wins the sort
  TEST_ASSERT_TRUE(rows[0].find("-45") != std::string::npos);
}

static void test_scan_rows_dedup_never_downgrades_lock() {
  // Same SSID seen encrypted on one band and (spuriously) open on another: the merged
  // row must stay locked - never present a secured network as open.
  std::vector<ScanHit> scan = { hit("Home", -60, /*locked=*/true), hit("Home", -50, /*locked=*/false) };
  auto rows = buildScanRows(scan, {});
  TEST_ASSERT_EQUAL_INT(1, (int)rows.size());
  TEST_ASSERT_TRUE(rows[0].find("lock") != std::string::npos);   // stays locked
}

static void test_scan_rows_mark_hidden_networks() {
  std::vector<ScanHit> scan = { hit("Named", -50), hit("", -60), hit("", -75) };
  auto rows = buildScanRows(scan, {});
  TEST_ASSERT_EQUAL_INT(2, (int)rows.size());          // one named + one hidden marker
  TEST_ASSERT_TRUE(starts(rows[0], "Named"));
  TEST_ASSERT_TRUE(rows[1].find("hidden") != std::string::npos);   // hidden is marked, and last
  TEST_ASSERT_TRUE(rows[1].find("2") != std::string::npos);        // both counted
}

static void test_scan_rows_empty_scan_is_empty_list() {
  TEST_ASSERT_EQUAL_INT(0, (int)buildScanRows({}, {}).size());
}

// ---- first-run setup instructions (CUM-259 / CUM-260) -----------------------
// printableAscii() (above) doubles as the em-dash guard: U+2014's UTF-8 bytes are
// all >= 0x80, so any em dash in user copy fails the ASCII check. setupSteps is
// multi-line on purpose (the panel renderer breaks on '\n'), so it is checked with
// a newline-tolerant variant that still catches any non-ASCII byte (em dash included).
static bool asciiAllowNewline(const std::string& s) {
  for (char c : s)
    if (c != '\n' && (c < 0x20 || c >= 0x7F)) return false;
  return true;
}

// The SetupInfo screen must LEAD WITH THE SEQUENCE, not just the facts: the owner who
// joined the hotspot was left with no idea what to do next. Steps 1 and 2, the join
// method, and the exact address must all be present, and it must be device-safe ASCII.
static void test_setup_steps_lead_with_the_sequence() {
  const std::string s =
      nimbus::wifi::setupSteps("Nimbus-4-setup", "http://192.168.4.1");
  TEST_ASSERT_TRUE(has(s, "1. Join"));
  TEST_ASSERT_TRUE(has(s, "Nimbus-4-setup"));
  TEST_ASSERT_TRUE(has(s, "scan"));                 // names the join method (the QR)
  TEST_ASSERT_TRUE(has(s, "2. Open"));
  TEST_ASSERT_TRUE(has(s, "http://192.168.4.1"));   // the next step - the missing piece
  TEST_ASSERT_TRUE(asciiAllowNewline(s));
  // An empty address must still name the AP default, never print an empty step.
  TEST_ASSERT_TRUE(has(nimbus::wifi::setupSteps("", ""), "192.168.4.1"));
}

static void test_setup_fallback_names_the_manual_address() {
  const std::string s = nimbus::wifi::setupFallbackLine("http://192.168.4.1");
  TEST_ASSERT_TRUE(has(s, "If nothing opens"));
  TEST_ASSERT_TRUE(has(s, "http://192.168.4.1"));
  TEST_ASSERT_TRUE(printableAscii(s));
}

// The class rule for CUM-260 leg 1: the OS captive-detection probe paths the device
// must answer with the portal. A 404 (or the OS's expected success response) on any
// of these tells the OS there is NO portal and nothing pops. So the table over the
// KNOWN probes must all classify as captive; a genuine app path must not; and the
// match ignores case and any query string. A new OS probe left out of the table is a
// silent miss - this iterates the whole set so adding one here is deliberate.
static void test_captive_probe_table_covers_every_known_os() {
  const char* probes[] = {
      "/hotspot-detect.html",        // iOS / macOS
      "/library/test/success.html",  // iOS / macOS (older)
      "/generate_204",               // Android / ChromeOS
      "/gen_204",                    // Android (short)
      "/ncsi.txt",                   // Windows NCSI
      "/connecttest.txt",            // Windows 10+
      "/canonical.html",             // Firefox
      "/success.txt",                // Firefox / NetworkManager
      "/check_network_status.txt",   // NetworkManager
      "/kindle-wifi/wifistub.html",  // Kindle
  };
  for (const char* p : probes) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "probe %s must be served the captive page", p);
    TEST_ASSERT_TRUE_MESSAGE(nimbus::wifi::isCaptiveProbePath(p), msg);
  }
  // Case-insensitive and query-tolerant (an OS may append cache-busters).
  TEST_ASSERT_TRUE(nimbus::wifi::isCaptiveProbePath("/Generate_204"));
  TEST_ASSERT_TRUE(nimbus::wifi::isCaptiveProbePath("/generate_204?t=123"));
  // A real application path is NOT a probe - it must reach its own handler.
  TEST_ASSERT_FALSE(nimbus::wifi::isCaptiveProbePath("/api/state"));
  TEST_ASSERT_FALSE(nimbus::wifi::isCaptiveProbePath("/"));
}

// The captive landing page must always state the next step (a mini-browser cannot run
// the setup app), name the manual address, carry the authenticated link, and never
// accidentally emit Apple's "Success" page (which would tell iOS there is NO portal).
static void test_captive_landing_states_next_step() {
  const std::string html =
      nimbus::wifi::captiveLandingHtml("Nimbus-setup", "/?t=abcd", "192.168.4.1");
  TEST_ASSERT_TRUE(has(html, "Nimbus-setup"));
  TEST_ASSERT_TRUE(has(html, "192.168.4.1"));       // the manual fallback address
  TEST_ASSERT_TRUE(has(html, "href=\"/?t=abcd\""));  // the way in, authenticated
  TEST_ASSERT_TRUE(printableAscii(html));
  TEST_ASSERT_FALSE(has(html, "<TITLE>Success</TITLE>"));  // never the Apple success page

  // The AP SSID is the owner-set device name: it must be HTML-escaped, never able to
  // inject a tag into the page served on the setup AP.
  const std::string evil =
      nimbus::wifi::captiveLandingHtml("Ni<script>x", "/?t=x", "192.168.4.1");
  TEST_ASSERT_FALSE(has(evil, "<script>"));            // the injected tag never survives raw
  TEST_ASSERT_TRUE(has(evil, "Ni&lt;script&gt;x"));    // it is escaped instead
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_row_label_is_complete_not_a_fragment);
  RUN_TEST(test_device_url_refuses_0_0_0_0);
  RUN_TEST(test_device_url_refuses_empty_ip);
  RUN_TEST(test_device_url_valid);
  RUN_TEST(test_device_url_without_code);
  RUN_TEST(test_net_status_uses_the_live_ap_ssid_not_the_macro);
  RUN_TEST(test_net_status_spells_wi_fi_with_a_hyphen);
  RUN_TEST(test_net_status_is_ascii_and_bounded_in_every_state);
  RUN_TEST(test_net_status_ap_down_announces_recovery_not_a_restart);
  RUN_TEST(test_online_outranks_expected_ap_shutdown);
  RUN_TEST(test_net_status_unset_vs_unreachable_differ);
  RUN_TEST(test_net_status_hold_reports_remaining_time);
  RUN_TEST(test_net_status_online_reports_reachable_address);
  RUN_TEST(test_wifi_row_label_per_state);
  RUN_TEST(test_wifi_row_label_always_marks_a_submenu);
  RUN_TEST(test_scan_row_shows_signal_and_saved_marker);
  RUN_TEST(test_scan_row_truncates_a_long_ssid);
  RUN_TEST(test_non_ascii_ssid_is_sanitised_not_passed_through);
  RUN_TEST(test_forget_row_marks_the_network_in_use);
  RUN_TEST(test_scan_rows_saved_first_then_by_signal);
  RUN_TEST(test_scan_rows_collapse_duplicate_ssids);
  RUN_TEST(test_scan_rows_dedup_never_downgrades_lock);
  RUN_TEST(test_scan_rows_mark_hidden_networks);
  RUN_TEST(test_scan_rows_empty_scan_is_empty_list);
  RUN_TEST(test_setup_steps_lead_with_the_sequence);
  RUN_TEST(test_setup_fallback_names_the_manual_address);
  RUN_TEST(test_captive_probe_table_covers_every_known_os);
  RUN_TEST(test_captive_landing_states_next_step);
  return UNITY_END();
}
