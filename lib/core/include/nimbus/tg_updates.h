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
// Largest inbound photo or document the device will download (the download cap and the
// size check before it). Sized so the download, its base64 in PSRAM, and the
// vision-token bill all stay modest.
constexpr uint32_t kPhotoBudget = 512u * 1024;
// The photo rendition a model reads: the one download both the head's image part and
// the vision caption take (the Telegram archive keeps its own largest copy). Telegram
// offers a phone photo at 90, 320, 800 and 1280 px; the 1280 px copy is 140-170 KB, so
// its request body (base64 plus the conversation) is 220-280 KB. Bench unit L, 26 Oct,
// internal heap fragmented to a 7,156-11,252 B largest block (Lumi's state), 17-30 KB
// free: with v4.6.1's pacing the 1280 px bodies went out 10 times of 11 (one cut at
// 98,304 B) and 800 px bodies (86-152 KB) 14 of 14; v4.6.0 paced every 220 KB photo
// request with a 60 s deadline into that deadline (4 of 4 lost). The 800 px copy halves
// the body, the pacing and the vision tokens, and a label stays legible at it (a cheap
// vision call read every value of a nutrition label at 800 px). So the photo path takes
// the LARGEST legible rendition within kPhotoPickBudget, which is the 800 px one (50-80
// KB), and when none fits, the SMALLEST legible one within kPhotoBudget: never a
// thumbnail a label cannot be read from while a legible copy exists. A photo with no
// legible copy keeps the old rule (the largest within kPhotoBudget). The archive's own
// largest copy is then a second download, made by the archive in its own time.
constexpr uint32_t kPhotoPickBudget = 80u * 1024;
constexpr uint32_t kPhotoMinLongSide = 640;   // 800 px copies qualify, 320 px ones do not
static_assert(kPhotoPickBudget < kPhotoBudget, "the pick budget sits under the download cap");
// Legible: the long side reaches kPhotoMinLongSide (Telegram always reports a photo's
// width and height; a rendition without them is not counted as legible).
inline bool photoLegible(uint32_t width, uint32_t height) {
  return (width > height ? width : height) >= kPhotoMinLongSide;
}

struct Attachment {
  // Append new kinds before Count, never reorder: the Telegram archive maps every
  // kind to a decision (nimbus/tg_archive.h contentForKind), and its class test
  // walks every value below Count, so a kind with no decision fails the build's tests.
  enum class Kind : uint8_t {
    None, Document, Photo, Voice, Video, Sticker, Other,
    Audio, VideoNote, Animation,
    Count
  };
  Kind        kind = Kind::None;
  std::string fileId;
  // Telegram's file_unique_id: the same for the same file over time and across bots
  // (it cannot be used to download). The duplicate guard keys a re-sent identical
  // file on it, and the files store receives it with the content hash.
  std::string fileUniqueId;
  std::string fileName;   // documents only ("" otherwise)
  std::string mime;       // "" if absent
  uint32_t    fileSize = 0;  // bytes as reported by Telegram (0 = unknown)
  // Pixel size of the chosen photo rendition (0 when absent or not a photo), for
  // the receive log lines.
  uint32_t    width = 0;
  uint32_t    height = 0;
};

struct Update {
  int32_t     updateId = 0;
  std::string chatId;     // message.chat.id - the auth gate is on THIS, never from.id
  std::string from;       // first_name, else username
  std::string text;       // text OR caption, fully unescaped UTF-8 ("" if neither)
  bool        textIsCaption = false;
  bool        forwarded = false;   // forwarded from someone else (forward_origin, or the
                                   // older forward_from / forward_from_chat / forward_date):
                                   // its words are not the sender's own, so a forwarded
                                   // "/command" never runs as one
  Attachment  attachment;
  int64_t     messageId = 0;    // message.message_id (0 when absent)
  int64_t     date = 0;         // message.date, Telegram server time (edit_date for an edit)
  std::string mediaGroupId;     // message.media_group_id: the items of one album share it
  // An edited_message update. It carries ids and the edit time only (no text, no
  // attachment), so the dispatch path treats it as handled, never as a new message.
  bool        edited = false;
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
