#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/orch/file_append.h"   // CsvDayTally, LogDay

// food_ledger - the one daily food ledger the assistant reads instead of its memory.
//
// On 26 Oct the owner's day of meals lived nowhere the model always saw: no food
// log existed, the model guessed three file names, logged meals to long-term
// memory (queued while embeddings were paused), and rebuilt the day's total from
// whatever history survived compaction - 2,310, 1,654, 442, 1,517 and 2,402 kcal
// within one hour. The device now keeps ONE ledger file per person, with a fixed
// name, and places today's rows and totals in the prompt of every turn whose chat
// has rows today (and of every food turn, with the exact call that starts it).
//
// The file is a keyed CSV (file_append.h): one row per eating occasion, identified
// by its date and meal, so a correction replaces its row and a day's total never
// counts a figure twice. Today's ledger is the file's rows dated today in the
// device's local day (logDayOf). One file, not one per day: the file index holds
// 256 entries in total, so a file per day would exhaust it within a year.
//
// Portable, Arduino-free, host-tested (test_food_ledger). The device streams the
// stored file through FoodLedgerScan (src/agent/food_ledger_device.cpp); the host
// lab and the tests use scanFoodLedger on a whole text.
namespace nimbus::orch {

// Where the ledger lives: project "food", the owner's file food-log.csv. A member
// cannot write a file the owner holds, so a member's ledger is
// food-log-<chat id>.csv in the same project.
inline constexpr const char kFoodLedgerProject[] = "food";
inline constexpr const char kFoodLedgerOwnerName[] = "food-log.csv";
// The columns a new ledger starts with (the first files.append carries this row).
inline constexpr const char kFoodLedgerHeader[] = "date,meal,items,kcal,protein_g";
// What the prompt shows of today's rows: the newest rows, at most this many and
// this many bytes; one row at most kLedgerRowMax bytes. The totals always count
// every row of the day.
inline constexpr size_t kLedgerShowRows = 24;
inline constexpr size_t kLedgerShowBytes = 1400;
inline constexpr size_t kLedgerRowMax = 160;

// The ledger's file name for one person: the owner's food-log.csv; a member's
// food-log-<letters and digits of the chat id>.csv ("" when nothing usable is left).
std::string foodLedgerName(bool owner, const std::string& chatId);

// May the person with namespace `ns` write project/name (the file store asks on every
// write)? The ledger names are reserved: in project food, a name that starts with
// "food-log" belongs to one person - food-log.csv to the owner, food-log-<id>.csv to
// the member whose chat id it names - so nobody can create another person's ledger
// before their first meal (compared without case, as the card's file system opens
// names). Every other name: true. The device owner: always true.
bool ledgerNameWritable(const std::string& project, const std::string& name, const std::string& ns,
                        bool owner);

// Today's ledger as the prompt and the fold read it.
struct FoodLedger {
  std::string today;     // YYYY-MM-DD, the local day ("" = the clock is not set: no ledger)
  std::string zone;      // "GMT", "BST", "UTC, no time zone is set"
  std::string project;   // kFoodLedgerProject
  std::string name;      // the person's file
  bool exists = false;   // the file is on the card
  std::string error;     // why it could not be read this turn ("" = it could, or it does not exist)
  std::string header;    // the stored header row ("" = none)
  std::vector<std::string> rows;   // today's rows shown, oldest first (the newest kept)
  int rowsToday = 0;     // every stored row dated today (shown or not)
  bool hasKcal = false;  // the header has a kcal column
  double kcal = 0;
  int kcalRows = 0;      // rows counted in kcal
  int kcalUnread = 0;    // rows dated today whose kcal is not a plain number
  bool hasProtein = false;
  double protein = 0;
  std::string mealColumn;  // the stored header's meal column (the keyed write's key; "" = none)
  std::string kcalColumn;  // its kcal column ("" = none)
  bool hasRows() const { return rowsToday > 0; }
  std::string where() const { return project + "/" + name; }
  std::string dayLabel() const { return today + (zone.empty() ? "" : " (" + zone + ")"); }
};

// The ledger read as a row stream: feed() every stored row in order (the first
// non-blank one is the header), then finish(). The kcal column is the header's
// "kcal" (else the first name holding kcal or calorie); protein is "protein_g"
// (else the first name holding protein). Rows dated today are found with the same
// reading the day total uses (CsvDayTally), so the rows shown and the total agree.
class FoodLedgerScan {
 public:
  explicit FoodLedgerScan(const std::string& today);
  ~FoodLedgerScan();
  FoodLedgerScan(const FoodLedgerScan&) = delete;
  FoodLedgerScan& operator=(const FoodLedgerScan&) = delete;
  void feed(const std::string& row, bool complete);
  // Fill everything but where/today/zone/exists/error.
  void finish(FoodLedger& out) const;

 private:
  void startHeader(const std::string& row);
  void keepRow(const std::string& row);
  std::string today_;
  std::string header_;
  bool sawHeader_ = false;
  std::string kcalColumn_, mealColumn_;
  std::unique_ptr<CsvDayTally> kcal_, protein_;
  std::vector<std::string> shown_;
  size_t shownBytes_ = 0;
  int rowsToday_ = 0;
};

// The whole-text form (the host lab, the tests). `fileText` "" with exists false =
// no ledger yet.
FoodLedger scanFoodLedger(const std::string& fileText, bool exists, const LogDay& day,
                          const std::string& project, const std::string& name);

// The per-turn block. A ledger with rows today: the rows and the totals, every
// turn. No rows today: on a food turn, the exact call that starts today's rows;
// otherwise "". A ledger that could not be read: on a food turn, a line that says
// so (never a total), otherwise "". No clock: "".
std::string renderLedgerBlock(const FoodLedger& l, bool foodTurn);

// The fold's view: the date and today's totals with every row of the day the
// prompt shows ("" when there is no ledger or no row today).
std::string renderLedgerForFold(const FoodLedger& l);

// The fold's day: a "[TODAY] <date> (<zone>)" line, then renderLedgerForFold ("" =
// no clock). Its first line and its totals line start with these, so the fold input
// can keep just them when the whole does not fit (buildCompactInputs).
inline constexpr const char kFoldTodayLine[] = "[TODAY] ";
inline constexpr const char kFoldTotalsLine[] = "Totals for ";
std::string renderFoldDay(const FoodLedger& l);

// Is this text about food: a meal, eating or drinking, calories or protein, a food
// named, or a weight in grams? Lexical and cheap, for deciding which turns get the
// food playbook and the empty-ledger line; a miss costs a skill read, never data.
bool looksFoodRelated(const std::string& text);
// What a food message is, when its words say so plainly: a report of food eaten
// ("I had", "breakfast:", "lunch was"), or a request for an answer or a plan ("how
// much", "plan my dinner", a question, a complaint about a plan). Unclear (both or
// neither) is Unclear. On 27 Oct a Mistral head logged a requested plan as a meal
// eaten in three of four runs; a one-line hint by the playbook stopped it.
// A Complaint ("you invented", "I didn't say") is a request too, with its own hint:
// own the mistake in a sentence, then the corrected answer.
enum class FoodIntent : uint8_t { Unclear = 0, Eaten, Ask, Complaint };
FoodIntent foodIntentOf(const std::string& text);
// The one-line hint for a clear intent ("" for Unclear).
std::string renderFoodIntentHint(FoodIntent intent);

// Does this text ask about the ledger itself: its list, a total, a sum or an
// earlier estimate ("give me the list", "your calc is off", "you estimated
// earlier")? Read only when today's ledger has rows.
bool mentionsLedger(const std::string& text);

}  // namespace nimbus::orch
