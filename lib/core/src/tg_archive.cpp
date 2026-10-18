// tg_archive - pure part: names, the raw page splitter, per-update metadata, the
// content policy table and the storage-pressure plan. The engine that writes the
// card is in tg_archive_engine.cpp.
#include "nimbus/tg_archive.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "nimbus/orch/loops.h"   // civilFromDays

namespace nimbus {
namespace tgarc {

using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonPairConst;
using ArduinoJson::JsonVariantConst;
using ArduinoJson::DeserializationOption::NestingLimit;

bool isArchivePath(const std::string& path) {
  const size_t n = std::strlen(kRoot);
  if (path.compare(0, n, kRoot) != 0) return false;
  return path.size() == n || path[n] == '/';
}

// ---- names ---------------------------------------------------------------------

bool validChatDir(const std::string& s) {
  if (s == kRestoredChat) return true;
  size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
  const size_t digits = s.size() - i;
  if (digits < 1 || digits > 20) return false;
  for (; i < s.size(); ++i)
    if (!std::isdigit((unsigned char)s[i])) return false;
  return true;
}

bool isTelegramChat(const std::string& s) { return s != kRestoredChat && validChatDir(s); }

bool localInjectPath(const std::string& fileId, std::string& path) {
  const size_t n = std::strlen(kLocalPrefix);
  if (fileId.compare(0, n, kLocalPrefix) != 0) return false;
  const std::string p = fileId.substr(n);
  if (p.size() < 6 || p.size() > 120 || p.compare(0, 5, "/mem/") != 0) return false;
  if (p.find("..") != std::string::npos) return false;
  for (char c : p)
    if (!(std::isalnum((unsigned char)c) || c == '/' || c == '.' || c == '_' || c == '-')) return false;
  path = p;
  return true;
}

bool validDay(const std::string& s) {
  if (s == "undated") return true;
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
    if (!std::isdigit((unsigned char)s[i])) return false;
  const int mo = (s[5] - '0') * 10 + (s[6] - '0');
  const int d = (s[8] - '0') * 10 + (s[9] - '0');
  return mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
}

bool validItemName(const std::string& s) {
  if (s.empty() || s.size() > 80 || s[0] == '.') return false;
  if (s.find("..") != std::string::npos) return false;
  for (char c : s) {
    const bool ok = std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string dayFromEpoch(int64_t epochSec) {
  // 2015-01-01 .. 2200-01-01: anything outside is a clock that was never set.
  if (epochSec < 1420070400LL || epochSec >= 7258118400LL) return "undated";
  const nimbus::orch::CivilTime c = nimbus::orch::civilFromDays((uint32_t)(epochSec / 86400));
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d", c.year, c.month, c.day);
  return b;
}

std::string chatDirPath(const std::string& chat) { return std::string(kRoot) + "/" + chat; }
std::string dayDirPath(const std::string& chat, const std::string& day) {
  return chatDirPath(chat) + "/" + day;
}
std::string itemPath(const std::string& chat, const std::string& day, const std::string& name) {
  return dayDirPath(chat, day) + "/" + name;
}
std::string indexPathOf(const std::string& chat, const std::string& day) {
  return itemPath(chat, day, "index.jsonl");
}
std::string inBase(int64_t updateId) {
  char b[24];
  std::snprintf(b, sizeof b, "%lld", (long long)updateId);
  return b;
}
std::string outBase(uint32_t seq) {
  char b[16];
  std::snprintf(b, sizeof b, "o%lu", (unsigned long)seq);
  return b;
}

uint32_t nextOutSeq(const std::vector<std::string>& names) {
  uint32_t best = 0;
  for (const std::string& n : names) {
    if (n.size() < 3 || n[0] != 'o' || !std::isdigit((unsigned char)n[1])) continue;
    size_t i = 1;
    uint64_t v = 0;
    while (i < n.size() && std::isdigit((unsigned char)n[i]) && v < 0xFFFFFFFFull)
      v = v * 10 + (uint64_t)(n[i++] - '0');
    if (i < n.size() && n[i] == '.' && v < 0xFFFFFFFFull && v > best) best = (uint32_t)v;
  }
  return best + 1;
}

// ---- content table ---------------------------------------------------------------

namespace {

// One row per Content value, in enum order. A value added to Content without a row
// here fails the build (the static_assert); the class test then checks each row.
const ContentInfo kContentTable[] = {
    {"text", Policy::RawOnly, ""},
    {"photo", Policy::Download, "jpg"},
    {"document", Policy::Download, "bin"},
    {"voice", Policy::Download, "ogg"},
    {"audio", Policy::Download, "mp3"},
    {"video", Policy::Download, "mp4"},
    {"video_note", Policy::Download, "mp4"},
    {"sticker", Policy::Download, "webp"},
    {"animation", Policy::Download, "mp4"},
    {"location", Policy::RawOnly, ""},
    {"contact", Policy::RawOnly, ""},
    {"venue", Policy::RawOnly, ""},
    {"poll", Policy::RawOnly, ""},
    {"dice", Policy::RawOnly, ""},
    {"service", Policy::RawOnly, ""},
    // A bot receives only a reference to a story, and cannot fetch paid media,
    // games or invoices as files: the raw JSON is the whole record.
    {"story", Policy::NotArchived, ""},
    {"paid_media", Policy::NotArchived, ""},
    {"game", Policy::NotArchived, ""},
    {"invoice", Policy::NotArchived, ""},
    {"unknown", Policy::NotArchived, ""},
};
static_assert(sizeof(kContentTable) / sizeof(kContentTable[0]) == (size_t)Content::Count,
              "every Content value needs an archive policy row");

// The parser's kinds, in tg::Attachment::Kind order.
const Content kKindTable[] = {
    Content::Text,      // None: a text or caption-only message
    Content::Document, Content::Photo, Content::Voice, Content::Video, Content::Sticker,
    Content::Unknown,   // Other
    Content::Audio, Content::VideoNote, Content::Animation,
};
static_assert(sizeof(kKindTable) / sizeof(kKindTable[0]) == (size_t)tg::Attachment::Kind::Count,
              "every parser attachment kind needs an archive decision");

}  // namespace

ContentInfo contentInfo(Content c) {
  if ((size_t)c >= (size_t)Content::Count) return {"", Policy::Unknown, ""};
  return kContentTable[(size_t)c];
}

Content contentForKind(tg::Attachment::Kind k) {
  if ((size_t)k >= (size_t)tg::Attachment::Kind::Count) return Content::Count;
  return kKindTable[(size_t)k];
}

// ---- raw page splitter -----------------------------------------------------------

namespace {

const char* skipWs(const char* p, const char* e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  return p;
}

// Past the closing quote of a string starting at p ('"'), or nullptr.
const char* skipString(const char* p, const char* e) {
  for (++p; p < e; ++p) {
    if (*p == '\\') { ++p; continue; }
    if (*p == '"') return p + 1;
  }
  return nullptr;
}

// Past a number, true, false or null starting at p, or nullptr when the body ends.
const char* skipScalar(const char* p, const char* e) {
  while (p < e && *p != ',' && *p != '}' && *p != ']' && !std::isspace((unsigned char)*p)) ++p;
  return p < e ? p : nullptr;
}

// Past the object or array whose opening bracket is at p, or nullptr when cut.
const char* skipContainer(const char* p, const char* e) {
  int depth = 0;
  for (; p < e; ++p) {
    if (*p == '"') {
      p = skipString(p, e);
      if (!p) return nullptr;
      --p;
    } else if (*p == '{' || *p == '[') {
      ++depth;
    } else if ((*p == '}' || *p == ']') && --depth == 0) {
      return p + 1;
    }
  }
  return nullptr;
}

// Past one JSON value starting at p, or nullptr when it is cut or malformed.
const char* skipValue(const char* p, const char* e) {
  if (p >= e) return nullptr;
  if (*p == '"') return skipString(p, e);
  if (*p == '{' || *p == '[') return skipContainer(p, e);
  return skipScalar(p, e);
}

// Split the elements of the array whose '[' is at p. Returns true when it closed.
bool splitArray(const char* base, const char* p, const char* e, std::vector<Span>& out) {
  p = skipWs(p + 1, e);
  if (p < e && *p == ']') return true;
  while (p < e) {
    const char* end = skipValue(p, e);
    if (!end) return false;
    out.push_back({(size_t)(p - base), (size_t)(end - p)});
    p = skipWs(end, e);
    if (p < e && *p == ']') return true;
    if (p >= e || *p != ',') return false;
    p = skipWs(p + 1, e);
  }
  return false;
}

}  // namespace

namespace {

// Reads `"key" :` at p. Sets isResult and returns the position of the value, or
// nullptr when the body is malformed there.
const char* readKey(const char* p, const char* e, bool& isResult) {
  const char* keyEnd = skipString(p, e);
  if (!keyEnd) return nullptr;
  isResult = (keyEnd - p) == 8 && std::strncmp(p, "\"result\"", 8) == 0;
  p = skipWs(keyEnd, e);
  if (p >= e || *p != ':') return nullptr;
  return skipWs(p + 1, e);
}

}  // namespace

bool splitResult(const char* body, size_t len, std::vector<Span>& out) {
  out.clear();
  if (!body || !len) return false;
  const char* e = body + len;
  const char* p = skipWs(body, e);
  if (p >= e || *p != '{') return false;
  p = skipWs(p + 1, e);
  while (p && p < e && *p == '"') {
    bool isResult = false;
    p = readKey(p, e, isResult);
    if (!p) return false;
    if (isResult) return p < e && *p == '[' && splitArray(body, p, e, out);
    p = skipValue(p, e);
    if (!p) return false;
    p = skipWs(p, e);
    if (p < e && *p == ',') p = skipWs(p + 1, e);
  }
  return false;
}

// ---- per-update metadata ---------------------------------------------------------

namespace {

std::string idText(JsonVariantConst v) {
  if (v.is<long long>()) return inBase(v.as<long long>());
  if (v.is<const char*>()) return v.as<const char*>();
  return std::string();
}

std::string str(JsonVariantConst v) {
  return v.is<const char*>() ? std::string(v.as<const char*>()) : std::string();
}

void fillFile(JsonObjectConst o, Media& m) {
  m.fileId = str(o["file_id"]);
  m.fileName = str(o["file_name"]);
  m.mime = str(o["mime_type"]);
  m.size = o["file_size"] | 0ull;
}

const char* stickerExt(JsonVariantConst sticker) {
  if (sticker["is_animated"] | false) return "tgs";
  if (sticker["is_video"] | false) return "webm";
  return "webp";
}

void fillFrom(JsonVariantConst from, InMeta& out) {
  const std::string fn = str(from["first_name"]);
  out.from = fn.empty() ? str(from["username"]) : fn;
  out.fromId = idText(from["id"]);
}

// The largest rendition: most pixels, then most bytes; the last wins a tie, as
// Telegram lists sizes ascending.
void fillLargestPhoto(JsonArrayConst sizes, Media& m) {
  uint64_t bestPx = 0, bestBytes = 0;
  bool any = false;
  for (JsonObjectConst ph : sizes) {
    const uint64_t px = (uint64_t)(ph["width"] | 0u) * (uint64_t)(ph["height"] | 0u);
    const uint64_t by = ph["file_size"] | 0ull;
    if (!any || px > bestPx || (px == bestPx && by >= bestBytes)) {
      fillFile(ph, m);
      bestPx = px;
      bestBytes = by;
      any = true;
    }
  }
}

// Message content keys in detection order: a venue also carries a location and an
// animation also carries a document, so the more specific key comes first.
struct KeyContent { const char* key; Content c; };
const KeyContent kContentKeys[] = {
    {"animation", Content::Animation}, {"photo", Content::Photo},
    {"video", Content::Video},         {"video_note", Content::VideoNote},
    {"voice", Content::Voice},         {"audio", Content::Audio},
    {"sticker", Content::Sticker},     {"document", Content::Document},
    {"venue", Content::Venue},         {"location", Content::Location},
    {"contact", Content::Contact},     {"poll", Content::Poll},
    {"dice", Content::Dice},           {"story", Content::Story},
    {"paid_media", Content::PaidMedia}, {"game", Content::Game},
    {"invoice", Content::Invoice},     {"text", Content::Text},
};

const char* const kServiceKeys[] = {
    "new_chat_members", "left_chat_member", "new_chat_title", "new_chat_photo",
    "delete_chat_photo", "group_chat_created", "supergroup_chat_created",
    "migrate_to_chat_id", "migrate_from_chat_id", "pinned_message",
    "message_auto_delete_timer_changed", "forum_topic_created", "forum_topic_edited",
    "forum_topic_closed", "forum_topic_reopened", "write_access_allowed",
    "users_shared", "chat_shared", "video_chat_started", "video_chat_ended",
};

void detectContent(JsonObjectConst msg, InMeta& out) {
  for (const KeyContent& kc : kContentKeys) {
    JsonVariantConst v = msg[kc.key];
    if (v.isNull()) continue;
    out.content = kc.c;
    if (contentInfo(kc.c).policy != Policy::Download) return;
    out.media.type = kc.c;
    if (kc.c == Content::Photo) fillLargestPhoto(v.as<JsonArrayConst>(), out.media);
    else fillFile(v.as<JsonObjectConst>(), out.media);
    if (kc.c == Content::Sticker) out.media.ext = stickerExt(v);
    out.hasMedia = !out.media.fileId.empty();
    return;
  }
  if (msg["caption"].is<const char*>()) { out.content = Content::Text; return; }
  for (const char* k : kServiceKeys)
    if (!msg[k].isNull()) { out.content = Content::Service; return; }
  out.content = Content::Unknown;
}

void fillMessage(JsonObjectConst msg, InMeta& out) {
  out.chat = idText(msg["chat"]["id"]);
  fillFrom(msg["from"], out);
  out.messageId = msg["message_id"] | 0ll;
  const long long date = msg["date"] | 0ll;
  const long long edit = msg["edit_date"] | 0ll;
  out.date = (out.edited && edit > 0) ? edit : date;
  out.mediaGroupId = idText(msg["media_group_id"]);
  detectContent(msg, out);
}

const char* const kMessageUpdates[] = {
    "message", "edited_message", "channel_post", "edited_channel_post",
    "business_message", "edited_business_message",
};

// An update with no message object (callback_query, my_chat_member, ...): keep
// its type, and its chat and date when they sit where Telegram usually puts them.
void fillOther(JsonObjectConst u, InMeta& out) {
  for (JsonPairConst kv : u) {
    if (std::strcmp(kv.key().c_str(), "update_id") == 0) continue;
    out.updateType = kv.key().c_str();
    JsonObjectConst o = kv.value().as<JsonObjectConst>();
    out.chat = idText(o["chat"]["id"]);
    if (out.chat.empty()) out.chat = idText(o["message"]["chat"]["id"]);
    out.date = o["date"] | (o["message"]["date"] | 0ll);
    fillFrom(o["from"], out);
    return;
  }
}

}  // namespace

bool parseUpdateMeta(const char* raw, size_t len, InMeta& out) {
  out = InMeta();
  if (!raw || !len) return false;
  JsonDocument doc = jsonAllocator() ? JsonDocument(jsonAllocator()) : JsonDocument();
  if (deserializeJson(doc, raw, len, NestingLimit(32))) return false;
  JsonObjectConst u = doc.as<JsonObjectConst>();
  if (u.isNull()) return false;
  out.updateId = u["update_id"] | 0ll;
  if (out.updateId <= 0) return false;
  for (const char* k : kMessageUpdates) {
    JsonObjectConst msg = u[k];
    if (msg.isNull()) continue;
    out.updateType = k;
    out.edited = std::strncmp(k, "edited_", 7) == 0;
    fillMessage(msg, out);
    return true;
  }
  fillOther(u, out);
  return true;
}

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// The extension of a file name when it is short and plain ("" otherwise).
std::string nameExt(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return std::string();
  const std::string ext = lower(name.substr(dot + 1));
  if (ext.size() > 8) return std::string();
  for (char c : ext)
    if (!std::isalnum((unsigned char)c)) return std::string();
  return ext;
}

const char* mimeExt(const std::string& mime) {
  static const char* const kMap[][2] = {
      {"image/jpeg", "jpg"}, {"image/png", "png"},        {"image/gif", "gif"},
      {"image/webp", "webp"}, {"application/pdf", "pdf"}, {"audio/ogg", "ogg"},
      {"audio/mpeg", "mp3"},  {"audio/mp4", "m4a"},       {"video/mp4", "mp4"},
      {"video/webm", "webm"}, {"video/quicktime", "mov"}, {"text/plain", "txt"},
      {"application/zip", "zip"}, {"application/x-tgsticker", "tgs"},
  };
  for (const auto& kv : kMap)
    if (mime == kv[0]) return kv[1];
  return nullptr;
}

}  // namespace

std::string mediaExt(const Media& m) {
  std::string ext;
  if (m.type == Content::Photo) ext = "jpg";
  else if (!m.ext.empty()) ext = m.ext;
  if (ext.empty()) ext = nameExt(m.fileName);
  if (ext.empty()) {
    const char* me = mimeExt(lower(m.mime));
    if (me) ext = me;
  }
  if (ext.empty()) ext = contentInfo(m.type).defExt;
  if (ext.empty()) ext = "bin";
  // Never shadow the archive's own records (<id>.json, index.jsonl, *.part, *.tmp).
  if (ext == "json" || ext == "jsonl" || ext == "part" || ext == "tmp") ext += ".bin";
  return ext;
}

// ---- storage pressure ------------------------------------------------------------

// 5% of the card, at least 512 MB on a card big enough to spare it (a tenth of a
// small card otherwise), so a small card is not always "low".
uint64_t lowWater(uint64_t totalBytes) {
  const uint64_t floor = std::min<uint64_t>(512ull * 1024 * 1024, totalBytes / 10);
  return std::max(floor, totalBytes / 20);
}

uint64_t targetFree(uint64_t totalBytes) {
  const uint64_t low = lowWater(totalBytes);
  return low + low / 2;
}

namespace {

// Removal order: stranger days first, then allowlisted days; within each, oldest
// first ("undated" as oldest), ties broken by chat so the order is total.
std::vector<size_t> removalOrder(const std::vector<DayUsage>& days) {
  auto key = [&](size_t i) { return days[i].day == "undated" ? std::string() : days[i].day; };
  std::vector<size_t> order(days.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (days[a].stranger != days[b].stranger) return days[a].stranger;
    const std::string ka = key(a), kb = key(b);
    return ka != kb ? ka < kb : days[a].chat < days[b].chat;
  });
  return order;
}

// Each allowlisted chat's newest dated day: never removed.
std::vector<std::pair<std::string, std::string>> newestOwnerDays(const std::vector<DayUsage>& days) {
  std::vector<std::pair<std::string, std::string>> newest;   // (chat, day)
  for (const DayUsage& d : days) {
    if (d.stranger || d.day == "undated") continue;
    auto it = std::find_if(newest.begin(), newest.end(),
                           [&](const std::pair<std::string, std::string>& e) { return e.first == d.chat; });
    if (it == newest.end()) newest.emplace_back(d.chat, d.day);
    else if (d.day > it->second) it->second = d.day;
  }
  return newest;
}

bool isKept(const std::vector<std::pair<std::string, std::string>>& keep, const DayUsage& d) {
  if (d.stranger) return false;
  for (const auto& e : keep)
    if (e.first == d.chat && e.second == d.day) return true;
  return false;
}

}  // namespace

PressurePlan planPressure(const std::vector<DayUsage>& days, uint64_t freeBytes,
                          uint64_t totalBytes) {
  PressurePlan plan;
  if (freeBytes >= lowWater(totalBytes) || days.empty()) return plan;
  plan.triggered = true;
  const auto keep = newestOwnerDays(days);
  uint64_t have = freeBytes;
  const uint64_t target = targetFree(totalBytes);
  for (size_t i : removalOrder(days)) {
    if (have >= target) break;
    if (isKept(keep, days[i])) continue;   // each allowlisted chat keeps its newest day
    plan.remove.push_back(i);
    plan.freed += days[i].bytes;
    have += days[i].bytes;
  }
  return plan;
}

}  // namespace tgarc
}  // namespace nimbus
