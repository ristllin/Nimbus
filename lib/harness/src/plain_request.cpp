// Request composition (plain/request.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-E2 writes them.
#include "nimbus/harness/plain/request.h"

namespace agent {
namespace plain {

Status composeSystem(const RequestFacts&, SystemBlocks&) { return Status::pending("plain::composeSystem"); }
Status toolManifest(const RequestFacts&, std::vector<ToolSpec>&) { return Status::pending("plain::toolManifest"); }

}  // namespace plain
}  // namespace agent
