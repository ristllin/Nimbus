#include "nimbus/orch/image_dims.h"

#include <cstring>

namespace nimbus {
namespace orch {

namespace {

uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint32_t le16(const uint8_t* p) { return (uint32_t)p[1] << 8 | p[0]; }
uint32_t le24(const uint8_t* p) { return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

ImageDims dims(uint32_t w, uint32_t h) {
  ImageDims d;
  if (w && h) {
    d.width = w;
    d.height = h;
  }
  return d;
}

// A start-of-frame marker: C0-CF except DHT (C4), JPG (C8) and DAC (CC).
bool isSof(uint8_t m) { return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC; }
// Markers with no length field: TEM, RSTn, SOI, EOI.
bool standalone(uint8_t m) { return m == 0x01 || (m >= 0xD0 && m <= 0xD9); }

ImageDims jpeg(const uint8_t* b, size_t len) {
  size_t i = 2;   // past SOI
  while (i + 4 <= len) {
    if (b[i] != 0xFF) return ImageDims();   // not on a marker: corrupt
    const uint8_t m = b[i + 1];
    if (m == 0xFF) {   // fill byte
      ++i;
      continue;
    }
    if (standalone(m)) {
      i += 2;
      continue;
    }
    if (m == 0xDA) return ImageDims();   // start of scan before any frame header
    const size_t seg = be16(b + i + 2);
    if (seg < 2) return ImageDims();
    if (isSof(m)) {
      if (i + 2 + seg > len || seg < 7) return ImageDims();
      return dims(be16(b + i + 7), be16(b + i + 5));   // [len][precision][height][width]
    }
    i += 2 + seg;
  }
  return ImageDims();
}

ImageDims png(const uint8_t* b, size_t len) {
  if (len < 24 || std::memcmp(b + 12, "IHDR", 4) != 0) return ImageDims();
  return dims(be32(b + 16), be32(b + 20));
}

ImageDims gif(const uint8_t* b, size_t len) {
  if (len < 10) return ImageDims();
  return dims(le16(b + 6), le16(b + 8));
}

ImageDims webp(const uint8_t* b, size_t len) {
  if (len < 30) return ImageDims();
  const uint8_t* c = b + 12;   // the first chunk
  if (std::memcmp(c, "VP8 ", 4) == 0) {
    // frame tag (3), then the key-frame start code 9d 01 2a, then 14-bit width and height
    if (b[23] != 0x9D || b[24] != 0x01 || b[25] != 0x2A) return ImageDims();
    return dims(le16(b + 26) & 0x3FFF, le16(b + 28) & 0x3FFF);
  }
  if (std::memcmp(c, "VP8L", 4) == 0) {
    if (b[20] != 0x2F) return ImageDims();   // the lossless signature
    const uint32_t v = (uint32_t)b[21] | (uint32_t)b[22] << 8 | (uint32_t)b[23] << 16 | (uint32_t)b[24] << 24;
    return dims((v & 0x3FFF) + 1, ((v >> 14) & 0x3FFF) + 1);
  }
  if (std::memcmp(c, "VP8X", 4) == 0) return dims(le24(b + 24) + 1, le24(b + 27) + 1);
  return ImageDims();
}

}  // namespace

ImageDims imageDims(const uint8_t* bytes, size_t len) {
  if (!bytes || len < 4) return ImageDims();
  if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) return jpeg(bytes, len);
  static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (len >= 8 && std::memcmp(bytes, kPng, 8) == 0) return png(bytes, len);
  if (len >= 6 && (std::memcmp(bytes, "GIF87a", 6) == 0 || std::memcmp(bytes, "GIF89a", 6) == 0)) return gif(bytes, len);
  if (len >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0) return webp(bytes, len);
  return ImageDims();
}

}  // namespace orch
}  // namespace nimbus
