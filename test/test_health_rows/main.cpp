// test_health_rows - the Settings storage and Providers rows (19 Oct triage).
//
// On 19 Oct Lumi's /api/health read 9 ok and 1 degraded while its settings store had
// 0 B free with 266 failed writes and two providers were refusing for billing. These
// cases walk every (low, fails, starved) combination, prove a parked provider shows
// in the Providers row (the one per-provider park) and leaves it when it answers,
// prove a healthy device gains no degraded row, and pin the Lumi-shaped report as a
// golden (the text system.health and the Home panel show).
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/health_rows.h"
#include "nimbus/orch/provider_park.h"

using nimbus::orch::HealthRow;
using nimbus::orch::PaidSeam;
using nimbus::orch::ParkKind;
using nimbus::orch::SettingsStorageInputs;
using nimbus::orch::providerPark;

static const uint32_t kFp = 0x5eed;   // a key fingerprint (the key is never compared here)

void setUp() {
  providerPark().forgetStorage();
  providerPark().clear();
}
void tearDown() {
  providerPark().forgetStorage();
  providerPark().clear();
}

// Every combination: a row exactly when something is wrong, naming each cause.
static void test_settings_storage_row_over_every_combination() {
  for (int low = 0; low < 2; low++)
    for (int fails = 0; fails < 2; fails++)
      for (int starved = 0; starved < 2; starved++) {
        SettingsStorageInputs in;
        in.reserveEntries = 16;
        in.freeEntries = low ? 9 : 40;
        in.failsSinceBoot = fails ? 266 : 0;
        in.lastFailKey = fails ? "usgLedger" : "";
        in.starved = starved != 0;
        HealthRow r;
        const bool shown = nimbus::orch::settingsStorageRow(in, r);
        char label[64];
        std::snprintf(label, sizeof label, "low=%d fails=%d starved=%d", low, fails, starved);
        TEST_ASSERT_EQUAL_MESSAGE(low || fails || starved, shown, label);
        if (!shown) continue;
        TEST_ASSERT_EQUAL_STRING_MESSAGE("degraded", r.state.c_str(), label);
        TEST_ASSERT_EQUAL_STRING("Settings storage", r.label.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(fails != 0, r.detail.find("266 saves failed since start, last usgLedger") != std::string::npos, label);
        TEST_ASSERT_EQUAL_MESSAGE(low != 0, r.detail.find("under the reserve of 16") != std::string::npos, label);
        TEST_ASSERT_EQUAL_MESSAGE(starved != 0, r.detail.find("reserve cannot be restored") != std::string::npos, label);
        TEST_ASSERT_TRUE(r.detail.find(low ? "9 entries free" : "40 entries free") != std::string::npos);
      }
}

// Counter-test: a healthy store (free above the reserve, no failure, not starved)
// adds no row - a fresh device shows nothing new.
static void test_healthy_device_has_no_row() {
  SettingsStorageInputs in;
  in.reserveEntries = 16;
  in.freeEntries = 16;   // exactly at the reserve is not under it
  HealthRow r;
  TEST_ASSERT_FALSE(nimbus::orch::settingsStorageRow(in, r));
  in.freeEntries = 400;
  TEST_ASSERT_FALSE(nimbus::orch::settingsStorageRow(in, r));
  const HealthRow p = nimbus::orch::providersRow(providerPark(), 1000);
  TEST_ASSERT_EQUAL_STRING("ok", p.state.c_str());
  TEST_ASSERT_EQUAL_STRING("every provider open", p.detail.c_str());
}

// A parked provider shows in the Providers row (what it refused, the HTTP status,
// when it is checked again); when it answers again it leaves the row, and the row is
// ok once every provider is open. Reading the row never starts a probe or ends a park.
static void test_parked_provider_row_and_unpark() {
  providerPark().park("openai", PaidSeam::Chat, kFp, ParkKind::Billing, 429, 1000);
  providerPark().park("anthropic", PaidSeam::Chat, kFp, ParkKind::Auth, 401, 1000);
  HealthRow row;
  for (int read = 0; read < 3; read++) row = nimbus::orch::providersRow(providerPark(), 1000 + 12 * 60000);
  TEST_ASSERT_EQUAL_STRING("providers", row.key.c_str());
  TEST_ASSERT_EQUAL_STRING("Providers", row.label.c_str());
  TEST_ASSERT_EQUAL_STRING("degraded", row.state.c_str());
  TEST_ASSERT_TRUE_MESSAGE(row.detail.find("openai says the account is out of credit (HTTP 429); "
                                           "it is checked again in 18 minutes") != std::string::npos,
                           row.detail.c_str());
  TEST_ASSERT_TRUE_MESSAGE(row.detail.find("anthropic refused the key (HTTP 401)") != std::string::npos,
                           row.detail.c_str());
  TEST_ASSERT_FALSE(providerPark().available("openai", PaidSeam::Chat, kFp, 1000 + 12 * 60000).open);
  providerPark().noteSuccess("openai", PaidSeam::Chat);
  row = nimbus::orch::providersRow(providerPark(), 1000 + 13 * 60000);
  TEST_ASSERT_TRUE(row.detail.find("openai") == std::string::npos);
  TEST_ASSERT_TRUE(row.detail.find("anthropic") != std::string::npos);
  providerPark().noteSuccess("anthropic", PaidSeam::Chat);
  row = nimbus::orch::providersRow(providerPark(), 1000 + 14 * 60000);
  TEST_ASSERT_EQUAL_STRING("ok", row.state.c_str());
  TEST_ASSERT_EQUAL_STRING("every provider open", row.detail.c_str());
  // The pause switch closes every provider: one line says so (and why).
  providerPark().pause(nimbus::orch::kPauseAllProviders, nimbus::orch::kPauseSpendCeiling);
  row = nimbus::orch::providersRow(providerPark(), 1000 + 15 * 60000);
  TEST_ASSERT_EQUAL_STRING("degraded", row.state.c_str());
  TEST_ASSERT_EQUAL_STRING("paid requests to every provider are paused: the monthly spend limit is reached",
                           row.detail.c_str());
}

// The Lumi 19 Oct shape, as system.health and the Home panel render it.
static void test_lumi_19oct_golden() {
  SettingsStorageInputs in;
  in.reserveEntries = 16;
  in.freeEntries = 0;
  in.failsSinceBoot = 266;
  in.lastFailKey = "usgLedger";
  in.starved = true;
  std::vector<HealthRow> rows(1);
  TEST_ASSERT_TRUE(nimbus::orch::settingsStorageRow(in, rows[0]));
  // Both providers refusing for billing, as on 19 Oct (parked 20 minutes ago).
  providerPark().park("openai", PaidSeam::Chat, kFp, ParkKind::Billing, 429, 0);
  providerPark().park("anthropic", PaidSeam::Chat, kFp, ParkKind::Billing, 400, 0);
  rows.push_back(nimbus::orch::providersRow(providerPark(), 20 * 60000));
  const std::string text = nimbus::orch::healthRowsText(rows);
  const char* path = "test/golden/health_rows_lumi_19oct.txt";
  const char* bless = std::getenv("GOLDEN_UPDATE");
  if (bless && !std::strcmp(bless, "1")) {
    FILE* f = std::fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return;
  }
  std::string golden;
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "missing golden (GOLDEN_UPDATE=1 to bless after a visual check)");
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) golden.append(buf, n);
  std::fclose(f);
  if (golden != text) {
    FILE* o = std::fopen("test/golden/out/health_rows_lumi_19oct.txt", "wb");
    if (o) { std::fwrite(text.data(), 1, text.size(), o); std::fclose(o); }
  }
  TEST_ASSERT_EQUAL_STRING(golden.c_str(), text.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_settings_storage_row_over_every_combination);
  RUN_TEST(test_healthy_device_has_no_row);
  RUN_TEST(test_parked_provider_row_and_unpark);
  RUN_TEST(test_lumi_19oct_golden);
  return UNITY_END();
}
