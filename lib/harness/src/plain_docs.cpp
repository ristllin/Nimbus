// Readable documents (plain/docs.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-P2 writes them.
#include "nimbus/harness/plain/docs.h"

namespace agent {
namespace plain {

Status documentRoute(const TurnCtx&, const std::string&, const std::string&, DocRoute&) {
  return Status::pending("plain::documentRoute");
}

}  // namespace plain
}  // namespace agent
