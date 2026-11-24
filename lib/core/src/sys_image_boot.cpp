#include "nimbus/sys/image_boot.h"

#include "nimbus/sys/reset_reason.h"

namespace nimbus::sys {

namespace {
constexpr size_t kIdMaxBytes = 32;

bool idChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
}  // namespace

const char* imageBootSlug(ImageBoot b) {
  switch (b) {
    case ImageBoot::First: return "first";
    case ImageBoot::Again: return "again";
    case ImageBoot::Unknown: break;
  }
  return "unknown";
}

std::string imageSeenId(const std::string& content) {
  size_t end = 0;
  while (end < content.size() && end < kIdMaxBytes && idChar(content[end])) ++end;
  if (end == 0) return {};
  if (end < content.size() && content[end] != ' ' && content[end] != '\n') return {};   // not an id
  return content.substr(0, end);
}

std::string renderImageSeen(const std::string& elf, uint32_t bootNo) {
  return elf.substr(0, kIdMaxBytes) + " " + std::to_string(bootNo) + "\n";
}

bool imageSeenWriteDue(const std::string& stored, const std::string& elf) {
  const std::string id = elf.substr(0, kIdMaxBytes);
  return !id.empty() && imageSeenId(stored) != id;
}

ImageBoot imageBootOf(const std::string& stored, const std::string& elf, bool wrote) {
  if (elf.empty()) return ImageBoot::Unknown;
  if (!imageSeenWriteDue(stored, elf)) return ImageBoot::Again;
  return wrote ? ImageBoot::First : ImageBoot::Unknown;
}

bool crashNoticeDue(int resetReason, ImageBoot image, bool ownDump) {
  if (!isCrashReset(resetReason)) return false;
  return ownDump || image != ImageBoot::First;
}

}  // namespace nimbus::sys
