#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/tx/status.h"

// tx_battery/deliver_oracle.h - the bytes a reply must arrive as (lane RB-B0b): the chunks
// the one portable deliver function (lib/core/src/orch_deliver_text.cpp, lane RB-Q1c:
// cleanReplyMarkup, then Telegram HTML and splitting) makes of the model's text. The
// battery compares what the channel received against it, so the device's bytes are the
// host's bytes and nothing is appended or rewritten on the way.
//
// PLUMBING: lane RB-E3 replaces the body with a call into that function. Until then it
// answers NotImplemented with its own name, which is the documented reason a
// test_delivery_bytes case fails when its ignore is removed early.
namespace txb {

inline nimbus::orch::tx::Status expectedChunks(const std::string& modelText, std::vector<std::string>& out) {
  (void)modelText;
  out.clear();
  return nimbus::orch::tx::Status::pending("txb::expectedChunks (wire to orch_deliver_text in RB-E3)");
}

}  // namespace txb
