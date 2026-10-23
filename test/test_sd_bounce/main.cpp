// The SD card's reserved DMA buffer (nimbus/sd_bounce.h): which path a transfer takes,
// and that the one-sector loops move every byte to and from the right sectors, stop at
// the first error and never touch the caller's buffer past it.
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "nimbus/sd_bounce.h"

using nimbus::sdbounce::Route;
using nimbus::sdbounce::readSectors;
using nimbus::sdbounce::route;
using nimbus::sdbounce::writeSectors;

void setUp() {}
void tearDown() {}

namespace {

constexpr size_t kSector = 512;

// A card of N sectors. Every transfer must come through the bounce buffer it was given
// (the only buffer the host could DMA), and one sector can be made to fail.
struct FakeCard {
  std::vector<uint8_t> data;
  const uint8_t* bounce = nullptr;
  size_t failAt = SIZE_MAX;
  int failErr = 0x107;   // an SD timeout
  int transfers = 0;
  bool foreignBuffer = false;
  explicit FakeCard(size_t sectors) : data(sectors * kSector) {
    for (size_t i = 0; i < data.size(); i++) data[i] = uint8_t(i * 7 + i / kSector);
  }
  int read(uint8_t* buf, size_t sector) {
    transfers++;
    if (buf != bounce) foreignBuffer = true;
    if (sector == failAt) return failErr;
    std::memcpy(buf, &data[sector * kSector], kSector);
    return 0;
  }
  int write(const uint8_t* buf, size_t sector) {
    transfers++;
    if (buf != bounce) foreignBuffer = true;
    if (sector == failAt) return failErr;
    std::memcpy(&data[sector * kSector], buf, kSector);
    return 0;
  }
};

}  // namespace

// The class rule over every input: a DMA-ready buffer always goes direct; otherwise the
// reserve is used exactly when it exists and holds one sector; nothing else bounces.
static void test_route_truth_table() {
  const size_t reserves[] = {0, 256, 511, 512, 4096};
  const size_t sectors[] = {0, 512, 1024, 4096};
  for (int ready = 0; ready < 2; ready++)
    for (size_t res : reserves)
      for (size_t ss : sectors) {
        const Route r = route(ready != 0, res, ss);
        Route want;
        if (ready) want = Route::Direct;
        else if (res != 0 && ss != 0 && ss <= res) want = Route::Bounce;
        else want = Route::Fallback;
        TEST_ASSERT_EQUAL_MESSAGE(int(want), int(r), "route() broke the rule");
      }
  // The shipped shape: a PSRAM buffer with the 512 B reserve on a 512 B card bounces.
  TEST_ASSERT_EQUAL(int(Route::Bounce), int(route(false, 512, 512)));
  // Without a reserve the driver keeps its own path (the pre-fix behavior).
  TEST_ASSERT_EQUAL(int(Route::Fallback), int(route(false, 0, 512)));
}

// A multi-sector read lands every byte of every sector in order, one transfer per
// sector, always through the reserve.
static void test_read_moves_every_sector() {
  FakeCard card(64);
  uint8_t bounce[kSector];
  card.bounce = bounce;
  std::vector<uint8_t> dst(9 * kSector, 0xEE);
  const int err = readSectors(bounce, kSector, dst.data(), 5, 9,
                              [&](uint8_t* b, size_t s) { return card.read(b, s); });
  TEST_ASSERT_EQUAL(0, err);
  TEST_ASSERT_EQUAL(9, card.transfers);
  TEST_ASSERT_FALSE(card.foreignBuffer);
  TEST_ASSERT_EQUAL_MEMORY(&card.data[5 * kSector], dst.data(), dst.size());
}

// A multi-sector write puts every byte on the right sectors and leaves the neighbours alone.
static void test_write_moves_every_sector() {
  FakeCard card(64);
  const std::vector<uint8_t> before = card.data;
  uint8_t bounce[kSector];
  card.bounce = bounce;
  std::vector<uint8_t> src(7 * kSector);
  for (size_t i = 0; i < src.size(); i++) src[i] = uint8_t(0xA5 ^ i ^ (i >> 9));
  const int err = writeSectors(bounce, kSector, src.data(), 20, 7,
                               [&](uint8_t* b, size_t s) { return card.write(b, s); });
  TEST_ASSERT_EQUAL(0, err);
  TEST_ASSERT_EQUAL(7, card.transfers);
  TEST_ASSERT_FALSE(card.foreignBuffer);
  TEST_ASSERT_EQUAL_MEMORY(src.data(), &card.data[20 * kSector], src.size());
  TEST_ASSERT_EQUAL_MEMORY(before.data(), card.data.data(), 20 * kSector);
  TEST_ASSERT_EQUAL_MEMORY(&before[27 * kSector], &card.data[27 * kSector], (64 - 27) * kSector);
}

// The first failing sector ends a read: its error comes back unchanged, nothing after it
// is attempted, and the caller's buffer past the good sectors is untouched.
static void test_read_stops_at_the_first_error() {
  FakeCard card(32);
  uint8_t bounce[kSector];
  card.bounce = bounce;
  card.failAt = 12;
  std::vector<uint8_t> dst(6 * kSector, 0xEE);
  const int err = readSectors(bounce, kSector, dst.data(), 10, 6,
                              [&](uint8_t* b, size_t s) { return card.read(b, s); });
  TEST_ASSERT_EQUAL(0x107, err);
  TEST_ASSERT_EQUAL(3, card.transfers);   // sectors 10, 11, then 12 fails
  TEST_ASSERT_EQUAL_MEMORY(&card.data[10 * kSector], dst.data(), 2 * kSector);
  for (size_t i = 2 * kSector; i < dst.size(); i++) TEST_ASSERT_EQUAL_HEX8(0xEE, dst[i]);
}

// The first failing sector ends a write: later sectors keep what they held.
static void test_write_stops_at_the_first_error() {
  FakeCard card(32);
  const std::vector<uint8_t> before = card.data;
  uint8_t bounce[kSector];
  card.bounce = bounce;
  card.failAt = 4;
  std::vector<uint8_t> src(5 * kSector, 0x3C);
  const int err = writeSectors(bounce, kSector, src.data(), 2, 5,
                               [&](uint8_t* b, size_t s) { return card.write(b, s); });
  TEST_ASSERT_EQUAL(0x107, err);
  TEST_ASSERT_EQUAL(3, card.transfers);   // 2, 3, then 4 fails
  TEST_ASSERT_EQUAL_MEMORY(src.data(), &card.data[2 * kSector], 2 * kSector);
  TEST_ASSERT_EQUAL_MEMORY(&before[4 * kSector], &card.data[4 * kSector], 28 * kSector);
}

// A zero-sector transfer does nothing and succeeds.
static void test_zero_sectors_is_a_no_op() {
  FakeCard card(4);
  uint8_t bounce[kSector];
  card.bounce = bounce;
  uint8_t dst[1] = {0xEE};
  TEST_ASSERT_EQUAL(0, readSectors(bounce, kSector, dst, 0, 0,
                                   [&](uint8_t* b, size_t s) { return card.read(b, s); }));
  TEST_ASSERT_EQUAL(0, writeSectors(bounce, kSector, dst, 0, 0,
                                    [&](uint8_t* b, size_t s) { return card.write(b, s); }));
  TEST_ASSERT_EQUAL(0, card.transfers);
  TEST_ASSERT_EQUAL_HEX8(0xEE, dst[0]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_route_truth_table);
  RUN_TEST(test_read_moves_every_sector);
  RUN_TEST(test_write_moves_every_sector);
  RUN_TEST(test_read_stops_at_the_first_error);
  RUN_TEST(test_write_stops_at_the_first_error);
  RUN_TEST(test_zero_sectors_is_a_no_op);
  return UNITY_END();
}
