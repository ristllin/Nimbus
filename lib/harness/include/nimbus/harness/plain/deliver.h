#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/tx/item.h"

namespace agent {
struct Channel;
}  // namespace agent
namespace nimbus {
namespace orch {
namespace tx {
class TxLog;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/deliver.h - the delivery seam (PRD section 1, step 6, and section 8.9): the model's
// text goes through one portable deliver function (markup sanitiser, Telegram HTML,
// splitting) used by the device and nimbusd; then the d and e items. A harness-written
// message is one of a closed list, at most one after a turn, each separate from the reply.
//
// Seam declared by lane RB-B0a; the bodies (plain_deliver.cpp) are lane RB-E3's.
namespace agent {
namespace plain {

// StatusLine, the closed list of harness-written messages, lives in plain/turn.h (the
// beforeDeliver seam carries one too).

struct DeliverDeps {
  const Channel* channel = nullptr;
  const PlainHooks* hooks = nullptr;
  nimbus::orch::tx::TxLog* log = nullptr;
};

// What one delivery sends. Nothing is copied: the reply and the status line are the
// engine's own strings, read for the length of the call (a 60 KB reply is never held twice
// at the point memory is tightest). A retry after a refused allocation passes `fromPart`,
// the reply parts that already went out: deliverTurn skips them, so nothing is sent twice.
struct DeliverInput {
  const TurnCtx* turn = nullptr;
  const std::string* text = nullptr;   // the model's text, exactly as returned; null or empty: none
  uint16_t fromPart = 0;               // reply parts already sent (skip them)
  StatusLine status = StatusLine::None;
  const std::string* statusText = nullptr;   // the one status line, separate from the reply
};

// Progress, recorded the moment each send succeeds (not when the call returns): when a
// later part throws, the engine reads what already went out and never sends it again.
struct DeliverOutcome {
  bool sent = false;                   // some of the reply went out
  uint16_t parts = 0;                  // reply parts that went out, in order
  bool replyDone = false;              // every part of the reply went out
  bool statusSent = false;             // the status line went out
  std::vector<int64_t> msgIds;
  uint32_t bytes = 0;                  // the delivered bytes (after markup and splitting)
};

Status deliverTurn(const DeliverDeps& deps, const DeliverInput& in, DeliverOutcome& out);
// The e item of a turn.
Status turnEndItem(const TurnCtx& turn, const nimbus::orch::tx::TurnEndFacts& facts, nimbus::orch::tx::Item& out);

}  // namespace plain
}  // namespace agent
