#pragma once
#include <cstdint>

// sd_dma_bounce (device glue) - every SD card transfer has the DMA buffer it needs,
// even when internal memory is short. The routing and copy loops are portable
// (lib/core nimbus/sd_bounce.h, host-tested); this file binds them to the IDF SDMMC
// driver through link-time wrappers (-Wl,--wrap=sdmmc_read_sectors and
// -Wl,--wrap=sdmmc_write_sectors on every full-firmware env in platformio.ini), so
// every mount path (boot, late mount, retry, format) is covered without touching the
// card driver.
//
//   reserve()  setup(), before the card mounts: one sector-sized internal DMA buffer
//              and its lock. A no-op on a board whose card is on the SPI bus (that
//              path never DMAs).
//   stats()    transfer counters for diagnostics.
//
// No new task and no NVS. Nothing here logs: a log line can itself write to the card.
namespace nimbus::sdbounce {

bool reserve();

struct Stats {
  uint32_t direct = 0;     // the caller's buffer was DMA-ready (no bounce needed)
  uint32_t bounced = 0;    // moved through the reserved buffer
  uint32_t fallback = 0;   // the driver's own allocating path (no reserve, or it stayed busy)
  uint16_t reserved = 0;   // bytes held for the card (0 = none)
};
Stats stats();

}  // namespace nimbus::sdbounce
