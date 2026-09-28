#pragma once
#include <ArduinoJson.h>

#include <cstdlib>
#include <cstring>
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
//   POST /api/telegram/remove    id         off the allowlist (its role goes too)
//   POST /api/telegram/rename    id, name   display name only
//   POST /api/telegram/role      id, owner  owner=1 -> admin, else user
//   POST /api/telegram/public    on         open access: refused on a hosted instance
//   GET  /api/tenant             {tenants:[{id,role,vectors,bytes,ttl,pins}], admins}
//   POST /api/tenant             id + remove=1 | role and/or vectors/bytes/ttl/pins
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
    static constexpr char kSub[] = "/api/telegram/";
    if (base == "/api/tenant") {
      out = method == "GET" ? tenantsGet() : method == "POST" ? tenantPost(form) : notAllowed();
      return true;
    }
    if (base == "/api/telegram") {
      out = method == "GET" ? telegramGet(hasToken) : notAllowed();
      return true;
    }
    if (base.rfind(kSub, 0) != 0) return false;
    out = method == "POST" ? telegramPost(base.substr(sizeof(kSub) - 1), form) : notAllowed();
    return true;
  }

 private:
  static TgWebResp ok() { return TgWebResp{200, R"({"ok":true})"}; }
  static TgWebResp notAllowed() { return TgWebResp{405, R"({"error":"method not allowed"})"}; }
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
    bool done = false;
    if (op == "add" || op == "approve") done = acc_->approve(id, form("name"), err);
    else if (op == "remove") done = acc_->remove(id, err);
    else if (op == "rename") done = acc_->rename(id, form("name"), err);
    else if (op == "role")   // the device's legacy owner flag, mapped onto the real role
      done = acc_->setRole(id, form("owner") == "1" ? TelegramAccess::Role::Admin
                                                    : TelegramAccess::Role::User, err);
    else return error(404, "not found");
    return done ? ok() : error(err == TelegramAccess::kErrSave ? 500 : 400, err);
  }

  // Open access is a device option (DANGER: anyone who finds the bot uses it). A
  // hosted instance runs on the owner's keys and reaches the owner's connectors, so
  // it is refused here; turning it OFF is always fine.
  static TgWebResp publicPost(const Form& form) {
    if (std::strtol(form("on").c_str(), nullptr, 10) == 0) return TgWebResp{200, R"({"public":false})"};
    return error(400, "Open access isn't available on a hosted instance. Approve each person instead.");
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

  // A limit is "" (unchanged) or a whole number in [0, cap] (0 restores the role
  // default). Anything else refuses the whole write: a wrapped negative would read
  // back as "never expires", the most permissive value (device webui.cpp).
  static bool parseLimit(const std::string& v, uint32_t cap, uint32_t& out, bool& any) {
    if (v.empty()) return true;
    char* end = nullptr;
    const long long n = std::strtoll(v.c_str(), &end, 10);
    if (*end != 0 || n < 0 || n > (long long)cap) return false;
    out = (uint32_t)n;
    any = true;
    return true;
  }

  // Device /api/tenant POST: remove=1, else a role and/or limits. Every field is
  // validated BEFORE anything is written, so a refused request changes nothing.
  TgWebResp tenantPost(const Form& form) {
    const std::string id = trimmed(form("id"));
    if (id.empty()) return error(400, "id required");
    if (!TelegramAccess::isChatId(id)) return error(400, TelegramAccess::kErrBadId);
    std::string err;
    if (form("remove") == "1") {
      acc_->removeTenant(id, err);
      return error(err == TelegramAccess::kErrNoTenant ? 404 : 409, err);
    }
    nimbus::orch::Role role = nimbus::orch::Role::Unknown;
    const std::string roleS = form("role");
    if (!roleS.empty() && !nimbus::orch::roleFromName(roleS, role)) return error(400, "bad role");
    nimbus::orch::Quota q;
    acc_->quotaOf(id, q);   // start from what is set today
    uint32_t pins = q.maxPins;
    bool any = false;
    const bool good = parseLimit(form("vectors"), 2147483647u, q.maxVectors, any) &&
                      parseLimit(form("bytes"), 2147483647u, q.maxBytes, any) &&
                      parseLimit(form("ttl"), 2147483647u, q.maxTtlHours, any) &&
                      parseLimit(form("pins"), 65535u, pins, any);
    if (!good) return error(400, "Limits must be 0 or more (0 restores the default for their role).");
    if (!roleS.empty() && !acc_->setRole(id, role, err)) return error(409, err);
    q.maxPins = (uint16_t)pins;
    if (any && !acc_->setQuota(id, q, err)) return error(err == TelegramAccess::kErrNoTenant ? 404 : 409, err);
    return ok();
  }

  TelegramAccess* acc_;
};

}  // namespace nimbusd
