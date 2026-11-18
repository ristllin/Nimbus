#pragma once
#include <ArduinoJson.h>

#include <map>
#include <string>
#include <vector>

#include "script.h"

// scripted_provider/request_view.h - a recorded request body read back into one shape
// for every dialect, so a property test (prefix, pairing, coverage, overhead) is written
// once and holds for every wire: the system text, the tool names, the tool choice and
// the messages with their blocks (text, image, tool call, tool result, thinking).
namespace harness_test {
namespace scripted {

enum class BlockKind : uint8_t { Text = 0, Image = 1, ToolCall = 2, ToolResult = 3, Thinking = 4, Other = 5 };

struct Block {
  BlockKind kind = BlockKind::Other;
  std::string text;      // Text: the text; ToolResult: the result text; Thinking: the content
  std::string id;        // ToolCall and ToolResult: the call id
  std::string name;      // ToolCall: the tool name
  std::string args;      // ToolCall: the arguments as JSON text
  bool isError = false;  // ToolResult
  std::string ref;       // Image: the file id or source as sent; Thinking: the signature
  bool operator==(const Block& o) const {
    return kind == o.kind && text == o.text && id == o.id && name == o.name && args == o.args &&
           isError == o.isError && ref == o.ref;
  }
};

struct Message {
  std::string role;      // "user" | "assistant" | "tool" | "system" | "developer"
  std::vector<Block> blocks;
  bool operator==(const Message& o) const { return role == o.role && blocks == o.blocks; }
};

struct RequestView {
  Dialect dialect = Dialect::Anthropic;
  bool parsed = false;
  std::string parseError;
  std::string model;
  std::vector<std::string> systemBlocks;   // in order
  std::vector<std::string> tools;          // advertised names, in order
  std::vector<std::string> toolsJson;      // each tool entry as sent (schema and description)
  std::string toolChoice;                  // "auto" | "none" | "any" | "required" | a name | ""
  std::vector<Message> messages;           // system-role chat messages are in systemBlocks
  uint32_t maxTokens = 0;
  size_t cacheMarks = 0;                   // Anthropic cache_control markers anywhere

  std::string system() const {
    std::string s;
    for (size_t i = 0; i < systemBlocks.size(); ++i) s += (i ? "\n" : "") + systemBlocks[i];
    return s;
  }
};

namespace detail {

inline std::string jsonText(JsonVariantConst v) {
  if (v.is<const char*>()) return v.as<const char*>();
  std::string s;
  serializeJson(v, s);
  return s;
}

inline size_t countKey(JsonVariantConst v, const char* key) {
  size_t n = 0;
  if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      if (kv.key() == key) ++n;
      n += countKey(kv.value(), key);
    }
  } else if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) n += countKey(e, key);
  }
  return n;
}

// Text of a content value that is a string or an array of text-bearing parts.
inline std::string contentText(JsonVariantConst c) {
  if (c.is<const char*>()) return c.as<const char*>();
  std::string s;
  for (JsonVariantConst p : c.as<JsonArrayConst>()) s += p["text"] | "";
  return s;
}

inline Block textBlock(std::string text) {
  Block b;
  b.kind = BlockKind::Text;
  b.text = std::move(text);
  return b;
}

inline Block anthropicBlock(JsonObjectConst o) {
  const std::string type = o["type"] | "";
  Block b;
  if (type == "text") return textBlock(o["text"] | "");
  if (type == "image" || type == "document") {
    b.kind = BlockKind::Image;
    b.ref = jsonText(o["source"]);
  }
  else if (type == "tool_use") {
    b.kind = BlockKind::ToolCall;
    b.id = o["id"] | "";
    b.name = o["name"] | "";
    b.args = jsonText(o["input"]);
  } else if (type == "tool_result") {
    b.kind = BlockKind::ToolResult;
    b.id = o["tool_use_id"] | "";
    b.text = contentText(o["content"]);
    b.isError = o["is_error"] | false;
  } else if (type == "thinking" || type == "redacted_thinking") {
    b.kind = BlockKind::Thinking;
    b.text = o["thinking"] | (o["data"] | "");
    b.ref = o["signature"] | "";
  }
  return b;
}

inline void readAnthropic(JsonObjectConst d, RequestView& v) {
  JsonVariantConst sys = d["system"];
  if (sys.is<const char*>()) v.systemBlocks.push_back(sys.as<const char*>());
  for (JsonObjectConst b : sys.as<JsonArrayConst>()) v.systemBlocks.push_back(b["text"] | "");
  for (JsonObjectConst t : d["tools"].as<JsonArrayConst>()) v.tools.push_back(t["name"] | (t["type"] | ""));
  JsonObjectConst tc = d["tool_choice"];
  if (!tc.isNull()) v.toolChoice = tc["name"] | (tc["type"] | "");
  v.maxTokens = d["max_tokens"] | 0u;
  v.cacheMarks = countKey(d, "cache_control");
  for (JsonObjectConst m : d["messages"].as<JsonArrayConst>()) {
    Message msg;
    msg.role = m["role"] | "";
    JsonVariantConst c = m["content"];
    if (c.is<const char*>()) msg.blocks.push_back(textBlock(c.as<const char*>()));
    for (JsonObjectConst b : c.as<JsonArrayConst>()) msg.blocks.push_back(anthropicBlock(b));
    v.messages.push_back(std::move(msg));
  }
}

inline Block responsesPart(JsonObjectConst p) {
  const std::string type = p["type"] | "";
  if (type == "input_text" || type == "output_text" || type == "text") return textBlock(p["text"] | "");
  Block b;
  if (type == "input_image" || type == "input_file") {
    b.kind = BlockKind::Image;
    b.ref = p["file_id"] | (p["image_url"] | "");
  }
  return b;
}

inline Message responsesItem(JsonObjectConst it) {
  const std::string type = it["type"] | "message";
  Message msg;
  Block b;
  if (type == "function_call") {
    msg.role = "assistant";
    b.kind = BlockKind::ToolCall;
    b.id = it["call_id"] | "";
    b.name = it["name"] | "";
    b.args = it["arguments"] | "";
    msg.blocks.push_back(b);
  } else if (type == "function_call_output") {
    msg.role = "tool";
    b.kind = BlockKind::ToolResult;
    b.id = it["call_id"] | "";
    b.text = jsonText(it["output"]);
    msg.blocks.push_back(b);
  } else if (type == "reasoning") {
    msg.role = "assistant";
    b.kind = BlockKind::Thinking;
    b.text = it["encrypted_content"] | "";
    b.ref = it["id"] | "";
    msg.blocks.push_back(b);
  } else {
    msg.role = it["role"] | "";
    JsonVariantConst c = it["content"];
    if (c.is<const char*>()) msg.blocks.push_back(textBlock(c.as<const char*>()));
    for (JsonObjectConst p : c.as<JsonArrayConst>()) msg.blocks.push_back(responsesPart(p));
  }
  return msg;
}

inline void readResponses(JsonObjectConst d, RequestView& v) {
  if (!d["instructions"].isNull()) v.systemBlocks.push_back(d["instructions"] | "");
  for (JsonObjectConst t : d["tools"].as<JsonArrayConst>()) v.tools.push_back(t["name"] | (t["type"] | ""));
  JsonVariantConst tc = d["tool_choice"];
  if (tc.is<const char*>()) v.toolChoice = tc.as<const char*>();
  else if (!tc.isNull()) v.toolChoice = tc["name"] | (tc["type"] | "");
  v.maxTokens = d["max_output_tokens"] | 0u;
  JsonVariantConst in = d["input"];
  if (in.is<const char*>()) {
    Message msg;
    msg.role = "user";
    msg.blocks.push_back(textBlock(in.as<const char*>()));
    v.messages.push_back(msg);
  }
  for (JsonObjectConst it : in.as<JsonArrayConst>()) v.messages.push_back(responsesItem(it));
}

inline void chatContent(JsonVariantConst c, Message& msg) {
  if (c.is<const char*>()) {
    msg.blocks.push_back(textBlock(c.as<const char*>()));
    return;
  }
  for (JsonObjectConst p : c.as<JsonArrayConst>()) {
    const std::string type = p["type"] | "";
    if (type == "text") msg.blocks.push_back(textBlock(p["text"] | ""));
    else {
      Block b;
      if (type == "image_url") {
        b.kind = BlockKind::Image;
        b.ref = jsonText(p["image_url"]);
      }
      msg.blocks.push_back(b);
    }
  }
}

inline Message chatMessage(JsonObjectConst m) {
  Message msg;
  msg.role = m["role"] | "";
  if (msg.role == "tool") {
    Block b;
    b.kind = BlockKind::ToolResult;
    b.id = m["tool_call_id"] | "";
    b.text = contentText(m["content"]);
    msg.blocks.push_back(b);
    return msg;
  }
  chatContent(m["content"], msg);
  for (JsonObjectConst tc : m["tool_calls"].as<JsonArrayConst>()) {
    Block b;
    b.kind = BlockKind::ToolCall;
    b.id = tc["id"] | "";
    b.name = tc["function"]["name"] | "";
    b.args = jsonText(tc["function"]["arguments"]);
    msg.blocks.push_back(b);
  }
  return msg;
}

inline void readChat(JsonObjectConst d, RequestView& v) {
  for (JsonObjectConst t : d["tools"].as<JsonArrayConst>())
    v.tools.push_back(t["function"]["name"] | (t["type"] | ""));
  JsonVariantConst tc = d["tool_choice"];
  if (tc.is<const char*>()) v.toolChoice = tc.as<const char*>();
  else if (!tc.isNull()) v.toolChoice = tc["function"]["name"] | (tc["type"] | "");
  v.maxTokens = d["max_tokens"] | (d["max_completion_tokens"] | 0u);
  for (JsonObjectConst m : d["messages"].as<JsonArrayConst>()) {
    const std::string role = m["role"] | "";
    if (role == "system") v.systemBlocks.push_back(contentText(m["content"]));
    else v.messages.push_back(chatMessage(m));
  }
}

}  // namespace detail

// Read one request body of dialect `d`. False (with parseError) when it is not JSON.
inline bool parseRequest(Dialect d, const std::string& body, RequestView& out) {
  out = RequestView{};
  JsonDocument doc;
  const DeserializationError e = deserializeJson(doc, body, DeserializationOption::NestingLimit(64));
  if (e) {
    out.parseError = e.c_str();
    return false;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  out.dialect = d;
  out.model = root["model"] | "";
  for (JsonVariantConst t : root["tools"].as<JsonArrayConst>()) out.toolsJson.push_back(detail::jsonText(t));
  switch (d) {
    case Dialect::Anthropic: detail::readAnthropic(root, out); break;
    case Dialect::Responses: detail::readResponses(root, out); break;
    case Dialect::Chat: detail::readChat(root, out); break;
  }
  out.parsed = true;
  return true;
}

// ---- properties ------------------------------------------------------------------

// The prefix property at message level: `later` shows the same system, the same tools
// (names, descriptions and schemas) and every message of `earlier` unchanged, in order,
// before anything new (images by their source, thinking with its signature). The model
// and tool_choice are not compared: the last round after a guard trip sets tool_choice
// none. Both views must have parsed.
inline bool isPrefix(const RequestView& earlier, const RequestView& later, std::string& why) {
  if (earlier.systemBlocks != later.systemBlocks) {
    why = "system changed";
    return false;
  }
  if (!earlier.parsed || !later.parsed) {
    why = "a request did not parse";
    return false;
  }
  if (earlier.tools != later.tools || earlier.toolsJson != later.toolsJson) {
    why = "tools changed";
    return false;
  }
  if (later.messages.size() < earlier.messages.size()) {
    why = "later request has fewer messages";
    return false;
  }
  for (size_t i = 0; i < earlier.messages.size(); ++i)
    if (!(earlier.messages[i] == later.messages[i])) {
      why = "message " + std::to_string(i) + " changed";
      return false;
    }
  return true;
}

// The bytes two bodies share from the start (cache stability at byte level).
inline size_t commonPrefixBytes(const std::string& a, const std::string& b) {
  size_t n = 0;
  while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
  return n;
}

namespace detail {

// The pairing walk: calls made and not yet answered, and results seen per call.
struct Pairing {
  std::map<std::string, int> results;   // call id -> results seen
  std::vector<std::string> pending;     // call ids still waiting, in order
  std::string why;

  bool onCall(const Block& b) {
    if (results.count(b.id)) return fail("call id " + b.id + " used twice");
    results[b.id] = 0;
    pending.push_back(b.id);
    return true;
  }
  bool onResult(const Block& b) {
    auto it = results.find(b.id);
    if (it == results.end()) return fail("result " + b.id + " answers no call");
    if (++it->second > 1) return fail("call " + b.id + " answered twice");
    for (size_t p = 0; p < pending.size(); ++p)
      if (pending[p] == b.id) {
        pending.erase(pending.begin() + (long)p);
        break;
      }
    return true;
  }
  bool onBlock(const Block& b) {
    if (b.kind == BlockKind::ToolCall) return onCall(b);
    if (b.kind == BlockKind::ToolResult) return onResult(b);
    return true;
  }
  bool fail(std::string w) {
    why = std::move(w);
    return false;
  }
};

inline bool hasCall(const Message& m) {
  for (const Block& b : m.blocks)
    if (b.kind == BlockKind::ToolCall) return true;
  return false;
}

// An assistant message that only continues the previous assistant message's round of
// calls (the Responses wire has one item per call).
inline bool sameRound(const RequestView& v, size_t i) {
  return v.dialect == Dialect::Responses && i > 0 && hasCall(v.messages[i]) && v.messages[i - 1].role == "assistant";
}

// While calls wait, the next message must open with a result: Anthropic puts the results
// first in the next user message, chat puts tool messages straight after the call.
inline bool opensWithResult(const Message& m) {
  return !m.blocks.empty() && m.blocks.front().kind == BlockKind::ToolResult;
}

}  // namespace detail

// The pairing property: every tool call id is unique and answered by exactly one result;
// while calls wait, the next message opens with a result (on the Responses wire, one
// item per call, the round's further call items come first); no result answers a call
// that was never made.
inline bool callsPaired(const RequestView& v, std::string& why) {
  if (!v.parsed) {
    why = "the request did not parse";
    return false;
  }
  detail::Pairing p;
  for (size_t i = 0; i < v.messages.size(); ++i) {
    const Message& m = v.messages[i];
    const bool waiting = !p.pending.empty() && !detail::sameRound(v, i);
    if (waiting && (m.role == "assistant" || !detail::opensWithResult(m))) {
      why = "call " + p.pending.front() + " has no result right after it (message " + std::to_string(i) + ")";
      return false;
    }
    for (const Block& b : m.blocks)
      if (!p.onBlock(b)) {
        why = p.why;
        return false;
      }
  }
  if (!p.pending.empty()) {
    why = "call " + p.pending.front() + " has no result";
    return false;
  }
  return true;
}

}  // namespace scripted
}  // namespace harness_test
