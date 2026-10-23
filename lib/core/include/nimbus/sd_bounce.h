#pragma once
// SD card transfers through one reserved DMA-capable buffer.
//
// The ESP32-S3 SDMMC host can only DMA to and from internal memory. FATFS keeps its
// window and per-file caches in PSRAM on this build (CONFIG_FATFS_ALLOC_PREFER_EXTRAM),
// so the IDF driver takes its bounce path on every card transfer and allocates a
// fresh sector-sized internal DMA buffer per call (sdmmc_read_sectors /
// sdmmc_write_sectors). When internal memory is short that allocation fails, the
// transfer returns ESP_ERR_NO_MEM ("not enough mem"), FATFS reports a disk error and
// the card looks broken although it is fine. The device instead reserves one
// sector-sized DMA buffer at boot and moves every transfer whose buffer the host
// cannot DMA through it, one sector at a time, exactly as the driver's own bounce
// path does, minus the allocation (src/sys/sd_dma_bounce.cpp).
//
// Portable: the transfer is a callback, so the host suite drives the routing and the
// copy loops over a fake card.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nimbus::sdbounce {

enum class Route : uint8_t {
  Direct,    // the caller's buffer is DMA-ready: the driver's own direct path
  Bounce,    // through the reserved buffer, one sector at a time
  Fallback,  // no usable reserve: the driver's own (allocating) bounce path
};

// dmaReady: the host can DMA the caller's buffer as it is. reserveBytes: the reserved
// buffer's size (0 = none was reserved). sectorBytes: the card's sector size.
constexpr Route route(bool dmaReady, std::size_t reserveBytes, std::size_t sectorBytes) {
  if (dmaReady) return Route::Direct;
  if (reserveBytes == 0 || sectorBytes == 0 || sectorBytes > reserveBytes) return Route::Fallback;
  return Route::Bounce;
}

// Read `count` sectors from `start` into `dst` through `bounce`. xfer(bounce, sector)
// moves one sector from the card into `bounce` and returns 0 or the driver's error,
// which stops the loop and is returned as is.
template <class Xfer>
int readSectors(std::uint8_t* bounce, std::size_t sectorBytes, void* dst, std::size_t start,
                std::size_t count, Xfer&& xfer) {
  auto* out = static_cast<std::uint8_t*>(dst);
  for (std::size_t i = 0; i < count; ++i) {
    const int err = xfer(bounce, start + i);
    if (err != 0) return err;
    std::memcpy(out + i * sectorBytes, bounce, sectorBytes);
  }
  return 0;
}

// Write `count` sectors from `src` to the card from `start` through `bounce`.
// xfer(bounce, sector) moves one sector from `bounce` to the card.
template <class Xfer>
int writeSectors(std::uint8_t* bounce, std::size_t sectorBytes, const void* src, std::size_t start,
                 std::size_t count, Xfer&& xfer) {
  const auto* in = static_cast<const std::uint8_t*>(src);
  for (std::size_t i = 0; i < count; ++i) {
    std::memcpy(bounce, in + i * sectorBytes, sectorBytes);
    const int err = xfer(bounce, start + i);
    if (err != 0) return err;
  }
  return 0;
}

}  // namespace nimbus::sdbounce
