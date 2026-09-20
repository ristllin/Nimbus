#pragma once
#include <string>

// voice_route - the PORTABLE half of the device voice adapters (STT + TTS): which
// host, path, model and key a voice call resolves to for a given effective
// provider, and how a router refusal is surfaced to the owner. The network TLS
// glue is a device seam (src/agent/adapters/audio_stt.*, audio_tts.*); keeping the
// routing decision here means it is host-tested (pio test -e native) with no
// hardware, exactly like nimbus::orch::embedRouteFor (the CUM-302 embeddings
// precedent this mirrors).
//
// One-key flagship (CUM-376): a Cumulo-only device has no direct openai/mistral
// key, so a voice call travels through the Cumulo router at
// /router/openai/v1/audio/{transcriptions,speech} with the single router key. The
// request/response bodies stay byte-identical to the OpenAI-compatible upstream, so
// the same device parse/synthesis code path serves cumulo and a direct provider.
namespace nimbus {
namespace orch {

// The provider that will actually serve a voice request, given the configured voice
// provider (store::sttProvider / ttsProvider) and which provider keys are present.
// Generalizes core::ttsActiveProvider to a third provider: cumulo is honored when
// explicitly selected AND keyed, and is the FINAL fallback when the configured
// provider and the other BYOK provider have no key but a cumulo key does (the
// one-key device, whose voice provider is still the shipped "mistral" default with
// no mistral key). Returns "openai", "mistral", or "cumulo". With NO keys at all it
// returns the configured provider mapped to a known slug (the caller then fails at
// the key check with a clear log), never an empty string. Pure + host-tested so the
// device (agent::stt/tts::activeProvider) and the test cannot drift.
std::string voiceActiveProvider(const std::string& configured, bool hasOpenaiKey,
                                bool hasMistralKey, bool hasCumuloKey);

// Which voice route: speech-to-text (transcriptions) or text-to-speech (speech).
enum class VoiceKind { Stt, Tts };

// Where the device POSTs a voice call for a given EFFECTIVE provider + route.
// openai/mistral hit the provider's own OpenAI-compatible /v1/audio/* directly;
// cumulo proxies the OpenAI upstream through the router at /router/openai/v1/audio/*
// with the single router key (host + key then come from cumuloBase + cumuloKey, not
// a direct provider). `model` is the transcription/speech model to send;
// `voiceDefault` is the default TTS voice slug for the route (empty for STT).
// `mistralShape` is true only for a direct Mistral call, which differs on the wire:
// it emits MP3 as base64 JSON (not raw binary) and ignores response_format - a
// cumulo call is openai-shaped even though the key is the router's. `known` is false
// for a provider with no device voice route.
struct VoiceRouteInfo {
  const char* path;         // HTTP path to POST
  const char* model;        // model id to send
  const char* voiceDefault; // default TTS voice slug ("" for STT)
  bool viaCumuloRouter;     // host + key come from the cumulo router, not a direct provider
  bool mistralShape;        // direct Mistral wire shape (base64 MP3, no response_format)
  bool known;               // false => this provider has no voice route
};
VoiceRouteInfo voiceRouteFor(const std::string& effectiveProvider, VoiceKind kind);

// Map a router/provider refusal error code (the JSON `{error:<code>}` body the
// router returns 4xx, per the CUM-376 route contract) to an honest one-line status
// for the owner - never silence. Covers the contract's codes
// (audio_duration_unknown, unsupported_media_type, funding_cap_reached,
// rate_limited) and returns a safe generic line for any unknown/empty code. Copy
// follows AGENTS section 6: sentence case, US English, no em dash, states what
// happened then the one next step. Pure + host-tested.
std::string voiceRefusalStatus(const std::string& code);

}  // namespace orch
}  // namespace nimbus
