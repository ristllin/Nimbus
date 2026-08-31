#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/wifi/policy.h"

using nimbus::wifi::Act;
using nimbus::wifi::Action;
using nimbus::wifi::classifyReason;
using nimbus::wifi::Inputs;
using nimbus::wifi::JoinFail;
using nimbus::wifi::KnownNet;
using nimbus::wifi::LinkState;
using nimbus::wifi::linkStateName;
using nimbus::wifi::PolicyCfg;
using nimbus::wifi::ScanHit;
using nimbus::wifi::WifiPolicy;

void setUp() {}
void tearDown() {}

static KnownNet mk(const char* ssid, const char* pass = "pw") {
  KnownNet n;
  n.ssid = ssid;
  n.pass = pass;
  return n;
}
static ScanHit hit(const char* ssid, int rssi) {
  ScanHit h;
  h.ssid = ssid;
  h.rssi = (int8_t)rssi;
  return h;
}

// A policy that has already settled into Unreachable with one known network that is
// NOT visible - i.e. exactly the board-in-hand situation.
static void toUnreachable(WifiPolicy& p, uint32_t& now, int knownCount = 1) {
  Inputs in;
  in.nowMs = now;
  in.knownCount = knownCount;
  Action a = p.tick(in);                     // Idle -> Scanning (StartScan)
  TEST_ASSERT_EQUAL((int)Act::StartScan, (int)a.kind);
  p.noteScanResults({hit("SomeoneElse", -40)}, now);   // nothing we know
  in.nowMs = (now += 10);
  a = p.tick(in);                            // Scanning -> Unreachable
  TEST_ASSERT_EQUAL((int)LinkState::Unreachable, (int)p.state());
}

// ---- reason classification --------------------------------------------------

static void test_classify_reason_table() {
  TEST_ASSERT_EQUAL((int)JoinFail::NotFound,      (int)classifyReason(201));
  TEST_ASSERT_EQUAL((int)JoinFail::AuthReject,    (int)classifyReason(2));
  TEST_ASSERT_EQUAL((int)JoinFail::AuthReject,    (int)classifyReason(15));
  TEST_ASSERT_EQUAL((int)JoinFail::AuthReject,    (int)classifyReason(202));
  TEST_ASSERT_EQUAL((int)JoinFail::AuthReject,    (int)classifyReason(205));
  TEST_ASSERT_EQUAL((int)JoinFail::SelfInitiated, (int)classifyReason(8));
  TEST_ASSERT_EQUAL((int)JoinFail::Transient,     (int)classifyReason(200));
  TEST_ASSERT_EQUAL((int)JoinFail::Transient,     (int)classifyReason(4));
}

static void test_link_state_names() {
  TEST_ASSERT_EQUAL_STRING("idle",        linkStateName(LinkState::Idle));
  TEST_ASSERT_EQUAL_STRING("scanning",    linkStateName(LinkState::Scanning));
  TEST_ASSERT_EQUAL_STRING("joining",     linkStateName(LinkState::Joining));
  TEST_ASSERT_EQUAL_STRING("online",      linkStateName(LinkState::Online));
  TEST_ASSERT_EQUAL_STRING("unreachable", linkStateName(LinkState::Unreachable));
}

// ---- the unprovisioned device -----------------------------------------------

static void test_empty_known_goes_unreachable_immediately() {
  WifiPolicy p;
  Inputs in;
  in.nowMs = 1000;
  in.knownCount = 0;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::EnterUnreachable, (int)a.kind);
  TEST_ASSERT_EQUAL((int)LinkState::Unreachable, (int)p.state());
  TEST_ASSERT_TRUE(a.stateChanged);
}

// A device with no credentials must never scan - that is the historical behaviour
// (WiFi.begin() was simply never called) and regressing it would starve the AP on a
// brand-new device, which is the worst possible moment.
static void test_empty_known_never_scans_over_24h() {
  WifiPolicy p;
  Inputs in;
  in.knownCount = 0;
  uint32_t now = 0;
  int scans = 0;
  for (int i = 0; i < 24 * 60 * 4; i++) {   // 24 h at 4 ticks/min
    in.nowMs = (now += 15000);
    if (p.tick(in).kind == Act::StartScan) scans++;
  }
  TEST_ASSERT_EQUAL(0, scans);
}

// ---- scan-then-match ---------------------------------------------------------

static void test_idle_with_known_starts_one_scan() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  Inputs in;
  in.nowMs = 500;
  in.knownCount = 1;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::StartScan, (int)a.kind);
  TEST_ASSERT_EQUAL((int)LinkState::Scanning, (int)p.state());
}

static void test_scan_hit_joins_strongest_known_with_its_password() {
  WifiPolicy p;
  p.setKnown({mk("Weak", "pw-weak"), mk("Strong", "pw-strong")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 2;
  p.tick(in);
  p.noteScanResults({hit("Weak", -80), hit("Unknown", -20), hit("Strong", -40)}, 0);
  in.nowMs = 10;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::Join, (int)a.kind);
  TEST_ASSERT_EQUAL_STRING("Strong", a.ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("pw-strong", a.pass.c_str());   // device never re-indexes
  TEST_ASSERT_EQUAL((int)LinkState::Joining, (int)p.state());
}

static void test_scan_no_match_goes_unreachable() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  uint32_t now = 0;
  toUnreachable(p, now);
}

static void test_scan_timeout_goes_unreachable() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  p.tick(in);                      // -> Scanning, results never arrive
  in.nowMs = 12001;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::EnterUnreachable, (int)a.kind);
}

// ---- join failure handling ---------------------------------------------------

static void test_auth_failure_does_not_retry_the_same_network() {
  WifiPolicy p;
  p.setKnown({mk("Bad", "wrong"), mk("Good", "right")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 2;
  p.tick(in);
  p.noteScanResults({hit("Bad", -30), hit("Good", -60)}, 0);
  in.nowMs = 10;
  Action a = p.tick(in);
  TEST_ASSERT_EQUAL_STRING("Bad", a.ssid.c_str());

  in.nowMs = 100;
  in.disconnected = true;
  in.lastReason = 15;              // wrong PSK
  a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::Join, (int)a.kind);
  TEST_ASSERT_EQUAL_STRING("Good", a.ssid.c_str());   // advanced, did NOT retry Bad
}

static void test_notfound_advances_immediately() {
  WifiPolicy p;
  p.setKnown({mk("Gone"), mk("Here")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 2;
  p.tick(in);
  p.noteScanResults({hit("Gone", -30), hit("Here", -60)}, 0);
  in.nowMs = 10;
  p.tick(in);
  in.nowMs = 50;
  in.disconnected = true;
  in.lastReason = 201;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL_STRING("Here", a.ssid.c_str());
}

static void test_transient_retries_once_then_advances() {
  WifiPolicy p;
  p.setKnown({mk("A"), mk("B")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 2;
  p.tick(in);
  p.noteScanResults({hit("A", -30), hit("B", -60)}, 0);
  in.nowMs = 10;
  p.tick(in);
  in.nowMs = 50;
  in.disconnected = true;
  in.lastReason = 200;                     // transient
  Action a = p.tick(in);
  TEST_ASSERT_EQUAL_STRING("A", a.ssid.c_str());   // retry #2 on the SAME network
  in.nowMs = 100;
  a = p.tick(in);
  TEST_ASSERT_EQUAL_STRING("B", a.ssid.c_str());   // budget spent -> advance
}

// Reason 8 is our OWN disconnectAsync(). Counting it would burn an attempt every
// time the machine deliberately drops the link.
static void test_reason_8_is_never_a_failure() {
  WifiPolicy p;
  p.setKnown({mk("A"), mk("B")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 2;
  p.tick(in);
  p.noteScanResults({hit("A", -30), hit("B", -60)}, 0);
  in.nowMs = 10;
  p.tick(in);
  in.nowMs = 50;
  in.disconnected = true;
  in.lastReason = 8;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::None, (int)a.kind);              // nothing consumed
  TEST_ASSERT_EQUAL((int)LinkState::Joining, (int)p.state());
  TEST_ASSERT_EQUAL((int)JoinFail::None, (int)p.lastFail());
}

static void test_candidates_exhausted_goes_unreachable() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  p.tick(in);
  p.noteScanResults({hit("A", -30)}, 0);
  in.nowMs = 10;
  p.tick(in);
  in.nowMs = 50;
  in.disconnected = true;
  in.lastReason = 201;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::EnterUnreachable, (int)a.kind);
}

// ---- mid-cycle known-list edit must not dereference stale candidates --------

// CUM-207 / F1: a Forget (or any list edit) while a join is in flight calls setKnown(),
// which clears the candidate list WITHOUT leaving Joining. A retry that then indexed the
// now-empty cands_ was an out-of-bounds read -> a stale Candidate whose knownIndex points
// past the shorter known list -> panic or a garbage-credential join, mid-recovery. The
// retry must fall to Unreachable when the candidate cursor is no longer in range. Asserted
// for BOTH retry paths: a Transient disconnect and a join timeout.
static void test_setknown_midjoin_then_transient_disconnect_is_safe() {
  WifiPolicy p;
  p.setKnown({mk("A"), mk("B")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 2;
  p.tick(in);
  p.noteScanResults({hit("A", -30), hit("B", -60)}, 0);
  in.nowMs = 10;
  Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::Join, (int)a.kind);   // Joining A, candIdx 0, cands has 2

  p.setKnown({mk("A")});                             // list edited mid-join -> cands_ cleared

  in.nowMs = 50;
  in.knownCount = 1;
  in.disconnected = true;
  in.lastReason = 200;                               // Transient: would retry the same candidate
  a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::EnterUnreachable, (int)a.kind);   // NOT a stale joinCandidate
  TEST_ASSERT_EQUAL((int)LinkState::Unreachable, (int)p.state());
}

static void test_setknown_midjoin_then_join_timeout_is_safe() {
  WifiPolicy p;
  p.setKnown({mk("A"), mk("B")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 2;
  p.tick(in);
  p.noteScanResults({hit("A", -30), hit("B", -60)}, 0);
  in.nowMs = 10;
  TEST_ASSERT_EQUAL((int)Act::Join, (int)p.tick(in).kind);

  p.setKnown({mk("A")});                             // list edited mid-join

  in.nowMs = 10 + 12001;                             // past the join timeout -> retry path
  in.knownCount = 1;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::EnterUnreachable, (int)a.kind);
  TEST_ASSERT_EQUAL((int)LinkState::Unreachable, (int)p.state());
}

// ---- THE invariant: Unreachable must not starve the AP ----------------------

// Named so it can never be quietly deleted. Back-to-back scanning in Unreachable is
// precisely the bug: it starves the setup AP's beacons and locks the owner out.
static void test_unreachable_never_scans_back_to_back() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  uint32_t now = 1000;
  toUnreachable(p, now);

  Inputs in;
  in.knownCount = 1;
  int scans = 0;
  uint32_t lastScanAt = 0, minGap = 0xFFFFFFFFu;
  for (int i = 0; i < 4 * 60 * 60; i++) {     // 1 h at 250 ms
    in.nowMs = (now += 250);
    if (p.tick(in).kind == Act::StartScan) {
      if (scans) {
        const uint32_t gap = now - lastScanAt;
        if (gap < minGap) minGap = gap;
      }
      lastScanAt = now;
      scans++;
      p.noteScanResults({hit("SomeoneElse", -40)}, now);   // still nothing known
    }
  }
  TEST_ASSERT_TRUE(scans > 0);                    // it keeps trying - not "give up"
  TEST_ASSERT_TRUE(scans < 30);                   // ...but nothing like continuously
  TEST_ASSERT_TRUE(minGap >= 30000);              // never tighter than the first backoff
}

static void test_unreachable_backoff_schedule_exact() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  uint32_t now = 0;
  toUnreachable(p, now);

  const uint32_t expect[] = {30000, 60000, 120000, 240000, 300000, 300000};
  Inputs in;
  in.knownCount = 1;
  for (int i = 0; i < 6; i++) {
    const uint32_t armed = now;
    // Nothing before the deadline.
    in.nowMs = armed + expect[i] - 250;
    TEST_ASSERT_EQUAL((int)Act::None, (int)p.tick(in).kind);
    // ...and a scan exactly at it.
    in.nowMs = armed + expect[i];
    TEST_ASSERT_EQUAL((int)Act::StartScan, (int)p.tick(in).kind);
    now = in.nowMs;
    p.noteScanResults({hit("Nope", -50)}, now);
    in.nowMs = (now += 10);
    p.tick(in);                                   // back to Unreachable, backoff doubles
  }
}

static void test_unreachable_defers_while_ap_client_present() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  uint32_t now = 0;
  toUnreachable(p, now);
  Inputs in;
  in.knownCount = 1;
  in.apStations = 1;                     // someone is configuring the device
  in.nowMs = now + 600000;               // long past any backoff
  TEST_ASSERT_EQUAL((int)Act::None, (int)p.tick(in).kind);
  in.apStations = 0;                     // they left
  in.nowMs += 250;
  TEST_ASSERT_EQUAL((int)Act::StartScan, (int)p.tick(in).kind);
}

// ---- the escape hatch --------------------------------------------------------

static void test_publish_ap_drops_sta_and_suppresses_every_scan() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  in.reqPublishAp = true;
  Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::DropSta, (int)a.kind);
  TEST_ASSERT_EQUAL((int)LinkState::Unreachable, (int)p.state());
  TEST_ASSERT_TRUE(p.apHoldSecLeft(0) > 0);

  in.reqPublishAp = false;
  uint32_t now = 0;
  for (int i = 0; i < 4 * 60 * 14; i++) {          // 14 min - inside the hold
    in.nowMs = (now += 250);
    TEST_ASSERT_EQUAL((int)Act::None, (int)p.tick(in).kind);
  }
}

// A permanent pin would be a NEW dead end (publish the AP, walk away, the device
// never rejoins). The hold must expire and the slow schedule resume.
static void test_ap_hold_expires_and_the_slow_schedule_resumes() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  in.reqPublishAp = true;
  p.tick(in);
  in.reqPublishAp = false;

  in.nowMs = 900000 + 1000;                        // past the 15-minute hold
  TEST_ASSERT_EQUAL(0u, p.apHoldSecLeft(in.nowMs));
  TEST_ASSERT_EQUAL((int)Act::StartScan, (int)p.tick(in).kind);
}

static void test_ap_hold_refreshes_while_a_station_is_associated() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  in.reqPublishAp = true;
  p.tick(in);
  in.reqPublishAp = false;
  in.apStations = 1;                                // phone stays connected
  in.nowMs = 900000 + 1000;
  p.tick(in);
  TEST_ASSERT_TRUE(p.apHoldSecLeft(in.nowMs) > 0);  // refreshed, not expired
}

// Isolates the HOLD from the backoff. Publishing the AP arms both, and they expire
// together, so a test that only walks that path proves nothing about the hold - it
// passes on the backoff alone (found by mutation-testing: deleting the hold guard
// left every other test green). Here the station's presence refreshes the hold past
// the retry deadline, then leaves, so the hold is the ONLY thing left blocking a scan.
static void test_ap_hold_alone_blocks_a_due_retry() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  in.reqPublishAp = true;
  p.tick(in);                       // hold until 900000; retry armed for 900000 too
  in.reqPublishAp = false;

  in.apStations = 1;                // a phone joins and stays a while...
  in.nowMs = 800000;
  p.tick(in);                       // ...which pushes the hold out to 1700000

  in.apStations = 0;                // they leave, so the ap-client guard is inactive
  in.nowMs = 900001;                // the retry deadline has PASSED
  TEST_ASSERT_TRUE(p.apHoldSecLeft(in.nowMs) > 0);
  TEST_ASSERT_EQUAL((int)Act::None, (int)p.tick(in).kind);   // only the hold stops this
}

static void test_cancel_hold_releases_the_quiet_window() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  in.reqPublishAp = true;
  p.tick(in);
  in.reqPublishAp = false;
  in.reqCancelHold = true;
  in.nowMs = 1000;
  p.tick(in);
  TEST_ASSERT_EQUAL(0u, p.apHoldSecLeft(1000));
}

// ---- direct join -------------------------------------------------------------

static void test_direct_join_bypasses_the_scan_and_clears_the_hold() {
  WifiPolicy p;
  p.setKnown({mk("A", "pa"), mk("B", "pb")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 2;
  in.reqPublishAp = true;
  p.tick(in);
  in.reqPublishAp = false;

  in.nowMs = 1000;
  in.reqJoinIndex = 1;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::Join, (int)a.kind);
  TEST_ASSERT_EQUAL_STRING("B", a.ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("pb", a.pass.c_str());
  TEST_ASSERT_EQUAL(0u, p.apHoldSecLeft(1000));
  TEST_ASSERT_EQUAL((int)LinkState::Joining, (int)p.state());
}

static void test_direct_join_out_of_range_index_is_ignored() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  in.reqJoinIndex = 7;
  const Action a = p.tick(in);
  TEST_ASSERT_TRUE(a.kind != Act::Join);
}

// ---- online / recovery -------------------------------------------------------

static void test_got_ip_goes_online_and_resets_backoff() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  p.tick(in);
  p.noteScanResults({hit("A", -40)}, 0);
  in.nowMs = 10;
  p.tick(in);
  in.nowMs = 500;
  in.gotIp = true;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)LinkState::Online, (int)p.state());
  TEST_ASSERT_TRUE(a.stateChanged);
  TEST_ASSERT_EQUAL_STRING("A", p.onlineSsid().c_str());
}

// A router blip must re-scan, never give up - this is what keeps flap-and-recover
// working.
static void test_online_drop_returns_to_scanning_not_unreachable() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  in.gotIp = true;
  p.tick(in);
  in.gotIp = false;
  in.staLinked = false;
  in.disconnected = true;
  in.lastReason = 200;
  in.nowMs = 1000;
  p.tick(in);                                   // grace starts
  in.disconnected = false;
  in.nowMs = 1000 + 5000;
  const Action a = p.tick(in);
  TEST_ASSERT_EQUAL((int)Act::StartScan, (int)a.kind);
  TEST_ASSERT_EQUAL((int)LinkState::Scanning, (int)p.state());
}

static void test_online_drop_honours_grace() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 1;
  in.gotIp = true;
  p.tick(in);
  in.gotIp = false;
  in.staLinked = false;
  in.disconnected = true;
  in.nowMs = 1000;
  p.tick(in);
  in.disconnected = false;
  in.nowMs = 1000 + 4000;                       // still inside the 5 s grace
  TEST_ASSERT_EQUAL((int)Act::None, (int)p.tick(in).kind);
}

// The core latches `first_connect` and forces one reconnect we never asked for.
// Rejecting it would leave the machine out of step with the real radio.
static void test_unsolicited_got_ip_is_accepted() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  uint32_t now = 0;
  toUnreachable(p, now);
  Inputs in;
  in.knownCount = 1;
  in.nowMs = now + 100;
  in.staLinked = true;                          // link appeared on its own
  p.tick(in);
  TEST_ASSERT_EQUAL((int)LinkState::Online, (int)p.state());
}

// ---- AP health + edges -------------------------------------------------------

static void test_ap_down_emits_reassert() {
  WifiPolicy p;
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 0;
  p.tick(in);                                   // -> Unreachable
  in.nowMs = 1000;
  in.apUp = false;
  TEST_ASSERT_EQUAL((int)Act::ReassertAp, (int)p.tick(in).kind);
}

static void test_state_changed_only_on_edges() {
  WifiPolicy p;
  Inputs in;
  in.nowMs = 0;
  in.knownCount = 0;
  TEST_ASSERT_TRUE(p.tick(in).stateChanged);    // Idle -> Unreachable
  in.nowMs = 250;
  TEST_ASSERT_FALSE(p.tick(in).stateChanged);   // still Unreachable
}

// ---- time safety -------------------------------------------------------------

static void test_wrap_safe_across_millis_rollover() {
  WifiPolicy p;
  p.setKnown({mk("Home")});
  uint32_t now = 0xFFFFFF00u;                   // ~1 minute before the 49-day wrap
  toUnreachable(p, now);
  Inputs in;
  in.knownCount = 1;
  in.nowMs = now + 29000;                       // before the 30 s backoff
  TEST_ASSERT_EQUAL((int)Act::None, (int)p.tick(in).kind);
  in.nowMs = now + 31000;                       // past it, having wrapped through 0
  TEST_ASSERT_EQUAL((int)Act::StartScan, (int)p.tick(in).kind);
}

// F23: a timestamp in the FUTURE must read as zero elapsed, not a 49-day underflow
// that fires every timeout at once.
static void test_future_timestamp_is_zero_elapsed() {
  WifiPolicy p;
  p.setKnown({mk("A")});
  Inputs in;
  in.nowMs = 100000;
  in.knownCount = 1;
  p.tick(in);                                   // Scanning, scanStart = 100000
  in.nowMs = 90000;                             // clock stepped BACKWARD
  TEST_ASSERT_EQUAL((int)Act::None, (int)p.tick(in).kind);   // not an instant timeout
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_classify_reason_table);
  RUN_TEST(test_link_state_names);
  RUN_TEST(test_empty_known_goes_unreachable_immediately);
  RUN_TEST(test_empty_known_never_scans_over_24h);
  RUN_TEST(test_idle_with_known_starts_one_scan);
  RUN_TEST(test_scan_hit_joins_strongest_known_with_its_password);
  RUN_TEST(test_scan_no_match_goes_unreachable);
  RUN_TEST(test_scan_timeout_goes_unreachable);
  RUN_TEST(test_auth_failure_does_not_retry_the_same_network);
  RUN_TEST(test_notfound_advances_immediately);
  RUN_TEST(test_transient_retries_once_then_advances);
  RUN_TEST(test_reason_8_is_never_a_failure);
  RUN_TEST(test_candidates_exhausted_goes_unreachable);
  RUN_TEST(test_setknown_midjoin_then_transient_disconnect_is_safe);
  RUN_TEST(test_setknown_midjoin_then_join_timeout_is_safe);
  RUN_TEST(test_unreachable_never_scans_back_to_back);
  RUN_TEST(test_unreachable_backoff_schedule_exact);
  RUN_TEST(test_unreachable_defers_while_ap_client_present);
  RUN_TEST(test_publish_ap_drops_sta_and_suppresses_every_scan);
  RUN_TEST(test_ap_hold_expires_and_the_slow_schedule_resumes);
  RUN_TEST(test_ap_hold_refreshes_while_a_station_is_associated);
  RUN_TEST(test_ap_hold_alone_blocks_a_due_retry);
  RUN_TEST(test_cancel_hold_releases_the_quiet_window);
  RUN_TEST(test_direct_join_bypasses_the_scan_and_clears_the_hold);
  RUN_TEST(test_direct_join_out_of_range_index_is_ignored);
  RUN_TEST(test_got_ip_goes_online_and_resets_backoff);
  RUN_TEST(test_online_drop_returns_to_scanning_not_unreachable);
  RUN_TEST(test_online_drop_honours_grace);
  RUN_TEST(test_unsolicited_got_ip_is_accepted);
  RUN_TEST(test_ap_down_emits_reassert);
  RUN_TEST(test_state_changed_only_on_edges);
  RUN_TEST(test_wrap_safe_across_millis_rollover);
  RUN_TEST(test_future_timestamp_is_zero_elapsed);
  return UNITY_END();
}
