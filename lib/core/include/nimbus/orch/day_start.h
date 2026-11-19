#pragma once
#include <cstddef>
#include <cstdint>

// day_start - the device's one day boundary and the message stamp built on it (RB-REC5,
// rulings 11, 41 and 42). Pure, portable, no allocation: engine 1 (compose.cpp's per-turn
// clock line, on the device, in nimbusd and in the harness lab) and engine 2
// (tx::messageStamp) call the same function, so the same instant and settings give the
// same bytes everywhere (test/test_stamp_day compares them).
//
// The setting is general: "day starts at" (key day_starts, "HH:MM", default 04:00) says
// when the running day ends for the whole device. Between local midnight and that time
// the day that is still running is yesterday, so a late entry, a late message or a
// digest belongs to it. 00:00 means the calendar day: never a clause.
//
// The stamp, for `epoch` at `offsetMin` minutes east of UTC (the offset valid at that
// instant, so a DST change is the caller's offset, never a guess here):
//   "2026-11-19 Thu 08:10"                                  at or after the day start
//   "2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)"     before it
//   "time unknown"                                          the clock was never set
// The year and the weekday are always there when there is a clock; a buffer too short for
// the clause gets the plain stamp, never a stamp without its year.
namespace nimbus {
namespace orch {
namespace day {

constexpr char kSettingKey[] = "day_starts";
constexpr uint16_t kDefaultStartMin = 4 * 60;   // 04:00
constexpr uint16_t kMinutesPerDay = 24 * 60;
// Below this the clock was never set (a device before SNTP counts from 1970): no date.
// The same line as history_rows.h kClockValidHours.
constexpr uint64_t kClockValidEpoch = 1000000000ULL;
// The last second a four-digit year can show (9999-12-31 23:59:59 UTC).
constexpr uint64_t kLastEpoch = 253402300799ULL;
// Offsets beyond +-18 h are no zone on Earth: refused.
constexpr int32_t kMaxOffsetMin = 18 * 60;

constexpr size_t kPlainBytes = 20;     // "2026-11-19 Thu 08:10"
constexpr size_t kClauseBytes = 29;    // " (day 2026-11-18 until 04:00)"
constexpr size_t kPlainCap = kPlainBytes + 1;
constexpr size_t kFullCap = kPlainBytes + kClauseBytes + 1;
extern const char kNoClock[];   // "time unknown" (in orch_day_start.cpp: model text, catalogued)

// Read a stored or posted "HH:MM" (two digits each, 00:00 to 23:59, nothing else).
// Returns nullptr and sets `minutes` when valid, else the plain sentence to show the
// person (and leaves `minutes` alone).
const char* parseStart(const char* text, uint16_t& minutes);
constexpr char kStartError[] = "Day starts at needs a time from 00:00 to 23:59, such as 04:00.";

// The setting as read from storage: an absent, empty or invalid value is 04:00.
uint16_t startOrDefault(const char* stored);

// "HH:MM" of a minute of the day (>= 1440 wraps). Writes 5 characters and a NUL.
void formatStart(uint16_t minutes, char out[6]);

// Was the clock ever set at `epoch`?
inline bool clockSet(uint64_t epoch) { return epoch >= kClockValidEpoch && epoch <= kLastEpoch; }

// The running day of `epoch`: days since 1970-01-01 of the local calendar date, one less
// while the local time is before the day start. Defined for every epoch and offset.
int64_t runningDay(uint64_t epoch, int32_t offsetMin, uint16_t startMin);

// The local calendar day number of `epoch` (runningDay with a 00:00 start).
inline int64_t calendarDay(uint64_t epoch, int32_t offsetMin) { return runningDay(epoch, offsetMin, 0); }

// "YYYY-MM-DD" of a day number. Writes 10 characters and a NUL; false (and "") when the
// day falls outside years 1 to 9999 or out is short.
bool formatDate(int64_t dayNumber, char* out, size_t cap);

// The running day of `epoch` as "YYYY-MM-DD" (the digest's day): false and "" with no
// clock, an offset out of range or a short buffer.
bool runningDate(uint64_t epoch, int32_t offsetMin, uint16_t startMin, char* out, size_t cap);

enum class StampForm : uint8_t {
  Full,      // the stamp, with its clause when the day start is still ahead
  Plain,     // the clause was due but `cap` held only the plain stamp
  NoClock,   // "time unknown": the clock was never set
  None,      // nothing written ("" when cap > 0): cap under kPlainCap, or an offset out of range
};

// The message stamp (see the top of this file). Never allocates.
StampForm stamp(uint64_t epoch, int32_t offsetMin, uint16_t startMin, char* out, size_t cap);

}  // namespace day
}  // namespace orch
}  // namespace nimbus
