#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// crash_summary - the portable half of the boot core-dump reader (crash forensics F5).
//
// Every panic, task-watchdog and interrupt-watchdog reset writes an ELF core dump to
// the 64 KB `coredump` flash partition. The device seam (src/sys/crash_report.cpp)
// reads it once per boot through the IDF user-mode API (image check, summary, panic
// reason) and hands the facts to this module, which formats the three durable lines:
//
//   crash: fp=1a2b3c4d new=1 this-reset=1 task=tg_poll pc=0x4201abcd cause=28(LoadProhibited) vaddr=0x00000000 size=23456/65536 elf=0badf00d fw-match=1
//   crash-bt: depth=12 corrupt=0 0x4201abcd,0x4201b0c4,...
//   crash-why: Stack canary watchpoint triggered (tg_poll)
//
// What it never prints: stack bytes or the exception registers (a0-a15). A stack in
// transit can hold a provider key or the bot token, and so can a register. The
// RawSummary below mirrors the IDF summary INCLUDING exc_a, so the host test can plant
// a pattern there and prove no formatter copies it; fromRaw() copies only program
// counters, the task name, the cause, the fault address and the ELF hash.
//
// Dedupe (no NVS): /log/crash.seen holds "<fp> <elf> <bootNo>" for the last dump that
// was reported. fp is the image's own stored CRC32 (the last 4 bytes of the image), so
// the same dump reads the same fingerprint every boot and a new panic changes it. The
// image is never erased by the reader: a bench can still pull it over USB, the next
// panic overwrites it, and a factory reset erases it.
namespace nimbus::crash {

inline constexpr size_t kBtMax = 16;
inline constexpr size_t kMaxLineBytes = 240;
inline constexpr size_t kMaxWhyBytes = 200;
inline constexpr uint32_t kPartitionBytes = 65536;

// Field-for-field mirror of the IDF Xtensa esp_core_dump_summary_t members the
// device copies (sizes as in IDF 5.5; app_elf_sha256 is CONFIG_APP_RETRIEVE_LEN_ELF_SHA
// (9) + 1). exc_a is here only so a test can prove it is never printed.
struct RawSummary {
  char exc_task[16] = {};
  uint32_t exc_pc = 0;
  uint32_t bt[kBtMax] = {};
  uint32_t depth = 0;
  bool corrupted = false;
  char app_elf_sha256[10] = {};
  uint32_t exc_cause = 0;
  uint32_t exc_vaddr = 0;
  uint32_t exc_a[16] = {};
};

enum class ImageState : uint8_t {
  None,       // blank or absent partition (a fresh device): nothing to report
  Valid,      // image present and its CRC checks
  BadCrc,     // image present but its checksum does not match (cut short, or corrupted)
  BadSize,    // the size header is not plausible
  ReadError,  // the flash read failed
};

const char* imageStateStr(ImageState s);

struct Report {
  ImageState state = ImageState::None;
  uint32_t fp = 0;                     // stored image CRC32 (0 when unreadable)
  uint32_t size = 0;                   // image bytes, checksum included
  uint32_t partBytes = kPartitionBytes;
  std::string task;                    // crashing task name
  uint32_t pc = 0;
  uint32_t cause = 0;                  // Xtensa EXCCAUSE
  uint32_t vaddr = 0;                  // EXCVADDR
  uint32_t bt[kBtMax] = {};
  uint8_t depth = 0;
  bool corrupt = false;                // the backtrace walk hit a corrupted frame
  std::string elf;                     // app_elf_sha256 prefix recorded in the dump
  std::string why;                     // panic reason text
  bool isNew = true;                   // fp differs from /log/crash.seen
  bool newKnown = true;                // false when no storage was attached to dedupe against
  bool thisReset = false;              // this boot's reset reason is a crash reset
  bool fwMatch = false;                // elf equals the running image's ELF hash
};

// Copy the printable facts out of a raw summary (never exc_a).
void fromRaw(const RawSummary& raw, Report& out);

// Xtensa EXCCAUSE name ("LoadProhibited") or "other".
const char* causeName(uint32_t cause);

std::string crashLine(const Report& r);
std::string crashBtLine(const Report& r);
std::string crashWhyLine(const Report& r);

// The durable lines for this boot: none for ImageState::None; only the crash: line for
// a dump already reported (new=0) or an invalid image; all three for a new valid dump.
std::vector<std::string> reportLines(const Report& r);

// The short form for the ev:boot row and /api/state, at most 160 B:
//   crash=tg_poll@0x4201abcd "Stack canary watchpoint triggered (tg_poll)"
// Empty when there is no valid image.
std::string oneLiner(const Report& r);

// ---- /log/crash.seen ---------------------------------------------------------
struct Seen {
  bool valid = false;
  uint32_t fp = 0;
  std::string elf;
  uint32_t bootNo = 0;
};
Seen parseSeen(const std::string& content);
std::string renderSeen(uint32_t fp, const std::string& elf, uint32_t bootNo);
// A dump is new unless the store already names its fingerprint.
bool isNewCrash(const Seen& seen, uint32_t fp);

// 8 lowercase hex digits.
std::string hex8(uint32_t v);

}  // namespace nimbus::crash
