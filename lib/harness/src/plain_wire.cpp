// The wire of each dialect (plain/wire.h), lane RB-B0a. Each wire lane (RB-W1a,
// RB-W2, RB-W3, RB-W4) replaces its own case with its wire and touches no other case.
#include "nimbus/harness/plain/wire.h"

namespace agent {
namespace plain {

Status wireFor(Dialect dialect, const WireCodec*& out) {
  out = nullptr;
  switch (dialect) {
    case Dialect::AnthropicMessages: return Status::pending("plain::wireFor anthropic");
    case Dialect::OpenAIResponses: return Status::pending("plain::wireFor responses");
    case Dialect::ChatCompletions: return Status::pending("plain::wireFor chat");
    case Dialect::Flat: return Status::pending("plain::wireFor flat");
  }
  return Status::fail(Err::InvalidArgument, "plain::wireFor");
}

}  // namespace plain
}  // namespace agent
