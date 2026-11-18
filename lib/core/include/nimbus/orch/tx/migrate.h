#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/status.h"

// tx/migrate.h - conversation migration into /mem/tx and the re-entry rule (PRD section 9,
// "v6 stores" and "Going back and forward again"). Migrations copy, the source stays; a
// dry run reports counts and anomaly classes only, never row content; a second migration
// over an existing /mem/tx is a re-entry (migrated.json); every step is resumable after
// a reset.
//
// Seam declared by lane RB-B0a; the bodies (orch_tx_migrate.cpp) are lane RB-MG1a's.
namespace nimbus {
namespace orch {
namespace tx {

// One day-stream row as the old engine wrote it (the archive both engines read).
struct DayRow {
  uint64_t id = 0;
  uint64_t t = 0;
  bool owner = false;      // owner message, else assistant reply
  std::string text;
};

// The dry-run report: counts and anomaly classes, never content.
struct MigrationReport {
  uint32_t rows = 0;
  uint32_t days = 0;
  uint32_t betweenPointers = 0;   // rows between the old summary's two pointers
  uint32_t anomalies = 0;
  std::vector<std::string> anomalyClasses;
};

// Where a migration is, stored in migrated.json so a reset resumes it.
enum class MigrationStep : uint8_t { NotStarted = 0, Digest0 = 1, Rows = 2, Marked = 3, Done = 4 };
struct MigrationState {
  MigrationStep step = MigrationStep::NotStarted;
  uint64_t rowsDone = 0;
};

Status dryRun(TxFiles& files, const std::string& chatKey, const std::vector<DayRow>& rows,
              MigrationReport& out);
// Run the next step; call until state.step is Done.
Status migrateStep(TxFiles& files, const std::string& chatKey, const std::vector<DayRow>& rows,
                   MigrationState& state);
// True when /mem/tx/<chatKey> already holds a migration: the start is a re-entry.
Status isReentry(TxFiles& files, const std::string& chatKey, bool& out);
// Re-entry: rows the other engine wrote after head.mHi become text items behind one
// device note ('[device] N turns between <t1> and <t2> were answered by the earlier engine').
Status reentryItems(const HeadState& head, const std::vector<DayRow>& rows, std::vector<Item>& out);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
