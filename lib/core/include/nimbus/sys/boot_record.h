#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// boot_record - the first durable line of every boot (crash forensics, F2).
//
//   boot: n=17 fw=v4.5.14 build=v4.5.14-3-gabc1234 reason=usb(0x15) wake=none mode=orch t=0 ota=ok prev=none
//
// It is emitted at the top of setup(), right after the OTA boot guard, before any
// filesystem, Wi-Fi or clock exists, so every field here is clock- and storage-free:
//   n      the boot number. Kept beside the log (/log/boot.n, never NVS); it is only
//          known once storage attaches, so the line is first written with "n=?" and the
//          durable log stamps the real number when it flushes its early backlog
//          (stampBootNo). Serial and the RAM ring keep "n=?".
//   reason the reset reason slug (reset_reason.h, ev:boot vocabulary) with the raw ROM code.
//   t      the wall clock the RTC kept across the reset (software, panic and watchdog
//          resets keep it), else 0. It anchors an SD boot in time even when it never syncs.
//   prev   where the previous run stopped, from the RTC breadcrumb, or "none".
//
// Every value is sanitized to printable ASCII with no spaces and capped, and the whole
// line stays within kMaxBootLineBytes (alogf formats into 256 B and the durable line
// carries a seq/ms/cat prefix). Field names avoid the log redactor's key words
// (key, token, pass, secret), so the line is never masked (test_crash_forensics).
namespace nimbus::sys {

inline constexpr size_t kMaxBootLineBytes = 240;

struct BootFacts {
  uint32_t bootNo = 0;        // 0 = not known yet ("n=?")
  std::string fw;             // NIMBUS_FW_VERSION
  std::string build;          // NIMBUS_FW_BUILD
  int reason = 0;             // esp_reset_reason_t value
  uint32_t raw = 0;           // esp_rom_get_reset_reason(0)
  std::string wake = "none";  // deep-sleep wake cause ("timer", "ext0", ... or "none")
  bool orch = false;          // Orchestrator mode (else Notifier)
  uint64_t epoch = 0;         // RTC wall clock at boot, 0 when it was lost
  std::string ota;            // last OTA result ("" = omitted)
  std::string prev = "none";  // RTC breadcrumb summary
};

// The boot line (no seq/ms prefix; the durable log adds it).
std::string bootLine(const BootFacts& f);

// The placeholder the durable log replaces once the boot number is known.
inline constexpr char kBootNoPlaceholder[] = "boot: n=? ";

// Replace the first "boot: n=? " in a formatted log line with "boot: n=<n> ". Returns
// true when a placeholder was stamped. n == 0 leaves the line unchanged.
bool stampBootNo(std::string& line, uint32_t n);

// The /log/boot.n counter file: one decimal number. Garbage or absent reads as 0.
uint32_t parseBootCounter(const std::string& content);
std::string renderBootCounter(uint32_t n);

// Printable ASCII, no spaces or '=', capped at `cap` bytes ("-" when empty). Shared by
// every forensic line so a field value can never split a field or forge another one.
std::string fieldValue(const std::string& in, size_t cap);

}  // namespace nimbus::sys
