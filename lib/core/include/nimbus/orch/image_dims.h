#pragma once
#include <cstddef>
#include <cstdint>

// image_dims - a photo's width and height, read from its own header bytes, so a
// head round can estimate what the photo costs in the model's window (each provider
// counts an image by its pixels, not its bytes) before the request is built.
//
// Read: JPEG (the first SOFn frame header: baseline SOF0, progressive SOF2 and the
// other frame types), PNG (IHDR), WebP (VP8 lossy, VP8L lossless, VP8X extended) and
// GIF (the logical screen). Anything else, or a header cut short, reads 0x0 and the
// caller falls back to a byte-based estimate. Reads only inside [bytes, bytes + len).
// Arduino-free, host-tested (test_image_dims).
namespace nimbus {
namespace orch {

struct ImageDims {
  uint32_t width = 0;
  uint32_t height = 0;
  bool known() const { return width > 0 && height > 0; }
};

ImageDims imageDims(const uint8_t* bytes, size_t len);

}  // namespace orch
}  // namespace nimbus
