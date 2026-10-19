#pragma once
#include <cstdint>
#include <string>

// reset_reason - the one table for every ESP-IDF reset reason (esp_reset_reason_t,
// 16 values in IDF 5.5, esp_system.h). Portable so the mapping is host-tested over
// every value (test/test_crash_forensics). The device static_asserts that each
// ESP_RST_* equals its mirror here and that ESP_RST_CPU_LOCKUP is the last value, so
// an IDF enum change fails the build instead of silently printing "unknown".
//
// Two frozen vocabularies (machine slugs stored on devices and parsed by tools):
//   BootRow - the episodic `ev:boot` row the head reads ("crash(panic)", "deep-sleep wake").
//   Wake    - the test console's WAKE? seam the HIL suite parses ("panic", "deep-sleep").
// The eight values the two old hand-written switches named keep their slugs. The
// values they missed get one new slug, the same in both vocabularies. "unknown" is
// reserved for ESP_RST_UNKNOWN (0) and out-of-range values, and always carries the
// number ("unknown(0)", "unknown(17)"), so the raw value is never lost.
//
// The ROM reset code (esp_rom_get_reset_reason(0), the hardware's own cause) is
// printed beside the slug: IDF folds several hardware causes into one enum value
// (on the S3, chip power-on, chip brownout and super-watchdog all read 0x01), and the
// raw code is what tells a USB-CDC reopen (0x15) from a real power cycle.
namespace nimbus::sys {

enum class ResetReason : uint8_t {
  Unknown = 0,
  PowerOn = 1,
  Ext = 2,
  Sw = 3,
  Panic = 4,
  IntWdt = 5,
  TaskWdt = 6,
  Wdt = 7,
  DeepSleep = 8,
  Brownout = 9,
  Sdio = 10,
  Usb = 11,
  Jtag = 12,
  Efuse = 13,
  PwrGlitch = 14,
  CpuLockup = 15,
};

inline constexpr int kResetReasonCount = 16;
inline constexpr int kLastResetReason = 15;   // ESP_RST_CPU_LOCKUP

enum class ResetVocab : uint8_t { BootRow, Wake };

struct ResetEntry {
  ResetReason id;
  const char* bootRow;   // frozen ev:boot vocabulary
  const char* wake;      // frozen WAKE? vocabulary
  bool crash;            // the previous run ended in a panic, a watchdog or a CPU lockup
};

// The table entry for `value`, or nullptr when it is out of range.
const ResetEntry* resetEntry(int value);

// The slug for `value` in `vocab`; "unknown(<value>)" for 0 and out-of-range values.
std::string resetSlug(int value, ResetVocab vocab);

// True exactly for a crash reset: PANIC, INT_WDT, TASK_WDT, WDT and CPU_LOCKUP.
bool isCrashReset(int value);

// "0x15": the raw ROM code, lowercase hex, at least two digits.
std::string hexCode(uint32_t raw);

// "usb(0x15)": the slug with the raw ROM code beside it.
std::string resetWithRaw(int value, uint32_t raw, ResetVocab vocab);

}  // namespace nimbus::sys
