#pragma once
#include <Arduino.h>

// audio_tts - text-to-speech via a provider (OpenAI /v1/audio/speech, model
// gpt-4o-mini-tts). Streams the audio response straight to a LittleFS file so a
// multi-KB clip never sits in internal heap (TLS on PSRAM). Two consumers:
//   - the on-device speaker readout (format "wav" -> solide::audio::playWavFile)
//   - Telegram voice replies (format "opus" -> telegram::sendMedia(voice))
// Fail-open: returns 0 on any failure (the caller falls back to text).
namespace agent {
namespace tts {

bool available();   // an OpenAI key is configured

// Synthesize `text` to `outPath` on LittleFS. `format` ("mp3" | "wav") is honored
// only by OpenAI; Mistral (Voxtral) always returns MP3. `voice` nullptr picks the
// provider default (Mistral en_paul_neutral / OpenAI alloy). Returns audio bytes
// written (0 = fail). Note: solide::audio plays PCM/WAV, so MP3 output is for
// Telegram audio, not the on-device speaker.
size_t synthesizeToFile(const String& text, const char* outPath,
                        const char* format = "mp3", const char* voice = nullptr);

}  // namespace tts
}  // namespace agent
