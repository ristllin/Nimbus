#pragma once
#include <cstddef>
#include <string>
#include <vector>

// deliver_text - the one portable function between the model's text and a chat (lane RB-Q1c,
// PRD section 1 step 6 and section 8 item 9). What the owner gets is exactly the model's text
// made safe to display: nothing is removed for what it says, nothing is appended.
//
//   1. the markup sanitiser (cleanReplyMarkup, reply_markup.h): reasoning, tool-call markup and
//      wrapper tags never reach a channel; unknown <word> tokens and code stay byte-exact;
//   2. the split: Telegram takes at most 4,096 characters in one message, so a longer text
//      goes out as consecutive parts of at most kTelegramPartBytes bytes each, cut at a
//      paragraph, then a line, then a space, outside a ``` code block where it can be, and
//      never inside a UTF-8 character. The parts are the text byte for byte: nothing is added
//      to them and nothing is lost between them. One reply is at most kTelegramMaxParts parts
//      (about 32 KB); a longer one ends with kTelegramOverflowNote on its last part, never a
//      silent cut;
//   3. Telegram HTML (tgHtml, tg_html.h) for each part, with the part itself as the plain
//      text that goes out when Telegram refuses the HTML (formatting may degrade: a marker
//      may show as a character or a character as formatting; no character is dropped).
//
// The device (src/agent/telegram.cpp: send() and sendNow take telegramParts, the reply-queue
// drain sends a part with telegramPart) and the hosted daemon (nimbusd/src/telegram.h
// sendMessage, deliverText) both call these functions, so a host test sees the bytes the
// device sends. Portable, Arduino-free, host-tested
// (test_delivery_e1).
//
// Replaces the reply guard's sentence deletion at the send seam (dropEchoSentences,
// dropOwnerMechanics), deleted in v5.2.0 with orch_reply_guard.cpp: the owner's month showed
// it cut or bent correct replies (rebuild/evidence/DIAGNOSIS.md RC-09).
namespace nimbus {
namespace orch {

// The most bytes one part carries. Under Telegram's 4,096-character limit with room to spare,
// and it fits the device's reply-queue slot (4,096 B + NUL) whole.
constexpr size_t kTelegramPartBytes = 4000;

// The most parts one reply goes out as (the device's reply queue holds 16), and the line the
// last of them ends with when the text needed more (a device line: the rest was not sent).
constexpr size_t kTelegramMaxParts = 8;
extern const char kTelegramOverflowNote[];

// One Telegram message: the HTML body sent with parse_mode=HTML, and the same part as plain
// text for the resend when Telegram refuses the HTML.
struct TelegramPart {
  std::string html;
  std::string plain;
};

// Step 1: the model's text made safe to display (the markup sanitiser). Idempotent.
std::string deliverableText(const std::string& modelText);

// Step 2: `text` (already deliverable) cut into parts of at most `maxBytes` bytes. Their
// concatenation is `text` byte for byte. An empty text is no part. `maxBytes` below 4 is
// taken as 4 (one UTF-8 character always fits).
std::vector<std::string> splitDeliverable(const std::string& text, size_t maxBytes = kTelegramPartBytes);

// Steps 1 and 2 with the reply bound: deliverableText, splitDeliverable, then at most
// `maxParts` parts, the last one ending with kTelegramOverflowNote when parts were left out.
std::vector<std::string> telegramParts(const std::string& modelText, size_t maxBytes = kTelegramPartBytes,
                                       size_t maxParts = kTelegramMaxParts);

// Step 3: one part as Telegram receives it. The part is sent as it is: it is not sanitised
// again (a part of a sanitised text could otherwise lose a word the whole text kept, such as
// a literal "<reply>" that now opens the part).
TelegramPart telegramPart(const std::string& part);

// The whole pipeline: telegramParts, then telegramPart on each part.
std::vector<TelegramPart> deliverText(const std::string& modelText, size_t maxBytes = kTelegramPartBytes,
                                      size_t maxParts = kTelegramMaxParts);

}  // namespace orch
}  // namespace nimbus
