// The plain engine (plain/engine.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-E1a writes them.
#include "nimbus/harness/plain/engine.h"

namespace agent {
namespace plain {

Status PlainEngine::runTurn(const TurnInput&, TurnOutcome&) { return Status::pending("plain::PlainEngine::runTurn"); }
Status PlainEngine::idle(const IdleCtx&, IdleOutcome&) { return Status::pending("plain::PlainEngine::idle"); }
Status PlainEngine::boot(const BootCtx&) { return Status::pending("plain::PlainEngine::boot"); }

}  // namespace plain
}  // namespace agent
