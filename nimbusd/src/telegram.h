#pragma once
#include <string>
#include <vector>

#include "daemon_config.h"
#include "nimbus/harness/http.h"
#include "nimbus/telegram_offset.h"
#include "nimbus/tg_updates.h"
#include "posix_fs.h"

// telegram - the daemon's Telegram channel (the persona lives here, same as the
// physical device). Long-poll getUpdates + sendMessage over the injected
// HttpTransport, using the SAME portable parser (nimbus::tg::parseUpdates) and
// offset arithmetic (nimbus::core::nextTelegramOffset) the device runs, so the
// wire handling is byte-identical. The ESP32 task/TLS discipline drops out; the
// long-poll loop, the getMe validation, the send path, and the chat-id auth gate
// stay.
//
// The transport is injected, so the whole channel is host-tested against a fake
// Telegram server (no network) - see tests/test_telegram.cpp.
//
// Offset durability: the confirmed offset is persisted (atomic write) after each
// batch, so a restart never re-delivers an already-handled update (which would
// re-run the turn and double-answer). This is the channel's restart-safety.
namespace nimbusd {

class TelegramChannel {
 public:
  // `allowChatId` empty = accept any chat (single-tenant instance, owner-only by
  // construction upstream); non-empty = only that chat's messages are dispatched
  // (the auth gate is on message.chat.id, never from.id).
  TelegramChannel(std::string token, agent::HttpTransport* http,
                  std::string offsetPath, std::string allowChatId = "")
      : token_(std::move(token)), http_(http),
        offsetPath_(std::move(offsetPath)), allowChatId_(std::move(allowChatId)) {
    std::string s;
    if (fsutil::readFile(offsetPath_, s)) offset_ = (int32_t)std::atoi(s.c_str());
  }

  // Validate the bot token via getMe. On success fills `botUsername` and returns
  // true; on failure fills `err`. The provisioning-time check the plan requires.
  bool getMe(std::string& botUsername, std::string& err) {
    agent::HttpResponse resp;
    if (!call("getMe", "", resp, err)) return false;
    if (resp.status != 200) {
      err = "getMe HTTP " + std::to_string(resp.status);
      return false;
    }
    if (resp.body.find("\"ok\":true") == std::string::npos) {
      err = "getMe not ok";
      return false;
    }
    // Minimal, dependency-light extraction of result.username.
    botUsername = jsonStr(resp.body, "username");
    return true;
  }

  // One long-poll cycle: getUpdates from the current offset, parse, and return
  // the accepted (allow-listed) updates in wire order. Advances + persists the
  // offset past the last WHOLE update received. `err` is set on a transport or
  // parse failure (returns false); an empty poll is a successful empty vector.
  bool poll(int timeoutS, std::vector<nimbus::tg::Update>& out, std::string& err) {
    out.clear();
    const std::string q = "offset=" + std::to_string(offset_) +
                          "&timeout=" + std::to_string(timeoutS) + "&limit=20";
    agent::HttpResponse resp;
    // Long-poll needs a client timeout longer than the server hold.
    if (!call("getUpdates?" + q, "", resp, err, (uint32_t)(timeoutS + 15) * 1000)) return false;
    if (resp.status != 200) { err = "getUpdates HTTP " + std::to_string(resp.status); return false; }

    std::vector<nimbus::tg::Update> ups;
    bool ok = false, truncated = false;
    if (!nimbus::tg::parseUpdates(resp.body.c_str(), resp.body.size(), ups, ok, truncated)) {
      err = "getUpdates parse failed";
      return false;
    }
    int32_t maxWhole = offset_ - 1;
    for (const auto& u : ups) {
      // Never ack past a truncated tail's last whole update.
      maxWhole = u.updateId;
      if (allowChatId_.empty() || u.chatId == allowChatId_) out.push_back(u);
    }
    if (!ups.empty() && !truncated) {
      offset_ = nimbus::core::nextTelegramOffset(offset_, maxWhole);
      persistOffset();
    }
    return true;
  }

  // Send a plain-text message to a chat. Returns false + err on failure.
  bool sendMessage(const std::string& chatId, const std::string& text, std::string& err) {
    const std::string body = std::string("{\"chat_id\":") + jsonNum(chatId) +
                             ",\"text\":" + jsonQuote(text) + "}";
    agent::HttpResponse resp;
    if (!call("sendMessage", body, resp, err)) return false;
    if (resp.status != 200) { err = "sendMessage HTTP " + std::to_string(resp.status); return false; }
    return true;
  }

  int32_t offset() const { return offset_; }

 private:
  bool call(const std::string& method, const std::string& jsonBody,
            agent::HttpResponse& resp, std::string& err, uint32_t timeoutMs = 30000) {
    if (!http_) { err = "no transport"; return false; }
    agent::HttpRequest req;
    req.method = jsonBody.empty() ? "GET" : "POST";
    req.host = "api.telegram.org";
    req.path = "/bot" + token_ + "/" + method;
    req.timeoutMs = timeoutMs;
    if (!jsonBody.empty()) {
      req.headers.push_back({"Content-Type", "application/json"});
      req.body = jsonBody;
    }
    return http_->exec(req, resp, err);
  }

  void persistOffset() { fsutil::writeFileAtomic(offsetPath_, std::to_string(offset_)); }

  // Telegram chat ids are numeric; pass through if it looks numeric, else quote.
  static std::string jsonNum(const std::string& s) {
    bool num = !s.empty();
    for (size_t i = 0; i < s.size(); i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (i == 0 && s[i] == '-'))) num = false;
    return num ? s : jsonQuote(s);
  }
  static std::string jsonQuote(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
      switch (ch) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
          if ((unsigned char)ch < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", ch); o += b; }
          else o += ch;
      }
    }
    o += "\"";
    return o;
  }
  // Very small "find \"key\":\"value\"" extractor (getMe username only).
  static std::string jsonStr(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return std::string();
    p += needle.size();
    size_t e = body.find('"', p);
    return e == std::string::npos ? std::string() : body.substr(p, e - p);
  }

  std::string token_;
  agent::HttpTransport* http_;
  std::string offsetPath_;
  std::string allowChatId_;
  int32_t offset_ = 0;
};

}  // namespace nimbusd
