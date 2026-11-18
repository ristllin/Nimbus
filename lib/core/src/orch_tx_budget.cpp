// Profiles and per-head budgets (tx/budget.h). Stub bodies from lane RB-B0a: each
// answers Err::NotImplemented until lane RB-T4 generates the table.
#include "nimbus/orch/tx/budget.h"

namespace nimbus {
namespace orch {
namespace tx {

Status profileMarks(Profile, ProfileMarks&) { return Status::pending("tx::profileMarks"); }
Status budgetFor(const HeadKey&, Profile, HeadBudget&) { return Status::pending("tx::budgetFor"); }

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
