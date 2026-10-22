#pragma once
#include <cstdint>
#include <string>

// output_caps - the ONE table of output budgets: per provider adapter, round kind
// and model family, how many tokens a request may produce, the reasoning effort it
// pins, and what the one retry after a cut changes.
//
// Why a table. The Anthropic head capped every tool round at 3000 tokens with no
// effort pinned. Every current Claude model thinks by default and the thinking
// counts toward max_tokens, so on 16-18 Oct the cap cut tool rounds, the final
// orch_turn and the compaction fold; the fabric then re-sent the IDENTICAL body,
// which was cut again, and paid a costly failover. Now:
//   - the caps fit a thinking model (8192, clamped to what the round's time allows);
//   - effort is pinned per model family (output_config.effort), only on families
//     that accept it (Haiku 4.5 answers 400 to the field); Mistral heads that can
//     reason get reasoning_effort "none" (Mistral documents only "high" and "none");
//   - every adapter checks completeness (stop_reason / finish_reason / status), and
//     a cut round is retried ONCE in the same step with a body that differs (more
//     room, lower effort) - never an identical body - before it fails as Cut;
//   - thinking is never disabled ({type:"disabled"} is a 400 on Opus 5.5 and
//     Fable 5.1, and on Opus 5 it can put a tool call into visible text).
namespace agent {
namespace providers {

// What a request is for. The fold is the compaction call (its own budget: the
// summary is small, the thinking should be too).
enum class RoundKind : uint8_t { ToolRound = 0, FinalRound, SingleShot, Fold };
inline constexpr int kRoundKindCount = 4;
const char* roundKindSlug(RoundKind k);   // "tool" | "final" | "single" | "fold"

struct OutputCap {
  uint32_t maxTokens = 0;        // first attempt; 0 = the field is not sent (provider default)
  const char* effort = "";       // first attempt effort; "" = not sent
  uint32_t retryTokens = 0;      // the one retry after a cut; 0 = no retry
  const char* retryEffort = "";  // effort on that retry; "" = not sent
  // A retry exists and its body differs from the first attempt's.
  bool hasRetry() const;
};

// The table. `host` is the adapter ("anthropic" | "openai" | "mistral" | "custom"),
// `model` the requested id. `roundMs` is the time the round may take (the round's
// socket timeout): the token caps are clamped so a generation at the family's
// slowest calibrated rate still finishes inside it (a non-streamed request that
// outlives its deadline is a timeout, and costs a failover). Under the clamp the
// first attempt takes three quarters of that fit (never under 3,000 tokens) and the
// retry after a cut keeps more room than it: the whole fit, and at least a third
// more than the first attempt, so every capped round has a retry (L19-44).
// 0 = no time clamp.
OutputCap outputCapFor(const std::string& host, const std::string& model, RoundKind kind,
                       uint32_t roundMs = 0);

// Whether a Claude model accepts output_config.effort (Opus 4.5+, Sonnet 4.6+,
// Haiku 5+, every Fable and Mythos). Unknown ids: no (omitting it is today's wire).
bool anthropicTakesEffort(const std::string& model);

// Whether a Mistral model accepts reasoning_effort (CUM-616): the models whose
// /v1/models entry says capabilities.reasoning (recorded 2026-10: Mistral Small
// 2603+, Medium, Magistral, Large 4, the Vibe CLI aliases, Leanstral, the hosted
// GLM). Any other model answers 400 code 3051 "reasoning_effort is not enabled for
// this model" (mistral-large-latest, ministral-*; recorded 22 Oct). A router pick
// carries a "mistral/" prefix.
bool mistralTakesEffort(const std::string& model);

// The family's slowest calibrated output rate, tokens per second (the time clamp).
uint32_t slowestTokensPerSecond(const std::string& host, const std::string& model);

// Test builds (the bench "cut" leg): when non-zero, the FIRST attempt of every
// capped request asks for this many tokens, so a real model is cut on demand and
// the retry with headroom runs. Defined only in test and host builds
// (NIMBUS_TEST / NIMBUS_NATIVE): the production image cannot call it. Returns true
// when the cap is set (one signature for every declaration of the test seams: a
// void here and a bool elsewhere would be two functions with one mangled name).
bool setTestFirstAttemptCap(uint32_t tokens);
uint32_t testFirstAttemptCap();

}  // namespace providers
}  // namespace agent
