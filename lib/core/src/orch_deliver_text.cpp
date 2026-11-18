#include "nimbus/orch/deliver_text.h"

#include "nimbus/orch/reply_markup.h"
#include "nimbus/tg_html.h"

namespace nimbus {
namespace orch {
namespace {

bool continuationByte(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

// Where the part that starts at `at` ends, given that it may not pass `end` (exclusive): after
// the last paragraph break, else the last line break, else the last space in the back half of
// the window; else at `end` itself.
size_t cutBefore(const std::string& text, size_t at, size_t end) {
  if (end - at < 2) return end;
  const size_t floor = at + (end - at) / 2;
  const size_t para = text.rfind("\n\n", end - 2);
  if (para != std::string::npos && para >= floor) return para + 2;
  for (const char c : {'\n', ' '}) {
    const size_t p = text.rfind(c, end - 1);
    if (p != std::string::npos && p >= floor) return p + 1;
  }
  return end;
}

}  // namespace

std::string deliverableText(const std::string& modelText) { return cleanReplyMarkup(modelText); }

std::vector<std::string> splitDeliverable(const std::string& text, size_t maxBytes) {
  if (maxBytes < 4) maxBytes = 4;
  std::vector<std::string> parts;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.size();
    if (end - at > maxBytes) {
      end = at + maxBytes;
      // Never inside a UTF-8 character: a character is at most 4 bytes, so at most 3
      // continuation bytes are stepped back over (invalid text is cut where it stands).
      for (int k = 0; k < 3 && continuationByte(text[end]); k++) end--;
      if (continuationByte(text[end])) end = at + maxBytes;
      end = cutBefore(text, at, end);
    }
    // A run of blanks long enough to be a part of its own is not a message (Telegram refuses
    // an empty text); everything else is kept byte for byte.
    std::string part = text.substr(at, end - at);
    if (!blank(part)) parts.push_back(std::move(part));
    at = end;
  }
  return parts;
}

TelegramPart telegramPart(const std::string& part) { return TelegramPart{tgHtml(part), part}; }

std::vector<TelegramPart> deliverText(const std::string& modelText, size_t maxBytes) {
  std::vector<TelegramPart> out;
  for (const std::string& part : splitDeliverable(deliverableText(modelText), maxBytes))
    out.push_back(telegramPart(part));
  return out;
}

}  // namespace orch
}  // namespace nimbus
