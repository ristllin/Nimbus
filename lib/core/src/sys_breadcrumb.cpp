#include "nimbus/sys/breadcrumb.h"

#include <cstring>

#include "nimbus/sys/boot_record.h"   // fieldValue

namespace nimbus::sys {

static constexpr const char* kStageNames[] = {"none", "ota-guard", "hal",  "config", "net",
                                               "storage", "web",     "orch", "idle",   "turn"};
static_assert(sizeof(kStageNames) / sizeof(kStageNames[0]) == (size_t)Stage::Count,
              "every stage has a name");

const char* stageName(uint8_t stage) {
  return stage < (uint8_t)Stage::Count ? kStageNames[stage] : "?";
}

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c ^= data[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

static uint32_t crcOf(const Breadcrumb& b) {
  return crc32(reinterpret_cast<const uint8_t*>(&b), offsetof(Breadcrumb, crc));
}

void markBreadcrumb(Breadcrumb& b, Stage stage, const char* mark, uint32_t upMs, uint32_t heapMin,
                    uint32_t bootNo) {
  b.magic = kBreadcrumbMagic;
  b.bootNo = bootNo;
  b.upMs = upMs;
  b.heapMin = heapMin;
  b.stage = (uint8_t)stage;
  std::memset(b.pad, 0, sizeof b.pad);
  std::memset(b.mark, 0, sizeof b.mark);
  if (mark) std::strncpy(b.mark, mark, sizeof b.mark - 1);
  b.crc = crcOf(b);
}

bool breadcrumbValid(const Breadcrumb& b) {
  return b.magic == kBreadcrumbMagic && b.mark[sizeof b.mark - 1] == '\0' && b.crc == crcOf(b);
}

std::string breadcrumbSummary(const Breadcrumb& b) {
  if (!breadcrumbValid(b)) return "none";
  std::string s;
  if (b.bootNo) s += "n" + std::to_string(b.bootNo) + "/";
  s += std::to_string(b.upMs / 1000) + "s/" + stageName(b.stage);
  if (b.mark[0]) s += "-" + fieldValue(std::string(b.mark), 12);
  s += "/h" + std::to_string(b.heapMin);
  return fieldValue(s, 48);
}

}  // namespace nimbus::sys
