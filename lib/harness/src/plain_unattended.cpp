// Routine runs and held delivery (plain/unattended.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-U1a writes them.
#include "nimbus/harness/plain/unattended.h"

namespace agent {
namespace plain {

Status installUnattended(PlainHooks&, const UnattendedDeps&) { return Status::pending("plain::installUnattended"); }
Status routineChatKey(const std::string&, std::string&) { return Status::pending("plain::routineChatKey"); }

}  // namespace plain
}  // namespace agent
