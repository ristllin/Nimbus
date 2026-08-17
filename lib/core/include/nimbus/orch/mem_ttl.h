#pragma once
// Memory time-to-live classes - the model chooses how long a durable fact
// should live when it stores one (mem_write[].ttl / memory.write ttl arg).
//
// A class maps to VecEntry.ttlHours. The base hours here are what a fact gets
// at creation; later work (strength-based / Ebbinghaus decay) may EXTEND the
// effective lifetime for frequently-recalled facts, but the class is always the
// baseline the model asked for. `permanent` is never-age-expires (ttlHours -1);
// it is a weaker guarantee than the separate permanentFlag pin (which also
// exempts from importance decay + cap eviction).
//
// Portable, host-tested, no Arduino.

#include <cstdint>
#include <cstring>

namespace nimbus {
namespace orch {

enum class TtlClass : uint8_t { Session, Days, Weeks, Months, Permanent };

// Baseline lifetime per class. -1 == never age-expires.
inline int32_t ttlHoursFor(TtlClass c) {
  switch (c) {
    case TtlClass::Session:   return 12;      // this-conversation scope
    case TtlClass::Days:      return 96;      // ~4 days
    case TtlClass::Weeks:     return 504;     // ~3 weeks (the default)
    case TtlClass::Months:    return 2160;    // ~90 days
    case TtlClass::Permanent: return -1;      // never age-expires
  }
  return ttlHoursFor(TtlClass::Weeks);
}

// The default when the model omits ttl: durable but not forever.
constexpr TtlClass kDefaultModelWrite = TtlClass::Weeks;

// Parse a wire enum string. Returns false (out unchanged) on empty/unknown so
// callers can fall back to a default.
inline bool ttlClassFromName(const char* s, TtlClass& out) {
  if (!s || !*s) return false;
  if (!std::strcmp(s, "session"))   { out = TtlClass::Session;   return true; }
  if (!std::strcmp(s, "days"))      { out = TtlClass::Days;      return true; }
  if (!std::strcmp(s, "weeks"))     { out = TtlClass::Weeks;     return true; }
  if (!std::strcmp(s, "months"))    { out = TtlClass::Months;    return true; }
  if (!std::strcmp(s, "permanent")) { out = TtlClass::Permanent; return true; }
  return false;
}

// Convenience: map a wire ttl string straight to hours, using the default class
// when it is empty/unknown.
inline int32_t ttlHoursFromName(const char* s) {
  TtlClass c = kDefaultModelWrite;
  ttlClassFromName(s, c);
  return ttlHoursFor(c);
}

}  // namespace orch
}  // namespace nimbus
