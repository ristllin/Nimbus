// The delivery seam (plain/deliver.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-E3 writes them.
#include "nimbus/harness/plain/deliver.h"

namespace agent {
namespace plain {

Status deliverTurn(const DeliverDeps&, const DeliverInput&, DeliverOutcome&) {
  return Status::pending("plain::deliverTurn");
}
Status turnEndItem(const TurnCtx&, const nimbus::orch::tx::TurnEndFacts&, nimbus::orch::tx::Item&) {
  return Status::pending("plain::turnEndItem");
}

}  // namespace plain
}  // namespace agent
