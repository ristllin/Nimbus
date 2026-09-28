#pragma once
#include <ArduinoJson.h>

#include <cstdlib>
#include <functional>
#include <string>

#include "nimbus/orch/rbac.h"
#include "tg_access.h"

// tg_web - the owner's Telegram access + people surface on a Virtual Nimbus: the
// SAME routes and body shapes the device serves (src/net/webui.cpp), so the web
// app's existing Telegram panel (Assistant, Connectors, Telegram: pending
// approvals, allowed chips, add by chat ID, the role chip) drives the hosted
// instance unchanged (CUM-459). Every route is token-gated at the http layer.
//
//   GET  /api/telegram           {public, hasToken, allow:[{id,name,owner}], pending:[...]}
//   POST /api/telegram/add       id, name   approve by chat ID
//   POST /api/telegram/approve   id, name   approve a pending sender
//   POST /api/telegram/deny      id         drop from the approval queue
//   POST /api/telegram/remove    id         off the allowlist (+ its RBAC row)
//   POST /api/telegram/rename    id, name   display name only
//   POST /api/telegram/role      id, owner  legacy owner flag
//   POST /api/telegram/public    on         open access: refused on a hosted instance
//   GET  /api/tenant             {tenants:[{id,role,vectors,bytes,ttl,pins}], admins}
//   POST /api/tenant             id + remove=1 | role | vectors/bytes/ttl/pins
namespace nimbusd {

struct TgWebResp {
  int status = 200;
  std::string body;
};

class TelegramWeb {
 public:
  // A posted form field by name ("" when absent).
  using Form = std::function<std::string(const std::string&)>;

  explicit TelegramWeb(TelegramAccess* access) : acc_(access) {}

  // Handle one Telegram / people route. False when `base` is not one of them.
  bool handle(const std::string& method, const std::string& base, const Form& form,
              bool hasToken, TgWebResp& out) {
    if (base == "/api/tenant") {
      out = method == "GET" ? tenantsGet() : tenantPost(form);
      return true;
    }
    if (base == "/api/telegram" && method == "GET") {
      out = telegramGet(hasToken);
      return true;
    }
    if (base.rfind("/api/telegram/", 0) != 0) return false;
    out = method == "POST" ? telegramPost(base.substr(14), form)
                           : TgWebResp{405, R"({"error":"use POST"})"};
    return true;
  }

 private:
  static TgWebResp ok() { return TgWebResp{200, R"({"ok":true})"}; }
  static TgWebResp error(int status, const std::string& msg) {
    JsonDocument d;
    d["error"] = msg;
    std::string s;
    serializeJson(d, s);
    return TgWebResp{status, s};
  }
  static std::string trimmed(const std::string& v) {
    const size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    return v.substr(b, v.find_last_not_of(" \t\r\n") - b + 1);
  }

  TgWebResp telegramGet(bool hasToken) const {
    JsonDocument d;
    d["public"] = false;   // no open access on a hosted instance
    d["hasToken"] = hasToken;
    JsonArray allow = d["allow"].to<JsonArray>();
    for (const auto& m : acc_->members()) {
      JsonObject o = allow.add<JsonObject>();
      o["id"] = m.id;
      o["name"] = m.name;
      o["owner"] = m.owner;
    }
    JsonArray pend = d["pending"].to<JsonArray>();
    for (const auto& p : acc_->pending()) {
      JsonObject o = pend.add<JsonObject>();
      o["chatId"] = p.chatId;
      o["name"] = p.name;
      o["preview"] = p.preview;
    }
    std::string s;
    serializeJson(d, s);
    return TgWebResp{200, s};
  }

  TgWebResp telegramPost(const std::string& op, const Form& form) {
    if (op == "public") return publicPost(form);
    const std::string id = trimmed(form("id"));
    if (op == "deny") {
      acc_->deny(id);
      return ok();
    }
    if (id.empty()) return error(400, "id required");
    std::string err;
    if (op == "add" || op == "approve") {
      if (!acc_->approve(id, form("name"), err)) return error(400, err);
      return ok();
    }
    if (op == "remove") return removePost(id);
    if (op == "rename") {
      if (!acc_->rename(id, form("name"), err)) return error(400, err);
      return ok();
    }
    if (op == "role") {
      if (!acc_->setOwner(id, form("owner") == "1", err)) return error(409, err);
      return ok();
    }
    return error(404, "not found");
  }

  // Open access is a device option (DANGER: anyone who finds the bot uses it). A
  // hosted instance runs on the owner's keys and reaches the owner's connectors, so
  // it is refused here; turning it OFF is always fine.
  static TgWebResp publicPost(const Form& form) {
    if (std::atoi(form("on").c_str()) == 0) return TgWebResp{200, R"({"public":false})"};
    return error(400, "Open access isn't available on a hosted instance. Approve each person instead.");
  }

  TgWebResp removePost(const std::string& id) {
    std::string note, err;
    if (!acc_->remove(id, note, err)) return error(500, err);
    if (note.empty()) return ok();
    JsonDocument d;
    d["ok"] = true;
    d["note"] = note;
    std::string s;
    serializeJson(d, s);
    return TgWebResp{200, s};
  }

  TgWebResp tenantsGet() const {
    JsonDocument d;
    JsonArray arr = d["tenants"].to<JsonArray>();
    size_t admins = 0;
    for (const auto& t : acc_->tenants(&admins)) {
      const auto q = nimbus::orch::effectiveQuota(t.role, t.quota);
      JsonObject o = arr.add<JsonObject>();
      o["id"] = t.chatId;
      o["role"] = nimbus::orch::roleName(t.role);
      o["vectors"] = q.maxVectors;
      o["bytes"] = q.maxBytes;
      o["ttl"] = q.maxTtlHours;
      o["pins"] = q.maxPins;
    }
    d["admins"] = (unsigned)admins;
    std::string s;
    serializeJson(d, s);
    return TgWebResp{200, s};
  }

  // Device /api/tenant POST: remove=1, else a role and/or limits.
  TgWebResp tenantPost(const Form& form) {
    const std::string id = trimmed(form("id"));
    if (id.empty()) return error(400, "id required");
    std::string err;
    if (form("remove") == "1") {
      if (acc_->removeTenant(id, err)) return ok();
      return error(err == "no such tenant" ? 404 : 409, err);
    }
    const std::string roleS = form("role");
    if (!roleS.empty()) {
      nimbus::orch::Role role;
      if (!nimbus::orch::roleFromName(roleS, role)) return error(400, "bad role");
      if (!acc_->setRole(id, role, err)) return error(409, err);
    }
    return quotaPost(id, form);
  }

  // A limit is "" (unchanged) or a whole number >= 0 (0 restores the role default).
  // Anything else refuses the whole write: a wrapped negative would read back as
  // "never expires", the most permissive value (device webui.cpp).
  static bool parseLimit(const std::string& v, uint32_t cap, uint32_t& out, bool& set) {
    set = false;
    if (v.empty()) return true;
    char* end = nullptr;
    const long long n = std::strtoll(v.c_str(), &end, 10);
    if (!end || *end != 0 || n < 0 || n > (long long)cap) return false;
    out = (uint32_t)n;
    set = true;
    return true;
  }

  TgWebResp quotaPost(const std::string& id, const Form& form) {
    nimbus::orch::Quota q;
    acc_->quotaOf(id, q);   // start from what is set today
    uint32_t pins = q.maxPins;
    bool sv = false, sb = false, st = false, sp = false;
    const bool good = parseLimit(form("vectors"), 2147483647u, q.maxVectors, sv) &&
                      parseLimit(form("bytes"), 2147483647u, q.maxBytes, sb) &&
                      parseLimit(form("ttl"), 2147483647u, q.maxTtlHours, st) &&
                      parseLimit(form("pins"), 65535u, pins, sp);
    if (!good) return error(400, "Limits must be 0 or more (0 restores the default for their role).");
    if (!(sv || sb || st || sp)) return ok();
    q.maxPins = (uint16_t)pins;
    std::string err;
    if (!acc_->setQuota(id, q, err)) return error(404, err);
    return ok();
  }

  TelegramAccess* acc_;
};

}  // namespace nimbusd
