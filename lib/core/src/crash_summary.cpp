#include "nimbus/crash_summary.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nimbus/sys/boot_record.h"   // fieldValue
#include "nimbus/sys/reset_reason.h"  // ResetReason (the notice's watchdog class)

namespace nimbus::crash {

namespace {

// Printable ASCII with control bytes as spaces and '"' as '\'', capped. Used for the
// panic reason, which is free text from the panic handler.
std::string reasonText(const std::string& in, size_t cap) {
  std::string out;
  out.reserve(in.size() < cap ? in.size() : cap);
  for (unsigned char c : in) {
    if (out.size() >= cap) break;
    if (c < 0x20 || c >= 0x7f) { out.push_back(' '); continue; }
    out.push_back(c == '"' ? '\'' : (char)c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string bounded(const char* s, size_t cap) {
  size_t n = 0;
  while (n < cap && s[n]) ++n;
  return std::string(s, n);
}

}  // namespace

std::string hex8(uint32_t v) {
  char b[12];
  std::snprintf(b, sizeof b, "%08x", (unsigned)v);
  return b;
}

const char* imageStateStr(ImageState s) {
  switch (s) {
    case ImageState::None:      return "none";
    case ImageState::Valid:     return "valid";
    case ImageState::BadCrc:    return "bad-crc";
    case ImageState::BadSize:   return "bad-size";
    case ImageState::ReadError: return "read-error";
  }
  return "none";
}

void fromRaw(const RawSummary& raw, Report& out) {
  out.task = bounded(raw.exc_task, sizeof raw.exc_task);
  out.pc = raw.exc_pc;
  out.depth = (uint8_t)(raw.depth > kBtMax ? kBtMax : raw.depth);
  for (size_t i = 0; i < kBtMax; ++i) out.bt[i] = i < out.depth ? raw.bt[i] : 0;
  out.corrupt = raw.corrupted;
  out.elf = bounded(raw.app_elf_sha256, sizeof raw.app_elf_sha256);
  out.cause = raw.exc_cause;
  out.vaddr = raw.exc_vaddr;
  // exc_a (the register file) is deliberately not copied: see the header.
}

// Xtensa EXCCAUSE codes (Xtensa ISA reference, exception causes).
struct CauseName {
  uint32_t code;
  const char* name;
};
constexpr CauseName kCauses[] = {
    {0, "IllegalInstruction"},     {1, "Syscall"},
    {2, "InstructionFetchError"},  {3, "LoadStoreError"},
    {4, "Level1Interrupt"},        {5, "Alloca"},
    {6, "IntegerDivideByZero"},    {8, "Privileged"},
    {9, "LoadStoreAlignment"},     {12, "InstrPIFDataError"},
    {13, "LoadStorePIFDataError"}, {14, "InstrPIFAddrError"},
    {15, "LoadStorePIFAddrError"}, {16, "InstTLBMiss"},
    {17, "InstTLBMultiHit"},       {18, "InstFetchPrivilege"},
    {20, "InstFetchProhibited"},   {24, "LoadStoreTLBMiss"},
    {25, "LoadStoreTLBMultiHit"},  {26, "LoadStorePrivilege"},
    {28, "LoadProhibited"},        {29, "StoreProhibited"},
    // IDF pseudo causes: XCHAL_EXCCAUSE_NUM (64) + PANIC_RSN_* (panic_reason.h).
    {64, "UnknownReason"},         {65, "DebugException"},
    {66, "DoubleException"},       {67, "KernelException"},
    {68, "CoprocessorException"},  {69, "IntWdtCpu0"},
    {70, "IntWdtCpu1"},            {71, "CacheError"},
};

const char* causeName(uint32_t cause) {
  for (const CauseName& c : kCauses)
    if (c.code == cause) return c.name;
  if (cause >= 32 && cause <= 39) return "CoprocessorDisabled";
  return "other";
}

// What a cause means when the dump recorded no panic message. `addr`: EXCVADDR is the
// faulting address for this cause. The end-of-stack watchpoint is the only debug
// exception this firmware raises (CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK).
struct CauseWhy {
  uint32_t code;
  const char* what;
  bool addr;
};
constexpr CauseWhy kCauseWhy[] = {
    {0, "an invalid instruction (often a corrupted stack or function pointer)", false},
    {2, "an instruction fetch error", true},
    {3, "a load or store error", true},
    {6, "an integer division by zero", false},
    {9, "an unaligned load or store", true},
    {20, "an instruction fetch from a prohibited address", true},
    {28, "a read from an invalid address", true},
    {29, "a write to an invalid address", true},
    {64, "a panic with no recorded cause", false},
    {65, "the end-of-stack watchpoint fired (a stack overflow)", false},
    {66, "a double exception (often a stack overflow)", false},
    {67, "an unhandled kernel exception", false},
    {68, "a coprocessor exception", false},
    {69, "the interrupt watchdog fired on CPU 0", false},
    {70, "the interrupt watchdog fired on CPU 1", false},
    {71, "a cache error (flash or PSRAM read while the cache was off)", false},
};

std::string whyOf(const Report& r) {
  const std::string recorded = reasonText(r.why, kMaxWhyBytes);
  if (!recorded.empty()) return recorded;
  const CauseWhy* w = nullptr;
  for (const CauseWhy& c : kCauseWhy) {
    if (c.code == r.cause) {
      w = &c;
      break;
    }
  }
  std::string l = std::string(causeName(r.cause)) + " in " + sys::fieldValue(r.task, 15) + ": ";
  l += w ? std::string(w->what) : "exception cause " + std::to_string(r.cause);
  if (w && w->addr) l += " (0x" + hex8(r.vaddr) + ")";
  l += "; no panic message in the dump";
  return reasonText(l, kMaxWhyBytes);
}

namespace {
bool hasText(const std::string& hay, const char* needle) {
  std::string h;
  h.reserve(hay.size());
  for (char c : hay) h.push_back((c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);
  return h.find(needle) != std::string::npos;
}
bool watchdogReset(int resetReason) {
  return resetReason == (int)sys::ResetReason::TaskWdt || resetReason == (int)sys::ResetReason::IntWdt ||
         resetReason == (int)sys::ResetReason::Wdt;
}
}  // namespace

CrashKind crashKind(const Report* dump, int resetReason) {
  if (dump && dump->state == ImageState::Valid) {
    const std::string& w = dump->why;
    if (dump->cause == 65 || hasText(w, "stack overflow") || hasText(w, "stack canary"))
      return CrashKind::TaskOutOfMemory;
    if (hasText(w, "alloc") || hasText(w, "out of memory") || hasText(w, "memory exhausted"))
      return CrashKind::OutOfMemory;
    if (dump->cause == 69 || dump->cause == 70 || hasText(w, "watchdog")) return CrashKind::Unresponsive;
  }
  return watchdogReset(resetReason) ? CrashKind::Unresponsive : CrashKind::SoftwareError;
}

const char* plainCause(CrashKind k) {
  switch (k) {
    case CrashKind::Unresponsive:    return "part of it stopped responding";
    case CrashKind::OutOfMemory:     return "it ran out of memory";
    case CrashKind::TaskOutOfMemory: return "one of its jobs ran out of memory";
    case CrashKind::SoftwareError:   break;
  }
  return "a software error";
}

static std::string newField(const Report& r) {
  if (!r.newKnown) return "?";
  return r.isNew ? "1" : "0";
}

static std::string sizeField(const Report& r) {
  return std::to_string(r.size) + "/" + std::to_string(r.partBytes);
}

std::string crashLine(const Report& r) {
  std::string l = "crash:";
  if (r.state != ImageState::Valid) l += std::string(" image=") + imageStateStr(r.state);
  l += " fp=" + hex8(r.fp) + " new=" + newField(r);
  l += std::string(" this-reset=") + (r.thisReset ? "1" : "0");
  if (r.state != ImageState::Valid) return l + " size=" + sizeField(r);
  l += " task=" + sys::fieldValue(r.task, 15);
  l += " pc=0x" + hex8(r.pc);
  l += " cause=" + std::to_string(r.cause) + "(" + causeName(r.cause) + ")";
  l += " vaddr=0x" + hex8(r.vaddr);
  l += " size=" + sizeField(r);
  l += " elf=" + sys::fieldValue(r.elf, 16);
  l += std::string(" fw-match=") + (r.fwMatch ? "1" : "0");
  return l.size() > kMaxLineBytes ? l.substr(0, kMaxLineBytes) : l;
}

std::string crashBtLine(const Report& r) {
  std::string l = "crash-bt: depth=" + std::to_string(r.depth);
  l += std::string(" corrupt=") + (r.corrupt ? "1" : "0");
  for (uint8_t i = 0; i < r.depth && i < kBtMax; ++i) {
    l += i ? ",0x" : " 0x";
    l += hex8(r.bt[i]);
  }
  return l;
}

std::string crashWhyLine(const Report& r) { return "crash-why: " + whyOf(r); }

std::vector<std::string> reportLines(const Report& r) {
  std::vector<std::string> out;
  if (r.state == ImageState::None) return out;
  out.push_back(crashLine(r));
  if (r.state != ImageState::Valid) return out;
  if (r.newKnown && !r.isNew) return out;   // already reported: the summary line only
  out.push_back(crashBtLine(r));
  out.push_back(crashWhyLine(r));
  return out;
}

bool describesThisCrash(const Report& r) { return r.thisReset && (!r.newKnown || r.isNew); }

std::string oneLiner(const Report& r) {
  if (r.state != ImageState::Valid) return {};
  if (r.thisReset && !describesThisCrash(r)) return kUnsavedCrashLine;
  std::string l = "crash=" + sys::fieldValue(r.task, 15) + "@0x" + hex8(r.pc);
  const std::string why = whyOf(r);
  const size_t room = 160 - l.size() - 3;   // space + two quotes
  l += " \"" + (why.size() > room ? why.substr(0, room) : why) + "\"";
  return l;
}

Seen parseSeen(const std::string& content) {
  Seen s;
  char fp[16] = {}, elf[24] = {};
  unsigned long boot = 0;
  if (std::sscanf(content.c_str(), "%15s %23s %lu", fp, elf, &boot) < 1) return s;
  if (std::strlen(fp) != 8) return s;
  char* end = nullptr;
  const unsigned long v = std::strtoul(fp, &end, 16);
  if (!end || *end) return s;
  s.valid = true;
  s.fp = (uint32_t)v;
  s.elf = elf;
  s.bootNo = (uint32_t)boot;
  return s;
}

std::string renderSeen(uint32_t fp, const std::string& elf, uint32_t bootNo) {
  return hex8(fp) + " " + sys::fieldValue(elf, 16) + " " + std::to_string(bootNo) + "\n";
}

bool isNewCrash(const Seen& seen, uint32_t fp) { return !seen.valid || seen.fp != fp; }

}  // namespace nimbus::crash
