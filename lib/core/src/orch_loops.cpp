#include "nimbus/orch/loops.h"

#include <cstdio>
#include <cstring>

// Local Loops pure core - see loops.h. Every function here is host-tested and
// Arduino-free; the device subsystem drives it (tick, persistence, tz/epoch).

namespace nimbus {
namespace orch {

// ---- civil-time math (Howard Hinnant) -------------------------------------

uint32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int      era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);                    // [0, 399]
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;        // [0, 146096]
  return (uint32_t)(era * 146097 + (int)doe - 719468);              // days since 1970-01-01
}

CivilTime civilFromDays(uint32_t dayNum) {
  int64_t z = (int64_t)dayNum + 719468;
  const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);                // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int      y   = (int)yoe + (int)(era * 400);
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp  = (5 * doy + 2) / 153;                          // [0, 11]
  const unsigned d   = doy - (153 * mp + 2) / 5 + 1;                 // [1, 31]
  const unsigned m   = mp < 10 ? mp + 3 : mp - 9;                    // [1, 12]
  CivilTime c;
  c.year  = y + (m <= 2);
  c.month = (int)m;
  c.day   = (int)d;
  c.wday  = (int)((dayNum + 4) % 7);   // 1970-01-01 was a Thursday; 0=Sunday
  return c;
}

// ---- schedule math --------------------------------------------------------

uint64_t nextIntervalEpoch(const SchedSpec& s, uint64_t base) {
  return base + (uint64_t)s.intervalSec;
}

CivilTime nextWallClockLocal(const SchedSpec& s, const CivilTime& now) {
  const uint32_t nowDay = daysFromCivil(now.year, (unsigned)now.month, (unsigned)now.day);
  const int      nowMin = now.hour * 60 + now.min;
  const int      target = (int)s.minuteOfDay;

  // Fire STRICTLY after now: today is a candidate only if target minute > now
  // minute (fire second is 0, so target==now goes to the next candidate day).
  const uint32_t startDay = (target > nowMin) ? nowDay : nowDay + 1;

  uint32_t fireDay = startDay;
  if (s.kind == SchedKind::Weekly) {
    for (int i = 0; i < 7; ++i) {
      const uint32_t day = startDay + (uint32_t)i;
      const int      wd  = (int)((day + 4) % 7);   // 0=Sun
      if (s.weekMask & (1u << wd)) { fireDay = day; break; }
    }
  }
  CivilTime c = civilFromDays(fireDay);
  c.hour = target / 60;
  c.min  = target % 60;
  c.sec  = 0;
  return c;
}

bool isDue(uint64_t nextRun, uint64_t nowEpoch) {
  return nextRun != 0 && nowEpoch >= nextRun;
}

// ---- the pure fire decision ------------------------------------------------

FireDecision evaluate(const LoopRecord& l, const DeviceCounters& dev,
                      const LoopCaps& caps, uint64_t nowEpoch,
                      bool clockValid, bool chatAllowed) {
  // Standing gates (a paused loop reports its reason even when not due).
  if (!l.enabled)                              return FireDecision::BlockedDisabled;
  if (!l.approved)                             return FireDecision::BlockedUnapproved;
  if (l.consecFails >= caps.maxConsecFails)    return FireDecision::BlockedConsecFails;
  if (l.sched.kind != SchedKind::Interval && !clockValid)
                                               return FireDecision::BlockedNoClock;
  // Cheapest "nothing to do" - before any cost/rate gate.
  if (!isDue(l.nextRun, nowEpoch))             return FireDecision::SkipNotDue;
  // Due - the "may it fire right now" gates.
  if (!chatAllowed)                            return FireDecision::BlockedChatRevoked;
  if (l.firesToday >= (uint16_t)caps.maxFiresPerDay)
                                               return FireDecision::BlockedLoopFires;
  if (l.tokensToday >= caps.maxTokensPerDay)   return FireDecision::BlockedLoopTokens;
  if (dev.firesInWindow >= (uint16_t)caps.devFiresWindow)
                                               return FireDecision::BlockedRateWindow;
  if (dev.tokensToday >= caps.devTokensPerDay) return FireDecision::BlockedDeviceTokens;
  return FireDecision::Fire;
}

// ---- validation ------------------------------------------------------------

static uint8_t dayNameToBit(const char* name) {
  if (!name) return 0;
  char a = name[0], b = name[0] ? name[1] : 0, c = name[1] ? name[2] : 0;
  auto lc = [](char x) { return (x >= 'A' && x <= 'Z') ? char(x + 32) : x; };
  a = lc(a); b = lc(b); c = lc(c);
  if (a == 's' && b == 'u') return 1u << 0;  // sun
  if (a == 'm' && b == 'o') return 1u << 1;  // mon
  if (a == 't' && b == 'u') return 1u << 2;  // tue
  if (a == 'w' && b == 'e') return 1u << 3;  // wed
  if (a == 't' && b == 'h') return 1u << 4;  // thu
  if (a == 'f' && b == 'r') return 1u << 5;  // fri
  if (a == 's' && b == 'a') return 1u << 6;  // sat
  (void)c;
  return 0;
}

bool parseSpec(ArduinoJson::JsonObjectConst schedule, const LoopCaps& caps,
               SchedSpec& out, std::string& err) {
  const char* kind = schedule["kind"] | "interval";
  if (!std::strcmp(kind, "interval")) {
    out.kind = SchedKind::Interval;
    uint32_t every = schedule["every_seconds"] | 0u;
    if (every < caps.minIntervalSec) {
      err = "every_seconds below the minimum (" + std::to_string(caps.minIntervalSec) + "s)";
      return false;
    }
    out.intervalSec = every;
    return true;
  }
  if (!std::strcmp(kind, "daily") || !std::strcmp(kind, "weekly")) {
    const char* at = schedule["at"] | "";
    int hh = -1, mm = -1;
    if (std::sscanf(at, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
      err = "'at' must be HH:MM (24h)";
      return false;
    }
    out.minuteOfDay = (uint16_t)(hh * 60 + mm);
    if (!std::strcmp(kind, "daily")) {
      out.kind = SchedKind::Daily;
      out.weekMask = 0x7F;
      return true;
    }
    out.kind = SchedKind::Weekly;
    uint8_t mask = 0;
    for (ArduinoJson::JsonVariantConst dv : schedule["days"].as<ArduinoJson::JsonArrayConst>())
      mask |= dayNameToBit(dv.as<const char*>());
    if (mask == 0) { err = "weekly needs at least one weekday in `days`"; return false; }
    out.weekMask = mask;
    return true;
  }
  if (!std::strcmp(kind, "once")) {
    // W20 one-shot wakeup: fires a single turn in_seconds from now, then
    // retires. NOT subject to the recurring minIntervalSec floor (its whole
    // point is "check back in 30 minutes"); its own bounds instead.
    out.kind = SchedKind::Once;
    uint32_t in = schedule["in_seconds"] | 0u;
    if (in < kWakeupMinSec || in > kWakeupMaxSec) {
      err = "in_seconds must be between " + std::to_string(kWakeupMinSec) + " (2 min) and " +
            std::to_string(kWakeupMaxSec) + " (7 days)";
      return false;
    }
    out.intervalSec = in;
    return true;
  }
  err = "schedule.kind must be interval | daily | weekly | once";
  return false;
}

// ---- day/window rollover (clock-safe) -------------------------------------

void rollDayIfNeeded(LoopRecord& l, uint32_t curDayNum, bool clockValid) {
  if (!clockValid) return;                 // NEVER roll in the boot-relative window
  if (l.lastSyncedDay == 0) {              // first valid sync - adopt, don't wipe
    l.lastSyncedDay = curDayNum;
    return;
  }
  if (curDayNum > l.lastSyncedDay) {       // a real new wall-day
    l.firesToday   = 0;
    l.tokensToday  = 0;
    l.lastSyncedDay = curDayNum;
  }
}

void adoptLocalDay(LoopRecord& l, uint32_t curDayNum) {
  // A TIMEZONE change moved the local calendar, not time itself - adopt the new
  // day frame with the counters UNTOUCHED. Both directions matter:
  //  - westward, the local day number DECREASES; rollDayIfNeeded's strict `>`
  //    would then never fire until the calendar catches up, stalling the daily
  //    counters (and with them the daily fire/token ceilings) for a day;
  //  - eastward, the day number JUMPS FORWARD; letting rollDayIfNeeded see that
  //    as a real midnight would hand every loop a fresh daily budget for free.
  // Adopting (not rolling) makes a tz edit budget-neutral: the counters keep
  // counting against the day in progress, and the next REAL local midnight
  // rolls them as usual.
  l.lastSyncedDay = curDayNum;
}

void rollDeviceDayIfNeeded(DeviceCounters& dev, const LoopCaps& caps,
                           uint32_t curDayNum, uint64_t nowEpoch, bool clockValid) {
  if (clockValid) {
    if (dev.dayNumber == 0)             dev.dayNumber = curDayNum;
    else if (curDayNum > dev.dayNumber) { dev.tokensToday = 0; dev.dayNumber = curDayNum; }
  }
  // Rate window is time-monotonic (works pre-sync too). Reset on a new/elapsed window.
  if (dev.windowStart == 0 || nowEpoch < dev.windowStart ||
      nowEpoch - dev.windowStart >= caps.windowSec) {
    dev.firesInWindow = 0;
    dev.windowStart   = nowEpoch;
  }
}

// ---- fire-result state machine --------------------------------------------

void onFireResult(LoopRecord& l, bool ok, const TokenUsage& used, uint64_t replyHash,
                  uint64_t nowEpoch) {
  l.lastRun      = nowEpoch;
  l.tokensToday += used.total();
  if (ok) {
    l.lastResult = LastResult::Ok;
    l.consecFails = 0;
    if (replyHash != 0 && l.lastReplyHash == replyHash) {
      if (l.repeatRun < 255) l.repeatRun++;
    } else {
      l.repeatRun = 0;
    }
    l.lastReplyHash = replyHash;
  } else {
    l.lastResult = LastResult::Fail;
    if (l.consecFails < 255) l.consecFails++;
    // a failure neither confirms nor breaks a repeat streak
  }
}

void deferLoop(LoopRecord& l, uint64_t nowEpoch, uint32_t deferSec) {
  // Not a fire: firesToday / lastRun / repeat state stay untouched - only the
  // schedule slips. A 0 defer still moves strictly after now (isDue is <=).
  l.nextRun = nowEpoch + (deferSec ? deferSec : 1);
}

bool isSemanticRepeat(const LoopRecord& l, int maxRepeats) {
  return maxRepeats > 0 && l.repeatRun >= maxRepeats;
}

uint64_t fnv64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;  // FNV-1a offset basis
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

// ---- serialization ---------------------------------------------------------

std::string dumpLoops(const std::vector<LoopRecord>& loops) {
  ArduinoJson::JsonDocument d;
  ArduinoJson::JsonArray arr = d.to<ArduinoJson::JsonArray>();
  for (const LoopRecord& l : loops) {
    ArduinoJson::JsonObject o = arr.add<ArduinoJson::JsonObject>();
    o["id"]        = l.id;
    o["name"]      = l.name;
    o["prompt"]    = l.prompt;
    o["chatId"]    = l.chatId;
    o["kind"]      = (int)l.sched.kind;
    o["interval"]  = l.sched.intervalSec;
    o["minute"]    = l.sched.minuteOfDay;
    o["weekMask"]  = l.sched.weekMask;
    o["createdBy"] = (int)l.createdBy;
    o["enabled"]   = l.enabled;
    o["approved"]  = l.approved;
    o["nextRun"]   = l.nextRun;
    o["lastRun"]   = l.lastRun;
    o["lastResult"]  = (int)l.lastResult;
    o["firesToday"]  = l.firesToday;
    o["tokensToday"] = l.tokensToday;
    o["lastSyncedDay"] = l.lastSyncedDay;
    o["consecFails"]   = l.consecFails;
    o["lastReplyHash"] = l.lastReplyHash;
    o["repeatRun"]     = l.repeatRun;
  }
  std::string out;
  ArduinoJson::serializeJson(d, out);
  return out;
}

std::string dumpWakeupsApi(const std::vector<LoopRecord>& loops,
                           bool wakeupsAskFirst, uint64_t nowEpoch) {
  static const char* kRes[] = {"none", "ok", "fail", "skipped", "paused"};
  ArduinoJson::JsonDocument d;
  d["approvalMode"] = wakeupsAskFirst ? "ask" : "auto";
  d["maxArmed"]     = (int)kWakeupMaxPending;
  int armed = 0;
  ArduinoJson::JsonArray arr = d["wakeups"].to<ArduinoJson::JsonArray>();
  for (const LoopRecord& l : loops) {
    if (l.sched.kind != SchedKind::Once) continue;   // wake-ups are Once loops only
    if (l.enabled) armed++;
    ArduinoJson::JsonObject o = arr.add<ArduinoJson::JsonObject>();
    o["id"]        = l.id;
    o["name"]      = l.name;
    o["note"]      = l.prompt;   // the note fired back on wake
    o["createdBy"] = l.createdBy == CreatedBy::Agent ? "agent" : "owner";
    o["enabled"]   = l.enabled;
    o["approved"]  = l.approved;
    o["pending"]   = l.enabled && !l.approved;   // awaiting the single approval card
    o["nextRun"]   = l.nextRun;
    // inSec: whole seconds until the next fire, floored at 0 (never negative), so
    // a UI can say "in ~3 min" without its own clock. Omitted when no clock is
    // supplied (nowEpoch == 0) or the loop has no scheduled time.
    if (nowEpoch && l.nextRun)
      o["inSec"] = l.nextRun > nowEpoch ? (uint32_t)(l.nextRun - nowEpoch) : 0u;
    o["lastResult"] = kRes[(int)l.lastResult % 5];
  }
  d["armed"] = armed;
  std::string out;
  ArduinoJson::serializeJson(d, out);
  return out;
}

bool loadLoops(const std::string& json, std::vector<LoopRecord>& out) {
  out.clear();
  if (json.empty()) return true;   // no file yet = zero loops, not an error
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, json)) return false;
  if (!d.is<ArduinoJson::JsonArray>()) return false;
  for (ArduinoJson::JsonObjectConst o : d.as<ArduinoJson::JsonArrayConst>()) {
    LoopRecord l;
    l.id      = o["id"]     | "";
    // Cap name/prompt on LOAD too (not just at create) so a hand-edited or
    // corrupted loops.json can never feed an unbounded prompt into a turn.
    l.name    = std::string(o["name"]   | "").substr(0, kLoopNameMax);
    l.prompt  = std::string(o["prompt"] | "").substr(0, kLoopPromptMax);
    l.chatId  = o["chatId"] | "";
    l.sched.kind        = (SchedKind)(int)(o["kind"] | 0);
    l.sched.intervalSec = o["interval"] | 21600u;
    l.sched.minuteOfDay = o["minute"]   | 510;
    l.sched.weekMask    = o["weekMask"] | 0x7F;
    l.createdBy   = (CreatedBy)(int)(o["createdBy"] | 0);
    l.enabled     = o["enabled"]  | true;
    l.approved    = o["approved"] | true;
    l.nextRun     = o["nextRun"]  | 0ULL;
    l.lastRun     = o["lastRun"]  | 0ULL;
    l.lastResult  = (LastResult)(int)(o["lastResult"] | 0);
    l.firesToday  = o["firesToday"]  | 0;
    l.tokensToday = o["tokensToday"] | 0u;
    l.lastSyncedDay = o["lastSyncedDay"] | 0u;
    l.consecFails   = o["consecFails"]   | 0;
    l.lastReplyHash = o["lastReplyHash"] | 0ULL;
    l.repeatRun     = o["repeatRun"]     | 0;
    if (!l.id.empty()) out.push_back(l);
  }
  return true;
}

long parseDurationSecs(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
  if (i == 0) return -1;                       // no leading number
  long long n = 0;
  for (size_t k = 0; k < i; k++) {
    n = n * 10 + (s[k] - '0');
    if (n > 100000000LL) { n = 100000000LL; break; }   // saturate, never overflow
  }
  if (n <= 0) return -1;
  std::string unit = s.substr(i);
  for (char& c : unit) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
  long long mult;
  if (unit.empty() || unit == "m" || unit == "min" || unit == "mins")      mult = 60;
  else if (unit == "s" || unit == "sec" || unit == "secs")                 mult = 1;
  else if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour") mult = 3600;
  else if (unit == "d" || unit == "day" || unit == "days")                 mult = 86400;
  else return -1;                              // unknown unit
  long long secs = n * mult;
  const long long kMax = 7LL * 24 * 3600;
  if (secs > kMax) return (long)(kMax + 1);    // over ceiling - caller rejects cleanly
  return (long)secs;
}

// ---- owner cap overrides (clamped; tighten-only) --------------------------

LoopCaps clampLoopCaps(const LoopCaps& base, const LoopCapOverrides& ov) {
  LoopCaps c = base;
  // A present override applies ONLY if it is stricter than the default: a
  // ceiling can move down, never up; the interval floor can move up, never down.
  auto downI = [](int cur, int o)      { return (o > 0 && o < cur) ? o : cur; };
  auto downU = [](uint32_t cur, uint32_t o) { return (o > 0 && o < cur) ? o : cur; };
  c.maxCount        = downI(c.maxCount, ov.maxCount);
  if (ov.minIntervalSec > c.minIntervalSec) c.minIntervalSec = ov.minIntervalSec;
  c.maxFiresPerDay  = downI(c.maxFiresPerDay, ov.maxFiresPerDay);
  c.maxTokensPerDay = downU(c.maxTokensPerDay, ov.maxTokensPerDay);
  c.devTokensPerDay = downU(c.devTokensPerDay, ov.devTokensPerDay);
  c.devFiresWindow  = downI(c.devFiresWindow, ov.devFiresWindow);
  c.maxConsecFails  = downI(c.maxConsecFails, ov.maxConsecFails);
  c.maxRepeats      = downI(c.maxRepeats, ov.maxRepeats);
  return c;
}

}  // namespace orch
}  // namespace nimbus
