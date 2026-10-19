#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// breadcrumb - where the previous run stopped (crash forensics F4).
//
// A core dump explains a panic or a watchdog, but not a brownout, a power glitch or a
// boot that died before storage mounted (the 22 Sep to 11 Oct run of ten 1.1 s deaths
// on a low pack left nothing at all). So the device keeps a 48-byte record in RTC slow
// memory (RTC_NOINIT_ATTR): the boot stage or turn step it last reached, its uptime and
// the lowest internal free heap so far. That memory survives software, panic, watchdog
// and USB resets and deep sleep, but not a power cycle; the next boot reads it into the
// boot line's prev= field before anything rewrites it. Magic plus CRC-32 reject the
// garbage a power-on leaves, so it then reads "none". Never NVS, never flash: it costs
// a few stores per update.
//
// Portable so the codec and the garbage rejection are host-tested (test_crash_forensics).
namespace nimbus::sys {

enum class Stage : uint8_t {
  None = 0,
  OtaGuard,   // the top of setup(): the OTA boot guard (an early abort lands here)
  Hal,        // display, LEDs, touch, PSRAM routing
  Config,     // settings loaded and applied
  Net,        // Wi-Fi / access point up
  Storage,    // SD mount
  Web,        // memory subsystem + web server
  Orch,       // orchestrator and Telegram task
  Idle,       // start-up done, no turn in flight
  Turn,       // a turn is running (mark = kind, then the round)
  Count,
};

const char* stageName(uint8_t stage);

// "NBC" + layout version: a firmware with another layout reads "none", never garbage.
inline constexpr uint32_t kBreadcrumbMagic = 0x4e424301u;

struct Breadcrumb {
  uint32_t magic;
  uint32_t bootNo;    // the run's boot number (0 until storage attached)
  uint32_t upMs;      // uptime at the last update
  uint32_t heapMin;   // lowest internal free heap seen by the run
  uint8_t stage;
  uint8_t pad[3];
  char mark[24];      // a short detail ("photo", "r2"), NUL-terminated
  uint32_t crc;       // CRC-32 over every byte above
};
static_assert(sizeof(Breadcrumb) == 48, "the RTC record stays 48 bytes");

uint32_t crc32(const uint8_t* data, size_t len);

// Record a step and seal the record (magic + CRC).
void markBreadcrumb(Breadcrumb& b, Stage stage, const char* mark, uint32_t upMs, uint32_t heapMin,
                    uint32_t bootNo);
bool breadcrumbValid(const Breadcrumb& b);

// The boot line's prev= value: "n17/367161s/turn-r2/h1180", or "none" for an invalid
// record. Printable, no spaces or '=', at most 48 bytes.
std::string breadcrumbSummary(const Breadcrumb& b);

}  // namespace nimbus::sys
