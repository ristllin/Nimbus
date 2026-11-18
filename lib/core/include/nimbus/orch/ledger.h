#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/tx/log.h"

// ledger.h - the food ledger store (PRD section 4, "Ledger"; lane RB-L1a). It replaces the
// keyed files.append of food/food-log.csv, which replaced, re-keyed and silently skipped rows
// (DIAGNOSIS RC-08). Here every row has a device-assigned id, every write is all or nothing,
// and the totals are computed by the device from the stored rows.
//
// Storage, under /mem/ledger/<person>/ (outside the file index; no file tool can reach it):
//   YYYY-MM.csv       the rows of one month, sorted by day then time. Line 1 is
//                     "# nimbus ledger v1 seq=<n>", the last change it holds; line 2 is the
//                     column header kColumns.
//   changes.log       the write-ahead record. One line per write:
//                     "L1 <seq> <bodyLen> <crc32> <body>\n". The append of that line is the
//                     commit point. The body holds the call id, the time, the receipt and the
//                     resulting rows (put) and removed ids, never the ops, so replay is
//                     idempotent. A line counts when its body has the stated length and crc.
//   changes.old.log   the previous log, kept when changes.log passes kLogRollBytes, so call
//                     ids and recent adds survive one roll.
//   YYYY-MM.csv.part / .bak   the month swap: the new copy is written to .part, the old one
//                     is renamed to .bak, .part is renamed into place, .bak is removed.
//
// Recovery (open(), run before the first read or write and after any failed step): leftover
// .part files go, a month whose file is missing but whose .bak exists gets the .bak back, a
// .bak beside its file goes; then every logged write a month file lacks (its seq is lower)
// is applied again in seq order. After a power cut at any step a month holds either the old
// rows or the new ones, never a mix.
//
// Figures are fixed-point integers; totals are exact sums:
//   kcal and per100 in 0.1 kcal, grams (protein .. alcohol) in 0.01 g, qty in 0.001 unit.
//
// The ledger day runs 04:00 to 04:00 local time: 03:59 on the 18th is the 17th.
namespace nimbus {
namespace orch {
namespace ledger {

inline constexpr const char* kLedgerRoot = "/mem/ledger";
inline constexpr const char* kColumns =
    "id,day,time,meal,item,qty,unit,kcal,protein_g,carb_g,fat_g,fibre_g,polyol_g,alcohol_g,"
    "per100,basis,src,note,call";
inline constexpr size_t kMaxOps = 16;               // per write; the tool allows 1 to 8
inline constexpr int kDayStartMinute = 4 * 60;      // the ledger day starts at 04:00 local
inline constexpr int64_t kIdenticalWindowSec = 30 * 60;
inline constexpr size_t kLogRollBytes = 256 * 1024;
inline constexpr int64_t kMinClockEpoch = 1700000000;   // below this the clock is not set
inline constexpr uint64_t kMaxSeq = (1ull << 21) - 1;   // ids are (seq * 16 + i) scrambled
// Text field ceilings in bytes; longer is an error naming the field, never a silent clip.
inline constexpr size_t kMaxItem = 200, kMaxMeal = 40, kMaxUnit = 16, kMaxSrc = 80,
                        kMaxNote = 400, kMaxCall = 80;
inline constexpr size_t kMaxIdsPerOp = 16;            // ids an update, remove or move may name
inline constexpr size_t kMaxFileBytes = 1024 * 1024;  // a month file or log larger is not read

// Decimal places of each figure.
inline constexpr int kKcalDp = 1, kGramDp = 2, kQtyDp = 3;

// A fixed-point figure: `v` in units of 10^-dp. `set` false means not given (blank in the CSV).
struct Fixed {
  int64_t v = 0;
  bool set = false;
  static Fixed of(int64_t raw) { return Fixed{raw, true}; }
  bool operator==(const Fixed& o) const { return set == o.set && (!set || v == o.v); }
  bool operator!=(const Fixed& o) const { return !(*this == o); }
};

// "118.3", "-2", "0.25" -> Fixed with `dp` decimals; more digits round half away from zero.
// False for anything else (exponents, two dots, empty, over 15 integer digits).
bool parseFixed(const std::string& text, int dp, Fixed& out);
// A JSON number as the tool layer reads it, rounded half away from zero to `dp` decimals.
// False for NaN, infinities and magnitudes past 1e12.
bool fixedFromDouble(double value, int dp, Fixed& out);
// "1,612" / "118.3" / "-0.05": thousands separated, trailing zeros dropped. "" when unset.
std::string formatFixed(const Fixed& f, int dp, bool thousands = true);

enum class Basis : uint8_t { None = 0, Label, Stated, Estimate, Legacy };
const char* basisSlug(Basis b);   // "", "label", "stated", "estimate", "legacy"
bool parseBasis(const std::string& slug, Basis& out);   // "" -> None

struct Row {
  std::string id;      // "L" + 5 base-32 characters, assigned by the store
  std::string day;     // ledger day YYYY-MM-DD
  std::string time;    // HH:MM local, or ""
  std::string meal;
  std::string item;
  Fixed qty;           // 0.001 unit
  std::string unit;
  Fixed kcal;          // 0.1 kcal, required
  Fixed protein, carb, fat, fibre, polyol, alcohol;   // 0.01 g
  Fixed per100;        // 0.1 kcal per 100 g or ml
  Basis basis = Basis::None;
  std::string src;
  std::string note;
  std::string call;    // the call id of the write that last set this row
  bool operator==(const Row& o) const;
  bool operator!=(const Row& o) const { return !(*this == o); }
};

// The columns an update can set, one bit each.
enum Field : uint32_t {
  kFDay = 1u << 0, kFTime = 1u << 1, kFMeal = 1u << 2, kFItem = 1u << 3, kFQty = 1u << 4,
  kFUnit = 1u << 5, kFKcal = 1u << 6, kFProtein = 1u << 7, kFCarb = 1u << 8, kFFat = 1u << 9,
  kFFibre = 1u << 10, kFPolyol = 1u << 11, kFAlcohol = 1u << 12, kFPer100 = 1u << 13,
  kFBasis = 1u << 14, kFSrc = 1u << 15, kFNote = 1u << 16,
};
// "kcal" -> kFKcal (column names of kColumns, "protein" and "protein_g" both accepted). 0 if
// the name is not a settable column (id and call are not).
uint32_t fieldByName(const std::string& name);

enum class OpKind : uint8_t { Add = 0, Update, Remove, Move };

// One op of a write. The tool layer (RB-L1b, Ruling 11) maps its JSON onto this:
//   add    {row, day?, time?, meal?}  -> row, with day/time/meal copied into it when given
//   update {id|ids, set}              -> ids + setMask + set (the values of the set fields)
//   remove {id|ids}                   -> ids
//   move   {id|ids, day}              -> ids + day
struct Op {
  OpKind kind = OpKind::Add;
  Row row;                         // add: the new row (id and call are ignored)
  std::vector<std::string> ids;    // update, remove, move (case-insensitive)
  uint32_t setMask = 0;            // update: which fields of `set` apply
  Row set;                         // update: the values (an unset Fixed or "" clears)
  std::string day;                 // move: the target ledger day
};

// The time of the call. The device passes the epoch and the local offset in force (from the
// owner's time zone rule); `set` false (or an epoch before kMinClockEpoch) is an unset clock.
struct Clock {
  int64_t epoch = 0;
  int32_t utcOffsetSec = 0;
  bool set = false;
  bool valid() const { return set && epoch >= kMinClockEpoch; }
};

// Daily targets for the day line; either may be unset.
struct Targets {
  Fixed kcal;      // 0.1 kcal
  Fixed protein;   // 0.01 g
};

struct DayTotals {
  std::string day;
  size_t rows = 0;
  int64_t kcal = 0;                                       // 0.1 kcal
  int64_t protein = 0, carb = 0, fat = 0, fibre = 0, polyol = 0, alcohol = 0;   // 0.01 g
  size_t noProtein = 0;                                   // rows without a protein figure
};

struct DayView {
  std::vector<Row> rows;   // the day's rows, by time (rows without a time last)
  DayTotals totals;
};

enum class WriteStatus : uint8_t {
  Ok = 0,            // committed; `text` is the receipt
  AlreadyApplied,    // the call id was seen: `text` is the first receipt, marked
  Refused,           // nothing was written; `text` names why (an error, never a heuristic)
  NotSaved,          // the storage refused the commit; nothing was written
  Uncertain,         // the commit failed part way and the log could not be read back: the
                     // change may or may not be stored. The same call id again settles it
                     // (it applies the change once, or says "already applied").
};

struct WriteResult {
  WriteStatus status = WriteStatus::Refused;
  std::string text;                    // the receipt or the error
  std::vector<Row> changed;            // the rows as written (removed rows as they were)
  std::vector<std::string> days;       // ledger days touched, sorted
  std::vector<std::string> warnings;   // facts appended to the receipt
  uint64_t seq = 0;                    // the record's sequence number
};

// ---- the ledger day -------------------------------------------------------------------
// Local date and time of `epoch` at `utcOffsetSec`.
void localDateTime(int64_t epoch, int32_t utcOffsetSec, std::string& date, std::string& hhmm);
// The ledger day of a local date and HH:MM: before 04:00 it is the previous date.
std::string ledgerDayOf(const std::string& date, const std::string& hhmm);
// The ledger day at `clock`; "" when the clock is not set.
std::string ledgerToday(const Clock& clock);
// For an add with only a time: the ledger day of the occurrence of `hhmm` nearest to `clock`
// (12 h either way; a tie goes to the past). "" when the clock is not set.
std::string ledgerDayForTime(const Clock& clock, const std::string& hhmm);
bool validDay(const std::string& day);    // YYYY-MM-DD, a real calendar date
bool validTime(const std::string& hhmm);  // HH:MM, 00:00 to 23:59
std::string addDays(const std::string& day, int delta);

// ---- rendering (the receipt and ledger_day use the same lines) ---------------------------
// "L3K9QZ | 312 kcal, P 10.5 g, C 54 g, F 6.2 g, fibre 8 g | 250 g oat porridge |
//  2026-11-17 08:10 breakfast | estimate". Figures first; absent figures are left out.
std::string renderRow(const Row& r);
// "day 2026-11-17 (ledger day, 04:00 to 04:00): 7 rows, 1,612 kcal, P 118.3 g; target
//  2,300 / 160 -> left 688 kcal, 41.7 g". Without targets the tail is "; no target set".
std::string renderDayLine(const DayTotals& t, const Targets* targets);
DayTotals totalsOf(const std::string& day, const std::vector<Row>& rows);

// ---- row checks -------------------------------------------------------------------------
// The errors that refuse an add or update ("" when the row may be stored).
std::string rowError(const Row& r);
// The figure warnings of one row (kJ-sized value, macro energy off by more than 15%).
std::vector<std::string> figureWarnings(const Row& r);
// The identical-row rule: same item text ignoring case (ASCII and Latin-1 letters), same qty
// and unit (unit ignoring case), same kcal, same ledger day.
bool identicalRows(const Row& a, const Row& b);
// Lower-cases ASCII and the Latin-1 capitals U+00C0 to U+00DE (not U+00D7).
std::string foldCase(const std::string& s);

// ---- the CSV and the ids -----------------------------------------------------------------
std::string encodeRow(const Row& r);                     // one CSV line, no newline
bool decodeRow(const std::string& line, Row& out);       // false on a malformed line
std::string rowId(uint64_t seq, unsigned index);         // "L" + 5 base-32, unique per pair
std::string normalizeId(const std::string& id);          // upper case, trimmed
bool validPerson(const std::string& person);             // [A-Za-z0-9_-], 1 to 32 bytes
uint32_t crc32(const std::string& data);

// ---- the store ---------------------------------------------------------------------------
// One LedgerStore per person, used from one task (the device's turn task, or one hosted
// loop); a second instance on the same folder is noticed by the log size and re-reads it
// before writing, but two writers at the same moment are not supported.
class LedgerStore {
 public:
  // `logRollBytes`: the size past which changes.log rolls (tests pass a small one).
  LedgerStore(tx::TxFiles& files, const std::string& person, size_t logRollBytes = kLogRollBytes);

  // Recovery (see above). Runs by itself before the first read or write and again after a
  // failed step. False when the storage cannot be read; `report` says what it did ("" if
  // nothing).
  bool open(std::string* report = nullptr);

  // Apply 1 to kMaxOps ops together, in order, all or nothing. A call id already in the
  // log returns its first receipt, marked "already applied". `targets` feed the day lines.
  WriteResult write(const std::vector<Op>& ops, const std::string& callId, const Clock& now,
                    const Targets* targets);

  // A day's rows and the device totals. False when the storage cannot be read or the
  // month file is damaged (`err` says which).
  bool day(const std::string& day, DayView& out, std::string* err = nullptr);
  // Every row of every month, by day then time (tests, the import's comparison, audits).
  bool allRows(std::vector<Row>& out, std::string* err = nullptr);

  const std::string& dir() const { return dir_; }
  uint64_t lastSeq() const { return lastSeq_; }

  struct Month;   // the parsed month file (orch_ledger.cpp)
  struct Record;  // one decoded log record (orch_ledger.cpp)

 private:
  struct Work;

  bool ensureOpen(std::string* err);
  bool finishSwaps(std::vector<std::string>& months, std::string& report);
  bool readLogs(std::vector<Record>& recs);
  void scanLogs(const std::vector<Record>& recs);
  void pruneAdds(int64_t newest);
  bool replay(std::vector<Record>& recs, std::string& report);
  bool loadMonth(const std::string& ym, Month& out, std::string* err, bool* io = nullptr);
  bool storeMonth(const Month& m);
  bool appendRecord(const std::string& line);
  bool findReceipt(uint64_t seq, std::string& receipt);
  WriteResult alreadyApplied(const std::string& call, uint64_t seq);
  std::string receiptOf(Work& w, const Targets* targets);
  bool commit(Work& w, const std::string& receipt, bool& known);
  bool logChanged();
  bool recordLanded(uint64_t seq, bool& known);
  bool replayRecord(const Record& r, std::map<std::string, Month>& pending, std::string& report);
  bool applyOp(Work& w, const Op& op, size_t index, std::string& err);
  bool applyAdd(Work& w, const Op& op, size_t index, std::string& err);
  bool applyChange(Work& w, const Op& op, std::string& err);
  bool locate(Work& w, const std::string& id, std::string& ym, size_t& pos, std::string& err);
  void identicalWarnings(Work& w, const Row& r);
  void maybeRoll();
  std::vector<std::string> monthNames();
  std::string path(const std::string& name) const { return dir_ + "/" + name; }

  tx::TxFiles& files_;
  std::string person_;
  std::string dir_;
  size_t rollBytes_;
  bool opened_ = false;
  bool dirty_ = false;       // a step failed: recover before the next read or write
  bool tornTail_ = false;    // changes.log does not end in a newline
  size_t logSize_ = 0;       // changes.log size as this instance last saw it
  uint64_t lastSeq_ = 0;
  std::map<std::string, uint64_t> calls_;     // call id -> seq of its record
  std::map<std::string, int64_t> addedAt_;    // id -> epoch of its add, recent adds only
  std::map<std::string, uint64_t> monthSeq_;  // YYYY-MM -> seq its file holds
};

}  // namespace ledger
}  // namespace orch
}  // namespace nimbus
