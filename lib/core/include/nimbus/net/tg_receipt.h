#pragma once
// tg_receipt - read Telegram's answer to a send (sendAudio, sendDocument, ...) into a
// delivery receipt: the message id Telegram gave the message, or why it refused.
//
// A send the device only queued, or whose answer it never read, proves nothing: on
// 22 Oct a voice reply was reported as sent while no upload ever ran. Telegram's
// result.message_id is the one proof the device has that a message reached the chat,
// so the tools that deliver report it and the claim check reads it. Portable and
// host-tested (test_tg_receipt).

#include <cstddef>
#include <cstdint>
#include <string>

namespace nimbus {
namespace net {

struct TgReceipt {
  bool        ok = false;      // Telegram accepted the message and named its id
  int64_t     messageId = 0;   // result.message_id (> 0 when ok)
  int         errorCode = 0;   // Telegram's error_code on a refusal (0 = none given)
  std::string error;           // why there is no receipt (printable, bounded); "" when ok
};

// Read the answer to one Bot API send. `transportOk` is false when the request did not
// complete with a 2xx status; `transportErr` then says why ("connect failed", "HTTP 400").
// Telegram's own description wins over the transport text when the body carries one. A
// 2xx answer without a positive result.message_id is not a receipt.
TgReceipt parseTgSendAnswer(bool transportOk, const std::string& transportErr,
                            const std::string& body);

// The one log line for a media send:
//   "telegram: sendAudio ok (message_id 812, 215433 bytes)"
//   "telegram: sendAudio FAIL (Bad Request: chat not found)"
std::string tgSendLogLine(const char* method, const TgReceipt& r, size_t bytes);

}  // namespace net
}  // namespace nimbus
