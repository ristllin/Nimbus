#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/http.h"

// scripted_provider/script.h - what a scripted model answers, one Step per provider
// request. A Step is the model's side of one exchange (text, tool calls, thinking,
// usage, how it finished) or a failure of the exchange (an HTTP error, a transport cut,
// a raw body). The same script format loads from JSON (loadScript) and drives the
// reference loop's Python twin (scripted_provider.py), so one script can run both arms.
//
// JSON format (every field optional; an unknown key or a wrongly typed field is refused):
//   {"dialect": "anthropic" | "responses" | "chat",
//    "steps": [{"text": "...", "calls": [{"name": "read_file", "args": {...}, "id": "..."}],
//               "thinking": "...", "finish": "auto" | "end" | "tool" | "cut" | "refusal",
//               "usage": {"input": 100, "output": 20, "cache_read": 0, "cache_write": 0, "cache_write_1h": 0},
//               "model": "...", "status": 200, "error_body": "...", "raw_body": "...",
//               "transport_cut": false, "takes_ms": 0}]}
namespace harness_test {
namespace scripted {

// The response shape of a request, decided by its path (dialectOfPath).
enum class Dialect : uint8_t {
  Anthropic = 0,   // POST /v1/messages
  Responses = 1,   // POST /v1/responses
  Chat = 2,        // POST .../chat/completions (Mistral, Z.ai, OpenAI-compatible, the router)
};

inline const char* dialectName(Dialect d) {
  switch (d) {
    case Dialect::Anthropic: return "anthropic";
    case Dialect::Responses: return "responses";
    case Dialect::Chat: return "chat";
  }
  return "chat";
}

inline bool dialectFromName(const std::string& s, Dialect& out) {
  for (Dialect d : {Dialect::Anthropic, Dialect::Responses, Dialect::Chat})
    if (s == dialectName(d)) {
      out = d;
      return true;
    }
  return false;
}

inline bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = std::char_traits<char>::length(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// The dialect of a request path; false for a path no dialect answers.
inline bool dialectOfPath(const std::string& path, Dialect& out) {
  const std::string p = path.substr(0, path.find('?'));
  if (endsWith(p, "/messages")) out = Dialect::Anthropic;
  else if (endsWith(p, "/responses")) out = Dialect::Responses;
  else if (endsWith(p, "/chat/completions")) out = Dialect::Chat;
  else return false;
  return true;
}

struct Call {
  std::string name;
  std::string argsJson = "{}";
  std::string id;   // "" = a deterministic id from the step and position (callId)
};

struct Usage {
  uint32_t input = 100;       // uncached prompt tokens
  uint32_t output = 20;
  uint32_t cacheRead = 0;
  uint32_t cacheWrite = 0;
  uint32_t cacheWrite1h = 0;  // the part of cacheWrite written with the 1-hour TTL (Anthropic)
};

enum class Finish : uint8_t {
  Auto = 0,       // ToolUse when the step calls a tool, else End
  End = 1,
  ToolUse = 2,
  MaxTokens = 3,  // a cut answer (stop_reason max_tokens, finish_reason length, incomplete)
  Refusal = 4,
};

struct Step {
  std::string text;
  std::vector<Call> calls;
  std::string thinking;            // Anthropic thinking block, Responses reasoning item
  Usage usage;
  Finish finish = Finish::Auto;
  std::string model = "scripted-model";
  // Failures of the exchange instead of an answer.
  int status = 200;                // not 2xx: errorBody, or the dialect's error shape
  std::string errorBody;
  std::string rawBody;             // non-empty: served verbatim with `status`
  bool transportCut = false;       // exec returns false with transportErr
  std::string transportErr = "scripted transport cut";
  uint32_t takesMs = 0;            // advanced on ScriptedProvider::clock
  agent::HeaderList headers;
};

inline Finish finishOf(const Step& s) {
  if (s.finish != Finish::Auto) return s.finish;
  return s.calls.empty() ? Finish::End : Finish::ToolUse;
}

// ---- builders -------------------------------------------------------------------
inline Step say(std::string text) {
  Step s;
  s.text = std::move(text);
  return s;
}
inline Step call(std::string name, std::string argsJson = "{}", std::string text = "") {
  Step s;
  s.text = std::move(text);
  s.calls.push_back(Call{std::move(name), std::move(argsJson), ""});
  return s;
}
inline Step calls(std::vector<Call> cs, std::string text = "") {
  Step s;
  s.text = std::move(text);
  s.calls = std::move(cs);
  return s;
}
inline Step cut(std::string partialText) {
  Step s;
  s.text = std::move(partialText);
  s.finish = Finish::MaxTokens;
  return s;
}
inline Step httpError(int status, std::string body = "") {
  Step s;
  s.status = status;
  s.errorBody = std::move(body);
  return s;
}
inline Step transportCut(uint32_t takesMs = 0, std::string err = "scripted transport cut") {
  Step s;
  s.transportCut = true;
  s.transportErr = std::move(err);
  s.takesMs = takesMs;
  return s;
}
inline Step raw(int status, std::string body) {
  Step s;
  s.status = status;
  s.rawBody = std::move(body);
  return s;
}

// ---- the JSON script format -------------------------------------------------------
inline bool finishFromName(const std::string& s, Finish& out) {
  static const std::pair<const char*, Finish> names[] = {{"auto", Finish::Auto},       {"end", Finish::End},
                                                         {"tool", Finish::ToolUse},    {"cut", Finish::MaxTokens},
                                                         {"refusal", Finish::Refusal}};
  for (const auto& n : names)
    if (s == n.first) {
      out = n.second;
      return true;
    }
  return false;
}

// A key the format does not define is an error, so a typo ("call" for "calls") cannot
// load as an empty answer.
inline bool onlyKeys(JsonObjectConst o, std::initializer_list<const char*> allowed, const char* what,
                     std::string& err) {
  for (JsonPairConst kv : o) {
    bool known = false;
    for (const char* k : allowed) known = known || kv.key() == k;
    if (!known) {
      err = std::string("unknown key ") + kv.key().c_str() + " in " + what;
      return false;
    }
  }
  return true;
}

// A present field must have its type; an absent one keeps the default.
template <class T>
inline bool typed(JsonObjectConst o, const char* key, T& out, std::string& err) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return true;
  if (!v.is<T>()) {
    err = std::string(key) + " has the wrong type";
    return false;
  }
  out = v.as<T>();
  return true;
}

inline bool loadUsage(JsonVariantConst v, Usage& out, std::string& err) {
  if (v.isNull()) return true;
  JsonObjectConst u = v.as<JsonObjectConst>();
  if (u.isNull()) {
    err = "usage is not an object";
    return false;
  }
  return onlyKeys(u, {"input", "output", "cache_read", "cache_write", "cache_write_1h"}, "usage", err) &&
         typed(u, "input", out.input, err) && typed(u, "output", out.output, err) &&
         typed(u, "cache_read", out.cacheRead, err) && typed(u, "cache_write", out.cacheWrite, err) &&
         typed(u, "cache_write_1h", out.cacheWrite1h, err);
}

inline bool loadCall(JsonVariantConst v, Call& k, std::string& err) {
  JsonObjectConst c = v.as<JsonObjectConst>();
  if (c.isNull()) {
    err = "a call is not an object";
    return false;
  }
  if (!onlyKeys(c, {"name", "args", "id"}, "a call", err) || !typed(c, "name", k.name, err) ||
      !typed(c, "id", k.id, err))
    return false;
  if (k.name.empty()) {
    err = "a call has no name";
    return false;
  }
  if (!c["args"].isNull()) {
    k.argsJson.clear();
    serializeJson(c["args"], k.argsJson);
  }
  return true;
}

inline bool loadCalls(JsonVariantConst v, std::vector<Call>& out, std::string& err) {
  if (v.isNull()) return true;
  if (!v.is<JsonArrayConst>()) {
    err = "calls is not an array";
    return false;
  }
  for (JsonVariantConst c : v.as<JsonArrayConst>()) {
    Call k;
    if (!loadCall(c, k, err)) return false;
    out.push_back(std::move(k));
  }
  return true;
}

// A finish that contradicts the calls renders an answer no provider sends.
inline bool finishFits(const Step& s, std::string& err) {
  if (s.finish == Finish::ToolUse && s.calls.empty()) err = "finish tool with no calls";
  else if (s.finish == Finish::End && !s.calls.empty()) err = "finish end with calls";
  else return true;
  return false;
}

inline bool loadFields(JsonObjectConst o, Step& s, std::string& err) {
  return typed(o, "text", s.text, err) && typed(o, "thinking", s.thinking, err) && typed(o, "model", s.model, err) &&
         typed(o, "status", s.status, err) && typed(o, "error_body", s.errorBody, err) &&
         typed(o, "raw_body", s.rawBody, err) && typed(o, "transport_cut", s.transportCut, err) &&
         typed(o, "takes_ms", s.takesMs, err);
}

inline bool loadStep(JsonVariantConst v, Step& s, std::string& err) {
  JsonObjectConst o = v.as<JsonObjectConst>();
  if (o.isNull()) {
    err = "not an object";
    return false;
  }
  static const std::initializer_list<const char*> keys = {
      "text", "calls", "thinking", "finish", "usage", "model", "status", "error_body", "raw_body", "transport_cut",
      "takes_ms"};
  if (!onlyKeys(o, keys, "a step", err) || !loadFields(o, s, err) || !loadUsage(o["usage"], s.usage, err) ||
      !loadCalls(o["calls"], s.calls, err))
    return false;
  std::string finish;
  if (!typed(o, "finish", finish, err)) return false;
  if (!finish.empty() && !finishFromName(finish, s.finish)) {
    err = "unknown finish " + finish;
    return false;
  }
  return finishFits(s, err);
}

// Parse a script. On success `steps` holds it and `dialect` is set when the script names
// one (then *named, when given, is true); on failure neither is touched.
inline bool loadScript(const std::string& json, Dialect& dialect, std::vector<Step>& steps, std::string& err,
                       bool* named = nullptr) {
  JsonDocument doc;
  const DeserializationError e = deserializeJson(doc, json, DeserializationOption::NestingLimit(64));
  if (e) {
    err = std::string("script is not JSON: ") + e.c_str();
    return false;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull() || !onlyKeys(root, {"dialect", "steps"}, "the script", err)) {
    if (err.empty()) err = "script is not an object";
    return false;
  }
  Dialect d = dialect;
  if (!root["dialect"].isNull() && !dialectFromName(root["dialect"] | "", d)) {
    err = "unknown dialect";
    return false;
  }
  if (!root["steps"].is<JsonArrayConst>()) {
    err = "script has no steps array";
    return false;
  }
  std::vector<Step> loaded;
  for (JsonVariantConst v : root["steps"].as<JsonArrayConst>()) {
    Step s;
    if (!loadStep(v, s, err)) {
      err = "step " + std::to_string(loaded.size()) + ": " + err;
      return false;
    }
    loaded.push_back(std::move(s));
  }
  steps = std::move(loaded);
  dialect = d;
  if (named) *named = !root["dialect"].isNull();
  return true;
}

}  // namespace scripted
}  // namespace harness_test
