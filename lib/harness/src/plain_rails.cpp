// The safety rails (plain/rails.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-RL writes them.
#include "nimbus/harness/plain/rails.h"

namespace agent {
namespace plain {

const char* railVerdictSlug(RailVerdict) { return "allow"; }
const RailRow* taintExemptions(size_t& count) {
  count = 0;
  return nullptr;
}
bool itemOutside(const nimbus::orch::tx::Item&) { return false; }
bool inputOutside(Trigger, const std::vector<nimbus::orch::tx::Part>&) { return false; }
bool readsOutsidePath(const std::string&, const std::string&) { return false; }
bool turnTainted(const TurnCtx&) { return false; }
Status railFor(const TurnCtx&, const ToolCall&, bool, RailVerdict&, size_t*) { return Status::pending("plain::railFor"); }
Status installRails(PlainHooks&, const RailsDeps&) { return Status::pending("plain::installRails"); }

}  // namespace plain
}  // namespace agent
