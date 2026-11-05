#pragma once
#include <cstddef>
#include <string>

#include "nimbus/orch/file_store.h"

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
//     <date, meal, item, grams, kcal, protein>, <ts, item, kcal, protein>,
//     with an item holding a comma in double quotes or inside parentheses;
//   - a quantity cell (grams, amount) has to start with a number: an item split
//     at a comma it did not quote shifts the columns after it, and the row drops
//     instead of reading grams as kcal;
//   - a row with no meal word takes its meal from the time of day (before
//     11:00 breakfast, before 15:30 lunch, before 20:00 dinner, else evening);
//   - rows group by (day, meal); a GENERATION is the rows of one timestamp -
//     one append's worth. The LAST generation wins outright (the owner's final
//     figures - the intermediate generations, a sub-agent's invented figures
//     and phantom estimates all drop);
//   - within the winning generation, rows naming the same item are generations
//     of that item: the LAST wins (three appends of one drink collapse to one);
//   - the ledger row joins the surviving items with "; " (never a comma - the
//     ledger is comma-separated) and sums their kcal and protein. An item that
//     keeps a comma of its own is written so that every ledger reader sees one
//     field (in quotes, unless parentheses that close already shield it).
// A row that maps to nothing is dropped and counted, never guessed, and a source
// with a dropped data row is kept (sourceFullyFolded).
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
  int droppedData = 0;  // of those, the rows that are not a header: figures the ledger lacks

  // The source file may go only when this holds: a dropped data row has no other copy.
  bool sourceFullyFolded() const { return ok && folded > 0 && droppedData == 0; }
};

// A source kept past its fold says so in a last row. The ledger existing is no such sign: a ledger
// and a legacy file side by side is what the fold is for. Folding a kept source at every boot would
// bring back a row the ledger has since removed or moved ("Boba??? Thats yesterday.").
inline constexpr const char kLegacyFoldedMark[] = "#folded into food/food-log.csv; the rows above that could not be read are not in it";
bool legacyFoldedAlready(const std::string& legacyText);
std::string markLegacyFolded(const std::string& legacyText);

// Only a legacy file the device owner controls folds, into the owner's ledger. A member
// can hold a file of that name in their own namespace (the name is not reserved); it
// stays theirs and stays where it is: the fold must not move it into the owner's prompt.
bool legacyLogFoldable(const FileStore& store, const FileEntry& legacy);

// Fold `legacyText` into ledger shape. `existingLedger` is the designed ledger's current text when
// one exists: it stays byte for byte and wins its (day, meal) keys, and only legacy rows of other
// keys are added after it. A ledger in other columns takes nothing (folded 0). "" when none exists.
// A source marked as folded takes no further part (folded 0).
LedgerMigration planLedgerMigration(const std::string& legacyText,
                                    const std::string& existingLedger = std::string());

// The fold replaces the ledger and deletes the legacy file, so it runs only on text read whole.
// The sizes are the files' on the card; a read that came back shorter, or a file over the read cap,
// is ok=false with nothing to write, and both files stay as they are.
inline constexpr size_t kLedgerFoldCap = 256 * 1024;
LedgerMigration planLedgerFold(const std::string& legacyText, size_t legacyOnDisk, bool ledgerStored,
                               const std::string& ledgerText, size_t ledgerOnDisk);

}  // namespace nimbus::orch
