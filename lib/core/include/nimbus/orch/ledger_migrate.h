#pragma once
#include <cstddef>
#include <string>

// ledger_migrate - the one-time fold of a legacy meal log into the designed
// food ledger (v4.6.3, the 30 Oct Lumi investigation).
//
// The designed ledger (ONE file, header date,meal,items,kcal,protein_g, one row
// per meal, corrections by key) is where meals belong. A device that had
// already been logging may hold an earlier ad-hoc file instead (the
// investigation's file: 39 rows, no header, five generations of the same three
// days in four different column schemas, phantom rows and shifted-column
// corrections). Every keyed write refused (no header, so no key column), so
// every "correction" appended a new duplicate and nothing ever replaced.
//
// This planner folds such a file ONCE, at boot, into the designed ledger:
//   - a row is read only in a shape it can map: a date or timestamp first,
//     then a meal word (breakfast, lunch, ...) or an item, then numbers it can
//     read kcal and protein from - the schemas the real file used:
//     <ts, meal, item, grams, kcal, protein>, <ts, item, amount, kcal, protein>,
//     <date, meal, item, grams, kcal, protein>, <ts, item, kcal, protein>;
//   - a row with no meal word takes its meal from the time of day (before
//     11:00 breakfast, before 15:30 lunch, before 20:00 dinner, else evening);
//   - rows group by (day, meal); a GENERATION is the rows of one timestamp -
//     one append's worth. The LAST generation wins outright (the owner's final
//     figures - the intermediate generations, a sub-agent's invented figures
//     and phantom estimates all drop);
//   - within the winning generation, rows naming the same item are generations
//     of that item: the LAST wins (three appends of one drink collapse to one);
//   - the ledger row joins the surviving items with "; " (never a comma - the
//     ledger is comma-separated) and sums their kcal and protein.
// A row that maps to nothing is dropped and counted, never guessed.
namespace nimbus::orch {

// The designed ledger's place and shape. These match the main branch's
// food_ledger.h constants, so a migrated file IS the designed ledger when the
// release line meets main's food-ledger work.
inline constexpr const char kLedgerProject[] = "food";
inline constexpr const char kLedgerName[] = "food-log.csv";
inline constexpr const char kLedgerHeader[] = "date,meal,items,kcal,protein_g";

struct LedgerMigration {
  bool ok = false;
  std::string err;
  std::string out;      // the whole new ledger file ("" = nothing to write)
  int folded = 0;       // ledger rows written
  int dropped = 0;      // legacy rows that mapped to nothing
};

// Fold `legacyText` into ledger shape. `existingLedger` is the designed
// ledger's current text when one exists (its rows are kept, and per (day,
// meal) key they count as the LATEST generation, so designed rows are never
// overwritten by stale legacy ones), or "" when none exists yet.
LedgerMigration planLedgerMigration(const std::string& legacyText,
                                    const std::string& existingLedger = std::string());

}  // namespace nimbus::orch
