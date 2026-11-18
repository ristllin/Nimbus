// Guards and suppression (plain/guards.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-E1b writes them.
#include "nimbus/harness/plain/guards.h"

namespace agent {
namespace plain {

Status guardLimitsFor(int, const HeadCost&, GuardLimits&) { return Status::pending("plain::guardLimitsFor"); }
Status checkGuards(const GuardLimits&, const GuardState&, GuardTrip&) { return Status::pending("plain::checkGuards"); }
Status isPureTool(const std::string&, bool&) { return Status::pending("plain::isPureTool"); }
Status installSuppression(PlainHooks&) { return Status::pending("plain::installSuppression"); }

}  // namespace plain
}  // namespace agent
