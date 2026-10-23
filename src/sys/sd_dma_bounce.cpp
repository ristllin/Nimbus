#include "sd_dma_bounce.h"

#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdmmc_cmd.h>
#include <soc/soc_caps.h>
#include <solide/boards/active_board.h>

#include <atomic>

#include "nimbus/sd_bounce.h"

// The IDF driver's own entry points, reached through the linker's --wrap: every
// reference to sdmmc_read_sectors / sdmmc_write_sectors (FATFS's diskio_sdmmc is the
// only caller) lands in the __wrap_ functions below, which call __real_ for the
// actual transfer.
extern "C" {
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_block,
                                    size_t block_count);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_block,
                                     size_t block_count);
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_block,
                                    size_t block_count);
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_block,
                                     size_t block_count);
}

namespace nimbus::sdbounce {
namespace {

constexpr bool kSdmmc = solide::activeBoard().sdKind == solide::SdKind::Sdmmc;
constexpr size_t kReserveBytes = 512;   // one sector: SDHC and SDXC cards are fixed at 512 B
// Another task mid-transfer through the buffer holds it for one sector move per
// sector (FATFS already serializes a volume, so this is a backstop); past this the
// transfer takes the driver's own path instead of waiting longer.
constexpr TickType_t kTakeTicks = pdMS_TO_TICKS(2000);

uint8_t* g_buf = nullptr;            // set once in reserve(), after g_mx
SemaphoreHandle_t g_mx = nullptr;
std::atomic<uint32_t> g_direct{0};
std::atomic<uint32_t> g_bounced{0};
std::atomic<uint32_t> g_fallback{0};

// The driver's own rule for its direct path (sdmmc_cmd.c): the host's alignment check
// passes and the buffer is not in PSRAM (the S3 SDMMC host cannot DMA PSRAM).
bool dmaReady(const sdmmc_card_t* card, const void* buf, size_t bytes) {
#if !SOC_SDMMC_PSRAM_DMA_CAPABLE
  if (esp_ptr_external_ram(buf)) return false;
#endif
  if (card->host.check_buffer_alignment)
    return card->host.check_buffer_alignment(card->host.slot, buf, bytes);
  return (reinterpret_cast<uintptr_t>(buf) % 4) == 0 && (bytes % 4) == 0;
}

// The route for one transfer, with the reserve taken when it says Bounce. A reserve
// that stays busy degrades to the driver's own path rather than waiting longer.
Route pick(const sdmmc_card_t* card, const void* buf, size_t count) {
  const size_t ss = card->csd.sector_size;
  Route r = route(dmaReady(card, buf, ss * count), g_buf ? kReserveBytes : 0, ss);
  if (r == Route::Bounce && xSemaphoreTake(g_mx, kTakeTicks) != pdTRUE) r = Route::Fallback;
  if (r == Route::Direct) g_direct.fetch_add(1, std::memory_order_relaxed);
  else if (r == Route::Bounce) g_bounced.fetch_add(1, std::memory_order_relaxed);
  else g_fallback.fetch_add(1, std::memory_order_relaxed);
  return r;
}

}  // namespace

bool reserve() {
  if (!kSdmmc) return false;
  if (g_buf) return true;
  if (!g_mx) g_mx = xSemaphoreCreateMutex();
  if (!g_mx) return false;
  g_buf = static_cast<uint8_t*>(
      heap_caps_aligned_alloc(4, kReserveBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return g_buf != nullptr;
}

Stats stats() {
  Stats s;
  s.direct = g_direct.load(std::memory_order_relaxed);
  s.bounced = g_bounced.load(std::memory_order_relaxed);
  s.fallback = g_fallback.load(std::memory_order_relaxed);
  s.reserved = g_buf ? kReserveBytes : 0;
  return s;
}

}  // namespace nimbus::sdbounce

using nimbus::sdbounce::Route;

extern "C" esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_block,
                                               size_t block_count) {
  if (block_count == 0 || !card) return __real_sdmmc_read_sectors(card, dst, start_block, block_count);
  const Route r = nimbus::sdbounce::pick(card, dst, block_count);
  if (r != Route::Bounce) return __real_sdmmc_read_sectors(card, dst, start_block, block_count);
  const int err = nimbus::sdbounce::readSectors(
      nimbus::sdbounce::g_buf, card->csd.sector_size, dst, start_block, block_count,
      [card](uint8_t* b, size_t sector) { return int(__real_sdmmc_read_sectors(card, b, sector, 1)); });
  xSemaphoreGive(nimbus::sdbounce::g_mx);
  return esp_err_t(err);
}

extern "C" esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_block,
                                                size_t block_count) {
  if (block_count == 0 || !card) return __real_sdmmc_write_sectors(card, src, start_block, block_count);
  const Route r = nimbus::sdbounce::pick(card, src, block_count);
  if (r != Route::Bounce) return __real_sdmmc_write_sectors(card, src, start_block, block_count);
  const int err = nimbus::sdbounce::writeSectors(
      nimbus::sdbounce::g_buf, card->csd.sector_size, src, start_block, block_count,
      [card](uint8_t* b, size_t sector) { return int(__real_sdmmc_write_sectors(card, b, sector, 1)); });
  xSemaphoreGive(nimbus::sdbounce::g_mx);
  return esp_err_t(err);
}
