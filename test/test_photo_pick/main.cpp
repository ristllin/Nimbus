#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/tg_updates.h"

// The photo rendition a model reads (tg_updates.h kPhotoPickBudget, kPhotoMinLongSide):
// the largest LEGIBLE rendition within the pick budget, else the smallest legible one
// within the download cap (kPhotoBudget), else the largest within the download cap,
// else the smallest. Telegram's own rendition sets drive the cases: a phone photo comes
// as 90, 320, 800 and 1280 px copies.

using nimbus::tg::Attachment;
using nimbus::tg::kPhotoMinLongSide;
using nimbus::tg::kPhotoPickBudget;
using nimbus::tg::parseUpdates;
using nimbus::tg::photoLegible;
using nimbus::tg::Update;

void setUp() {}
void tearDown() {}

struct Rendition {
  const char* id;
  unsigned w, h, size;
};

static std::string page(const std::vector<Rendition>& rs) {
  std::string photos;
  for (const Rendition& r : rs) {
    if (!photos.empty()) photos += ",";
    photos += std::string("{\"file_id\":\"") + r.id + "\"";
    if (r.w || r.h) photos += ",\"width\":" + std::to_string(r.w) + ",\"height\":" + std::to_string(r.h);
    if (r.size) photos += ",\"file_size\":" + std::to_string(r.size);
    photos += "}";
  }
  return "{\"ok\":true,\"result\":[{\"update_id\":9,\"message\":{\"chat\":{\"id\":10},\"photo\":[" + photos +
         "]}}]}";
}

static std::string picked(const std::vector<Rendition>& rs) {
  const std::string body = page(rs);
  std::vector<Update> u;
  bool ok = false, trunc = false;
  TEST_ASSERT_TRUE(parseUpdates(body.data(), body.size(), u, ok, trunc));
  TEST_ASSERT_EQUAL_INT(1, (int)u.size());
  TEST_ASSERT_EQUAL_INT((int)Attachment::Kind::Photo, (int)u[0].attachment.kind);
  return u[0].attachment.fileId;
}

// Lumi's photos (720x1280 phone photos, the 1280 px copy 139-170 KB): the 800 px copy,
// whose request body the fragmented heap sends reliably, never the 1280 px one.
static void test_a_phone_photo_reads_the_800_px_copy() {
  for (unsigned big : {139000u, 155000u, 170226u, 4000000u}) {
    TEST_ASSERT_EQUAL_STRING("x", picked({{"s", 90, 160, 1500}, {"m", 180, 320, 15400}, {"x", 450, 800, 66700},
                                          {"y", 720, 1280, big}})
                                      .c_str());
  }
  // Landscape the same way.
  TEST_ASSERT_EQUAL_STRING("x", picked({{"s", 90, 68, 1500}, {"m", 320, 240, 21000}, {"x", 800, 600, 70000},
                                        {"y", 1280, 960, 170226}})
                                    .c_str());
}

// A detailed scene whose 800 px copy is over the budget still reads it: it is the
// smallest copy a label can be read from, and far smaller than the 1280 px one.
static void test_an_800_px_copy_over_budget_is_still_taken() {
  TEST_ASSERT_EQUAL_STRING("x", picked({{"s", 90, 160, 1900}, {"m", 180, 320, 24000},
                                        {"x", 450, 800, kPhotoPickBudget + 9000}, {"y", 720, 1280, 240000}})
                                    .c_str());
}

// No 800 px copy: the 1280 px one, never the 320 px thumbnail (a label is unreadable
// at 320 px). The album fixture of lane D has exactly this shape.
static void test_never_a_thumbnail_while_a_legible_copy_exists() {
  TEST_ASSERT_EQUAL_STRING("l", picked({{"s", 90, 68, 1500}, {"m", 320, 240, 21000}, {"l", 1280, 960, 170226}})
                                    .c_str());
  // The bench fixture shape (a 90 px thumbnail plus one large card file).
  TEST_ASSERT_EQUAL_STRING("LOCAL_card", picked({{"THUMB", 90, 60, 1200}, {"LOCAL_card", 2560, 1920, 130537}})
                                             .c_str());
}

// A small legible copy within the budget beats a larger one within it too: the largest
// legible copy that fits wins.
static void test_the_largest_legible_copy_that_fits_wins() {
  TEST_ASSERT_EQUAL_STRING("x2", picked({{"m", 180, 320, 15000}, {"x1", 450, 800, 40000}, {"x2", 600, 1066, 70000},
                                         {"y", 720, 1280, 160000}})
                                     .c_str());
}

// A photo that is small everywhere (no copy reaches 640 px) keeps the old rule: the
// largest within the download cap, not the largest within the pick budget.
static void test_a_small_photo_takes_the_largest_copy_that_fits() {
  TEST_ASSERT_EQUAL_STRING("m", picked({{"s", 90, 90, 1100}, {"m", 320, 320, 19000}}).c_str());
  TEST_ASSERT_EQUAL_STRING("o", picked({{"s", 90, 90, 1100}, {"m", 320, 320, 20000}, {"o", 600, 600, 95000}})
                                    .c_str());
}

// A copy over the download cap is never chosen while another fits it (it would be
// refused as too large); when every copy is over the cap, the smallest.
static void test_the_download_cap_still_bounds_the_pick() {
  TEST_ASSERT_EQUAL_STRING("t", picked({{"t", 90, 60, 1200}, {"full", 2560, 1920, nimbus::tg::kPhotoBudget + 1}})
                                    .c_str());
  TEST_ASSERT_EQUAL_STRING("a", picked({{"a", 1280, 960, 2000000}, {"b", 2560, 1920, 5000000}}).c_str());
}

// Renditions without width and height (never from Telegram, but the Bot API marks no
// field as guaranteed) are not counted as legible: the old largest-within-cap rule.
static void test_renditions_without_dimensions_keep_the_old_rule() {
  TEST_ASSERT_EQUAL_STRING("mid", picked({{"thumb", 0, 0, 900}, {"mid", 0, 0, 120000}, {"huge", 0, 0, 4000000}})
                                      .c_str());
}

// Sizes in bytes missing everywhere: the last (full-size) entry, as before.
static void test_sizeless_renditions_take_the_last() {
  TEST_ASSERT_EQUAL_STRING("y", picked({{"s", 90, 160, 0}, {"x", 450, 800, 0}, {"y", 720, 1280, 0}}).c_str());
}

// The class: whatever Telegram offers, the pick never needs the download refused when a
// copy fits the cap, is legible whenever a legible copy fits the cap, and is in the
// 800 px class (within the pick budget) whenever a legible copy fits that budget.
static void checkPick(const std::vector<Rendition>& rs) {
  const unsigned cap = nimbus::tg::kPhotoBudget;
  const std::string got = picked(rs);
  const Rendition* p = nullptr;
  bool legibleInCap = false, legibleFits = false, anyInCap = false;
  for (const Rendition& r : rs) {
    if (got == r.id) p = &r;
    anyInCap = anyInCap || r.size <= cap;
    legibleInCap = legibleInCap || (photoLegible(r.w, r.h) && r.size <= cap);
    legibleFits = legibleFits || (photoLegible(r.w, r.h) && r.size <= kPhotoPickBudget);
  }
  TEST_ASSERT_NOT_NULL(p);
  if (anyInCap) TEST_ASSERT_TRUE(p->size <= cap);                   // never a refused copy
  if (legibleInCap) TEST_ASSERT_TRUE(photoLegible(p->w, p->h));     // never a thumbnail
  if (legibleFits) TEST_ASSERT_TRUE(p->size <= kPhotoPickBudget);   // the 800 px class
}

static void test_the_pick_is_legible_whenever_it_can_be() {
  const unsigned sides[] = {90, 320, 480, 640, 800, 1280, 2560};
  const unsigned cap = nimbus::tg::kPhotoBudget;
  const unsigned sizes[] = {1200, 60000, kPhotoPickBudget, kPhotoPickBudget + 1, 170000, cap + 1};
  for (unsigned a : sides)
    for (unsigned b : sides)
      for (unsigned sa : sizes)
        for (unsigned sb : sizes)
          for (unsigned sc : {15000u, cap + 7})
            checkPick({{"a", a, a / 2 + 1, sa}, {"b", b, b * 3 / 4 + 1, sb}, {"c", 320, 180, sc}});
}

static void test_legibility_rule() {
  TEST_ASSERT_FALSE(photoLegible(0, 0));   // not reported
  TEST_ASSERT_TRUE(photoLegible(450, 800));
  TEST_ASSERT_TRUE(photoLegible(800, 600));
  TEST_ASSERT_TRUE(photoLegible(kPhotoMinLongSide, 10));
  TEST_ASSERT_FALSE(photoLegible(kPhotoMinLongSide - 1, kPhotoMinLongSide - 1));
  TEST_ASSERT_FALSE(photoLegible(180, 320));
  TEST_ASSERT_FALSE(photoLegible(320, 240));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_phone_photo_reads_the_800_px_copy);
  RUN_TEST(test_an_800_px_copy_over_budget_is_still_taken);
  RUN_TEST(test_never_a_thumbnail_while_a_legible_copy_exists);
  RUN_TEST(test_the_largest_legible_copy_that_fits_wins);
  RUN_TEST(test_a_small_photo_takes_the_largest_copy_that_fits);
  RUN_TEST(test_the_download_cap_still_bounds_the_pick);
  RUN_TEST(test_renditions_without_dimensions_keep_the_old_rule);
  RUN_TEST(test_sizeless_renditions_take_the_last);
  RUN_TEST(test_the_pick_is_legible_whenever_it_can_be);
  RUN_TEST(test_legibility_rule);
  return UNITY_END();
}
