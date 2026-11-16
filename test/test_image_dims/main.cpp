// test_image_dims - a photo's width and height from its own header bytes, for the
// per-provider window estimate of a photo round (image_parts photoTokens). Every
// format the head takes is read from a real file's header; anything else, and every
// header cut short, reads 0x0 and the caller falls back to a byte estimate.
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "fixtures.h"
#include "nimbus/orch/image_dims.h"

using nimbus::orch::ImageDims;
using nimbus::orch::imageDims;

void setUp() {}
void tearDown() {}

struct Case {
  const char* name;
  const uint8_t* bytes;
  size_t len;
  uint32_t w, h;
};
#define CASE(n, w, h) Case{#n, fixtures::k_##n, sizeof(fixtures::k_##n), w, h}

static const Case kCases[] = {
    CASE(jpeg_baseline_1280x960, 1280, 960),   CASE(jpeg_progressive_1024x768, 1024, 768),
    CASE(jpeg_exif_1280x960, 1280, 960),       CASE(png_800x600, 800, 600),
    CASE(gif_640x480, 640, 480),               CASE(webp_vp8_1600x1200, 1600, 1200),
    CASE(webp_vp8l_300x200, 300, 200),         CASE(webp_vp8x_2048x1536, 2048, 1536),
};

static void test_every_format_reads_its_size() {
  for (const Case& c : kCases) {
    const ImageDims d = imageDims(c.bytes, c.len);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.w, d.width, c.name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.h, d.height, c.name);
    TEST_ASSERT_TRUE_MESSAGE(d.known(), c.name);
  }
}

// The class: every prefix of every header either reads the right size or reads 0x0
// (never a wrong size, never a read past the end: the sanitizer-free host build reads
// each prefix from its own exact-size copy).
static void test_every_cut_header_reads_zero_or_right() {
  for (const Case& c : kCases) {
    for (size_t n = 0; n <= c.len; ++n) {
      std::vector<uint8_t> copy(c.bytes, c.bytes + n);
      const ImageDims d = imageDims(n ? copy.data() : nullptr, n);
      const bool zero = d.width == 0 && d.height == 0;
      const bool right = d.width == c.w && d.height == c.h;
      if (!zero && !right) {
        const std::string msg = std::string(c.name) + " cut at " + std::to_string(n);
        TEST_FAIL_MESSAGE(msg.c_str());
      }
    }
  }
}

static void test_garbage_reads_zero() {
  const uint8_t text[] = "not an image at all, just words";
  TEST_ASSERT_FALSE(imageDims(text, sizeof text).known());
  TEST_ASSERT_FALSE(imageDims(nullptr, 100).known());
  // A JPEG whose markers wander off the marker grid, and one that starts its scan with no
  // frame header, read 0x0.
  uint8_t bad[64] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, 0x12, 0x34};
  TEST_ASSERT_FALSE(imageDims(bad, sizeof bad).known());
  uint8_t scanFirst[16] = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08};
  TEST_ASSERT_FALSE(imageDims(scanFirst, sizeof scanFirst).known());
  // A zero-sized PNG and a WebP with an unknown chunk read 0x0.
  std::vector<uint8_t> png(fixtures::k_png_800x600, fixtures::k_png_800x600 + 32);
  std::memset(png.data() + 16, 0, 8);
  TEST_ASSERT_FALSE(imageDims(png.data(), png.size()).known());
  std::vector<uint8_t> webp(fixtures::k_webp_vp8_1600x1200, fixtures::k_webp_vp8_1600x1200 + 64);
  std::memcpy(webp.data() + 12, "ALPH", 4);
  TEST_ASSERT_FALSE(imageDims(webp.data(), webp.size()).known());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_format_reads_its_size);
  RUN_TEST(test_every_cut_header_reads_zero_or_right);
  RUN_TEST(test_garbage_reads_zero);
  return UNITY_END();
}
