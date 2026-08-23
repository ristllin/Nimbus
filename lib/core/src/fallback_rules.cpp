#include "nimbus/orch/fallback_rules.h"

#include <cstring>

namespace nimbus {
namespace orch {

namespace {
struct ClassTok {
  ErrorClass e;
  const char* t;
};
const ClassTok kClasses[] = {
    {ErrorClass::RateLimited, "rate_limited"}, {ErrorClass::Timeout, "timeout"},
    {ErrorClass::ServerError, "server_error"}, {ErrorClass::Network, "network"},
    {ErrorClass::Auth, "auth"},                {ErrorClass::BadRequest, "bad_request"},
    {ErrorClass::NotFound, "not_found"},       {ErrorClass::Unsupported, "unsupported"},
    {ErrorClass::Unpriced, "unpriced"},        {ErrorClass::Disabled, "disabled"},
};

// any-of match, with model entries allowing a trailing-'*' prefix glob. An empty
// predicate list matches anything.
bool anyOf(const std::vector<std::string>& pred, const std::string& val, bool glob) {
  if (pred.empty()) return true;
  for (const std::string& p : pred) {
    if (glob && !p.empty() && p.back() == '*') {
      if (val.rfind(p.substr(0, p.size() - 1), 0) == 0) return true;
    } else if (p == val) {
      return true;
    }
  }
  return false;
}

void readStrArray(JsonVariantConst v, std::vector<std::string>& out) {
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) {
      const char* s = e | "";
      if (s && *s) out.emplace_back(s);
    }
  } else if (v.is<const char*>()) {  // tolerate a bare string for a single-value predicate
    const char* s = v | "";
    if (s && *s) out.emplace_back(s);
  }
}

const char* jsonBodyStart(const std::string& body) {
  size_t p = body.find("\r\n\r\n");
  if (p != std::string::npos) return body.c_str() + p + 4;
  p = body.find("\n\n");
  if (p != std::string::npos) return body.c_str() + p + 2;
  return body.c_str();
}
}  // namespace

const char* errorClassToken(ErrorClass e) {
  for (const ClassTok& c : kClasses)
    if (c.e == e) return c.t;
  return "";
}
ErrorClass errorClassFromToken(const std::string& t) {
  for (const ClassTok& c : kClasses)
    if (t == c.t) return c.e;
  return ErrorClass::None;
}

ErrorClass errorClassFromFabric(int fabricErr) {
  // nimbus::harness FabricErr order: Ok=0, Network, Auth, RateLimited, BadRequest,
  // NotFound, Unsupported, Timeout, RemoteFail, ParseFail.
  switch (fabricErr) {
    case 1: return ErrorClass::Network;
    case 2: return ErrorClass::Auth;
    case 3: return ErrorClass::RateLimited;
    case 4: return ErrorClass::BadRequest;
    case 5: return ErrorClass::NotFound;
    case 6: return ErrorClass::Unsupported;
    case 7: return ErrorClass::Timeout;
    case 8: return ErrorClass::ServerError;  // remote_fail -> server_error
    case 9: return ErrorClass::None;         // parse_fail -> hard never-fallback
    default: return ErrorClass::None;
  }
}

std::string sizeClassWord(char sizeClass) {
  switch (sizeClass) {
    case 'S': return "small";
    case 'M': return "medium";
    case 'L': return "large";
    default: return "";
  }
}

size_t parseFallbackRules(const std::string& body, FallbackRuleSet& out,
                          ArduinoJson::Allocator* alloc) {
  JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
  if (deserializeJson(doc, jsonBodyStart(body), DeserializationOption::NestingLimit(12))) return 0;
  out.rules.clear();
  out.version = doc["version"] | 1;
  for (JsonObjectConst r : doc["rules"].as<JsonArrayConst>()) {
    FallbackRule rule;
    rule.id = (const char*)(r["id"] | "");
    rule.description = (const char*)(r["description"] | "");
    rule.enabled = r["enabled"] | true;
    JsonObjectConst m = r["match"];
    readStrArray(m["provider"], rule.provider);
    readStrArray(m["model"], rule.model);
    readStrArray(m["sizeClass"], rule.sizeClass);
    readStrArray(m["capability"], rule.capability);
    readStrArray(m["errorClass"], rule.errorClass);
    for (JsonObjectConst t : r["to"].as<JsonArrayConst>()) {
      FallbackTarget tgt;
      tgt.provider = (const char*)(t["provider"] | "");
      tgt.model = (const char*)(t["model"] | "");
      if (!tgt.provider.empty()) rule.to.push_back(std::move(tgt));
    }
    if (!rule.to.empty()) out.rules.push_back(std::move(rule));
  }
  return out.rules.size();
}

void fallbackRulesToJson(const FallbackRuleSet& rs, JsonObject out) {
  out["version"] = rs.version;
  JsonArray rules = out["rules"].to<JsonArray>();
  for (const FallbackRule& r : rs.rules) {
    JsonObject ro = rules.add<JsonObject>();
    ro["id"] = r.id;
    if (!r.description.empty()) ro["description"] = r.description;
    ro["enabled"] = r.enabled;
    JsonObject m = ro["match"].to<JsonObject>();
    auto emit = [&](const char* k, const std::vector<std::string>& v) {
      if (v.empty()) return;
      JsonArray a = m[k].to<JsonArray>();
      for (const std::string& s : v) a.add(s);
    };
    emit("provider", r.provider);
    emit("model", r.model);
    emit("sizeClass", r.sizeClass);
    emit("capability", r.capability);
    emit("errorClass", r.errorClass);
    JsonArray to = ro["to"].to<JsonArray>();
    for (const FallbackTarget& t : r.to) {
      JsonObject tj = to.add<JsonObject>();
      tj["provider"] = t.provider;
      if (!t.model.empty()) tj["model"] = t.model;
    }
  }
}

bool ruleMatches(const FallbackRule& rule, const TurnContext& ctx) {
  if (!rule.enabled) return false;
  if (!anyOf(rule.provider, ctx.provider, false)) return false;
  if (!anyOf(rule.model, ctx.model, true)) return false;
  if (!anyOf(rule.sizeClass, ctx.sizeClass, false)) return false;
  if (!anyOf(rule.capability, ctx.capability, false)) return false;
  if (!rule.errorClass.empty()) {
    const std::string tok = errorClassToken(ctx.errorClass);
    if (!anyOf(rule.errorClass, tok, false)) return false;
  }
  return true;
}

FallbackChoice selectFallback(
    const FallbackRuleSet& rs, const TurnContext& ctx,
    const std::function<bool(const std::string&, const std::string&)>& isAvailable) {
  FallbackChoice choice;
  // Hard invariants: embeddings never fall back cross-provider; a parse_fail (None)
  // is a device-side contract error identical on every host - never fall back.
  if (ctx.embeddings || ctx.errorClass == ErrorClass::None) return choice;
  for (const FallbackRule& rule : rs.rules) {
    if (!ruleMatches(rule, ctx)) continue;
    for (const FallbackTarget& t : rule.to) {
      if (t.provider == ctx.provider && (t.model.empty() || t.model == ctx.model)) continue;
      if (isAvailable && !isAvailable(t.provider, t.model)) continue;
      choice.found = true;
      choice.ruleId = rule.id;
      choice.target = t;
      return choice;
    }
  }
  return choice;
}

FallbackRuleSet defaultRuleSet(const std::vector<std::string>& priority) {
  FallbackRuleSet rs;
  const char* classes[] = {"small", "medium", "large"};
  for (const char* cls : classes) {
    FallbackRule r;
    r.id = std::string("size-") + cls + "-default";
    r.description = "shipped default: walk provider priority at this size class";
    r.sizeClass.push_back(cls);
    for (const std::string& p : priority) {
      FallbackTarget t;
      t.provider = p;  // model "" = the provider's same-size-class default
      r.to.push_back(t);
    }
    if (!r.to.empty()) rs.rules.push_back(std::move(r));
  }
  return rs;
}

std::string fallbackNote(const std::string& fromProvider, const std::string& fromModel,
                         const std::string& toProvider, const std::string& toModel,
                         ErrorClass reason) {
  std::string note = "[FALLBACK] switched " + fromProvider;
  if (!fromModel.empty()) note += "/" + fromModel;
  note += " -> " + toProvider;
  if (!toModel.empty()) note += "/" + toModel;
  const char* r = errorClassToken(reason);
  if (r && *r) note += " (" + std::string(r) + ")";
  return note;
}

}  // namespace orch
}  // namespace nimbus
