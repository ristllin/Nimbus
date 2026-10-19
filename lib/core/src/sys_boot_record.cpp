#include "nimbus/sys/boot_record.h"

#include "nimbus/sys/reset_reason.h"

namespace nimbus::sys {

std::string fieldValue(const std::string& in, size_t cap) {
  std::string out;
  out.reserve(in.size() < cap ? in.size() : cap);
  for (unsigned char c : in) {
    if (out.size() >= cap) break;
    if (c <= 0x20 || c >= 0x7f || c == '=') { out.push_back('_'); continue; }
    out.push_back((char)c);
  }
  return out.empty() ? std::string("-") : out;
}

std::string bootLine(const BootFacts& f) {
  std::string line = "boot: n=";
  line += f.bootNo ? std::to_string(f.bootNo) : std::string("?");
  line += " fw=" + fieldValue(f.fw, 24);
  line += " build=" + fieldValue(f.build, 40);
  line += " reason=" + resetWithRaw(f.reason, f.raw, ResetVocab::BootRow);
  line += " wake=" + fieldValue(f.wake, 12);
  line += f.orch ? " mode=orch" : " mode=notifier";
  line += " t=" + std::to_string(f.epoch);
  if (!f.ota.empty()) line += " ota=" + fieldValue(f.ota, 32);
  line += " prev=" + fieldValue(f.prev, 48);
  // The fields above are capped, so only a pathological combination reaches the
  // limit; cut at the last field boundary rather than mid-value.
  if (line.size() > kMaxBootLineBytes) {
    const size_t sp = line.rfind(' ', kMaxBootLineBytes);
    line.resize(sp == std::string::npos ? kMaxBootLineBytes : sp);
  }
  return line;
}

bool stampBootNo(std::string& line, uint32_t n) {
  if (n == 0) return false;
  const size_t at = line.find(kBootNoPlaceholder);
  if (at == std::string::npos) return false;
  const size_t q = at + sizeof(kBootNoPlaceholder) - 3;   // index of '?'
  line.replace(q, 1, std::to_string(n));
  return true;
}

uint32_t parseBootCounter(const std::string& content) {
  uint64_t n = 0;
  size_t digits = 0;
  for (char c : content) {
    if (c >= '0' && c <= '9') {
      n = n * 10 + (uint64_t)(c - '0');
      if (++digits > 10 || n > 0xFFFFFFFFull) return 0;
      continue;
    }
    if (c == '\n' || c == '\r' || c == ' ') break;
    return 0;
  }
  return (uint32_t)n;
}

std::string renderBootCounter(uint32_t n) { return std::to_string(n) + "\n"; }

}  // namespace nimbus::sys
