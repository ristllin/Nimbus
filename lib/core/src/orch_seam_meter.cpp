#include "nimbus/orch/seam_meter.h"

#include <cstring>

#include "nimbus/orch/token_usage_json.h"

namespace nimbus {
namespace orch {

namespace {

uint32_t sat32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }

// A reported audio length past this is not a recording (a hostile or broken answer): it
// counts as this long, so the conversion never overflows (about 49 days).
constexpr double kMaxReportedAudioSec = 4294967.0;

bool billed(const TokenUsage& u) {
  return !u.empty() || u.cacheReadTokens || u.cacheWriteTokens;
}

// The first top-level `usage` object of a JSON body (filtered: any body size).
bool usageOf(const char* body, ArduinoJson::JsonDocument& doc) {
  if (!body || !*body) return false;
  ArduinoJson::JsonDocument filter;
  filter["usage"] = true;
  if (deserializeJson(doc, body, ArduinoJson::DeserializationOption::Filter(filter))) return false;
  return doc["usage"].is<ArduinoJson::JsonObjectConst>();
}

}  // namespace

uint32_t seamUnitPrice(const SeamUnitPrice* table, size_t n, const char* model) {
  uint32_t fallback = 0, best = 0;
  size_t bestLen = 0;
  for (size_t i = 0; i < n; ++i) {
    const char* fam = table[i].family;
    if (std::strcmp(fam, "*") == 0) {
      fallback = table[i].micro;
      continue;
    }
    const size_t fl = std::strlen(fam);
    if (model && std::strncmp(model, fam, fl) == 0 && fl > bestLen) {
      best = table[i].micro;
      bestLen = fl;
    }
  }
  return bestLen ? best : fallback;
}

SeamRoute seamRouteFor(const char* host, const char* path, const char* model) {
  SeamRoute r;
  r.model = model ? model : "";
  const std::string h = host ? host : "", p = path ? path : "";
  static const char kRouter[] = "/router/";
  if (p.rfind(kRouter, 0) == 0) {
    const size_t from = sizeof(kRouter) - 1, slash = p.find('/', from);
    r.provider = "cumulo";
    if (slash != std::string::npos && slash > from && r.model.find('/') == std::string::npos)
      r.model = p.substr(from, slash - from) + "/" + r.model;
    return r;
  }
  if (h.find("openai.com") != std::string::npos) r.provider = "openai";
  else if (h.find("anthropic.com") != std::string::npos) r.provider = "anthropic";
  else if (h.find("mistral.ai") != std::string::npos) r.provider = "mistral";
  else if (h.find("z.ai") != std::string::npos || h.find("bigmodel.cn") != std::string::npos) r.provider = "zai";
  else r.provider = "cumulo";   // fail-closed: priced at the highest row of every provider
  return r;
}

TokenUsage estimateTextUsage(size_t requestChars, uint32_t outCap) {
  TokenUsage u;
  u.promptTokens = sat32((requestChars + kEstimateCharsPerToken - 1) / kEstimateCharsPerToken);
  u.completionTokens = outCap;
  u.responses = 1;
  return u;
}

void meterSeamAnswer(SpendSeam seam, const char* provider, const char* model,
                     ArduinoJson::JsonVariantConst usage, size_t requestChars, uint32_t outCap) {
  TokenUsage u;
  if (usage.is<ArduinoJson::JsonObjectConst>())
    u = tokenUsageFromJson(usage.as<ArduinoJson::JsonObjectConst>());
  if (!billed(u)) u = estimateTextUsage(requestChars, outCap);
  meterSeamUsage(seam, provider, model, u);
}

void meterSeamAnswerBody(SpendSeam seam, const char* provider, const char* model,
                         const char* body, size_t requestChars, uint32_t outCap) {
  ArduinoJson::JsonDocument doc;
  const bool has = usageOf(body, doc);
  meterSeamAnswer(seam, provider, model,
                  has ? doc["usage"].as<ArduinoJson::JsonVariantConst>()
                      : ArduinoJson::JsonVariantConst(),
                  requestChars, outCap);
}

void meterSubAnswer(const char* provider, const char* model, ArduinoJson::JsonVariantConst usage,
                    size_t requestChars, size_t replyChars, uint32_t webRuns) {
  TokenUsage u;
  if (usage.is<ArduinoJson::JsonObjectConst>()) {
    u = tokenUsageFromJson(usage.as<ArduinoJson::JsonObjectConst>());
    // Mistral bills the connector traffic of a Conversations run beside the prompt.
    u.promptTokens = sat32((uint64_t)u.promptTokens + (usage["connector_tokens"] | 0u));
  }
  if (!billed(u)) u = estimateTextUsage(requestChars, sat32((replyChars + 1) / 2));
  meterSeamUsage(SpendSeam::Sub, provider, model, u);
  meterSubSearchRuns(provider, webRuns);
}

void meterSubSearchRuns(const char* provider, uint32_t webRuns) {
  if (webRuns) meterSeamMicro(SpendSeam::Sub, provider, (uint64_t)webRuns * kProviderSearchRunMicro);
}

uint32_t audioMsEstimate(size_t bytes, bool pcm16kMono) {
  // 16 kHz mono 16-bit = 32 bytes a millisecond; compressed: one second per kilobyte.
  return sat32(pcm16kMono ? (uint64_t)bytes / 32 : (uint64_t)bytes * 1000 / 1024);
}

uint64_t sttMicro(const char* model, uint32_t audioMs) {
  const uint64_t perMin = seamUnitPrice(kSttPerMinute, sizeof(kSttPerMinute) / sizeof(kSttPerMinute[0]),
                                        model);
  return ((uint64_t)audioMs * perMin + 59999) / 60000;   // rounded up
}

void meterTranscription(const char* provider, const char* model, const char* body,
                        uint32_t audioMs) {
  ArduinoJson::JsonDocument doc;
  if (usageOf(body, doc)) {
    ArduinoJson::JsonObjectConst usage = doc["usage"].as<ArduinoJson::JsonObjectConst>();
    // Billed by length: OpenAI's "duration" usage, Mistral's audio seconds.
    double seconds = usage["prompt_audio_seconds"] | (usage["seconds"] | 0.0);
    if (seconds > 0) {   // false for NaN too
      if (seconds > kMaxReportedAudioSec) seconds = kMaxReportedAudioSec;   // never wraps
      uint64_t ms = (uint64_t)(seconds * 1000.0);
      if ((double)ms < seconds * 1000.0) ++ms;   // a part millisecond counts whole
      meterSeamMicro(SpendSeam::Stt, provider, sttMicro(model, sat32(ms)));
      return;
    }
    const TokenUsage u = tokenUsageFromJson(usage);
    if (billed(u)) {
      meterSeamUsage(SpendSeam::Stt, provider, model, u);
      return;
    }
  }
  meterSeamMicro(SpendSeam::Stt, provider, sttMicro(model, audioMs));
}

uint64_t ttsMicro(const char* model, size_t chars) {
  const uint64_t perM = seamUnitPrice(kTtsPerMChars, sizeof(kTtsPerMChars) / sizeof(kTtsPerMChars[0]),
                                      model);
  return ((uint64_t)chars * perM + 999999) / 1000000;   // rounded up
}

void meterSpeech(const char* provider, const char* model, size_t chars) {
  meterSeamMicro(SpendSeam::Tts, provider, ttsMicro(model, chars));
}

uint64_t imageMicro(const char* model, const char* quality) {
  // gpt-image-1 at its largest size (1536x1024): low $0.016, medium $0.063, high $0.25.
  // No model named: the request builder's default, gpt-image-1.
  const bool gptImage1 = !model || !*model || std::strcmp(model, "gpt-image-1") == 0;
  if (gptImage1 && quality && std::strcmp(quality, "low") == 0) return 16000;
  if (gptImage1 && quality && std::strcmp(quality, "medium") == 0) return 63000;
  return 250000;
}

void meterImage(const char* provider, const char* model, const char* quality,
                const char* usageJson) {
  if (usageJson && *usageJson) {
    ArduinoJson::JsonDocument doc;
    if (!deserializeJson(doc, usageJson) && doc.is<ArduinoJson::JsonObjectConst>()) {
      const TokenUsage u = tokenUsageFromJson(doc.as<ArduinoJson::JsonObjectConst>());
      if (billed(u)) {
        meterSeamUsage(SpendSeam::ImageGen, provider, model && *model ? model : "gpt-image-1", u);
        return;
      }
    }
  }
  meterSeamMicro(SpendSeam::ImageGen, provider, imageMicro(model, quality));
}

TokenUsage anthropicSessionUsage(ArduinoJson::JsonVariantConst usage) {
  TokenUsage u;
  if (!usage.is<ArduinoJson::JsonObjectConst>()) return u;
  // Each count saturates to 32 bits before any arithmetic (a hostile count never wraps).
  const uint64_t oneHour = sat32(usage["cache_creation"]["ephemeral_1h_input_tokens"] | 0ull);
  uint64_t write = usage["cache_creation"]["ephemeral_5m_input_tokens"] | 0ull;
  if (write == 0 && oneHour == 0) write = usage["cache_creation_input_tokens"] | 0ull;
  u.promptTokens = sat32((usage["input_tokens"] | 0ull) + 2 * oneHour);
  u.completionTokens = sat32(usage["output_tokens"] | 0ull);
  u.cacheReadTokens = sat32(usage["cache_read_input_tokens"] | 0ull);
  u.cacheWriteTokens = sat32(write);
  u.responses = billed(u) ? 1 : 0;
  return u;
}

}  // namespace orch
}  // namespace nimbus
