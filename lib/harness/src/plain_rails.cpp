// The safety rails (plain/rails.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-RL writes them.
#include "nimbus/harness/plain/rails.h"

namespace agent {
namespace plain {

Status railFor(const TurnCtx&, const ToolCall&, RailVerdict&) { return Status::pending("plain::railFor"); }
Status installRails(PlainHooks&, const RailsDeps&) { return Status::pending("plain::installRails"); }

}  // namespace plain
}  // namespace agent
