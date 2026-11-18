// Photos on the transcript (plain/media.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-P1 writes them.
#include "nimbus/harness/plain/media.h"

namespace agent {
namespace plain {

Status photoAllowed(const TurnCtx&, const MediaDeps&, bool&) { return Status::pending("plain::photoAllowed"); }
Status installMedia(PlainHooks&, const MediaDeps&) { return Status::pending("plain::installMedia"); }

}  // namespace plain
}  // namespace agent
