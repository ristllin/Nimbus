// Compaction (plain/compact.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-C2 writes them.
#include "nimbus/harness/plain/compact.h"

namespace agent {
namespace plain {

Status compactSlice(const CompactDeps&, const IdleCtx&, CompactOutcome&) {
  return Status::pending("plain::compactSlice");
}
Status installCompaction(PlainHooks&, const CompactDeps&) { return Status::pending("plain::installCompaction"); }

}  // namespace plain
}  // namespace agent
