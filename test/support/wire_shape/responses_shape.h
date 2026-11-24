#pragma once
// wire_shape/responses_shape.h - the strict documented-shape checker for OpenAI Responses requests
// (router design sections 4 and 11, layer L2; lane E2-R3b). Shape from
// P/analysis-2026-11-24/research-provider-wire-facts.md sections 3 to 10, which read on 24 Nov 2026:
//   https://developers.openai.com/api/reference/resources/responses/methods/create
//   https://developers.openai.com/api/docs/guides/function-calling
//   https://developers.openai.com/api/docs/guides/reasoning
//   https://developers.openai.com/api/docs/guides/images-vision
//   https://developers.openai.com/api/docs/guides/prompt-caching
// What it holds a body to: only the keys the engine may send; `input` a non-empty list of message,
// function_call, function_call_output and reasoning items; the first item a user's or the
// developer's; arguments a JSON string holding an object; a call's `call_id` in the call form,
// never the item id (`fc_...`), and no `id` on a function_call item (design section 4); every call
// answered by exactly one output after it and before the next user message, no output alone;
// reasoning items only with store:false and include "reasoning.encrypted_content", and each
// followed by the call or message it came with; tools flat, names unique; tool_choice a documented
// value naming a known tool; max_output_tokens at least 16; Bearer headers on POST .../responses.
#include <set>

#include "common.h"

namespace wire_shape {
namespace responses_detail {

struct Calls {
  std::set<std::string> open;      // called, not yet answered
  std::set<std::string> seen;      // every call id so far
  bool reasoningPending = false;   // a reasoning item waits for its call or message
  bool anyCall = false;
};

inline bool callId(const std::string& id, C& c, const std::string& w) {
  if (!pattern(id, 64)) return c.fail(w + ": call_id empty or outside [A-Za-z0-9_-]{1,64}");
  if (id.rfind("fc_", 0) == 0) return c.fail(w + ": call_id holds the item id (fc_...), not the call id");
  return true;
}

// A cache breakpoint on an input_text part, {"mode":"explicit"} (gpt-5.6 and later; the prompt
// caching guide as research-provider-wire-facts.md section 10 read it, and engine 1's
// prompt_layout.cpp putOpenAIInput; lane E2-R7).
inline bool inputText(JsonObjectConst p, C& c, const std::string& w) {
  if (!visible(str(p["text"]))) return c.fail(w + ": text empty or whitespace only");
  JsonVariantConst bp = p["prompt_cache_breakpoint"];
  if (!bp.isNull() && (str(bp["mode"]) != "explicit" || !onlyKeys(bp.as<JsonObjectConst>(), {"mode"}, c, w + ".prompt_cache_breakpoint")))
    return c.fail(w + ": prompt_cache_breakpoint is not {\"mode\":\"explicit\"}");
  return onlyKeys(p, {"type", "text", "prompt_cache_breakpoint"}, c, w);
}

inline bool inputImage(JsonObjectConst p, C& c, const std::string& w) {
  if (!p["detail"].isNull() && !oneOf(str(p["detail"]), {"low", "high", "auto", "original"}))
    return c.fail(w + ": detail '" + str(p["detail"]) + "'");
  const bool url = !p["image_url"].isNull(), file = !p["file_id"].isNull();
  if (url == file) return c.fail(w + ": input_image needs exactly one of image_url and file_id");
  if (url && !imageUrl(str(p["image_url"]), {"jpeg", "png", "gif", "webp"}, c, w)) return false;
  if (file && str(p["file_id"]).empty()) return c.fail(w + ": empty file_id");
  return onlyKeys(p, {"type", "image_url", "file_id", "detail"}, c, w);
}

// Parts of a message's content (or of a function_call_output's output list).
inline bool parts(JsonVariantConst content, const std::string& role, C& c, const std::string& w) {
  if (content.is<const char*>()) return visible(str(content)) || c.fail(w + ": content empty or whitespace only");
  if (!content.is<JsonArrayConst>() || content.size() == 0) return c.fail(w + ": content missing or empty");
  size_t k = 0;
  for (JsonObjectConst p : content.as<JsonArrayConst>()) {
    const std::string pw = w + ".content[" + std::to_string(k++) + "]";
    const std::string type = str(p["type"]);
    if (role == "assistant") {
      if (type != "output_text") return c.fail(pw + ": assistant part '" + type + "'");
      if (!visible(str(p["text"]))) return c.fail(pw + ": text empty or whitespace only");
      if (!onlyKeys(p, {"type", "text", "annotations"}, c, pw)) return false;
    } else if (type == "input_text") {
      if (!inputText(p, c, pw)) return false;
    } else if (type == "input_image" && role == "user") {
      if (!inputImage(p, c, pw)) return false;
    } else {
      return c.fail(pw + ": part '" + type + "' in a message of role " + role);
    }
  }
  return true;
}

inline bool message(JsonObjectConst m, Calls& s, C& c, const std::string& w) {
  const std::string role = str(m["role"]);
  if (!oneOf(role, {"user", "assistant", "developer", "system"})) return c.fail(w + ": role '" + role + "'");
  if (!m["type"].isNull() && str(m["type"]) != "message") return c.fail(w + ": type '" + str(m["type"]) + "'");
  if (role == "user" && !s.open.empty()) return c.fail(w + ": a user message before every call has its output");
  if (role == "assistant") s.reasoningPending = false;
  if (role != "assistant" && s.reasoningPending) return c.fail(w + ": a reasoning item with no call or message after it");
  return onlyKeys(m, {"type", "role", "content"}, c, w) && parts(m["content"], role, c, w);
}

inline bool functionCall(JsonObjectConst m, Calls& s, C& c, const std::string& w) {
  if (!m["id"].isNull()) return c.fail(w + ": a function_call carries an item id (design section 4: none rendered)");
  const std::string id = str(m["call_id"]);
  if (!callId(id, c, w)) return false;
  if (!pattern(str(m["name"]), 64)) return c.fail(w + ": name pattern");
  if (!argsString(m["arguments"])) return c.fail(w + ": arguments is not a JSON string holding an object");
  if (!s.seen.insert(id).second) return c.fail(w + ": call_id repeated");
  s.open.insert(id);
  s.reasoningPending = false;
  s.anyCall = true;
  return onlyKeys(m, {"type", "call_id", "name", "arguments"}, c, w);
}

inline bool callOutput(JsonObjectConst m, Calls& s, C& c, const std::string& w) {
  const std::string id = str(m["call_id"]);
  if (!callId(id, c, w)) return false;
  if (!s.open.erase(id)) return c.fail(w + ": output " + id + " answers no open call");
  JsonVariantConst out = m["output"];
  if (out.is<const char*>()) {
    if (str(out).empty()) return c.fail(w + ": empty output");
  } else if (!parts(out, "user", c, w + ".output")) {
    return false;
  }
  return onlyKeys(m, {"type", "call_id", "output"}, c, w);
}

inline bool reasoning(JsonObjectConst m, bool replayOn, Calls& s, C& c, const std::string& w) {
  if (!replayOn) return c.fail(w + ": a reasoning item without store:false and include reasoning.encrypted_content");
  if (str(m["id"]).rfind("rs_", 0) != 0) return c.fail(w + ": reasoning id is not rs_...");
  if (str(m["encrypted_content"]).empty()) return c.fail(w + ": reasoning without encrypted_content");
  if (!m["summary"].is<JsonArrayConst>()) return c.fail(w + ": reasoning summary is not a list");
  if (s.reasoningPending) return c.fail(w + ": two reasoning items with nothing between");
  s.reasoningPending = true;
  return onlyKeys(m, {"type", "id", "encrypted_content", "summary"}, c, w);
}

// Reasoning may be replayed only with store:false and include "reasoning.encrypted_content".
inline bool replayOn(JsonObjectConst root) {
  if (!root["store"].is<bool>() || root["store"].as<bool>()) return false;
  for (JsonVariantConst x : root["include"].as<JsonArrayConst>())
    if (str(x) == "reasoning.encrypted_content") return true;
  return false;
}

inline bool item(JsonObjectConst m, const std::string& type, bool replay, Calls& s, C& c, const std::string& w) {
  if (type == "message") return message(m, s, c, w);
  if (type == "function_call") return functionCall(m, s, c, w);
  if (type == "function_call_output") return callOutput(m, s, c, w);
  if (type == "reasoning") return reasoning(m, replay, s, c, w);
  return c.fail(w + ": item type '" + type + "'");
}

inline bool input(JsonObjectConst root, Calls& s, C& c) {
  JsonArrayConst in = root["input"];
  if (in.isNull() || in.size() == 0) return c.fail("input missing or empty");
  const bool replay = replayOn(root);
  size_t i = 0;
  for (JsonObjectConst m : in) {
    const std::string w = "input[" + std::to_string(i) + "]";
    const std::string type = m["type"].isNull() ? "message" : str(m["type"]);
    if (i++ == 0 && !(type == "message" && oneOf(str(m["role"]), {"user", "developer", "system"})))
      return c.fail(w + ": the first item is not a user, developer or system message");
    if (!item(m, type, replay, s, c, w)) return false;
  }
  if (!s.open.empty()) return c.fail("call " + *s.open.begin() + " has no output after it");
  return !s.reasoningPending || c.fail("the input ends on a reasoning item");
}

inline bool toolEntry(JsonObjectConst t, std::set<std::string>& names, C& c, const std::string& w) {
  if (str(t["type"]) != "function") return c.fail(w + ": type is not function");
  if (!t["function"].isNull()) return c.fail(w + ": a nested Chat-style function object (Responses tools are flat)");
  if (!pattern(str(t["name"]), 64)) return c.fail(w + ": name pattern");
  if (!names.insert(str(t["name"])).second) return c.fail(w + ": name repeated");
  if (!t["description"].isNull() && !t["description"].is<const char*>()) return c.fail(w + ": description");
  if (!objectSchema(t["parameters"])) return c.fail(w + ": parameters is not an object schema");
  if (!t["strict"].isNull() && !t["strict"].is<bool>()) return c.fail(w + ": strict is not a bool");
  return onlyKeys(t, {"type", "name", "description", "parameters", "strict"}, c, w);
}

inline bool toolChoice(JsonVariantConst tc, const std::set<std::string>& names, C& c) {
  if (tc.is<const char*>()) return oneOf(str(tc), {"none", "auto", "required"}) || c.fail("tool_choice '" + str(tc) + "'");
  if (str(tc["type"]) != "function" || !names.count(str(tc["name"]))) return c.fail("tool_choice names no tool");
  return onlyKeys(tc, {"type", "name"}, c, "tool_choice");
}

inline bool tools(JsonObjectConst root, bool anyCall, C& c) {
  JsonVariantConst ts = root["tools"];
  if (ts.isNull()) {
    if (anyCall) return c.fail("function_call items without tools");
    return root["tool_choice"].isNull() || c.fail("tool_choice without tools");
  }
  if (!ts.is<JsonArrayConst>()) return c.fail("tools is not a list");
  std::set<std::string> names;
  size_t i = 0;
  for (JsonObjectConst t : ts.as<JsonArrayConst>())
    if (!toolEntry(t, names, c, "tools[" + std::to_string(i++) + "]")) return false;
  return root["tool_choice"].isNull() || toolChoice(root["tool_choice"], names, c);
}

inline bool reasoningConfig(JsonObjectConst r, C& c) {
  if (!r["effort"].isNull() && !oneOf(str(r["effort"]), {"none", "minimal", "low", "medium", "high", "xhigh", "max"}))
    return c.fail("reasoning.effort '" + str(r["effort"]) + "'");
  if (!r["summary"].isNull() && !oneOf(str(r["summary"]), {"auto", "concise", "detailed"}))
    return c.fail("reasoning.summary '" + str(r["summary"]) + "'");
  return onlyKeys(r, {"effort", "summary"}, c, "reasoning");
}

inline bool textConfig(JsonObjectConst t, C& c) {
  JsonObjectConst f = t["format"];
  if (f.isNull()) return c.fail("text without format");
  const std::string type = str(f["type"]);
  if (type == "text" || type == "json_object") return onlyKeys(f, {"type"}, c, "text.format") && onlyKeys(t, {"format"}, c, "text");
  if (type != "json_schema") return c.fail("text.format.type '" + type + "'");
  if (!pattern(str(f["name"]), 64)) return c.fail("text.format.name pattern");
  if (!objectSchema(f["schema"])) return c.fail("text.format.schema is not an object schema");
  if (!f["strict"].isNull() && !f["strict"].is<bool>()) return c.fail("text.format.strict is not a bool");
  return onlyKeys(f, {"type", "name", "schema", "strict", "description"}, c, "text.format") && onlyKeys(t, {"format"}, c, "text");
}

inline bool scalars(JsonObjectConst root, C& c) {
  if (str(root["model"]).empty()) return c.fail("model missing");
  if (!root["instructions"].isNull() && !visible(str(root["instructions"]))) return c.fail("instructions empty or not a string");
  JsonVariantConst cap = root["max_output_tokens"];
  if (!cap.isNull() && (!cap.is<unsigned>() || cap.as<unsigned>() < 16)) return c.fail("max_output_tokens below 16 or not a number");
  if (!root["parallel_tool_calls"].isNull() && !root["parallel_tool_calls"].is<bool>()) return c.fail("parallel_tool_calls is not a bool");
  if (!root["store"].isNull() && !root["store"].is<bool>()) return c.fail("store is not a bool");
  return root["prompt_cache_key"].isNull() || !str(root["prompt_cache_key"]).empty() || c.fail("empty prompt_cache_key");
}

inline bool includeOk(JsonVariantConst inc, C& c) {
  if (inc.isNull()) return true;
  if (!inc.is<JsonArrayConst>()) return c.fail("include is not a list");
  for (JsonVariantConst x : inc.as<JsonArrayConst>())
    if (str(x) != "reasoning.encrypted_content") return c.fail("include '" + str(x) + "'");
  return true;
}

inline bool envelope(JsonObjectConst root, C& c) {
  if (!onlyKeys(root, {"model", "instructions", "input", "tools", "tool_choice", "parallel_tool_calls", "max_output_tokens",
                       "reasoning", "store", "include", "text", "prompt_cache_key"},
                c, "body"))
    return false;
  if (!scalars(root, c) || !includeOk(root["include"], c)) return false;
  if (!root["reasoning"].isNull() && !reasoningConfig(root["reasoning"], c)) return false;
  return root["text"].isNull() || textConfig(root["text"], c);
}

}  // namespace responses_detail

// The whole request: headers, then the body.
inline bool responsesRequest(const Request& r, std::string& why) {
  namespace d = responses_detail;
  C c;
  JsonDocument doc;
  d::Calls s;
  bool ok = bearerHeaders(r, "/responses", false, c) && parseBody(r.body, doc, c);
  JsonObjectConst root = doc.as<JsonObjectConst>();
  ok = ok && d::envelope(root, c) && d::input(root, s, c) && d::tools(root, s.anyCall, c);
  why = c.why;
  return ok;
}

}  // namespace wire_shape
