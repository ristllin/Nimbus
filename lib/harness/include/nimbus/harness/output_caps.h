#pragma once
#include <cstddef>
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

// The table row alone: no time clamp, no load, and none of the bench seams below (lane W
// builds a round's first body with it, then sends the cap that fits the body's load, so a
// bench seam is spent once per round, by that load-aware cap).
OutputCap tableCap(const std::string& host, const std::string& model, RoundKind kind);

// What a round carries, for a cap that fits the round's time (lane P0 declared it; lane
// B's time fit reads it): the body's bytes with any photo base64 spliced in, the photos'
// share of them, the prompt's tokens (photo tokens included; 0 = estimated from the text
// bytes), whether the Cumulo router relays the request, and whether the heap was
// fragmented at round start (heapFragmented: the transport paces its slices then).
struct RoundLoad {
  size_t bodyBytes = 0;
  size_t imageBytes = 0;
  uint32_t promptTokens = 0;
  bool routed = false;
  bool fragmented = false;
  uint32_t cachedTokens = 0;   // prompt tokens the provider reads from its cache (0 = none: the worst case)
  uint8_t uploads = 1;         // times the body goes up for this answer (2: a router 402 clamp, then the capped resend)
};

// The load-aware caps (lane B; plan v3 caps protocol). The round's read window W is the
// host's (request_budget.h readWindowMsFor: Mistral 60 s, Z.ai 50 s, routed at most 90 s),
// or `roundMs` when that is shorter (0 = the host's). The fit is
//   (W - connect - upload - (fragmented ? pace : 0) - uncached prompt / prefill floor)
//     x the decode floor at the prompt's context
// (roundFitTokens; a Server link has no upload or pace term). Then
//   first = min(table cap, fit, max(minFinal, 0.75 x fit));
//   retry = min(table retry, fit), sent only when it gives at least kRetryGainTokens more
//           than the first attempt or a lower effort (else no retry).
// The table caps and efforts are outputCapFor's above. OpenAI and custom get no cap, as
// before; a fold round uses the same fit on the fold body. A capped round always sends a
// cap: when nothing fits it is 1 token, and the wire, which checks roundFitTokens against
// minFinal before it sends, refuses the round locally instead.
OutputCap outputCapFor(const std::string& host, const std::string& model, RoundKind kind,
                       uint32_t roundMs, const RoundLoad& load);
uint32_t roundFitTokens(const std::string& host, const std::string& model, uint32_t roundMs,
                        const RoundLoad& load);
inline constexpr uint32_t kRetryGainTokens = 400;

// Whether the transport will pace this round's slices: the largest free block of the
// DMA-capable pool under kDmaChunkFloor (http.h), the pool and the floor the transport's
// own pacing samples (RULINGS-v3 item 8).
bool heapFragmented(uint32_t largestDmaFreeBlock);

// The one note a person hears when they pick a model the parity eval found unfit for
// daily use ("" for the others). Plain words, for the reply. Evidence:
// docs/quick-start/provider-keys.md and the lane's parity table.
const char* modelFitNote(const std::string& provider, const std::string& model);

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

// The reasoning_effort a head round sends on this Mistral model ("" = none): the lowest
// value the model takes. Large 4 and the hosted GLM refuse "none"; Small, Medium,
// Magistral and the Vibe aliases refuse "low" (recorded 1 Nov).
const char* mistralHeadEffort(const std::string& model);

// The same-provider model a cut round steps down to before a different provider is
// asked ("" = none). Only a head that cannot turn its reasoning off needs one: Large 4
// refuses effort "none", so its output budget can go to reasoning even at "low" (Lumi
// 3 Nov: cut at 4,125 then 5,500 tokens for a 135-byte reply, 38,656 tokens billed as
// reasoning). Measured 3 Nov on the real turn shapes (evidence/defect5/): every other
// Mistral head runs at effort "none" and answers in a few hundred tokens, so its cut is
// a genuinely long answer, not reasoning; Z.ai has no non-reasoning GLM, and every
// current Claude line takes effort. The step-down model itself runs at "none".
const char* sameHostNonReasoningModel(const std::string& host, const std::string& model);

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
// Test builds: the next `rounds` capped rounds get `tokens` on BOTH attempts, so a real
// model is cut twice and the fabric's own recovery runs (lane OVF's live leg).
bool setTestCutRounds(int rounds, uint32_t tokens);

}  // namespace providers
}  // namespace agent
