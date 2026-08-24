#include <unity.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nimbus/config_store.h"
#include "nimbus/profile.h"

// F10 persistence-CONTRACT regression (the HIL test spec).
//
// The bug (the HIL test spec, flagged in the P4 review, never tested):
//   "Overrides won't persist. Config blob is SD-backed (putBlob); no SD -> only
//    mode (NVS int) survives reboot."
//
// The portable round-trip (serialize/deserialize) is already covered by
// test_config_store. What was NEVER pinned in code is the *storage-tier
// decision* the device edge (src/sys/config_nvs.cpp) makes:
//
//   - the active Mode is a single NVS int  -> setInt/getInt -> survives with NO
//     SD card (NVS is on-chip flash, always present, survives reflash).
//   - the Config blob (profile + sparse overrides) rides the SD blob store ->
//     putBlob/getBlob -> present ONLY when an SD card is mounted.
//
// solide::memory documents exactly this split (include/solide/memory.h):
//   "small typed config -> NVS ... works with no SD card;
//    larger JSON/blobs -> SD ... returns false/0 when the card is absent."
//
// These tests model both backends with the SAME graceful-degradation semantics
// and drive the SAME save/load logic config_nvs.cpp runs, so the SD-vs-NVS
// decision - and its user-visible consequence (a profile/override edit made
// with no card is silently lost across reboot) - is asserted here on the host
// instead of being discovered on hardware.
//
// We intentionally do NOT #include Arduino-dependent sys/config_nvs.h on the
// host; we re-run its byte-moving logic against a faithful fake so the CONTRACT
// is the thing under test, not the driver glue.

using namespace nimbus;

void setUp() {}
void tearDown() {}

namespace {

// The two persistence keys, mirroring src/sys/config_nvs.cpp.
constexpr char kCfgBlob[] = "nimbus_cfg";   // SD blob: the versioned Config
constexpr char kModeKey[] = "nimbus_mode";  // NVS int: the active Mode

enum class Mode : int32_t { Notifier = 0, Orchestrator = 1 };

// A host model of solide::memory with the documented two-tier behaviour:
//   * NVS half (typed KV): on-chip, ALWAYS available, survives a simulated
//     reflash/reboot.
//   * SD half (blobs): available ONLY while a card is "inserted"; putBlob /
//     getBlob degrade to false / 0 when it is absent - never crash.
// Distinct maps so a reboot can wipe volatile-but-really-just-absent SD state
// without touching NVS, exactly as the hardware does.
struct FakeStore {
  bool nvsOk = true;                       // NVS namespace opened (solide::memory::ok())
  bool sdPresent = true;                   // an SD card is mounted
  std::map<std::string, int32_t> nvsInt;   // typed KV
  std::map<std::string, std::vector<uint8_t>> sdBlob;  // /memory/blob/<name>.bin

  bool ok() const { return nvsOk; }

  // ---- NVS typed int (always works when the namespace is open) ----
  bool setInt(const char* k, int32_t v) {
    if (!nvsOk) return false;
    nvsInt[k] = v;
    return true;
  }
  int32_t getInt(const char* k, int32_t def) const {
    if (!nvsOk) return def;
    auto it = nvsInt.find(k);
    return it == nvsInt.end() ? def : it->second;
  }

  // ---- SD blob (needs a card) ----
  bool putBlob(const char* k, const uint8_t* data, size_t n) {
    if (!sdPresent) return false;          // no card -> write silently fails
    sdBlob[k] = std::vector<uint8_t>(data, data + n);
    return true;
  }
  size_t getBlob(const char* k, uint8_t* out, size_t maxN) const {
    if (!sdPresent) return 0;              // no card -> reads as absent
    auto it = sdBlob.find(k);
    if (it == sdBlob.end()) return 0;
    size_t n = it->second.size();
    if (n > maxN) n = maxN;
    std::memcpy(out, it->second.data(), n);
    return n;
  }
};

// ---- the device edge, re-expressed against the fake (byte-for-byte the logic
//      of src/sys/config_nvs.cpp) --------------------------------------------

bool saveConfig(FakeStore& s, const Config& cfg) {
  if (!s.ok()) return false;
  uint8_t buf[kConfigMaxBytes];
  size_t n = serializeConfig(cfg, buf, sizeof(buf));
  if (n == 0) return false;
  return s.putBlob(kCfgBlob, buf, n);       // SD-backed
}

bool loadConfig(const FakeStore& s, Config& out) {
  if (!s.ok()) return false;
  uint8_t buf[kConfigMaxBytes];
  size_t n = s.getBlob(kCfgBlob, buf, sizeof(buf));  // SD-backed
  if (n == 0) return false;                 // absent -> keep defaults
  return deserializeConfig(buf, n, out);
}

bool saveMode(FakeStore& s, Mode m) {
  if (!s.ok()) return false;
  return s.setInt(kModeKey, int32_t(m));    // NVS int
}

Mode loadMode(const FakeStore& s, Mode def) {
  if (!s.ok()) return def;
  int32_t v = s.getInt(kModeKey, int32_t(def));  // NVS int
  return v == int32_t(Mode::Orchestrator) ? Mode::Orchestrator : Mode::Notifier;
}

// Simulate a power cycle: NVS persists; the SD "card" content persists only if a
// card stays inserted. Passing sdPresentAfter=false models the no-card device.
void reboot(FakeStore& s, bool sdPresentAfter) {
  FakeStore next;
  next.nvsOk = s.nvsOk;
  next.nvsInt = s.nvsInt;                    // NVS survives
  next.sdPresent = sdPresentAfter;
  if (sdPresentAfter) next.sdBlob = s.sdBlob;  // same card still holds its files
  s = next;
}

}  // namespace

// ---------------------------------------------------------------------------
// THE CONTRACT, part 1 - WITH an SD card, both tiers survive a reboot.
// A user picks Desk, tweaks brightness, and sets Orchestrator mode; after a
// power cycle every choice is restored.
// ---------------------------------------------------------------------------
static void test_with_sd_config_and_mode_both_persist() {
  FakeStore store;                 // card present, NVS open
  store.sdPresent = true;

  Config cfg;
  cfg.setProfile(ProfileId::Desk);
  cfg.setOverride(Param::RingBrightness, 123);
  cfg.setOverride(Param::CoalesceMs, 45000);
  TEST_ASSERT_TRUE(saveConfig(store, cfg));
  TEST_ASSERT_TRUE(saveMode(store, Mode::Orchestrator));

  reboot(store, /*sdPresentAfter=*/true);

  Config restored;                 // starts at defaults (Balanced, no overrides)
  TEST_ASSERT_TRUE(loadConfig(store, restored));
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(restored.profile()));
  TEST_ASSERT_TRUE(restored.hasOverride(Param::RingBrightness));
  TEST_ASSERT_EQUAL(123, restored.effective(Param::RingBrightness));
  TEST_ASSERT_EQUAL(45000, restored.effective(Param::CoalesceMs));
  TEST_ASSERT_EQUAL(int(Mode::Orchestrator), int(loadMode(store, Mode::Notifier)));
}

// ---------------------------------------------------------------------------
// THE CONTRACT, part 2 - the F10 BUG, pinned. WITHOUT an SD card:
//   * saveConfig() SILENTLY FAILS (putBlob returns false) - the edit is never
//     written, and nothing surfaces that failure to the caller-as-user.
//   * mode STILL persists (NVS int).
//   * after reboot, the Config falls all the way back to DEFAULTS - the user's
//     profile + override choices are LOST - while mode is intact.
// This is the exact "overrides won't persist" symptom; the test encodes it so a
// future SD-independent fix (e.g. moving the blob to NVS/LittleFS) is a visible,
// green diff here rather than a hardware surprise.
// ---------------------------------------------------------------------------
static void test_no_sd_only_mode_survives_config_lost() {
  FakeStore store;
  store.sdPresent = false;         // no card mounted (NVS still open)

  Config cfg;
  cfg.setProfile(ProfileId::Desk);
  cfg.setOverride(Param::RingBrightness, 200);

  // The blob write fails, silently, right now - this is the bug.
  TEST_ASSERT_FALSE(saveConfig(store, cfg));
  // ...but the mode int is written fine (different tier).
  TEST_ASSERT_TRUE(saveMode(store, Mode::Orchestrator));

  reboot(store, /*sdPresentAfter=*/false);

  // Config load fails -> caller keeps DEFAULTS (Balanced, no overrides).
  Config restored;
  TEST_ASSERT_FALSE(loadConfig(store, restored));
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(restored.profile()));  // default
  TEST_ASSERT_FALSE(restored.hasOverride(Param::RingBrightness));        // lost
  TEST_ASSERT_EQUAL(presetValue(ProfileId::Balanced, Param::RingBrightness),
                    restored.effective(Param::RingBrightness));           // 30, not 200

  // Mode DID survive: this asymmetry is the whole point of F10.
  TEST_ASSERT_EQUAL(int(Mode::Orchestrator), int(loadMode(store, Mode::Notifier)));
}

// ---------------------------------------------------------------------------
// A card that is present at SAVE time but MISSING at boot (card pulled) loses
// the config exactly as the never-had-a-card case: the guarantee is "survives
// iff a card is mounted at BOTH save and load", nothing weaker.
// ---------------------------------------------------------------------------
static void test_sd_pulled_between_save_and_reboot_loses_config() {
  FakeStore store;
  store.sdPresent = true;
  Config cfg;
  cfg.setProfile(ProfileId::Desk);
  cfg.setOverride(Param::RingFps, 45);
  TEST_ASSERT_TRUE(saveConfig(store, cfg));   // written to the card
  TEST_ASSERT_TRUE(saveMode(store, Mode::Orchestrator));

  reboot(store, /*sdPresentAfter=*/false);    // card removed before boot

  Config restored;
  TEST_ASSERT_FALSE(loadConfig(store, restored));
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(restored.profile()));
  TEST_ASSERT_FALSE(restored.hasOverride(Param::RingFps));
  // Mode independent of the card.
  TEST_ASSERT_EQUAL(int(Mode::Orchestrator), int(loadMode(store, Mode::Notifier)));
}

// ---------------------------------------------------------------------------
// The graceful-degradation guarantee the header promises: with the NVS
// namespace itself closed (no backing store at all), EVERY op fails softly and
// loaders leave the caller's in-RAM defaults untouched - never a crash, never a
// half-applied config.
// ---------------------------------------------------------------------------
static void test_no_backing_store_at_all_degrades_softly() {
  FakeStore store;
  store.nvsOk = false;             // namespace never opened
  store.sdPresent = false;

  Config cfg;
  cfg.setProfile(ProfileId::Desk);
  cfg.setOverride(Param::RingBrightness, 200);
  TEST_ASSERT_FALSE(saveConfig(store, cfg));
  TEST_ASSERT_FALSE(saveMode(store, Mode::Orchestrator));

  // loadMode returns the caller's default; loadConfig leaves `out` untouched.
  TEST_ASSERT_EQUAL(int(Mode::Notifier), int(loadMode(store, Mode::Notifier)));
  Config out;
  out.setProfile(ProfileId::Desk);
  out.setOverride(Param::RingBrightness, 55);
  TEST_ASSERT_FALSE(loadConfig(store, out));
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(out.profile()));   // untouched
  TEST_ASSERT_EQUAL(55, out.effective(Param::RingBrightness));   // untouched
}

// ---------------------------------------------------------------------------
// A corrupt blob on the card is rejected all-or-nothing: loadConfig returns
// false and the caller keeps its defaults (never a partial apply). This is the
// storage-tier view of test_config_store's deserialize-corruption coverage -
// it must hold through the getBlob path too.
// ---------------------------------------------------------------------------
static void test_corrupt_blob_on_card_falls_back_to_defaults() {
  FakeStore store;
  store.sdPresent = true;
  // Hand-place a blob with valid magic/version but a garbage profile byte.
  uint8_t bad[] = {'N', 'C', kConfigStoreVersion, kProfileCount /*out of range*/, 0};
  store.sdBlob[kCfgBlob] = std::vector<uint8_t>(bad, bad + sizeof bad);

  Config out;
  out.setProfile(ProfileId::Desk);
  out.setOverride(Param::RingBrightness, 77);
  TEST_ASSERT_FALSE(loadConfig(store, out));       // rejected
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(out.profile()));
  TEST_ASSERT_EQUAL(77, out.effective(Param::RingBrightness));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_with_sd_config_and_mode_both_persist);
  RUN_TEST(test_no_sd_only_mode_survives_config_lost);
  RUN_TEST(test_sd_pulled_between_save_and_reboot_loses_config);
  RUN_TEST(test_no_backing_store_at_all_degrades_softly);
  RUN_TEST(test_corrupt_blob_on_card_falls_back_to_defaults);
  return UNITY_END();
}
