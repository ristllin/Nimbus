// The wire of each dialect (plain/wire.h), lane RB-B0a. Router design section 2 C13 (lane E2-R4a):
// each codec lane changes its own one line below, and no two of those lines are next to each other:
// the Responses codec (R7) its case line, the Chat codec (R6) the pending line under its case (Flat
// maps there).
#include "nimbus/harness/plain/wire.h"

namespace agent {
namespace plain {

// Each codec's file defines its own; the two not yet built are declared here so their lanes change
// only their case line.
const WireCodec& responsesWire();   // providers/plain_wire_openai.cpp (lane R7)
const WireCodec& anthropicWire();   // providers/plain_wire_anthropic.cpp (render RB-W1a, parse RB-W1b)
const WireCodec& chatWire();        // providers/plain_wire_chat.cpp (lane R6)

namespace {
Status found(const WireCodec& w, const WireCodec*& out) {
  out = &w;
  return Status::success();
}
}  // namespace

Status wireFor(Dialect dialect, const WireCodec*& out) {
  out = nullptr;
  switch (dialect) {
    case Dialect::OpenAIResponses: return Status::pending("plain::wireFor responses");   // lane R7
    case Dialect::AnthropicMessages: return found(anthropicWire(), out);
    case Dialect::Flat:   // the Chat codec with the row's tools bit off (router design section 2 C10)
    case Dialect::ChatCompletions:   // lane R6
      return Status::pending(dialect == Dialect::Flat ? "plain::wireFor flat" : "plain::wireFor chat");
  }
  return Status::fail(Err::InvalidArgument, "plain::wireFor");
}

Status renderWireFor(Dialect dialect, const WireCodec*& out) {
  return wireFor(dialect, out);   // every landed render belongs to a complete wire
}

}  // namespace plain
}  // namespace agent
