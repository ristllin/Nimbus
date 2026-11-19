// The wire of each dialect (plain/wire.h), lane RB-B0a. Each wire lane (RB-W1a,
// RB-W2, RB-W3, RB-W4) replaces its own case with its wire and touches no other case.
#include "nimbus/harness/plain/wire.h"

namespace agent {
namespace plain {

const WireCodec& anthropicWire();   // providers/plain_wire_anthropic.cpp (render RB-W1a, parse RB-W1b)

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

Status renderWireFor(Dialect dialect, const WireCodec*& out) {
  if (dialect == Dialect::AnthropicMessages) {   // render landed (RB-W1a); wireFor waits for the parse (RB-W1b)
    out = &anthropicWire();
    return Status::success();
  }
  return wireFor(dialect, out);
}

}  // namespace plain
}  // namespace agent
