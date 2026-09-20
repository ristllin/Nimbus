#include "nimbus/orch/voice_route.h"

namespace nimbus {
namespace orch {

std::string voiceActiveProvider(const std::string& configured, bool hasOpenaiKey,
                                bool hasMistralKey, bool hasCumuloKey) {
  // Explicit Cumulo selection: route through the router when it is keyed; otherwise
  // fall back to a BYOK provider that has a key so the device still works, else stay
  // on cumulo (the caller fails at the key check with a clear log).
  if (configured == "cumulo") {
    if (hasCumuloKey) return "cumulo";
    if (hasOpenaiKey) return "openai";
    if (hasMistralKey) return "mistral";
    return "cumulo";
  }
  // OpenAI configured: keep it if keyed; else the other BYOK; else the router; else
  // the configured provider (no keys - the caller reports it honestly).
  if (configured == "openai") {
    if (hasOpenaiKey) return "openai";
    if (hasMistralKey) return "mistral";
    if (hasCumuloKey) return "cumulo";
    return "openai";
  }
  // Default / "mistral" / any unknown slug is treated as Mistral (the shipped voice
  // default). A one-key device sits here: no mistral or openai key, a cumulo key ->
  // cumulo. This is the fallback that lights up voice on the flagship one-key device.
  if (hasMistralKey) return "mistral";
  if (hasOpenaiKey) return "openai";
  if (hasCumuloKey) return "cumulo";
  return "mistral";
}

VoiceRouteInfo voiceRouteFor(const std::string& effectiveProvider, VoiceKind kind) {
  const bool stt = (kind == VoiceKind::Stt);
  // Cumulo proxies the OpenAI upstream: same paths as a direct OpenAI call but under
  // /router/openai, same models, openai-shaped bodies. The key/host are the router's
  // (viaCumuloRouter=true); the wire shape is NOT Mistral's.
  if (effectiveProvider == "cumulo") {
    if (stt) return {"/router/openai/v1/audio/transcriptions", "gpt-4o-mini-transcribe",
                     "", /*viaCumuloRouter=*/true, /*mistralShape=*/false, /*known=*/true};
    return {"/router/openai/v1/audio/speech", "gpt-4o-mini-tts", "alloy",
            true, false, true};
  }
  if (effectiveProvider == "openai") {
    if (stt) return {"/v1/audio/transcriptions", "gpt-4o-mini-transcribe", "",
                     false, false, true};
    return {"/v1/audio/speech", "gpt-4o-mini-tts", "alloy", false, false, true};
  }
  if (effectiveProvider == "mistral") {
    if (stt) return {"/v1/audio/transcriptions", "voxtral-mini-latest", "",
                     false, /*mistralShape=*/true, true};
    return {"/v1/audio/speech", "voxtral-mini-tts-latest", "en_paul_neutral",
            false, true, true};
  }
  return {"", "", "", false, false, /*known=*/false};
}

std::string voiceRefusalStatus(const std::string& code) {
  // The device panel and serial are ASCII, sentence case, no em dash, no exclamation:
  // say what happened, then the one next step.
  if (code == "funding_cap_reached")    return "Voice is out of credit for now. Try again later.";
  if (code == "rate_limited")           return "Voice is busy right now. Wait a moment and try again.";
  if (code == "audio_duration_unknown") return "Could not read that recording. Try again.";
  if (code == "unsupported_media_type") return "That audio format is not supported.";
  return "Voice is unavailable right now. Try again soon.";
}

}  // namespace orch
}  // namespace nimbus
