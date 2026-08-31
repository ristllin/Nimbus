#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/wifi/known_networks.h"

using nimbus::wifi::Candidate;
using nimbus::wifi::dumpNetworks;
using nimbus::wifi::findNetwork;
using nimbus::wifi::forgetNetwork;
using nimbus::wifi::kMaxKnownNetworks;
using nimbus::wifi::KnownNet;
using nimbus::wifi::loadNetworks;
using nimbus::wifi::migrateLegacySlot;
using nimbus::wifi::moveNetwork;
using nimbus::wifi::pickBest;
using nimbus::wifi::rankCandidates;
using nimbus::wifi::ScanHit;
using nimbus::wifi::touchNetwork;
using nimbus::wifi::upsertNetwork;
using nimbus::wifi::UpsertResult;

void setUp() {}
void tearDown() {}

static KnownNet mk(const char* ssid, const char* pass = "", uint32_t day = 0,
                   bool autoJoin = true) {
  KnownNet n;
  n.ssid = ssid;
  n.pass = pass;
  n.lastOkDay = day;
  n.autoJoin = autoJoin;
  return n;
}
static ScanHit hit(const char* ssid, int rssi, bool locked = true) {
  ScanHit h;
  h.ssid = ssid;
  h.rssi = (int8_t)rssi;
  h.locked = locked;
  return h;
}

// ---- (de)serialization ------------------------------------------------------

static void test_dump_load_roundtrip() {
  std::vector<KnownNet> in = {mk("Home", "pw1", 20661), mk("Office", "pw2", 20658, false)};
  std::vector<KnownNet> out;
  TEST_ASSERT_TRUE(loadNetworks(dumpNetworks(in), out));
  TEST_ASSERT_EQUAL(2, (int)out.size());
  TEST_ASSERT_EQUAL_STRING("Home", out[0].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("pw1", out[0].pass.c_str());
  TEST_ASSERT_EQUAL(20661u, out[0].lastOkDay);
  TEST_ASSERT_TRUE(out[0].autoJoin);
  TEST_ASSERT_EQUAL_STRING("Office", out[1].ssid.c_str());
  TEST_ASSERT_FALSE(out[1].autoJoin);
}

// A virgin device has no NVS key at all. That is "no networks yet", NOT a parse
// failure - treating it as an error would make every first boot look corrupt.
static void test_empty_blob_is_zero_entries_not_an_error() {
  std::vector<KnownNet> out = {mk("stale")};
  TEST_ASSERT_TRUE(loadNetworks("", out));
  TEST_ASSERT_EQUAL(0, (int)out.size());
}

static void test_malformed_blob_returns_false_and_leaves_list_empty() {
  std::vector<KnownNet> out;
  TEST_ASSERT_FALSE(loadNetworks("{\"v\":1,\"n\":[{\"s\":\"A\"", out));
  TEST_ASSERT_EQUAL(0, (int)out.size());
  TEST_ASSERT_FALSE(loadNetworks("[1,2,3]", out));   // array root, not our schema
  TEST_ASSERT_EQUAL(0, (int)out.size());
}

static void test_missing_version_parses_as_v1() {
  std::vector<KnownNet> out;
  TEST_ASSERT_TRUE(loadNetworks("{\"n\":[{\"s\":\"A\",\"p\":\"x\"}]}", out));
  TEST_ASSERT_EQUAL(1, (int)out.size());
  TEST_ASSERT_EQUAL_STRING("A", out[0].ssid.c_str());
}

static void test_oversize_fields_clamped_on_load() {
  const std::string longSsid(80, 'S'), longPass(200, 'P');
  std::string json = "{\"v\":1,\"n\":[{\"s\":\"" + longSsid + "\",\"p\":\"" + longPass + "\"}]}";
  std::vector<KnownNet> out;
  TEST_ASSERT_TRUE(loadNetworks(json, out));
  TEST_ASSERT_EQUAL(1, (int)out.size());
  TEST_ASSERT_EQUAL(32, (int)out[0].ssid.size());
  // 64, not 63: a 64-char hex string is a valid credential (the raw PMK), so the
  // cap has to admit it. Anything longer is still clamped - this asserts the
  // clamp still happens, only at the right boundary.
  TEST_ASSERT_EQUAL((int)nimbus::wifi::kPassMax, (int)out[0].pass.size());
  TEST_ASSERT_EQUAL(64, (int)out[0].pass.size());
}

static void test_empty_ssid_entries_dropped() {
  std::vector<KnownNet> out;
  TEST_ASSERT_TRUE(loadNetworks("{\"v\":1,\"n\":[{\"s\":\"\",\"p\":\"x\"},{\"s\":\"B\"}]}", out));
  TEST_ASSERT_EQUAL(1, (int)out.size());
  TEST_ASSERT_EQUAL_STRING("B", out[0].ssid.c_str());
}

static void test_nine_entry_blob_truncates_to_five() {
  std::string json = "{\"v\":1,\"n\":[";
  for (int i = 0; i < 9; i++) {
    if (i) json += ",";
    json += "{\"s\":\"N" + std::to_string(i) + "\"}";
  }
  json += "]}";
  std::vector<KnownNet> out;
  TEST_ASSERT_TRUE(loadNetworks(json, out));
  TEST_ASSERT_EQUAL(kMaxKnownNetworks, (int)out.size());
}

// The literal field bug behind F8/F9 was a password whose '!' went missing. A PSK
// must survive the round-trip byte-for-byte, punctuation and spaces included.
static void test_psk_with_punctuation_survives_verbatim() {
  const char* psk = "p@ss w0rd!#$%&'()*+,-./:;<=>?[\\]^_`{|}~\"";
  std::vector<KnownNet> in = {mk("Net", psk)};
  std::vector<KnownNet> out;
  TEST_ASSERT_TRUE(loadNetworks(dumpNetworks(in), out));
  TEST_ASSERT_EQUAL_STRING(psk, out[0].pass.c_str());
}

static void test_open_network_empty_password_roundtrips() {
  std::vector<KnownNet> in = {mk("CafeOpen", "")};
  std::vector<KnownNet> out;
  TEST_ASSERT_TRUE(loadNetworks(dumpNetworks(in), out));
  TEST_ASSERT_EQUAL(1, (int)out.size());
  TEST_ASSERT_EQUAL_STRING("", out[0].pass.c_str());
}

// ---- list operations --------------------------------------------------------

// 802.11 SSIDs are case-sensitive octet strings (F8). Folding case here would join
// or overwrite the wrong network.
static void test_ssid_match_is_case_sensitive() {
  std::vector<KnownNet> nets = {mk("TestNet", "a")};
  TEST_ASSERT_EQUAL(0, findNetwork(nets, "TestNet"));
  TEST_ASSERT_EQUAL(-1, findNetwork(nets, "testnet"));
  TEST_ASSERT_EQUAL(-1, findNetwork(nets, "TESTNET"));
}

static void test_upsert_replaces_password_and_promotes_to_head() {
  std::vector<KnownNet> nets = {mk("A", "old", 5), mk("B", "b", 9)};
  TEST_ASSERT_EQUAL((int)UpsertResult::Updated,
                    (int)upsertNetwork(nets, mk("A", "new"), kMaxKnownNetworks, ""));
  TEST_ASSERT_EQUAL(2, (int)nets.size());
  TEST_ASSERT_EQUAL_STRING("A", nets[0].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("new", nets[0].pass.c_str());
  TEST_ASSERT_EQUAL(5u, nets[0].lastOkDay);   // join history preserved
}

static void test_upsert_at_capacity_evicts_lowest_lastokday() {
  std::vector<KnownNet> nets = {mk("A", "", 50), mk("B", "", 10), mk("C", "", 30),
                                mk("D", "", 40), mk("E", "", 20)};
  KnownNet evicted;
  TEST_ASSERT_EQUAL((int)UpsertResult::Evicted,
                    (int)upsertNetwork(nets, mk("NEW"), kMaxKnownNetworks, "", &evicted));
  TEST_ASSERT_EQUAL_STRING("B", evicted.ssid.c_str());   // lastOkDay 10 = least recent
  TEST_ASSERT_EQUAL(kMaxKnownNetworks, (int)nets.size());
  TEST_ASSERT_EQUAL_STRING("NEW", nets[0].ssid.c_str());
  TEST_ASSERT_EQUAL(-1, findNetwork(nets, "B"));
}

// Never drop the network currently in use to make room for one that has never
// worked - that would turn "add a network" into "lose your connection".
static void test_upsert_never_evicts_the_protected_ssid() {
  std::vector<KnownNet> nets = {mk("A", "", 50), mk("InUse", "", 0), mk("C", "", 30),
                                mk("D", "", 40), mk("E", "", 20)};
  KnownNet evicted;
  TEST_ASSERT_EQUAL((int)UpsertResult::Evicted,
                    (int)upsertNetwork(nets, mk("NEW"), kMaxKnownNetworks, "InUse", &evicted));
  TEST_ASSERT_TRUE(findNetwork(nets, "InUse") >= 0);
  TEST_ASSERT_EQUAL_STRING("E", evicted.ssid.c_str());   // next-lowest day (20)
}

static void test_upsert_rejects_empty_or_oversize_ssid() {
  std::vector<KnownNet> nets = {mk("A")};
  TEST_ASSERT_EQUAL((int)UpsertResult::Rejected,
                    (int)upsertNetwork(nets, mk(""), kMaxKnownNetworks, ""));
  KnownNet big = mk("x");
  big.ssid = std::string(33, 'x');
  TEST_ASSERT_EQUAL((int)UpsertResult::Rejected,
                    (int)upsertNetwork(nets, big, kMaxKnownNetworks, ""));
  TEST_ASSERT_EQUAL(1, (int)nets.size());   // untouched
}

static void test_forget_removes_and_missing_is_a_no_op() {
  std::vector<KnownNet> nets = {mk("A"), mk("B")};
  TEST_ASSERT_TRUE(forgetNetwork(nets, "A"));
  TEST_ASSERT_EQUAL(1, (int)nets.size());
  TEST_ASSERT_FALSE(forgetNetwork(nets, "nope"));
  TEST_ASSERT_EQUAL(1, (int)nets.size());
}

// NVS has finite erase cycles and touch() runs on every reconnect, so an unchanged
// head must report "nothing to persist".
static void test_touch_returns_false_when_head_unchanged() {
  std::vector<KnownNet> nets = {mk("A", "", 100), mk("B", "", 90)};
  TEST_ASSERT_FALSE(touchNetwork(nets, "A", 100));   // already head, already stamped
  TEST_ASSERT_TRUE(touchNetwork(nets, "A", 101));    // new day -> persist
  TEST_ASSERT_TRUE(touchNetwork(nets, "B", 101));    // promotion -> persist
  TEST_ASSERT_EQUAL_STRING("B", nets[0].ssid.c_str());
  TEST_ASSERT_FALSE(touchNetwork(nets, "absent", 101));
}

// ---- reorder (optional user priority, CUM-207) ------------------------------

static void test_move_reorders_and_reports_change() {
  std::vector<KnownNet> nets = {mk("A"), mk("B"), mk("C")};
  TEST_ASSERT_TRUE(moveNetwork(nets, "C", 0));       // C to the front
  TEST_ASSERT_EQUAL_STRING("C", nets[0].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("A", nets[1].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("B", nets[2].ssid.c_str());
}

static void test_move_carries_the_password_and_history() {
  std::vector<KnownNet> nets = {mk("A", "pa", 10), mk("B", "pb", 20)};
  TEST_ASSERT_TRUE(moveNetwork(nets, "A", 1));
  TEST_ASSERT_EQUAL_STRING("A", nets[1].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("pa", nets[1].pass.c_str());   // moved intact, nothing lost
  TEST_ASSERT_EQUAL(10u, nets[1].lastOkDay);
}

static void test_move_to_same_index_is_a_no_op() {
  std::vector<KnownNet> nets = {mk("A"), mk("B")};
  TEST_ASSERT_FALSE(moveNetwork(nets, "A", 0));      // already there -> no persist
}

static void test_move_clamps_out_of_range_index() {
  std::vector<KnownNet> nets = {mk("A"), mk("B"), mk("C")};
  TEST_ASSERT_TRUE(moveNetwork(nets, "A", 99));      // clamps to the last slot
  TEST_ASSERT_EQUAL_STRING("A", nets[2].ssid.c_str());
  TEST_ASSERT_TRUE(moveNetwork(nets, "A", -5));      // clamps to the head
  TEST_ASSERT_EQUAL_STRING("A", nets[0].ssid.c_str());
}

// The last entry moved to a past-the-end index clamps back onto itself -> no-op, false.
static void test_move_last_to_past_end_is_a_no_op() {
  std::vector<KnownNet> nets = {mk("A"), mk("B"), mk("C")};
  TEST_ASSERT_FALSE(moveNetwork(nets, "C", 99));   // already last; clamp lands on itself
  TEST_ASSERT_EQUAL_STRING("C", nets[2].ssid.c_str());
}

static void test_move_absent_ssid_is_false() {
  std::vector<KnownNet> nets = {mk("A")};
  TEST_ASSERT_FALSE(moveNetwork(nets, "nope", 0));
  TEST_ASSERT_EQUAL(1, (int)nets.size());
}

// ---- candidate selection ----------------------------------------------------

// The heart of scan-then-match: join the strongest network we KNOW, ignoring
// stronger networks we have no credentials for.
static void test_pick_best_is_strongest_known() {
  std::vector<KnownNet> nets = {mk("A"), mk("C")};
  std::vector<ScanHit> scan = {hit("A", -70), hit("B", -45), hit("C", -80)};
  const int best = pickBest(nets, scan);
  TEST_ASSERT_EQUAL(0, best);                                  // A, not the louder B
  TEST_ASSERT_EQUAL_STRING("A", nets[(size_t)best].ssid.c_str());
}

static void test_pick_best_empty_scan_is_minus_one() {
  std::vector<KnownNet> nets = {mk("A")};
  TEST_ASSERT_EQUAL(-1, pickBest(nets, {}));
}

// No known network visible is exactly the trap condition - it must report "none"
// so the policy can stop scanning instead of retrying forever.
static void test_pick_best_no_overlap_is_minus_one() {
  std::vector<KnownNet> nets = {mk("Home")};
  std::vector<ScanHit> scan = {hit("Someone-Else", -40)};
  TEST_ASSERT_EQUAL(-1, pickBest(nets, scan));
}

static void test_pick_best_ignores_autojoin_false() {
  std::vector<KnownNet> nets = {mk("Manual", "", 0, /*autoJoin=*/false), mk("Auto")};
  std::vector<ScanHit> scan = {hit("Manual", -30), hit("Auto", -85)};
  TEST_ASSERT_EQUAL(1, pickBest(nets, scan));   // Auto, despite being far weaker
}

static void test_rank_candidates_is_ordered_strongest_first() {
  std::vector<KnownNet> nets = {mk("A"), mk("B"), mk("C")};
  std::vector<ScanHit> scan = {hit("A", -80), hit("B", -40), hit("C", -60)};
  const std::vector<Candidate> r = rankCandidates(nets, scan);
  TEST_ASSERT_EQUAL(3, (int)r.size());
  TEST_ASSERT_EQUAL(1, r[0].knownIndex);   // B  -40
  TEST_ASSERT_EQUAL(2, r[1].knownIndex);   // C  -60
  TEST_ASSERT_EQUAL(0, r[2].knownIndex);   // A  -80
}

// A mesh/repeater broadcasts one SSID from several radios; take the nearest.
static void test_duplicate_ssid_in_scan_uses_strongest_sighting() {
  std::vector<KnownNet> nets = {mk("Mesh"), mk("Other")};
  std::vector<ScanHit> scan = {hit("Mesh", -85), hit("Other", -60), hit("Mesh", -35)};
  const std::vector<Candidate> r = rankCandidates(nets, scan);
  TEST_ASSERT_EQUAL(2, (int)r.size());
  TEST_ASSERT_EQUAL(0, r[0].knownIndex);
  TEST_ASSERT_EQUAL(-35, (int)r[0].rssi);
}

// ---- migration --------------------------------------------------------------

static void test_migration_single_slot_becomes_entry_one() {
  std::vector<KnownNet> nets;
  TEST_ASSERT_TRUE(migrateLegacySlot(nets, "TestNet", "secret!"));
  TEST_ASSERT_EQUAL(1, (int)nets.size());
  TEST_ASSERT_EQUAL_STRING("TestNet", nets[0].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("secret!", nets[0].pass.c_str());
}

// Runs on every boot of an upgraded device: the second run must change nothing.
static void test_migration_is_idempotent() {
  std::vector<KnownNet> nets;
  TEST_ASSERT_TRUE(migrateLegacySlot(nets, "Home", "pw"));
  TEST_ASSERT_FALSE(migrateLegacySlot(nets, "Home", "pw"));
  TEST_ASSERT_EQUAL(1, (int)nets.size());
}

static void test_migration_of_unprovisioned_slot_is_a_no_op() {
  std::vector<KnownNet> nets;
  TEST_ASSERT_FALSE(migrateLegacySlot(nets, "", ""));
  TEST_ASSERT_EQUAL(0, (int)nets.size());
}

// The upgrade path that matters for the board in hand: it has ONE stored network
// that is currently unreachable. Migration must preserve it exactly - recovery
// must not cost the owner their credentials.
static void test_migration_preserves_an_unreachable_network() {
  std::vector<KnownNet> nets;
  TEST_ASSERT_TRUE(migrateLegacySlot(nets, "OwnerHotspot", "hotspot pw!"));
  std::vector<KnownNet> reloaded;
  TEST_ASSERT_TRUE(loadNetworks(dumpNetworks(nets), reloaded));
  TEST_ASSERT_EQUAL(1, (int)reloaded.size());
  TEST_ASSERT_EQUAL_STRING("OwnerHotspot", reloaded[0].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("hotspot pw!", reloaded[0].pass.c_str());
  // ...and it is still not a join candidate while it is out of range.
  TEST_ASSERT_EQUAL(-1, pickBest(reloaded, {hit("Bench-AP", -40)}));
}

// A 64-character hex string is a valid credential - the raw 256-bit PMK, which
// both ESP-IDF and the Arduino core accept by branching on exactly strlen==64.
// The cap was 63, so such a key was truncated on load, on upsert AND during
// legacy migration, and the damaged copy was mirrored back over the legacy slot:
// the owner's credential destroyed with no owner action, unrecoverable even by
// an OTA rollback. This is the only defect in the branch that did that.
static void test_a_64_hex_psk_survives_intact() {
  const std::string pmk(64, 'a');
  std::vector<KnownNet> nets;
  KnownNet n; n.ssid = "Home"; n.pass = pmk;
  KnownNet ev;
  TEST_ASSERT_TRUE(upsertNetwork(nets, n, kMaxKnownNetworks, "", &ev) != UpsertResult::Rejected);
  TEST_ASSERT_EQUAL_UINT32(64, (uint32_t)nets[0].pass.size());
  TEST_ASSERT_EQUAL_STRING(pmk.c_str(), nets[0].pass.c_str());

  // ...and through a persistence round-trip.
  std::vector<KnownNet> back;
  TEST_ASSERT_TRUE(loadNetworks(dumpNetworks(nets), back, kMaxKnownNetworks));
  TEST_ASSERT_EQUAL_UINT32(64, (uint32_t)back[0].pass.size());
  TEST_ASSERT_EQUAL_STRING(pmk.c_str(), back[0].pass.c_str());

  // ...and through migration, which is the path that runs with no owner action.
  std::vector<KnownNet> fresh;
  TEST_ASSERT_TRUE(migrateLegacySlot(fresh, "Home", pmk, kMaxKnownNetworks));
  TEST_ASSERT_EQUAL_STRING(pmk.c_str(), fresh[0].pass.c_str());
}

// Migration is idempotent on the SSID, but it must not IGNORE a legacy password
// that has since changed: an older image (a rollback, or [env:provision]) writes
// the legacy slot directly, so a correction can land there while the list keeps
// the stale copy - and a later mirror would write the stale one back over the fix.
static void test_migration_takes_a_corrected_legacy_password() {
  std::vector<KnownNet> nets;
  TEST_ASSERT_TRUE(migrateLegacySlot(nets, "Home", "old-secret", kMaxKnownNetworks));
  TEST_ASSERT_EQUAL_INT(1, (int)nets.size());

  // Same SSID, SAME password -> genuinely nothing to do.
  TEST_ASSERT_FALSE(migrateLegacySlot(nets, "Home", "old-secret", kMaxKnownNetworks));
  TEST_ASSERT_EQUAL_INT(1, (int)nets.size());

  // Same SSID, DIFFERENT password -> adopt it, without duplicating the entry.
  TEST_ASSERT_TRUE(migrateLegacySlot(nets, "Home", "new-secret", kMaxKnownNetworks));
  TEST_ASSERT_EQUAL_INT(1, (int)nets.size());
  TEST_ASSERT_EQUAL_STRING("new-secret", nets[0].pass.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_64_hex_psk_survives_intact);
  RUN_TEST(test_migration_takes_a_corrected_legacy_password);
  RUN_TEST(test_dump_load_roundtrip);
  RUN_TEST(test_empty_blob_is_zero_entries_not_an_error);
  RUN_TEST(test_malformed_blob_returns_false_and_leaves_list_empty);
  RUN_TEST(test_missing_version_parses_as_v1);
  RUN_TEST(test_oversize_fields_clamped_on_load);
  RUN_TEST(test_empty_ssid_entries_dropped);
  RUN_TEST(test_nine_entry_blob_truncates_to_five);
  RUN_TEST(test_psk_with_punctuation_survives_verbatim);
  RUN_TEST(test_open_network_empty_password_roundtrips);
  RUN_TEST(test_ssid_match_is_case_sensitive);
  RUN_TEST(test_upsert_replaces_password_and_promotes_to_head);
  RUN_TEST(test_upsert_at_capacity_evicts_lowest_lastokday);
  RUN_TEST(test_upsert_never_evicts_the_protected_ssid);
  RUN_TEST(test_upsert_rejects_empty_or_oversize_ssid);
  RUN_TEST(test_forget_removes_and_missing_is_a_no_op);
  RUN_TEST(test_touch_returns_false_when_head_unchanged);
  RUN_TEST(test_move_reorders_and_reports_change);
  RUN_TEST(test_move_carries_the_password_and_history);
  RUN_TEST(test_move_to_same_index_is_a_no_op);
  RUN_TEST(test_move_clamps_out_of_range_index);
  RUN_TEST(test_move_last_to_past_end_is_a_no_op);
  RUN_TEST(test_move_absent_ssid_is_false);
  RUN_TEST(test_pick_best_is_strongest_known);
  RUN_TEST(test_pick_best_empty_scan_is_minus_one);
  RUN_TEST(test_pick_best_no_overlap_is_minus_one);
  RUN_TEST(test_pick_best_ignores_autojoin_false);
  RUN_TEST(test_rank_candidates_is_ordered_strongest_first);
  RUN_TEST(test_duplicate_ssid_in_scan_uses_strongest_sighting);
  RUN_TEST(test_migration_single_slot_becomes_entry_one);
  RUN_TEST(test_migration_is_idempotent);
  RUN_TEST(test_migration_of_unprovisioned_slot_is_a_no_op);
  RUN_TEST(test_migration_preserves_an_unreachable_network);
  return UNITY_END();
}
