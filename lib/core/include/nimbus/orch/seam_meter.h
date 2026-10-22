#pragma once
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/token_usage.h"

// seam_meter - what a paid call outside a turn costs, for the monthly spend ceiling
// (lane DEF). Portable (NO Arduino), host-tested in test/test_seam_meter.
//
// The usage the provider reported wins whenever the answer carries it: the answer's
// `usage` object, priced by the usage ledger at the model's own row (fail-closed for a
// model with no row). Where an answer carries none, the cost is ESTIMATED, always on
// the high side, so the count never reads lower than the bill:
//   - text (an embedding, a caption, the download scan, moderation): one token per two
//     characters of request (real text runs about four) and the request's whole output
//     cap as output;
//   - speech to text: the audio's length at the model's per-minute price (the
//     provider's own reported seconds when it gives them); compressed audio of unknown
//     length counts one second per kilobyte (voice notes run two to four);
//   - text to speech: per character, at a rate above every published per-minute price;
//   - an image: the largest size's price at the requested quality.
// The rates are list prices as of kBuiltinPricesAsOf (model_price_defaults.h), rounded
// up. docs/spend-ceiling.md lists them for the owner.
namespace nimbus {
namespace orch {

inline constexpr uint32_t kEstimateCharsPerToken = 2;

// A unit price for a model family ("*" = the seam's highest, for any other model).
struct SeamUnitPrice {
  const char* family;
  uint32_t    micro;
};

// Speech to text: micro-USD per audio minute.
inline constexpr SeamUnitPrice kSttPerMinute[] = {
    {"gpt-4o-mini-transcribe", 3000},   // $0.003 a minute
    {"voxtral-mini", 2000},             // $0.001 a minute, doubled
    {"*", 6000},                        // whisper-1, gpt-4o-transcribe: $0.006 a minute
};
// Text to speech: micro-USD per million characters.
inline constexpr SeamUnitPrice kTtsPerMChars[] = {
    {"gpt-4o-mini-tts", 25000000},   // $0.015 a minute at a slow 600 characters a minute
    {"*", 30000000},                 // tts-1-hd, the highest published rate
};

// The unit price of `model` in `table`: the longest family it starts with, else "*".
uint32_t seamUnitPrice(const SeamUnitPrice* table, size_t n, const char* model);

// An image in a request whose answer reports no usage counts this many characters
// (16,000 input tokens, above every provider's tokens for one picture).
inline constexpr size_t kImageEstimateChars = 32000;

// The (provider, model) a hand-built request is metered under, from where it went: a
// provider's own host, or a router path "/router/<upstream>/..." (provider "cumulo",
// model "<upstream>/<model>", priced at its upstream's row). Any other host is metered
// as "cumulo" with the bare model: fail-closed, the highest row of every provider.
struct SeamRoute {
  std::string provider, model;
};
SeamRoute seamRouteFor(const char* host, const char* path, const char* model);

// A text request's estimated usage (no usage object in the answer).
TokenUsage estimateTextUsage(size_t requestChars, uint32_t outCap);

// One answered request of a text seam: `usage` (the answer's usage object) when it has
// counts, else the estimate. Reported through meterSeamUsage.
void meterSeamAnswer(SpendSeam seam, const char* provider, const char* model,
                     ArduinoJson::JsonVariantConst usage, size_t requestChars, uint32_t outCap);
// The same from a raw answer body: only its top-level `usage` object is read.
void meterSeamAnswerBody(SpendSeam seam, const char* provider, const char* model,
                         const char* body, size_t requestChars, uint32_t outCap);

// A sub-agent that ran inside one request (Mistral, Z.ai, Cumulo, a custom endpoint):
// its answer's usage, else an estimate from the request and the reply it returned. A
// provider-side web search or fetch the sub ran bills per call beside the tokens and is
// in no usage object: each counts kProviderSearchRunMicro.
inline constexpr uint32_t kProviderSearchRunMicro = 30000;   // $0.03 a run ($10-30 per 1,000)
// An OpenAI sub whose run used the code interpreter bills a container beside the tokens.
inline constexpr uint32_t kProviderContainerMicro = 30000;   // $0.03 a container
void meterSubAnswer(const char* provider, const char* model, ArduinoJson::JsonVariantConst usage,
                    size_t requestChars, size_t replyChars, uint32_t webRuns);
void meterSubSearchRuns(const char* provider, uint32_t webRuns);
// A finished managed-agent session whose usage could not be read (a refused read; a
// busy one is retried) counts this many input tokens and its reply at two characters a
// token, at the session model's price: a session's input is its whole tool loop.
inline constexpr uint32_t kSessionInputEstimateTokens = 50000;

// Speech to text. The length of `bytes` of audio in milliseconds: exact for 16 kHz
// mono 16-bit PCM, one second per kilobyte for anything compressed.
uint32_t audioMsEstimate(size_t bytes, bool pcm16kMono);
uint64_t sttMicro(const char* model, uint32_t audioMs);
// A transcription answer: the provider's token usage (OpenAI), else its reported audio
// seconds (a "duration" usage, or Mistral's prompt_audio_seconds), else `audioMs`.
void meterTranscription(const char* provider, const char* model, const char* body,
                        uint32_t audioMs);

// Text to speech: the answer is audio, so always the per-character estimate.
uint64_t ttsMicro(const char* model, size_t chars);
void meterSpeech(const char* provider, const char* model, size_t chars);

// Image generation. The per-image price at `quality` ("low", "medium", "high"; any
// other value, or a model other than gpt-image-1, prices as the highest).
uint64_t imageMicro(const char* model, const char* quality);
// An image answer: `usageJson` points at the answer's usage object (it may run on into
// the rest of the body) or is null when none was seen; else the per-image price.
void meterImage(const char* provider, const char* model, const char* quality,
                const char* usageJson);

// A managed-agent session's usage (GET /v1/sessions/<id>, Anthropic): input, cache
// reads and output in the Messages meaning (cache traffic outside input_tokens); a
// 5-minute cache write is a write, a 1-hour write counts as two input tokens (it bills
// at twice the input price). A session spans many requests, so no request size is
// known: it prices at the standard (not the long-request) rate.
TokenUsage anthropicSessionUsage(ArduinoJson::JsonVariantConst usage);

}  // namespace orch
}  // namespace nimbus
