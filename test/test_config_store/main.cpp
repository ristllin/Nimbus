#include <unity.h>

#include <cstring>

#include "nimbus/config_store.h"

using namespace nimbus;

void setUp() {}
void tearDown() {}

static void test_roundtrip_no_overrides() {
  Config c;
  c.setProfile(ProfileId::Desk);
  uint8_t buf[kConfigMaxBytes];
  size_t n = serializeConfig(c, buf, sizeof buf);
  TEST_ASSERT_EQUAL(kConfigHeaderBytes, n);  // header only

  Config out;
  TEST_ASSERT_TRUE(deserializeConfig(buf, n, out));
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(out.profile()));
  for (int i = 0; i < kParamCount; ++i)
    TEST_ASSERT_FALSE(out.hasOverride(Param(i)));
}

static void test_roundtrip_sparse_overrides() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  c.setOverride(Param::RingBrightness, 77);
  c.setOverride(Param::AttnLedIndex, 22);
  c.setOverride(Param::CoalesceMs, 45000);
  c.setOverride(Param::AttnHue, -1);  // negative value survives (i32)

  uint8_t buf[kConfigMaxBytes];
  size_t n = serializeConfig(c, buf, sizeof buf);
  TEST_ASSERT_EQUAL(kConfigHeaderBytes + 4 * kConfigRecordBytes, n);

  Config out;
  TEST_ASSERT_TRUE(deserializeConfig(buf, n, out));
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(out.profile()));
  TEST_ASSERT_TRUE(out.hasOverride(Param::RingBrightness));
  TEST_ASSERT_EQUAL(77, out.effective(Param::RingBrightness));
  TEST_ASSERT_EQUAL(22, out.effective(Param::AttnLedIndex));
  TEST_ASSERT_EQUAL(45000, out.effective(Param::CoalesceMs));
  TEST_ASSERT_EQUAL(-1, out.effective(Param::AttnHue));
  // A non-overridden param falls back to the (Balanced) preset.
  TEST_ASSERT_FALSE(out.hasOverride(Param::RingFps));
  TEST_ASSERT_EQUAL(presetValue(ProfileId::Balanced, Param::RingFps),
                    out.effective(Param::RingFps));
}

static void test_all_params_overridden_fits_max() {
  Config c;
  for (int i = 0; i < kParamCount; ++i) c.setOverride(Param(i), i * 100 - 50);
  uint8_t buf[kConfigMaxBytes];
  size_t n = serializeConfig(c, buf, sizeof buf);
  TEST_ASSERT_EQUAL(kConfigMaxBytes, n);

  Config out;
  TEST_ASSERT_TRUE(deserializeConfig(buf, n, out));
  for (int i = 0; i < kParamCount; ++i) {
    TEST_ASSERT_TRUE(out.hasOverride(Param(i)));
    TEST_ASSERT_EQUAL(i * 100 - 50, out.effective(Param(i)));
  }
}

static void test_serialize_rejects_small_buffer() {
  Config c;
  c.setOverride(Param::RingFps, 10);
  uint8_t tiny[kConfigHeaderBytes];  // no room for the record
  TEST_ASSERT_EQUAL(0, serializeConfig(c, tiny, sizeof tiny));
}

static void test_deserialize_rejects_corruption() {
  Config c;
  c.setProfile(ProfileId::Desk);
  c.setOverride(Param::RingBrightness, 5);
  uint8_t buf[kConfigMaxBytes];
  size_t n = serializeConfig(c, buf, sizeof buf);

  Config out;

  // Bad magic.
  uint8_t bad[kConfigMaxBytes];
  std::memcpy(bad, buf, n);
  bad[0] = 'X';
  TEST_ASSERT_FALSE(deserializeConfig(bad, n, out));

  // Wrong version.
  std::memcpy(bad, buf, n);
  bad[2] = 99;
  TEST_ASSERT_FALSE(deserializeConfig(bad, n, out));

  // Profile out of range.
  std::memcpy(bad, buf, n);
  bad[3] = kProfileCount;
  TEST_ASSERT_FALSE(deserializeConfig(bad, n, out));

  // Param out of range.
  std::memcpy(bad, buf, n);
  bad[kConfigHeaderBytes] = kParamCount;  // first record's param byte
  TEST_ASSERT_FALSE(deserializeConfig(bad, n, out));

  // Truncated (count says 1 record but bytes are missing).
  TEST_ASSERT_FALSE(deserializeConfig(buf, kConfigHeaderBytes + 2, out));

  // Too short for even a header.
  TEST_ASSERT_FALSE(deserializeConfig(buf, 3, out));
}

// A rejected blob must leave the destination Config untouched (defaults kept).
static void test_failed_parse_leaves_out_untouched() {
  Config out;
  out.setProfile(ProfileId::Desk);
  out.setOverride(Param::RingBrightness, 123);

  uint8_t garbage[] = {'X', 'Y', 1, 0, 0};
  TEST_ASSERT_FALSE(deserializeConfig(garbage, sizeof garbage, out));
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(out.profile()));
  TEST_ASSERT_EQUAL(123, out.effective(Param::RingBrightness));
}

// Mirror of the device persistConfig() invariant (src/main.cpp): the persisted
// profile byte must be the USER's pick (Selector::user()), never the transient
// forced/VBUS profile that a battery T1 event drops into the ACTIVE Config
// profile. Regression guard for the bug where a menu/web edit during a T1 window
// persisted BatterySaver and silently lost the user's real pick.
static void test_persist_stores_user_pick_not_forced_active() {
  // User picked Balanced; overrides are live and must survive persistence.
  Selector sel;
  sel.setUser(ProfileId::Balanced);
  Config live;
  live.setOverride(Param::RingBrightness, 42);

  // Battery drops to T1: the power tick forces Battery Saver, so the ACTIVE
  // Config profile becomes the resolved (transient) value.
  sel.setForced(true);
  live.setProfile(sel.resolve());  // == BatterySaver (active/transient)
  TEST_ASSERT_EQUAL(int(ProfileId::BatterySaver), int(live.profile()));

  // persistConfig(): serialize a copy whose profile is the user's pick, not the
  // active/transient one.
  Config toSave = live;
  toSave.setProfile(sel.user());  // == Balanced
  uint8_t buf[kConfigMaxBytes];
  size_t n = serializeConfig(toSave, buf, sizeof buf);
  TEST_ASSERT_TRUE(n > 0);

  // The stored profile byte is the user's pick, and overrides round-trip intact.
  Config restored;
  TEST_ASSERT_TRUE(deserializeConfig(buf, n, restored));
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(restored.profile()));
  TEST_ASSERT_TRUE(restored.hasOverride(Param::RingBrightness));
  TEST_ASSERT_EQUAL(42, restored.effective(Param::RingBrightness));

  // On reboot the selector is re-seeded from the restored profile: still the
  // user's pick, so resolve() recovers Balanced once T1 clears.
  Selector rebooted;
  rebooted.setUser(restored.profile());
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(rebooted.user()));
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(rebooted.resolve()));
}

// L1 persistence CONTRACT for F10 (the HIL test spec, host half of R_F10).
//
// The device split (src/sys/config_nvs.cpp) is: the operating MODE is a standalone
// NVS int (saveMode -> setInt), while the sparse OVERRIDES ride the SD-backed Config
// blob (saveConfig -> putBlob). So with NO SD, only the mode survives a reboot and
// overrides are lost - the real F10 bug. That decision must be PINNED in code here
// (portable, host-runnable now) rather than discovered on hardware:
//   1. Mode is NOT encoded in the serialized Config blob (it is stored separately),
//      so the blob's presence/absence is exactly what gates override persistence.
//   2. Overrides ARE encoded in the blob (one record each), so losing the blob
//      loses the overrides - nothing else carries them.
// If a future fix moves overrides into NVS (or guarantees the SD blob), THIS test's
// assumptions change and it must be revisited alongside the on-device xfail
// (tests/hil/test_net.py::test_persist_across_reboot).
static void test_f10_persistence_contract_mode_separate_from_blob() {
  // The Config blob encodes profile + overrides ONLY - no mode byte. Two Configs
  // that differ solely by "which mode we're in" cannot differ, because Config has
  // no mode field: the blob size is a pure function of the override count, and the
  // mode is never serialized here.
  Config noOv;
  noOv.setProfile(ProfileId::Desk);
  uint8_t buf0[kConfigMaxBytes];
  size_t n0 = serializeConfig(noOv, buf0, sizeof buf0);
  // Header only - profile is in the header; there is no mode record and no override.
  TEST_ASSERT_EQUAL(kConfigHeaderBytes, n0);

  // Each override adds exactly one record to the blob - overrides live in the blob,
  // nowhere else. Losing the blob (no SD) therefore loses every override.
  Config withOv = noOv;
  withOv.setOverride(Param::RingBrightness, 99);
  uint8_t buf1[kConfigMaxBytes];
  size_t n1 = serializeConfig(withOv, buf1, sizeof buf1);
  TEST_ASSERT_EQUAL(kConfigHeaderBytes + kConfigRecordBytes, n1);

  // Round-trip proves the override is recoverable ONLY from the blob bytes: parse
  // the blob and the override is back; without those bytes it is unrecoverable.
  Config restored;
  TEST_ASSERT_TRUE(deserializeConfig(buf1, n1, restored));
  TEST_ASSERT_TRUE(restored.hasOverride(Param::RingBrightness));
  TEST_ASSERT_EQUAL(99, restored.effective(Param::RingBrightness));

  // The "no SD" reality, modeled at the contract level: with NO blob bytes, a fresh
  // Config keeps only its defaults (no overrides survive) - exactly what the device
  // sees when saveConfig()->putBlob no-ops. The mode int is orthogonal and would be
  // restored from NVS by loadMode(), not from this blob.
  Config noBlob;  // simulates deserialize never running (blob absent)
  TEST_ASSERT_FALSE(noBlob.hasOverride(Param::RingBrightness));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip_no_overrides);
  RUN_TEST(test_roundtrip_sparse_overrides);
  RUN_TEST(test_all_params_overridden_fits_max);
  RUN_TEST(test_serialize_rejects_small_buffer);
  RUN_TEST(test_deserialize_rejects_corruption);
  RUN_TEST(test_failed_parse_leaves_out_untouched);
  RUN_TEST(test_persist_stores_user_pick_not_forced_active);
  RUN_TEST(test_f10_persistence_contract_mode_separate_from_blob);
  return UNITY_END();
}
