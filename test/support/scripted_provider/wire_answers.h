#pragma once
#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "script.h"

// scripted_provider/wire_answers.h - one Step rendered as the response body a real
// provider sends for that dialect: Anthropic Messages, OpenAI Responses, or chat
// completions. Usage follows each provider's own meaning: Anthropic's input_tokens
// excludes the cache, OpenAI's input_tokens and prompt_tokens include it.
// The chat answer has four shapes (ChatShape, lane E2-R3a; router design section 11): OpenAI's
// (tool_calls with type "function", null content beside calls, the error nested under "error"),
// Mistral's (no type on a call, "" content beside calls, thinking as a content chunk list,
// model_length, a top-level error object), Z.ai's (reasoning_content, arguments as a JSON object,
// sensitive, the error code a numeric string under "error") and a text-mode server's (no tools:
// a scripted call comes back as plain text, never as a call).
namespace harness_test {
namespace scripted {

enum class ChatShape : uint8_t { OpenAI = 0, Mistral = 1, Zai = 2, Text = 3 };

// A deterministic call id for call `i` of step `step`, shaped like the dialect's own
// (chat ids are 9 alphanumerics, which Mistral requires).
inline std::string callId(Dialect d, size_t step, size_t i) {
  char buf[40];
  switch (d) {
    case Dialect::Anthropic: std::snprintf(buf, sizeof buf, "toolu_s%03zuc%02zu", step % 1000, i % 100); break;
    case Dialect::Responses: std::snprintf(buf, sizeof buf, "call_s%03zuc%02zu", step % 1000, i % 100); break;
    case Dialect::Chat: std::snprintf(buf, sizeof buf, "s%03zuc%04zu", step % 1000, i % 10000); break;
  }
  return buf;
}

inline std::string idOf(Dialect d, const Step& s, size_t step, size_t i) {
  return s.calls[i].id.empty() ? callId(d, step, i) : s.calls[i].id;
}

inline const char* anthropicStop(Finish f) {
  switch (f) {
    case Finish::ToolUse: return "tool_use";
    case Finish::MaxTokens: return "max_tokens";
    case Finish::Refusal: return "refusal";
    case Finish::Context: return "model_context_window_exceeded";
    default: return "end_turn";
  }
}

// Anthropic's input is always a JSON object, even in a cut answer: arguments that are not
// valid JSON (a cut call scripted for the string-argument dialects) render as {}.
inline std::string anthropicInput(const std::string& argsJson) {
  JsonDocument d;
  if (deserializeJson(d, argsJson, DeserializationOption::NestingLimit(64)) || !d.is<JsonObjectConst>()) return "{}";
  return argsJson;
}

inline std::string anthropicAnswer(const Step& s, size_t step) {
  JsonDocument doc;
  doc["id"] = "msg_scripted_" + std::to_string(step);
  doc["type"] = "message";
  doc["role"] = "assistant";
  doc["model"] = s.model;
  JsonArray content = doc["content"].to<JsonArray>();
  if (!s.thinking.empty()) {
    JsonObject b = content.add<JsonObject>();
    b["type"] = "thinking";
    b["thinking"] = s.thinking;
    b["signature"] = "sig_scripted_" + std::to_string(step);
  }
  if (!s.text.empty()) {
    JsonObject b = content.add<JsonObject>();
    b["type"] = "text";
    b["text"] = s.text;
  }
  for (size_t i = 0; i < s.calls.size(); ++i) {
    JsonObject b = content.add<JsonObject>();
    b["type"] = "tool_use";
    b["id"] = idOf(Dialect::Anthropic, s, step, i);
    b["name"] = s.calls[i].name;
    b["input"] = serialized(anthropicInput(s.calls[i].argsJson));
  }
  doc["stop_reason"] = anthropicStop(finishOf(s));
  doc["stop_sequence"] = nullptr;
  JsonObject u = doc["usage"].to<JsonObject>();
  u["input_tokens"] = s.usage.input;
  u["output_tokens"] = s.usage.output;
  u["cache_read_input_tokens"] = s.usage.cacheRead;
  u["cache_creation_input_tokens"] = s.usage.cacheWrite;
  u["cache_creation"]["ephemeral_5m_input_tokens"] = s.usage.cacheWrite - std::min(s.usage.cacheWrite, s.usage.cacheWrite1h);
  u["cache_creation"]["ephemeral_1h_input_tokens"] = std::min(s.usage.cacheWrite, s.usage.cacheWrite1h);
  std::string out;
  serializeJson(doc, out);
  return out;
}

inline void responsesOutput(JsonArray output, const Step& s, size_t step) {
  if (!s.thinking.empty()) {
    JsonObject r = output.add<JsonObject>();
    r["type"] = "reasoning";
    r["id"] = "rs_scripted_" + std::to_string(step);
    r["summary"].to<JsonArray>();
    r["encrypted_content"] = s.thinking;
  }
  if (!s.text.empty()) {
    JsonObject m = output.add<JsonObject>();
    m["type"] = "message";
    m["id"] = "msg_scripted_" + std::to_string(step);
    m["role"] = "assistant";
    m["status"] = "completed";
    JsonObject t = m["content"].to<JsonArray>().add<JsonObject>();
    if (finishOf(s) == Finish::Refusal) {   // a refusal is its own content part
      t["type"] = "refusal";
      t["refusal"] = s.text;
    } else {
      t["type"] = "output_text";
      t["text"] = s.text;
      t["annotations"].to<JsonArray>();
    }
  }
  for (size_t i = 0; i < s.calls.size(); ++i) {
    JsonObject c = output.add<JsonObject>();
    c["type"] = "function_call";
    c["id"] = "fc_scripted_" + std::to_string(step) + "_" + std::to_string(i);
    c["call_id"] = idOf(Dialect::Responses, s, step, i);
    c["name"] = s.calls[i].name;
    c["arguments"] = s.calls[i].argsJson;
    c["status"] = "completed";
  }
}

inline std::string responsesAnswer(const Step& s, size_t step) {
  JsonDocument doc;
  doc["id"] = "resp_scripted_" + std::to_string(step);
  doc["object"] = "response";
  const bool cutAnswer = finishOf(s) == Finish::MaxTokens || finishOf(s) == Finish::Context;
  doc["status"] = cutAnswer ? "incomplete" : "completed";
  if (cutAnswer) doc["incomplete_details"]["reason"] = "max_output_tokens";
  else doc["incomplete_details"] = nullptr;
  doc["model"] = s.model;
  responsesOutput(doc["output"].to<JsonArray>(), s, step);
  JsonObject u = doc["usage"].to<JsonObject>();
  u["input_tokens"] = s.usage.input + s.usage.cacheRead;
  u["input_tokens_details"]["cached_tokens"] = s.usage.cacheRead;
  u["output_tokens"] = s.usage.output;
  u["output_tokens_details"]["reasoning_tokens"] = 0;
  u["total_tokens"] = s.usage.input + s.usage.cacheRead + s.usage.output;
  std::string out;
  serializeJson(doc, out);
  return out;
}

inline const char* chatFinish(Finish f, ChatShape shape = ChatShape::OpenAI) {
  switch (f) {
    case Finish::ToolUse: return shape == ChatShape::Text ? "stop" : "tool_calls";
    case Finish::MaxTokens: return "length";
    case Finish::Context:
      return shape == ChatShape::Mistral ? "model_length" : shape == ChatShape::Zai ? "model_context_window_exceeded" : "length";
    case Finish::Refusal: return shape == ChatShape::Zai ? "sensitive" : "stop";
    default: return "stop";
  }
}

// A text-mode server has no tools: what the script calls comes back as text the codec must not run.
inline std::string textModeContent(const Step& s) {
  std::string out = s.text;
  for (const Call& c : s.calls) out += (out.empty() ? "" : "\n") + c.name + "(" + c.argsJson + ")";
  return out;
}

inline void chatContent(JsonObject msg, const Step& s, ChatShape shape) {
  const bool refused = finishOf(s) == Finish::Refusal;
  if (shape == ChatShape::Text) {
    msg["content"] = textModeContent(s);
    return;
  }
  if (shape == ChatShape::Mistral && !s.thinking.empty()) {   // thinking rides the content as chunks
    JsonArray chunks = msg["content"].to<JsonArray>();
    JsonObject th = chunks.add<JsonObject>();
    th["type"] = "thinking";
    JsonObject inner = th["thinking"].to<JsonArray>().add<JsonObject>();
    inner["type"] = "text";
    inner["text"] = s.thinking;
    if (!s.text.empty()) {
      JsonObject t = chunks.add<JsonObject>();
      t["type"] = "text";
      t["text"] = s.text;
    }
    return;
  }
  if (shape == ChatShape::Mistral) msg["content"] = s.text;   // "" beside calls, never null
  else if (s.text.empty() || (refused && shape == ChatShape::OpenAI)) msg["content"] = nullptr;
  else msg["content"] = s.text;
  if (refused && shape == ChatShape::OpenAI) msg["refusal"] = s.text;   // OpenAI's shape: finish_reason stays "stop"
  if (!s.thinking.empty() && shape != ChatShape::Mistral) msg["reasoning_content"] = s.thinking;   // Z.ai, DeepSeek
}

inline void chatCalls(JsonObject msg, const Step& s, size_t step, ChatShape shape) {
  if (s.calls.empty() || shape == ChatShape::Text) return;
  JsonArray tcs = msg["tool_calls"].to<JsonArray>();
  for (size_t i = 0; i < s.calls.size(); ++i) {
    JsonObject tc = tcs.add<JsonObject>();
    tc["id"] = idOf(Dialect::Chat, s, step, i);
    if (shape != ChatShape::Mistral) tc["type"] = "function";
    if (shape != ChatShape::OpenAI) tc["index"] = i;
    tc["function"]["name"] = s.calls[i].name;
    JsonDocument args;   // Z.ai's answer documents arguments as an object; a cut (non-JSON) one stays a string
    if (shape == ChatShape::Zai && !deserializeJson(args, s.calls[i].argsJson) && args.is<JsonObjectConst>())
      tc["function"]["arguments"] = serialized(s.calls[i].argsJson);
    else tc["function"]["arguments"] = s.calls[i].argsJson;
  }
}

inline std::string chatAnswer(const Step& s, size_t step, ChatShape shape = ChatShape::OpenAI) {
  JsonDocument doc;
  doc["id"] = "chatcmpl-scripted-" + std::to_string(step);
  doc["object"] = "chat.completion";
  doc["created"] = 1763460000;
  doc["model"] = s.model;
  if (shape == ChatShape::Zai) doc["request_id"] = "req-scripted-" + std::to_string(step);
  JsonObject choice = doc["choices"].to<JsonArray>().add<JsonObject>();
  choice["index"] = 0;
  JsonObject msg = choice["message"].to<JsonObject>();
  msg["role"] = "assistant";
  chatContent(msg, s, shape);
  chatCalls(msg, s, step, shape);
  choice["finish_reason"] = chatFinish(finishOf(s), shape);
  JsonObject u = doc["usage"].to<JsonObject>();
  u["prompt_tokens"] = s.usage.input + s.usage.cacheRead;
  u["completion_tokens"] = s.usage.output;
  u["total_tokens"] = s.usage.input + s.usage.cacheRead + s.usage.output;
  u["prompt_tokens_details"]["cached_tokens"] = s.usage.cacheRead;
  std::string out;
  serializeJson(doc, out);
  return out;
}

// The answer body of one 2xx step.
inline std::string renderAnswer(Dialect d, const Step& s, size_t step, ChatShape shape = ChatShape::OpenAI) {
  switch (d) {
    case Dialect::Anthropic: return anthropicAnswer(s, step);
    case Dialect::Responses: return responsesAnswer(s, step);
    case Dialect::Chat: return chatAnswer(s, step, shape);
  }
  return "";
}

inline const char* anthropicErrorType(int status) {
  switch (status) {
    case 400: return "invalid_request_error";
    case 401: return "authentication_error";
    case 402: return "billing_error";
    case 403: return "permission_error";
    case 404: return "not_found_error";
    case 413: return "request_too_large";
    case 429: return "rate_limit_error";
    case 529: return "overloaded_error";
    default: return status >= 500 ? "api_error" : "invalid_request_error";
  }
}

inline const char* openaiErrorType(int status) {
  if (status == 429) return "rate_limit_exceeded";
  if (status == 401) return "invalid_api_key";
  if (status >= 500) return "server_error";
  return "invalid_request_error";
}

// Z.ai's business code for a status (https://docs.z.ai/api-reference/api-code, read 2026-11-24).
inline const char* zaiCode(int status) {
  switch (status) {
    case 401: return "1001";
    case 403: return "1220";
    case 429: return "1302";
    case 400: return "1214";
    default: return "1234";
  }
}

// The error body a provider of this dialect (and chat shape) sends with `status`.
inline std::string renderError(Dialect d, int status, ChatShape shape = ChatShape::OpenAI) {
  JsonDocument doc;
  const std::string message = "scripted error " + std::to_string(status);
  if (d == Dialect::Chat && shape == ChatShape::Mistral) {   // a top-level error object
    doc["object"] = "error";
    doc["message"] = message;
    doc["type"] = status == 429 ? "rate_limited" : "invalid_request_error";
    doc["param"] = nullptr;
    doc["code"] = std::to_string(status);
  } else if (d == Dialect::Chat && shape == ChatShape::Zai) {
    doc["error"]["code"] = zaiCode(status);
    doc["error"]["message"] = message;
  } else if (d == Dialect::Anthropic) {
    doc["type"] = "error";
    doc["error"]["type"] = anthropicErrorType(status);
    doc["error"]["message"] = message;
  } else {
    doc["error"]["message"] = message;
    doc["error"]["type"] = openaiErrorType(status);
    doc["error"]["code"] = std::to_string(status);
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

}  // namespace scripted
}  // namespace harness_test
