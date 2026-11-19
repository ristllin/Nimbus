#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/deliver_text.h"
#include "nimbus/orch/tx/status.h"

// tx_battery/deliver_oracle.h - the bytes a reply must arrive as (lane RB-B0b): the chunks
// the one portable deliver function (lib/core/src/orch_deliver_text.cpp, lane RB-Q1c:
// cleanReplyMarkup, then Telegram HTML and splitting) makes of the model's text. The
// battery compares what the channel received against it, so the device's bytes are the
// host's bytes and nothing is appended or rewritten on the way.
//
// Wired by lane RB-E3: the parts nimbus::orch::telegramParts makes, the function the device's
// Telegram send (src/agent/telegram.cpp) and nimbusd call. Each part reaches the channel as
// it is; the Telegram binding turns it into HTML with telegramPart.
namespace txb {

inline nimbus::orch::tx::Status expectedChunks(const std::string& modelText, std::vector<std::string>& out) {
  out = nimbus::orch::telegramParts(modelText);
  return nimbus::orch::tx::Status::success();
}

}  // namespace txb
