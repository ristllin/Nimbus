#include "nimbus/orch/deliver_text.h"

#include "nimbus/orch/reply_markup.h"
#include "nimbus/tg_html.h"

namespace nimbus {
namespace orch {

const char kTelegramOverflowNote[] = "\n\xE2\x80\xA6(truncated - ask for the rest, or have it sent as a file)";

namespace {

bool continuationByte(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

// Where each ``` that opens a line sits: tgHtml renders a block between two such lines as
// code, so a cut between them would leave markdown inside code (or code read as markdown).
std::vector<size_t> fenceLines(const std::string& text) {
  std::vector<size_t> at;
  for (size_t p = text.find("```"); p != std::string::npos; p = text.find("```", p + 3))
    if (p == 0 || text[p - 1] == '\n') at.push_back(p);
  return at;
}

// True when a cut at `p` falls inside a code block (after an odd number of fence lines).
bool insideFence(const std::vector<size_t>& fences, size_t p) {
  size_t n = 0;
  for (size_t f : fences) n += f < p ? 1 : 0;
  return n % 2 == 1;
}

// The last cut after `sep` in [floor, end] (`sep` ends at or before `end`) that is outside a
// code block, or npos.
size_t lastCutAfter(const std::string& text, const std::string& sep, size_t floor, size_t end,
                    const std::vector<size_t>& fences) {
  if (end < floor + sep.size()) return std::string::npos;
  for (size_t p = text.rfind(sep, end - sep.size()); p != std::string::npos && p >= floor;
       p = p == 0 ? std::string::npos : text.rfind(sep, p - 1)) {
    if (!insideFence(fences, p + sep.size())) return p + sep.size();
  }
  return std::string::npos;
}

// Where the part that starts at `at` ends, given that it may not pass `end` (exclusive): after
// the last paragraph break, else the last line break, else the last space in the back half of
// the window, outside a code block; else just before the code block that `end` falls in, when
// it opened inside this part; else at `end` itself (a code block longer than a part).
size_t cutBefore(const std::string& text, size_t at, size_t end, const std::vector<size_t>& fences) {
  if (end - at < 2) return end;
  const size_t floor = at + (end - at) / 2;
  for (const char* sep : {"\n\n", "\n", " "}) {
    const size_t cut = lastCutAfter(text, sep, floor, end, fences);
    if (cut != std::string::npos) return cut;
  }
  if (insideFence(fences, end)) {
    size_t open = std::string::npos;
    for (size_t f : fences)
      if (f < end) open = f;
    if (open != std::string::npos && open > at) return open;
  }
  return end;
}

}  // namespace

std::string deliverableText(const std::string& modelText) { return cleanReplyMarkup(modelText); }

std::vector<std::string> splitDeliverable(const std::string& text, size_t maxBytes) {
  if (maxBytes < 4) maxBytes = 4;
  std::vector<std::string> parts;
  if (text.size() <= maxBytes) {
    if (!blank(text)) parts.push_back(text);
    return parts;
  }
  const std::vector<size_t> fences = fenceLines(text);
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.size();
    if (end - at > maxBytes) {
      end = at + maxBytes;
      // Never inside a UTF-8 character: a character is at most 4 bytes, so at most 3
      // continuation bytes are stepped back over (invalid text is cut where it stands).
      for (int k = 0; k < 3 && continuationByte(text[end]); k++) end--;
      if (continuationByte(text[end])) end = at + maxBytes;
      end = cutBefore(text, at, end, fences);
    }
    // A run of blanks long enough to be a part of its own is not a message (Telegram refuses
    // an empty text); everything else is kept byte for byte.
    std::string part = text.substr(at, end - at);
    if (!blank(part)) parts.push_back(std::move(part));
    at = end;
  }
  return parts;
}

std::vector<std::string> telegramParts(const std::string& modelText, size_t maxBytes, size_t maxParts) {
  std::vector<std::string> parts = splitDeliverable(deliverableText(modelText), maxBytes);
  if (maxParts > 0 && parts.size() > maxParts) {
    parts.resize(maxParts);
    parts.back() += kTelegramOverflowNote;
  }
  return parts;
}

TelegramPart telegramPart(const std::string& part) { return TelegramPart{tgHtml(part), part}; }

std::vector<TelegramPart> deliverText(const std::string& modelText, size_t maxBytes, size_t maxParts) {
  std::vector<TelegramPart> out;
  for (const std::string& part : telegramParts(modelText, maxBytes, maxParts)) out.push_back(telegramPart(part));
  return out;
}

}  // namespace orch
}  // namespace nimbus
