// The truth check binding (plain/truth.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-G1 writes them.
#include "nimbus/harness/plain/truth.h"

namespace agent {
namespace plain {

Status installTruth(PlainHooks&, const TruthDeps&) { return Status::pending("plain::installTruth"); }

}  // namespace plain
}  // namespace agent
