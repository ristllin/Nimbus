#include <unity.h>

#include <string>

// CUM-514: what the head says about its hardware comes from the build's board
// profile, checked here against the solide-drivers board constants it describes.
// The Freenove all-in-one used to describe a 45-LED ring, resistive touch and an
// INMP441 microphone - the Solide S3 it is not.
#include "nimbus/harness/board_identity.h"
#include "solide/boards/active_board.h"   // the default build board (SOLIDE_BOARD unset)
#include "solide/boards/board_freenove_s3.h"
#include "solide/boards/board_solide_s3.h"

using agent::BoardIdentity;

void setUp() {}
void tearDown() {}

static bool has(const char* text, const char* needle) {
  return std::string(text).find(needle) != std::string::npos;
}

// Every fact an identity states must match the profile it names. One check per
// board-profile field the prose mentions, so a new board (or a new profile field
// in the prose) cannot drift silently.
static void checkAgainstProfile(const BoardIdentity& id, const solide::Board& b) {
  TEST_ASSERT_EQUAL_MESSAGE(b.hasRing, id.physicalRing, id.name);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(b.led.count, id.ledCount, id.name);
  const std::string ring = std::to_string(b.led.count) + "-LED";
  TEST_ASSERT_EQUAL_MESSAGE(b.hasRing, has(id.hardware, ring.c_str()), id.name);
  TEST_ASSERT_EQUAL_MESSAGE(!b.hasRing, has(id.hardware, "NO\nLED ring") ||
                                            has(id.hardware, "NO LED ring"), id.name);
  const bool cap = b.touchKind == solide::TouchKind::CapacitiveI2c;
  TEST_ASSERT_EQUAL_MESSAGE(cap, has(id.hardware, "capacitive"), id.name);
  TEST_ASSERT_EQUAL_MESSAGE(!cap, has(id.hardware, "resistive"), id.name);
  const bool codec = b.audioKind == solide::AudioKind::Es8311Codec;
  TEST_ASSERT_EQUAL_MESSAGE(codec, has(id.hardware, "ES8311"), id.name);
  TEST_ASSERT_EQUAL_MESSAGE(!codec, has(id.hardware, "INMP441"), id.name);
  const bool sdmmc = b.sdKind == solide::SdKind::Sdmmc;
  TEST_ASSERT_EQUAL_MESSAGE(sdmmc, has(id.hardware, "SDMMC"), id.name);
  const std::string cells = std::to_string(b.batt.cells) + "S Li-ion";
  TEST_ASSERT_TRUE_MESSAGE(has(id.hardware, cells.c_str()), id.name);
}

static void test_solide_s3_identity_matches_its_profile() {
  const BoardIdentity& id = agent::boardIdentity("solide_s3", 320, 240);
  TEST_ASSERT_EQUAL_STRING("solide_s3", id.slug);
  TEST_ASSERT_TRUE(has(id.name, "Solide S3"));
  checkAgainstProfile(id, solide::kBoardSolideS3);
}

// The bench unit: a Freenove 2.8" CYD (GET /api/state board=freenove_s3,
// hasRing=false, touchCap=true).
static void test_freenove_identity_matches_its_profile() {
  const BoardIdentity& id = agent::boardIdentity("freenove_s3", 320, 240);
  TEST_ASSERT_EQUAL_STRING("freenove_s3", id.slug);
  TEST_ASSERT_TRUE(has(id.name, "Freenove"));
  TEST_ASSERT_TRUE(has(id.name, "2.8\""));
  TEST_ASSERT_EQUAL_STRING("2.8\" color touchscreen", id.display);
  checkAgainstProfile(id, solide::kBoardFreenoveS3);
}

// Every Freenove panel size the build matrix ships (esp32s3-cyd / -cyd-35 / -cyd-40)
// names its own screen and shares the board profile's facts; an unlisted size of a
// known board still gets that board, never another one.
static void test_every_freenove_panel_names_its_screen() {
  struct { int w, h; const char* inch; } sizes[] = {{320, 240, "2.8\""}, {480, 320, "3.5\""},
                                                    {480, 480, "4.0\""}};
  for (const auto& sz : sizes) {
    const BoardIdentity& id = agent::boardIdentity("freenove_s3", sz.w, sz.h);
    TEST_ASSERT_TRUE_MESSAGE(has(id.name, sz.inch), sz.inch);
    TEST_ASSERT_TRUE_MESSAGE(has(id.display, sz.inch), sz.inch);
    checkAgainstProfile(id, solide::kBoardFreenoveS3);
  }
  TEST_ASSERT_TRUE(has(agent::boardIdentity("freenove_s3", 800, 480).name, "Freenove"));
}

// The class: no entry ever names another board, and an unknown slug gets the
// generic identity, which names none.
static void test_no_identity_names_another_board() {
  for (const BoardIdentity& id : agent::kBoardIdentities) {
    const bool solideEntry = std::string(id.slug) == "solide_s3";
    TEST_ASSERT_EQUAL_MESSAGE(solideEntry, has(id.hardware, "Solide"), id.name);
    TEST_ASSERT_EQUAL_MESSAGE(solideEntry, has(id.hardware, "DevKitC"), id.name);
    TEST_ASSERT_EQUAL_MESSAGE(!solideEntry, has(id.hardware, "Freenove"), id.name);
  }
  const BoardIdentity& g = agent::boardIdentity("some_future_board", 320, 240);
  TEST_ASSERT_EQUAL_PTR(&agent::kGenericBoardIdentity, &g);
  TEST_ASSERT_FALSE(has(g.name, "Solide") || has(g.name, "Freenove"));
  TEST_ASSERT_FALSE(has(g.hardware, "Solide") || has(g.hardware, "Freenove"));
  TEST_ASSERT_EQUAL_PTR(&agent::kGenericBoardIdentity, &agent::boardIdentity(nullptr, 0, 0));
}

// The build's own board uses the same token /api/state reports: with SOLIDE_BOARD
// unset (this host build) that is the solide-drivers default, solide_s3.
static void test_build_board_is_the_drivers_active_board() {
  const BoardIdentity& id = agent::buildBoardIdentity();
  TEST_ASSERT_EQUAL_STRING("solide_s3", id.slug);
  TEST_ASSERT_EQUAL(solide::activeBoard().hasRing, id.physicalRing);
  TEST_ASSERT_EQUAL_UINT16(solide::activeBoard().led.count, id.ledCount);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_solide_s3_identity_matches_its_profile);
  RUN_TEST(test_freenove_identity_matches_its_profile);
  RUN_TEST(test_every_freenove_panel_names_its_screen);
  RUN_TEST(test_no_identity_names_another_board);
  RUN_TEST(test_build_board_is_the_drivers_active_board);
  UNITY_END();
  return 0;
}
