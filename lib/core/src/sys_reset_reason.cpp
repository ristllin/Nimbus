#include "nimbus/sys/reset_reason.h"

#include <cstdio>

namespace nimbus::sys {

// Index == enum value; test_crash_forensics asserts it for every row.
static constexpr ResetEntry kTable[kResetReasonCount] = {
    {ResetReason::Unknown,   "unknown",          "unknown",          false},
    {ResetReason::PowerOn,   "power-on",         "power-on",         false},
    {ResetReason::Ext,       "external-pin",     "external-pin",     false},
    {ResetReason::Sw,        "software-restart", "software-restart", false},
    {ResetReason::Panic,     "crash(panic)",     "panic",            true},
    {ResetReason::IntWdt,    "crash(watchdog)",  "watchdog",         true},
    {ResetReason::TaskWdt,   "crash(watchdog)",  "watchdog",         true},
    {ResetReason::Wdt,       "crash(watchdog)",  "watchdog",         true},
    {ResetReason::DeepSleep, "deep-sleep wake",  "deep-sleep",       false},
    {ResetReason::Brownout,  "brownout",         "brownout",         false},
    {ResetReason::Sdio,      "sdio",             "sdio",             false},
    {ResetReason::Usb,       "usb",              "usb",              false},
    {ResetReason::Jtag,      "jtag",             "jtag",             false},
    {ResetReason::Efuse,     "efuse",            "efuse",            false},
    {ResetReason::PwrGlitch, "power-glitch",     "power-glitch",     false},
    {ResetReason::CpuLockup, "cpu-lockup",       "cpu-lockup",       true},
};

const ResetEntry* resetEntry(int value) {
  if (value < 0 || value >= kResetReasonCount) return nullptr;
  return &kTable[value];
}

std::string resetSlug(int value, ResetVocab vocab) {
  const ResetEntry* e = resetEntry(value);
  if (!e || e->id == ResetReason::Unknown) return "unknown(" + std::to_string(value) + ")";
  return vocab == ResetVocab::BootRow ? e->bootRow : e->wake;
}

bool isCrashReset(int value) {
  const ResetEntry* e = resetEntry(value);
  return e && e->crash;
}

std::string hexCode(uint32_t raw) {
  char b[12];
  std::snprintf(b, sizeof b, "0x%02x", (unsigned)raw);
  return b;
}

std::string resetWithRaw(int value, uint32_t raw, ResetVocab vocab) {
  return resetSlug(value, vocab) + "(" + hexCode(raw) + ")";
}

}  // namespace nimbus::sys
