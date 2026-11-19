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

struct DeliverInput {
  const TurnCtx* turn = nullptr;
  std::string text;                    // the model's text, exactly as returned
  StatusLine status = StatusLine::None;
  std::string statusText;              // the one status line, separate from the reply
};

struct DeliverOutcome {
  bool sent = false;
  std::vector<int64_t> msgIds;
  uint32_t bytes = 0;                  // the delivered bytes (after markup and splitting)
};

Status deliverTurn(const DeliverDeps& deps, const DeliverInput& in, DeliverOutcome& out);
// The e item of a turn.
Status turnEndItem(const TurnCtx& turn, const nimbus::orch::tx::TurnEndFacts& facts, nimbus::orch::tx::Item& out);

}  // namespace plain
}  // namespace agent
