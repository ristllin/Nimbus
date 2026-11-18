// Crash resume (plain/resume.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-X1 writes them.
#include "nimbus/harness/plain/resume.h"

namespace agent {
namespace plain {

Status installResume(PlainHooks&, const ResumeDeps&) { return Status::pending("plain::installResume"); }

}  // namespace plain
}  // namespace agent
