#include "nimbus/crash_summary.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nimbus/sys/boot_record.h"   // fieldValue

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
};

const char* causeName(uint32_t cause) {
  for (const CauseName& c : kCauses)
    if (c.code == cause) return c.name;
  if (cause >= 32 && cause <= 39) return "CoprocessorDisabled";
  return "other";
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

std::string crashWhyLine(const Report& r) {
  std::string why = reasonText(r.why, kMaxWhyBytes);
  return "crash-why: " + (why.empty() ? std::string("(no reason recorded)") : why);
}

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

std::string oneLiner(const Report& r) {
  if (r.state != ImageState::Valid) return {};
  std::string l = "crash=" + sys::fieldValue(r.task, 15) + "@0x" + hex8(r.pc);
  const std::string why = reasonText(r.why, kMaxWhyBytes);
  if (why.empty()) return l;
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
