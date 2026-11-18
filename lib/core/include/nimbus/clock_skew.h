#pragma once
// clock_skew - the arithmetic of the bench console's CLOCK verb (src/test_console.cpp,
// test builds only). A skew moves the device's wall clock forward and is held: an SNTP
// resync lands the network time and the console puts the skew back on top, so a leg that
// placed the device at 04:00 tomorrow stays there. Header-only and pure (libc time only),
// so it is host-tested (test/test_clock_skew) and links nothing into a build that never
// calls it. The limits match nimbusd's test clock (nimbusd/src/posix_platform.h vclock).
//
//   CLOCK?            read: epoch, held skew, local time
//   CLOCK +<s>        skew forward by s seconds (1..366 days), on top of any skew held
//   CLOCK at HH:MM    skew forward to the next HH:MM local time (the device TZ), strictly ahead
//   CLOCK off         take the held skew back off and stop holding it
//   CLOCK <epoch>     set the clock (the older absolute form); clears the held skew
#include <cstdint>
#include <cstring>
#include <ctime>

namespace nimbus::clockskew {

constexpr int64_t kMaxStepSec = 366LL * 24 * 3600;        // one CLOCK +N or CLOCK at
constexpr int64_t kMaxSkewSec = 10LL * 366 * 24 * 3600;   // the held total
constexpr int64_t kValidEpoch = 1600000000;               // below this the clock was never set

enum class Op { Read, Forward, At, Off, Set, Bad };

struct Cmd {
  Op op = Op::Bad;
  int64_t value = 0;   // Forward: seconds; Set: the epoch
  int hour = 0, minute = 0;   // At
};

// A run of decimal digits only (no sign, no space), as a value up to 10^15; -1 otherwise.
inline int64_t digits(const char* s, size_t n) {
  if (n == 0 || n > 15) return -1;
  int64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

// "HH:MM", 00:00..23:59.
inline bool parseWall(const char* s, int* hour, int* minute) {
  if (std::strlen(s) != 5 || s[2] != ':') return false;
  const int64_t h = digits(s, 2), m = digits(s + 3, 2);
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  *hour = (int)h;
  *minute = (int)m;
  return true;
}

// The argument after "CLOCK" ("?" reads; "" is not a command).
inline Cmd parse(const char* arg) {
  Cmd c;
  const size_t n = std::strlen(arg);
  if (std::strcmp(arg, "?") == 0) { c.op = Op::Read; return c; }
  if (std::strcmp(arg, "off") == 0) { c.op = Op::Off; return c; }
  if (n > 1 && arg[0] == '+') {
    const int64_t s = digits(arg + 1, n - 1);
    if (s >= 1 && s <= kMaxStepSec) { c.op = Op::Forward; c.value = s; }
    return c;
  }
  if (n > 3 && std::strncmp(arg, "at ", 3) == 0) {
    if (parseWall(arg + 3, &c.hour, &c.minute)) c.op = Op::At;
    return c;
  }
  const int64_t e = digits(arg, n);
  if (e >= 0) { c.op = Op::Set; c.value = e; }
  return c;
}

// The held total after one more forward step, or false past kMaxSkewSec (nothing moves).
inline bool addSkew(int64_t held, int64_t step, int64_t* out) {
  if (step < 1 || step > kMaxStepSec || held < 0 || held + step > kMaxSkewSec) return false;
  *out = held + step;
  return true;
}

// The next time the local wall clock (the process TZ) reads hour:minute, strictly after now.
// The calendar day steps, so a DST change keeps the wall time; 0 if libc cannot place it.
inline time_t nextLocal(time_t now, int hour, int minute) {
  struct tm day;
  if (!localtime_r(&now, &day)) return 0;
  for (int ahead = 0; ahead < 3; ++ahead) {
    struct tm at = day;
    at.tm_mday += ahead;
    at.tm_hour = hour;
    at.tm_min = minute;
    at.tm_sec = 0;
    at.tm_isdst = -1;   // let libc decide summer time for that date
    const time_t t = mktime(&at);
    if (t != (time_t)-1 && t > now) return t;
  }
  return 0;
}

}  // namespace nimbus::clockskew
