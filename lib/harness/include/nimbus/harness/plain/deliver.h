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

// The complete list of harness-written messages (PRD section 8.9). Slugs are the
// exemption names of the delivered-bytes test: frozen. Append only.
enum class StatusLine : uint8_t {
  None = 0,
  UpdateNotice = 1,
  VoiceHeard = 2,
  LimitReached = 3,
  CutAnswer = 4,
  ProviderError = 5,
  CrashNotice = 6,
  PausedSpend = 7,
  FailedWrite = 8,
  SummarizerTrouble = 9,
  StorageState = 10,
  Stopped = 11,
  ModerationBlock = 12,
};
inline constexpr StatusLine kAllStatusLines[] = {
    StatusLine::UpdateNotice,  StatusLine::VoiceHeard,   StatusLine::LimitReached,      StatusLine::CutAnswer,
    StatusLine::ProviderError, StatusLine::CrashNotice,  StatusLine::PausedSpend,       StatusLine::FailedWrite,
    StatusLine::SummarizerTrouble, StatusLine::StorageState, StatusLine::Stopped,       StatusLine::ModerationBlock,
};
inline const char* statusLineSlug(StatusLine s) {
  switch (s) {
    case StatusLine::None: return "none";
    case StatusLine::UpdateNotice: return "update-notice";
    case StatusLine::VoiceHeard: return "voice-heard";
    case StatusLine::LimitReached: return "limit-reached";
    case StatusLine::CutAnswer: return "cut-answer";
    case StatusLine::ProviderError: return "provider-error";
    case StatusLine::CrashNotice: return "crash-notice";
    case StatusLine::PausedSpend: return "paused-spend";
    case StatusLine::FailedWrite: return "failed-write";
    case StatusLine::SummarizerTrouble: return "summarizer-trouble";
    case StatusLine::StorageState: return "storage-state";
    case StatusLine::Stopped: return "stopped";
    case StatusLine::ModerationBlock: return "moderation-block";
  }
  return "none";
}

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
