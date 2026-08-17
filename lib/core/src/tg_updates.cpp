#include "nimbus/tg_updates.h"

#include <ArduinoJson.h>

#include <cstdio>

namespace nimbus {
namespace tg {

using ArduinoJson::DeserializationError;
using ArduinoJson::DeserializationOption::Filter;
using ArduinoJson::DeserializationOption::NestingLimit;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObject;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;

namespace {

// Telegram ids are int64 (chat ids go negative for groups/channels). Render
// exactly, never via a lossy float.
std::string idToStr(JsonVariantConst v) {
  if (v.is<long long>()) {
    char b[24];
    snprintf(b, sizeof(b), "%lld", v.as<long long>());
    return std::string(b);
  }
  if (v.is<const char*>()) return std::string(v.as<const char*>());
  return std::string();
}

}  // namespace

bool parseUpdates(const char* body, size_t len, std::vector<Update>& out,
                  bool& okFlag, bool& truncatedTail) {
  out.clear();
  okFlag = false;
  truncatedTail = false;
  if (!body || len == 0) return false;

  // Filter: keep only the fields we act on. Filtered parse allocates for the KEPT
  // subset only, so a big body stays cheap. One array-element filter (result[0])
  // applies to every result entry.
  JsonDocument filter;
  filter["ok"] = true;
  JsonObject r = filter["result"][0].to<JsonObject>();
  r["update_id"] = true;
  JsonObject m = r["message"].to<JsonObject>();
  m["text"] = true;
  m["caption"] = true;
  m["chat"]["id"] = true;
  m["from"]["first_name"] = true;
  m["from"]["username"] = true;
  m["voice"]["file_id"] = true;
  m["voice"]["file_size"] = true;
  JsonObject md = m["document"].to<JsonObject>();
  md["file_id"] = true;
  md["file_name"] = true;
  md["mime_type"] = true;
  md["file_size"] = true;
  JsonObject mp = m["photo"][0].to<JsonObject>();
  mp["file_id"] = true;
  mp["file_size"] = true;
  m["video"]["file_id"] = true;
  m["video"]["file_size"] = true;
  m["sticker"]["file_id"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, body, len, Filter(filter), NestingLimit(12));
  if (err) {
    // A body cut mid-JSON (oversized batch) - signal the caller to re-poll narrow.
    if (err == DeserializationError::IncompleteInput) truncatedTail = true;
    return false;
  }
  okFlag = doc["ok"] | false;
  if (!okFlag) return true;   // parsed fine, but Telegram reported not-ok

  for (JsonObjectConst u : doc["result"].as<JsonArrayConst>()) {
    Update up;
    up.updateId = u["update_id"] | 0;
    if (up.updateId <= 0) continue;
    JsonObjectConst msg = u["message"];
    // Non-message updates (edited_message, channel_post, callback_query, ...) have
    // no "message"; keep the update (so the caller can still advance the offset)
    // but with empty content.
    if (!msg.isNull()) {
      up.chatId = idToStr(msg["chat"]["id"]);
      const char* fn = msg["from"]["first_name"] | "";
      const char* un = msg["from"]["username"] | "";
      up.from = fn[0] ? std::string(fn) : std::string(un);
      if (msg["text"].is<const char*>()) {
        up.text = msg["text"].as<const char*>();
      } else if (msg["caption"].is<const char*>()) {
        up.text = msg["caption"].as<const char*>();
        up.textIsCaption = true;
      }
      // At most one attachment; documents/photos are the files-lane handoff.
      Attachment& a = up.attachment;
      if (msg["voice"]["file_id"].is<const char*>()) {
        a.kind = Attachment::Kind::Voice;
        a.fileId = msg["voice"]["file_id"].as<const char*>();
        a.fileSize = msg["voice"]["file_size"] | 0u;
      } else if (msg["document"]["file_id"].is<const char*>()) {
        a.kind = Attachment::Kind::Document;
        a.fileId = msg["document"]["file_id"].as<const char*>();
        a.fileName = msg["document"]["file_name"] | "";
        a.mime = msg["document"]["mime_type"] | "";
        a.fileSize = msg["document"]["file_size"] | 0u;
      } else if (!msg["photo"].isNull()) {
        // Telegram offers the same photo at several resolutions. Pick the LARGEST
        // that fits kPhotoBudget, not the largest outright: the device downloads
        // it over one TLS socket, base64s it into PSRAM, and pays for it in vision
        // tokens, and a full-resolution phone photo is several megabytes of all
        // three for a description a mid-size rendition answers just as well.
        // If every rendition is over budget (or none reports a size), take the
        // smallest - something legible beats refusing the photo.
        // file_size is OPTIONAL in the Bot API. When it is missing everywhere,
        // there is nothing to compare, so take the LAST entry: Telegram lists
        // renditions ascending, and the last is the full-size one the old
        // largest-wins code chose. (Falling back to "smallest" here pinned every
        // sizeless photo to the ~90px thumbnail - legible to nobody.)
        JsonVariantConst best, smallest, last;
        uint32_t bestSz = 0, smallestSz = 0;
        bool anySize = false;
        for (JsonVariantConst ph : msg["photo"].as<JsonArrayConst>()) {
          const uint32_t sz = ph["file_size"] | 0u;
          last = ph;
          if (sz) {
            anySize = true;
            if (smallest.isNull() || sz < smallestSz) { smallest = ph; smallestSz = sz; }
            if (sz <= kPhotoBudget && sz >= bestSz) { best = ph; bestSz = sz; }
          }
        }
        // Every rendition over budget: take the smallest - something legible
        // beats refusing the photo.
        if (best.isNull() && anySize) { best = smallest; bestSz = smallestSz; }
        if (best.isNull()) { best = last; bestSz = 0; }
        if (!best.isNull() && best["file_id"].is<const char*>()) {
          a.kind = Attachment::Kind::Photo;
          a.fileId = best["file_id"].as<const char*>();
          a.fileSize = bestSz;
        }
      } else if (msg["video"]["file_id"].is<const char*>()) {
        a.kind = Attachment::Kind::Video;
        a.fileId = msg["video"]["file_id"].as<const char*>();
        a.fileSize = msg["video"]["file_size"] | 0u;
      } else if (msg["sticker"]["file_id"].is<const char*>()) {
        a.kind = Attachment::Kind::Sticker;
        a.fileId = msg["sticker"]["file_id"].as<const char*>();
      }
    }
    out.push_back(std::move(up));
  }
  return true;
}

}  // namespace tg
}  // namespace nimbus
