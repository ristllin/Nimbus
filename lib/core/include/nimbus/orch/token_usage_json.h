#pragma once
#include <ArduinoJson.h>

#include "nimbus/orch/token_usage.h"

// ArduinoJson helper for TokenUsage - kept OUT of token_usage.h so pure code can
// accumulate usage without pulling in ArduinoJson. Adapters include this and call
// tokenUsageFromJson() on the provider's `usage` object. Tolerant of BOTH provider
// naming conventions so one helper serves every adapter:
//   Anthropic:        usage.input_tokens  / usage.output_tokens
//   OpenAI / Mistral: usage.prompt_tokens / usage.completion_tokens
// A missing/null usage object yields an all-zero (empty) TokenUsage.

namespace nimbus {
namespace orch {

inline TokenUsage tokenUsageFromJson(ArduinoJson::JsonObjectConst usage) {
  TokenUsage t;
  if (usage.isNull()) return t;
  // Prefer the provider-native field; fall back to the other convention. A real
  // turn always reports a non-zero prompt count, so "0 => try the alias" is safe.
  uint32_t prompt = usage["input_tokens"] | 0u;
  if (prompt == 0) prompt = usage["prompt_tokens"] | 0u;
  uint32_t completion = usage["output_tokens"] | 0u;
  if (completion == 0) completion = usage["completion_tokens"] | 0u;
  t.promptTokens     = prompt;
  t.completionTokens = completion;
  // Prompt-cache counters (v4.1.1). Anthropic reports them as siblings of
  // input_tokens; OpenAI nests the read side under prompt_tokens_details.
  t.cacheReadTokens  = usage["cache_read_input_tokens"] | 0u;
  if (t.cacheReadTokens == 0)   // OpenAI Responses API shape (the one our adapter uses)
    t.cacheReadTokens = usage["input_tokens_details"]["cached_tokens"] | 0u;
  if (t.cacheReadTokens == 0)   // OpenAI Chat Completions shape (defensive)
    t.cacheReadTokens = usage["prompt_tokens_details"]["cached_tokens"] | 0u;
  t.cacheWriteTokens = usage["cache_creation_input_tokens"] | 0u;
  return t;
}

// Capture the top-level served "model" a provider echoes in its response body into
// a TokenUsage (CUM-236 served-by). Both OpenAI-compatible and Anthropic responses
// carry a top-level "model" string, so one reader serves every adapter. No-op when
// the field is absent or the target is null.
inline void captureServedModel(TokenUsage* u, ArduinoJson::JsonVariantConst doc) {
  if (!u) return;
  auto m = doc["model"];
  if (m.is<const char*>()) {
    const char* s = m.as<const char*>();
    if (s && *s) u->servedModel = s;
  }
}

}  // namespace orch
}  // namespace nimbus
