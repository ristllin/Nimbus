#pragma once
#include <cstdint>
#include <string>

// The single cost-accounting seam for the orchestrator. Before this, no adapter
// or turn path surfaced provider token usage anywhere (prism-review finding):
// cost caps had nothing real to meter. Each adapter fills a TokenUsage from the
// provider's `usage` object in its raw response - Anthropic `input_tokens` /
// `output_tokens`, OpenAI + Mistral `prompt_tokens` / `completion_tokens` - and
// SUMS it across a turn's tool-loop rounds (`operator+=`). The total rides out on
// the turn so callers (Local Loops cost caps; future per-turn cost UI) meter true
// billed spend, not a chars/4 estimate. Portable + host-tested (NO Arduino).

namespace nimbus {
namespace orch {

struct TokenUsage {
  uint32_t promptTokens     = 0;  // provider input / prompt tokens
  uint32_t completionTokens = 0;  // provider output / completion tokens
  // Prompt-cache visibility (v4.1.1): tokens served FROM the provider's prompt
  // cache (billed ~0.1x on Anthropic, ~0.5x on OpenAI) and tokens WRITTEN to it
  // (billed 1.25x on Anthropic). Informational - promptTokens keeps its
  // provider-native meaning (Anthropic EXCLUDES cached tokens from
  // input_tokens; OpenAI INCLUDES them), so total() stays comparable with the
  // pre-cache ledger on each provider rather than silently changing scale.
  uint32_t cacheReadTokens  = 0;
  uint32_t cacheWriteTokens = 0;
  // R3 (22 Oct item): the part of cacheWriteTokens written with the 1-hour
  // TTL (Anthropic's cache_creation.ephemeral_1h_input_tokens; 0 on every
  // other provider). Priced at 2x the input rate, so the spend ceiling never
  // under-counts.
  uint32_t cacheWrite1hTokens = 0;
  // Served model (CUM-236): the "model" the provider echoed in its response. Rides
  // out on the turn's usage so the engine can disclose a fallback substitution. Not
  // a count - merged as "last non-empty wins" so the FINAL served host's model
  // survives an accumulation across tool-loop rounds and failover attempts.
  std::string servedModel;
  // Billed responses (2026-10, per-host spend): how many provider usage objects
  // were added in - tokenUsageFromJson sets 1 per parsed response, += sums them, so
  // a host's TurnSpend slot can say how many HTTP 200s it billed. Saturates.
  uint16_t responses = 0;
  // The largest single request's whole prompt (input plus, where the provider leaves
  // them out of input, cache reads and writes): what a request-size price tier is
  // decided on (claude-haiku-5-5 over 100K). += keeps the max.
  uint32_t maxRequestIn = 0;

  uint32_t total() const { return promptTokens + completionTokens; }
  bool     empty() const { return promptTokens == 0 && completionTokens == 0; }

  void add(uint32_t prompt, uint32_t completion) {
    promptTokens     += prompt;
    completionTokens += completion;
  }
  TokenUsage& operator+=(const TokenUsage& o) {
    promptTokens     += o.promptTokens;
    completionTokens += o.completionTokens;
    cacheReadTokens  += o.cacheReadTokens;
    cacheWriteTokens += o.cacheWriteTokens;
    cacheWrite1hTokens += o.cacheWrite1hTokens;
    responses = (uint16_t)((uint32_t)responses + o.responses > 0xFFFFu ? 0xFFFFu
                                                                        : responses + o.responses);
    if (o.maxRequestIn > maxRequestIn) maxRequestIn = o.maxRequestIn;
    if (!o.servedModel.empty()) servedModel = o.servedModel;
    return *this;
  }
};

}  // namespace orch
}  // namespace nimbus
