#include "nimbus/orch/day_start.h"

#include <cstring>

// day_start - see day_start.h. No allocation anywhere in this file.
namespace nimbus {
namespace orch {
namespace day {

namespace {

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// Days since 1970-01-01 to a proleptic Gregorian date (H. Hinnant's civil_from_days).
void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = floorDiv(z, 146097);
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);
}

char* put2(char* p, unsigned v) {
  *p++ = static_cast<char>('0' + v / 10 % 10);
  *p++ = static_cast<char>('0' + v % 10);
  return p;
}

// "YYYY-MM-DD" at p (10 characters, no NUL); false outside years 1 to 9999.
bool putDate(char* p, int64_t dayNumber) {
  int64_t y = 0;
  unsigned m = 0, d = 0;
  civilFromDays(dayNumber, y, m, d);
  if (y < 1 || y > 9999) return false;
  p = put2(p, static_cast<unsigned>(y / 100));
  p = put2(p, static_cast<unsigned>(y % 100));
  *p++ = '-';
  p = put2(p, m);
  *p++ = '-';
  put2(p, d);
  return true;
}

bool digit(char c) { return c >= '0' && c <= '9'; }

void writeEmpty(char* out, size_t cap) {
  if (out && cap) out[0] = '\0';
}

}  // namespace

const char* parseStart(const char* text, uint16_t& minutes) {
  if (!text || std::strlen(text) != 5 || text[2] != ':') return kStartError;
  if (!digit(text[0]) || !digit(text[1]) || !digit(text[3]) || !digit(text[4])) return kStartError;
  const unsigned h = static_cast<unsigned>((text[0] - '0') * 10 + (text[1] - '0'));
  const unsigned mm = static_cast<unsigned>((text[3] - '0') * 10 + (text[4] - '0'));
  if (h > 23 || mm > 59) return kStartError;
  minutes = static_cast<uint16_t>(h * 60 + mm);
  return nullptr;
}

uint16_t startOrDefault(const char* stored) {
  uint16_t m = kDefaultStartMin;
  return parseStart(stored, m) ? kDefaultStartMin : m;
}

void formatStart(uint16_t minutes, char out[6]) {
  const unsigned m = minutes % kMinutesPerDay;
  char* p = put2(out, m / 60);
  *p++ = ':';
  p = put2(p, m % 60);
  *p = '\0';
}

int64_t runningDay(uint64_t epoch, int32_t offsetMin, uint16_t startMin) {
  const int64_t e = epoch > kLastEpoch ? static_cast<int64_t>(kLastEpoch) : static_cast<int64_t>(epoch);
  const int64_t local = e + static_cast<int64_t>(offsetMin) * 60;
  return floorDiv(local - static_cast<int64_t>(startMin % kMinutesPerDay) * 60, 86400);
}

bool formatDate(int64_t dayNumber, char* out, size_t cap) {
  if (!out || cap < 11) {
    writeEmpty(out, cap);
    return false;
  }
  if (!putDate(out, dayNumber)) {
    out[0] = '\0';
    return false;
  }
  out[10] = '\0';
  return true;
}

bool runningDate(uint64_t epoch, int32_t offsetMin, uint16_t startMin, char* out, size_t cap) {
  if (!clockSet(epoch) || offsetMin > kMaxOffsetMin || offsetMin < -kMaxOffsetMin) {
    writeEmpty(out, cap);
    return false;
  }
  return formatDate(runningDay(epoch, offsetMin, startMin), out, cap);
}

StampForm stamp(uint64_t epoch, int32_t offsetMin, uint16_t startMin, char* out, size_t cap) {
  static const char kDays[] = "ThuFriSatSunMonTueWed";   // day 0 (1970-01-01) was a Thursday
  if (!out || cap < kPlainCap || offsetMin > kMaxOffsetMin || offsetMin < -kMaxOffsetMin) {
    writeEmpty(out, cap);
    return StampForm::None;
  }
  if (!clockSet(epoch)) {
    std::memcpy(out, kNoClock, sizeof kNoClock);
    return StampForm::NoClock;
  }
  const int64_t local = static_cast<int64_t>(epoch) + static_cast<int64_t>(offsetMin) * 60;
  const int64_t today = floorDiv(local, 86400);
  const unsigned minuteOfDay = static_cast<unsigned>((local - today * 86400) / 60);
  char* p = out;
  if (!putDate(p, today)) {
    out[0] = '\0';
    return StampForm::None;
  }
  p += 10;
  *p++ = ' ';
  std::memcpy(p, kDays + 3 * static_cast<unsigned>(((today % 7) + 7) % 7), 3);
  p += 3;
  *p++ = ' ';
  p = put2(p, minuteOfDay / 60);
  *p++ = ':';
  p = put2(p, minuteOfDay % 60);
  *p = '\0';
  const uint16_t start = startMin % kMinutesPerDay;
  if (minuteOfDay >= start) return StampForm::Full;
  // Before the day start: yesterday is still running. The clause is whole or absent.
  if (cap < kFullCap || !putDate(p + 6, today - 1)) {
    *p = '\0';
    return StampForm::Plain;
  }
  std::memcpy(p, " (day ", 6);
  p += 16;
  std::memcpy(p, " until ", 7);
  p += 7;
  formatStart(start, p);
  p += 5;
  *p++ = ')';
  *p = '\0';
  return StampForm::Full;
}

}  // namespace day
}  // namespace orch
}  // namespace nimbus
