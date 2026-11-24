#pragma once
// wire_shape/chat_shape.h - the strict documented-shape checker for chat-completions requests as
// Mistral, Z.ai and an OpenAI-compatible custom endpoint take them (router design sections 4 and
// 11, layer L2; lane E2-R3b). Shape from P/analysis-2026-11-24/research-provider-wire-facts.md
// sections 3 to 12 and tools/model_caps/models.toml, which read on 24 Nov 2026:
//   https://docs.mistral.ai/api/endpoint/chat
//   https://docs.mistral.ai/capabilities/function_calling/
//   https://docs.mistral.ai/studio-api/conversations/reasoning
//   https://docs.z.ai/api-reference/llm/chat-completion
//   https://docs.z.ai/guides/capabilities/function-calling
//   https://docs.z.ai/guides/capabilities/thinking-mode
//   https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create
// What it holds a body to: only the keys the flavor documents; system messages only at the start;
// the first other message the user's; the last the user's or a tool result (a Mistral assistant
// with prefix:true excepted); no empty text; images as image_url in a format the flavor takes;
// every assistant tool call answered by exactly one tool message right after it, in the id form
// the flavor needs (Mistral: exactly 9 alphanumerics), and no tool message alone; arguments a JSON
// string holding an object; tool_calls and tool messages only with a tools array (text mode has
// none); reasoning only where the flavor replays it (Mistral thinking chunks, Z.ai
// reasoning_content); tool_choice, effort and response_format inside the flavor's documented set.
#include <set>

#include "common.h"

namespace wire_shape {

enum class ChatFlavor { Mistral, Zai, Custom };

namespace chat_detail {

struct F {
  ChatFlavor f;
  bool mistral() const { return f == ChatFlavor::Mistral; }
  bool zai() const { return f == ChatFlavor::Zai; }
};

inline bool callIdOk(const std::string& id, const F& f) {
  if (!f.mistral()) return pattern(id, 64);
  if (id.size() != 9) return false;
  for (char ch : id)
    if (!std::isalnum((unsigned char)ch)) return false;
  return true;
}

inline bool imagePart(JsonObjectConst p, const F& f, C& c, const std::string& w) {
  JsonVariantConst iu = p["image_url"];
  std::string url;
  if (iu.is<const char*>() && f.mistral()) {
    url = str(iu);   // Mistral also takes the URL as a bare string
  } else if (iu.is<JsonObjectConst>()) {
    url = str(iu["url"]);
    if (!iu["detail"].isNull() && (f.zai() || !oneOf(str(iu["detail"]), {"low", "high", "auto"})))
      return c.fail(w + ": detail '" + str(iu["detail"]) + "'");
    if (!onlyKeys(iu, {"url", "detail"}, c, w + ".image_url")) return false;
  } else {
    return c.fail(w + ": image_url is neither an object nor (Mistral) a string");
  }
  const bool ok = f.zai() ? imageUrl(url, {"jpeg", "png"}, c, w) : imageUrl(url, {"jpeg", "png", "gif", "webp"}, c, w);
  return ok && onlyKeys(p, {"type", "image_url"}, c, w);
}

inline bool textPart(JsonObjectConst p, C& c, const std::string& w) {
  if (!visible(str(p["text"]))) return c.fail(w + ": text empty or whitespace only");
  return onlyKeys(p, {"type", "text"}, c, w);
}

// Mistral's thinking chunk: {"type":"thinking","thinking":[text parts],"signature"?}.
inline bool thinkingChunk(JsonObjectConst p, C& c, const std::string& w) {
  if (!p["thinking"].is<JsonArrayConst>() || p["thinking"].size() == 0) return c.fail(w + ": thinking is not a list of text");
  for (JsonObjectConst t : p["thinking"].as<JsonArrayConst>())
    if (str(t["type"]) != "text" || !t["text"].is<const char*>() || !onlyKeys(t, {"type", "text"}, c, w + ".thinking"))
      return c.fail(w + ": a thinking part that is not text");
  if (!p["signature"].isNull() && !p["signature"].is<const char*>()) return c.fail(w + ": signature is not a string");
  return onlyKeys(p, {"type", "thinking", "signature", "closed"}, c, w);   // "closed": as served (R10)
}

inline bool part(JsonObjectConst p, const std::string& role, const F& f, C& c, const std::string& w) {
  const std::string type = str(p["type"]);
  if (type == "text") return textPart(p, c, w);
  if (type == "image_url" && role == "user") return imagePart(p, f, c, w);
  if (type == "thinking" && role == "assistant" && f.mistral()) return thinkingChunk(p, c, w);
  return c.fail(w + ": part '" + type + "' in a message of role " + role);
}

// A content: a non-empty visible string, or a list of parts the role may carry.
inline bool content(JsonVariantConst v, const std::string& role, const F& f, C& c, const std::string& w) {
  if (v.is<const char*>()) return visible(str(v)) || c.fail(w + ": content empty or whitespace only");
  if (!v.is<JsonArrayConst>() || v.size() == 0) return c.fail(w + ": content missing or empty");
  size_t k = 0;
  bool text = false;
  for (JsonObjectConst p : v.as<JsonArrayConst>()) {
    text = text || str(p["type"]) == "text";
    if (!part(p, role, f, c, w + ".content[" + std::to_string(k++) + "]")) return false;
  }
  return role != "assistant" || text || c.fail(w + ": an assistant content of thinking alone");
}

struct State {
  std::set<std::string> open;   // calls of the last assistant message not yet answered
  std::set<std::string> seen;
  bool tools = false;
  bool nonSystem = false;
  std::string lastRole;
  bool prefix = false;
};

inline bool toolCalls(JsonArrayConst calls, const F& f, State& s, C& c, const std::string& w) {
  if (!s.tools) return c.fail(w + ": tool_calls without tools (text mode carries none)");
  if (calls.size() == 0) return c.fail(w + ": empty tool_calls");
  size_t k = 0;
  for (JsonObjectConst t : calls) {
    const std::string tw = w + ".tool_calls[" + std::to_string(k++) + "]";
    const std::string id = str(t["id"]);
    if (!callIdOk(id, f)) return c.fail(tw + ": id '" + id + "' is not in the flavor's id form");
    if (!s.seen.insert(id).second) return c.fail(tw + ": id repeated");
    if (str(t["type"]) != "function") return c.fail(tw + ": type is not function");
    if (!pattern(str(t["function"]["name"]), 64, ".")) return c.fail(tw + ": name pattern");
    if (!argsString(t["function"]["arguments"])) return c.fail(tw + ": arguments is not a JSON string holding an object");
    if (!onlyKeys(t["function"], {"name", "arguments"}, c, tw + ".function") || !onlyKeys(t, {"id", "type", "function"}, c, tw))
      return false;
    s.open.insert(id);
  }
  return true;
}

inline bool assistant(JsonObjectConst m, const F& f, State& s, C& c, const std::string& w) {
  if (!onlyKeys(m, {"role", "content", "tool_calls", "reasoning_content", "prefix"}, c, w)) return false;
  if (!m["reasoning_content"].isNull() && (!f.zai() || !m["reasoning_content"].is<const char*>()))
    return c.fail(w + ": reasoning_content outside Z.ai or not a string");
  if (!m["prefix"].isNull() && (!f.mistral() || !m["prefix"].is<bool>())) return c.fail(w + ": prefix outside Mistral");
  s.prefix = m["prefix"] | false;
  const bool calls = !m["tool_calls"].isNull();
  JsonVariantConst ct = m["content"];
  const bool blank = ct.isNull() || (ct.is<const char*>() && str(ct).empty());
  if (!(calls && blank) && !content(ct, "assistant", f, c, w)) return false;
  return !calls || toolCalls(m["tool_calls"], f, s, c, w);
}

inline bool tool(JsonObjectConst m, const F& f, State& s, C& c, const std::string& w) {
  if (!s.tools) return c.fail(w + ": a tool message without tools");
  if (!onlyKeys(m, f.mistral() ? std::initializer_list<const char*>{"role", "content", "tool_call_id", "name"}
                               : std::initializer_list<const char*>{"role", "content", "tool_call_id"},
                c, w))
    return false;
  const std::string id = str(m["tool_call_id"]);
  if (!s.open.erase(id)) return c.fail(w + ": tool result " + id + " answers no call of the assistant message before it");
  if (!m["content"].is<const char*>() || !visible(str(m["content"]))) return c.fail(w + ": tool content empty or not a string");
  return true;
}

inline bool message(JsonObjectConst m, const F& f, State& s, C& c, const std::string& w) {
  const std::string role = str(m["role"]);
  if (!oneOf(role, {"system", "user", "assistant", "tool"})) return c.fail(w + ": role '" + role + "'");
  if (role != "tool" && !s.open.empty()) return c.fail(w + ": a " + role + " message before every call has its result");
  if (role == "system") {
    if (s.nonSystem) return c.fail(w + ": a system message after the conversation began");
    if (!onlyKeys(m, {"role", "content"}, c, w)) return false;
    return m["content"].is<const char*>() ? content(m["content"], role, f, c, w) : c.fail(w + ": system content is not a string");
  }
  if (!s.nonSystem && role != "user") return c.fail(w + ": the first message after the system is not the user's");
  s.nonSystem = true;
  if (role == "user") return onlyKeys(m, {"role", "content"}, c, w) && content(m["content"], role, f, c, w);
  if (role == "assistant") return assistant(m, f, s, c, w);
  return tool(m, f, s, c, w);
}

inline bool messages(JsonObjectConst root, const F& f, State& s, C& c) {
  JsonArrayConst ms = root["messages"];
  if (ms.isNull() || ms.size() == 0) return c.fail("messages missing or empty");
  size_t i = 0;
  for (JsonObjectConst m : ms) {
    s.prefix = false;
    if (!message(m, f, s, c, "messages[" + std::to_string(i++) + "]")) return false;
    s.lastRole = str(m["role"]);
  }
  if (!s.nonSystem) return c.fail("only system messages");
  if (!s.open.empty()) return c.fail("call " + *s.open.begin() + " has no result after it");
  if (s.lastRole == "assistant" && !s.prefix) return c.fail("the last message is the assistant's (no prefix)");
  return true;
}

inline bool toolEntry(JsonObjectConst t, const F& f, std::set<std::string>& names, C& c, const std::string& w) {
  JsonObjectConst fn = t["function"];
  if (str(t["type"]) != "function" || fn.isNull()) return c.fail(w + ": not {type:function, function:{...}}");
  if (!pattern(str(fn["name"]), 64, ".")) return c.fail(w + ": name pattern");
  if (!names.insert(str(fn["name"])).second) return c.fail(w + ": name repeated");
  if (!fn["description"].isNull() && !fn["description"].is<const char*>()) return c.fail(w + ": description");
  if (!objectSchema(fn["parameters"])) return c.fail(w + ": parameters is not an object schema");
  if (!fn["strict"].isNull() && (f.zai() || !fn["strict"].is<bool>())) return c.fail(w + ": strict");
  return onlyKeys(fn, {"name", "description", "parameters", "strict"}, c, w + ".function") && onlyKeys(t, {"type", "function"}, c, w);
}

inline bool toolList(JsonObjectConst root, const F& f, std::set<std::string>& names, C& c) {
  JsonVariantConst ts = root["tools"];
  if (!ts.is<JsonArrayConst>() || ts.size() == 0) return c.fail("tools is not a non-empty list");
  if (f.zai() && ts.size() > 128) return c.fail("more than 128 tools");
  size_t i = 0;
  for (JsonObjectConst t : ts.as<JsonArrayConst>())
    if (!toolEntry(t, f, names, c, "tools[" + std::to_string(i++) + "]")) return false;
  return true;
}

inline bool toolChoice(JsonVariantConst tc, const F& f, const std::set<std::string>& names, C& c) {
  if (tc.is<const char*>()) {
    const std::string v = str(tc);
    const bool ok = f.zai() ? oneOf(v, {"auto", "required"})
                  : f.mistral() ? oneOf(v, {"auto", "none", "any", "required"})
                                : oneOf(v, {"auto", "none", "required"});
    return ok || c.fail("tool_choice '" + v + "'");
  }
  if (f.zai()) return c.fail("a named tool_choice on Z.ai");
  if (str(tc["type"]) != "function" || !names.count(str(tc["function"]["name"]))) return c.fail("tool_choice names no tool");
  return onlyKeys(tc, {"type", "function"}, c, "tool_choice") && onlyKeys(tc["function"], {"name"}, c, "tool_choice.function");
}

inline bool tools(JsonObjectConst root, const F& f, State& s, C& c) {
  s.tools = !root["tools"].isNull();
  if (!s.tools) {
    if (!root["tool_choice"].isNull()) return c.fail("tool_choice without tools");
    return root["parallel_tool_calls"].isNull() || c.fail("parallel_tool_calls without tools");
  }
  std::set<std::string> names;
  if (!toolList(root, f, names, c)) return false;
  if (!root["parallel_tool_calls"].isNull() && (f.zai() || !root["parallel_tool_calls"].is<bool>()))
    return c.fail("parallel_tool_calls (not documented on Z.ai, or not a bool)");
  return root["tool_choice"].isNull() || toolChoice(root["tool_choice"], f, names, c);
}

inline bool responseFormat(JsonObjectConst rf, const F& f, C& c) {
  const std::string type = str(rf["type"]);
  if (type == "text" || type == "json_object") return onlyKeys(rf, {"type"}, c, "response_format");
  if (type != "json_schema" || f.zai()) return c.fail("response_format.type '" + type + "'");
  JsonObjectConst js = rf["json_schema"];
  if (!pattern(str(js["name"]), 64) || !objectSchema(js["schema"])) return c.fail("response_format.json_schema needs a name and an object schema");
  if (!js["strict"].isNull() && !js["strict"].is<bool>()) return c.fail("response_format.json_schema.strict is not a bool");
  return onlyKeys(js, {"name", "schema", "strict", "description"}, c, "response_format.json_schema") &&
         onlyKeys(rf, {"type", "json_schema"}, c, "response_format");
}

inline bool keysOk(JsonObjectConst root, const F& f, C& c) {
  if (f.mistral())
    return onlyKeys(root, {"model", "messages", "tools", "tool_choice", "parallel_tool_calls", "max_tokens", "reasoning_effort",
                           "response_format", "prompt_cache_key"},
                    c, "body");
  if (f.zai())
    return onlyKeys(root, {"model", "messages", "tools", "tool_choice", "max_tokens", "reasoning_effort", "thinking", "response_format"},
                    c, "body");
  return onlyKeys(root, {"model", "messages", "tools", "tool_choice", "parallel_tool_calls", "max_tokens", "reasoning_effort",
                         "response_format"},
                  c, "body");
}

inline bool effortOk(JsonVariantConst e, const F& f, C& c) {
  if (e.isNull()) return true;
  const std::string v = str(e);
  const bool known = f.zai() ? oneOf(v, {"none", "minimal", "low", "high", "max"})
                             : oneOf(v, {"none", "minimal", "low", "medium", "high", "xhigh"});
  return known || c.fail("reasoning_effort '" + v + "'");
}

// Z.ai's thinking switch (keysOk keeps it off the other flavors).
inline bool thinkingOk(JsonVariantConst v, C& c) {
  if (v.isNull()) return true;
  JsonObjectConst th = v;
  if (!oneOf(str(th["type"]), {"enabled", "disabled"})) return c.fail("thinking.type '" + str(th["type"]) + "'");
  if (!th["clear_thinking"].isNull() && !th["clear_thinking"].is<bool>()) return c.fail("thinking.clear_thinking is not a bool");
  return onlyKeys(th, {"type", "clear_thinking"}, c, "thinking");
}

inline bool envelope(JsonObjectConst root, const F& f, C& c) {
  if (!keysOk(root, f, c)) return false;
  if (str(root["model"]).empty()) return c.fail("model missing");
  JsonVariantConst cap = root["max_tokens"];
  if (!cap.isNull() && (!cap.is<unsigned>() || cap.as<unsigned>() < 1 || (f.zai() && cap.as<unsigned>() > 131072)))
    return c.fail("max_tokens out of range");
  if (!effortOk(root["reasoning_effort"], f, c) || !thinkingOk(root["thinking"], c)) return false;
  if (!root["prompt_cache_key"].isNull() && str(root["prompt_cache_key"]).empty()) return c.fail("empty prompt_cache_key");
  return root["response_format"].isNull() || responseFormat(root["response_format"], f, c);
}

}  // namespace chat_detail

// The whole request: headers, then the body. A custom endpoint may carry no key.
inline bool chatRequest(const Request& r, ChatFlavor flavor, std::string& why) {
  namespace d = chat_detail;
  C c;
  JsonDocument doc;
  const d::F f{flavor};
  d::State s;
  bool ok = bearerHeaders(r, "/chat/completions", flavor == ChatFlavor::Custom, c) && parseBody(r.body, doc, c);
  JsonObjectConst root = doc.as<JsonObjectConst>();
  ok = ok && d::envelope(root, f, c) && d::tools(root, f, s, c) && d::messages(root, f, s, c);
  why = c.why;
  return ok;
}

}  // namespace wire_shape
