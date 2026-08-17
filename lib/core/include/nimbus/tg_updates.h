#pragma once
// Portable Telegram getUpdates parser. Replaces the hand-rolled flat jsonStr +
// 2 KB scan-window that (a) truncated any message past 255 chars, (b) mangled
// JSON escapes (\n became the letter 'n', \uXXXX became "uXXXX"), and (c) could
// not see fields of a large update. This uses ArduinoJson's FILTERED parse, so
// it decodes escapes correctly (incl. surrogate pairs -> UTF-8) and only
// allocates the handful of fields it keeps, regardless of body size.
//
// Portable + host-tested (test_tg_updates) with canned bodies. The device feeds
// the whole getUpdates body (in a PSRAM buffer) and gets back structured updates
// in wire order; the chat-id auth gate and per-message dispatch stay in the
// device layer.
//
// Arduino-free at the interface (std::string / std::vector); the .cpp includes
// ArduinoJson, which is already a lib/core dependency.

#include <cstdint>
#include <string>
#include <vector>

namespace nimbus {
namespace tg {

// At most one media attachment per message (Telegram never mixes them). Metadata
// only - the download is the device's job (and, for documents/photos, the files
// lane). fileId is the getFile handle.
// Largest photo rendition the device will fetch for a description. Sized so the
// download, its base64 in PSRAM, and the vision-token bill all stay modest; a
// ~500 KB JPEG is already far more detail than a one-paragraph description uses.
constexpr uint32_t kPhotoBudget = 512u * 1024;

struct Attachment {
  enum class Kind : uint8_t { None, Document, Photo, Voice, Video, Sticker, Other };
  Kind        kind = Kind::None;
  std::string fileId;
  std::string fileName;   // documents only ("" otherwise)
  std::string mime;       // "" if absent
  uint32_t    fileSize = 0;  // bytes as reported by Telegram (0 = unknown)
};

struct Update {
  int32_t     updateId = 0;
  std::string chatId;     // message.chat.id - the auth gate is on THIS, never from.id
  std::string from;       // first_name, else username
  std::string text;       // text OR caption, fully unescaped UTF-8 ("" if neither)
  bool        textIsCaption = false;
  Attachment  attachment;
};

// Parse a getUpdates response body. Returns true when the JSON parsed cleanly.
// okFlag is Telegram's top-level "ok". truncatedTail is set when the body ended
// mid-JSON (incomplete input) - the caller must NOT ack past the last WHOLE
// update it received (re-poll with limit=1 at the same offset). Updates come out
// in wire order.
bool parseUpdates(const char* body, size_t len, std::vector<Update>& out,
                  bool& okFlag, bool& truncatedTail);

}  // namespace tg
}  // namespace nimbus
