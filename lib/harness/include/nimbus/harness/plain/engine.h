#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/tx/item.h"

namespace agent {
struct Platform;
struct Channel;
struct HarnessConfig;
class HttpTransport;
}  // namespace agent
namespace nimbus {
namespace orch {
namespace tx {
class TxArena;
class TxLog;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/engine.h - the plain engine (PRD section 1): one turn is intake, turn open, a loop
// of render, post, store; guards; the truth seam; deliver; deferred effects; idle work.
// It runs on the existing single task and TLS slot behind the existing contracts
// (Platform, HarnessConfig, HttpTransport, Channel) and the seams of plain/hooks.h.
// runTurn's own frame stays at most 512 B: large locals live in a PSRAM turn state.
//
// Seam declared by lane RB-B0a; the body (plain_engine.cpp) is written only by lanes
// RB-E1a and RB-E1b.
namespace agent {
namespace plain {

// One registry dispatch, with the caller's Principal (TurnCtx::who).
using ToolDispatchFn = std::function<ToolOutput(const ToolCall& call, const TurnCtx& turn)>;

struct EngineDeps {
  const Platform* platform = nullptr;
  const HarnessConfig* config = nullptr;
  HttpTransport* transport = nullptr;
  const Channel* channel = nullptr;
  PlainHooks* hooks = nullptr;
  nimbus::orch::tx::TxLog* log = nullptr;
  nimbus::orch::tx::TxArena* arena = nullptr;
  ToolDispatchFn dispatch;
};

// What a turn starts from: the owner message (already stored as a u item by intake) or
// another trigger's text.
struct TurnInput {
  TurnCtx ctx;
  std::string text;
  std::vector<nimbus::orch::tx::Part> parts;
  std::vector<uint64_t> src;     // Telegram update ids folded into the message
};

struct TurnOutcome {
  bool delivered = false;
  std::string text;              // what was delivered (the model's text)
  uint16_t rounds = 0;
  uint16_t toolCalls = 0;
  uint16_t headRequests = 0;
  GuardTrip trip = GuardTrip::None;
  StatusLine status = StatusLine::None;
  nimbus::orch::tx::TurnEndFacts end;
};

class PlainEngine {
 public:
  explicit PlainEngine(const EngineDeps& deps) : deps_(deps) {}

  // One whole turn, from turn open to the e item.
  Status runTurn(const TurnInput& in, TurnOutcome& out);
  // One unit of idle work through the idleStep seam.
  Status idle(const IdleCtx& ctx, IdleOutcome& out);
  // Once at boot, through the bootResume seam.
  Status boot(const BootCtx& ctx);

  const EngineDeps& deps() const { return deps_; }

 private:
  EngineDeps deps_;
};

}  // namespace plain
}  // namespace agent
