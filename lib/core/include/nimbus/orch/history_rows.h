#pragma once
#include <cstdint>
#include <string>

// history_rows - small facts about episodic rows that the turn-structured
// history (lib/harness conversation.cpp), the fold store (compact.h) and the
// memory_episodic tool (memory_tools.h) share: how row ids order, how a row's
// text is hashed for repeat detection, and how a row's hour is written.
// Header-only, Arduino-free.
namespace nimbus {
namespace orch {

// Row ids are "m" + 8 hex digits from one boot-stable counter: a longer id is
// newer, then lexical order is numeric order.
inline bool rowIdAfter(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return a.size() > b.size();
  return a > b;
}

// FNV-1a over a row's text: the repeat check (an owner message identical to
// the one before it). Never 0, so 0 can mean "no message yet".
inline uint32_t rowTextHash(const std::string& t) {
  uint32_t h = 2166136261u;
  for (unsigned char c : t) {
    h ^= c;
    h *= 16777619u;
  }
  return h ? h : 1u;
}

// Before the clock syncs, rows are stamped with boot-relative hours: a tsHours
// under this is no date.
constexpr uint32_t kClockValidHours = 1000000000u / 3600u + 1u;

// "Sun".."Sat" for a CivilTime wday (0 = Sunday), "Jan".."Dec" for a month 1..12.
inline const char* wdayAbbr(int wday) {
  static const char* const kWd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return kWd[((wday % 7) + 7) % 7];
}
inline const char* monthAbbr(int month) {
  static const char* const kMo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return kMo[((month - 1) % 12 + 12) % 12];
}

}  // namespace orch
}  // namespace nimbus
