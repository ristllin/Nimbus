// Conversation migration and re-entry (tx/migrate.h). Stub bodies from lane RB-B0a:
// each answers Err::NotImplemented until lane RB-MG1a writes them.
#include "nimbus/orch/tx/migrate.h"

namespace nimbus {
namespace orch {
namespace tx {

Status dryRun(TxFiles&, const std::string&, const std::vector<DayRow>&, MigrationReport&) {
  return Status::pending("tx::dryRun");
}
Status migrateStep(TxFiles&, const std::string&, const std::vector<DayRow>&, MigrationState&) {
  return Status::pending("tx::migrateStep");
}
Status isReentry(TxFiles&, const std::string&, bool&) { return Status::pending("tx::isReentry"); }
Status reentryItems(const HeadState&, const std::vector<DayRow>&, std::vector<Item>&) {
  return Status::pending("tx::reentryItems");
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
