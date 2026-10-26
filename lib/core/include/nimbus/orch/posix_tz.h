#pragma once
#include <cctype>
#include <cstddef>
#include <string>

// posix_tz - is a timezone rule one the device's C library can read? (QW5) Portable
// and host-tested (test/test_posix_tz).
//
// The device keeps its timezone as a POSIX TZ rule (devTz) and hands it to
// setenv("TZ")/tzset(). A rule the library cannot parse is silently treated as UTC, so
// a typo saved from Settings or the setup wizard left routines and food-log days on
// UTC while the page said "Saved". POST /api/config refuses such a rule with the
// sentence below, before anything is stored.
//
// The accepted grammar is the POSIX form newlib parses (and every rule the web page
// derives from the browser, ui_js.h tzFromBrowser):
//   std offset [dst [offset] [,start[/time],end[/time]]]
//   name    3+ letters, or <3+ of letters, digits, + and -> (the quoted form, <+09>)
//   offset  [+|-]hh[:mm[:ss]], hours 0-24
//   start/end  Mm.w.d (month 1-12, week 1-5, day 0-6) | Jn (1-365) | n (0-365)
//   time    hh[:mm[:ss]], hours 0-167 (a rule like "Saturday at 24:00" or 26:00);
//           a negative time is not accepted
// Blank means UTC and is accepted.
namespace nimbus {
namespace orch {

constexpr size_t kDevTzMax = 48;   // the devTz store bound (webui.cpp)

namespace tzdetail {
struct Cur {
  const std::string& s;
  size_t i;
  bool at(char ch) const { return i < s.size() && s[i] == ch; }
  bool eat(char ch) {
    if (!at(ch)) return false;
    ++i;
    return true;
  }
};
inline bool nameChar(char ch, bool quoted) {
  const unsigned char u = static_cast<unsigned char>(ch);
  return quoted ? (std::isalnum(u) || ch == '+' || ch == '-') : std::isalpha(u) != 0;
}
inline bool name(Cur& c) {
  const bool quoted = c.eat('<');
  size_t n = 0;
  while (c.i < c.s.size() && nameChar(c.s[c.i], quoted)) ++c.i, ++n;
  if (n < 3) return false;
  return !quoted || c.eat('>');
}
// 1 to maxDigits digits, value in [lo, hi].
inline bool num(Cur& c, int lo, int hi, int maxDigits) {
  int v = 0, n = 0;
  while (c.i < c.s.size() && std::isdigit(static_cast<unsigned char>(c.s[c.i])) && n < maxDigits)
    v = v * 10 + (c.s[c.i++] - '0'), ++n;
  return n > 0 && v >= lo && v <= hi;
}
inline bool hms(Cur& c, int maxH) {
  if (!num(c, 0, maxH, 3)) return false;
  if (c.eat(':') && !num(c, 0, 59, 2)) return false;
  if (c.eat(':') && !num(c, 0, 59, 2)) return false;
  return true;
}
inline bool offset(Cur& c) {
  if (!c.eat('+')) c.eat('-');
  return hms(c, 24);
}
inline bool date(Cur& c) {
  if (c.eat('M'))
    return num(c, 1, 12, 2) && c.eat('.') && num(c, 1, 5, 1) && c.eat('.') && num(c, 0, 6, 1);
  if (c.eat('J')) return num(c, 1, 365, 3);
  return num(c, 0, 365, 3);
}
inline bool rule(Cur& c) { return date(c) && (!c.eat('/') || hms(c, 167)); }
// The daylight part after the standard name and offset: name [offset] [,rule,rule].
inline bool daylight(Cur& c) {
  if (!name(c)) return false;
  if (!c.at(',') && c.i < c.s.size() && !offset(c)) return false;
  if (c.i == c.s.size()) return true;   // daylight time on the default rule
  return c.eat(',') && rule(c) && c.eat(',') && rule(c) && c.i == c.s.size();
}
inline bool wellFormed(const std::string& tz) {
  Cur c{tz, 0};
  if (!name(c) || !offset(c)) return false;
  return c.i == tz.size() || daylight(c);   // standard time only, or with daylight time
}
}  // namespace tzdetail

// "" when `tz` is a rule the device can read (or blank, meaning UTC), else the
// owner-facing refusal.
inline std::string posixTzError(const std::string& tz) {
  if (tz.empty()) return std::string();
  if (tz.size() > kDevTzMax) return "That timezone rule is too long. Pick a suggestion from the list.";
  if (tzdetail::wellFormed(tz)) return std::string();
  return "That timezone rule is not one the device can read. Pick a suggestion from the list, "
         "or use this computer's timezone.";
}

}  // namespace orch
}  // namespace nimbus
