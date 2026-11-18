#pragma once
#include <ArduinoJson.h>

#include <cstdint>
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
// JSON format (every field optional):
//   {"dialect": "anthropic" | "responses" | "chat",
//    "steps": [{"text": "...", "calls": [{"name": "read_file", "args": {...}, "id": "..."}],
//               "thinking": "...", "finish": "auto" | "end" | "tool" | "cut" | "refusal",
//               "usage": {"input": 100, "output": 20, "cache_read": 0, "cache_write": 0},
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

inline void loadUsage(JsonObjectConst u, Usage& out) {
  out.input = u["input"] | out.input;
  out.output = u["output"] | out.output;
  out.cacheRead = u["cache_read"] | out.cacheRead;
  out.cacheWrite = u["cache_write"] | out.cacheWrite;
}

inline bool loadCalls(JsonArrayConst arr, std::vector<Call>& out, std::string& err) {
  for (JsonObjectConst c : arr) {
    Call k;
    k.name = c["name"] | "";
    if (k.name.empty()) {
      err = "a call has no name";
      return false;
    }
    if (!c["args"].isNull()) {
      k.argsJson.clear();
      serializeJson(c["args"], k.argsJson);
    }
    k.id = c["id"] | "";
    out.push_back(std::move(k));
  }
  return true;
}

inline bool loadStep(JsonObjectConst o, Step& s, std::string& err) {
  s.text = o["text"] | "";
  s.thinking = o["thinking"] | "";
  s.model = o["model"] | s.model;
  s.status = o["status"] | 200;
  s.errorBody = o["error_body"] | "";
  s.rawBody = o["raw_body"] | "";
  s.transportCut = o["transport_cut"] | false;
  s.takesMs = o["takes_ms"] | 0u;
  loadUsage(o["usage"], s.usage);
  if (!o["finish"].isNull() && !finishFromName(o["finish"] | "", s.finish)) {
    err = "unknown finish";
    return false;
  }
  return loadCalls(o["calls"], s.calls, err);
}

// Parse a script. `dialect` is set when the script names one (else left as it was), and
// then *named (when given) is true.
inline bool loadScript(const std::string& json, Dialect& dialect, std::vector<Step>& steps, std::string& err,
                       bool* named = nullptr) {
  JsonDocument doc;
  const DeserializationError e = deserializeJson(doc, json, DeserializationOption::NestingLimit(64));
  if (e) {
    err = std::string("script is not JSON: ") + e.c_str();
    return false;
  }
  if (!doc["dialect"].isNull() && !dialectFromName(doc["dialect"] | "", dialect)) {
    err = "unknown dialect";
    return false;
  }
  if (named) *named = !doc["dialect"].isNull();
  if (!doc["steps"].is<JsonArrayConst>()) {
    err = "script has no steps array";
    return false;
  }
  steps.clear();
  for (JsonObjectConst o : doc["steps"].as<JsonArrayConst>()) {
    Step s;
    if (!loadStep(o, s, err)) {
      err = "step " + std::to_string(steps.size()) + ": " + err;
      return false;
    }
    steps.push_back(std::move(s));
  }
  return true;
}

}  // namespace scripted
}  // namespace harness_test
