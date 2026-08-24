#pragma once
#include <Arduino.h>

// audio_tts - text-to-speech via a provider (OpenAI gpt-4o-mini-tts, or Mistral/
// Voxtral voxtral-mini-tts). Streams the audio response straight to a LittleFS file
// so a multi-KB clip never sits in internal heap (TLS on PSRAM). Two consumers:
//   - the on-device speaker readout: OpenAI "wav" -> solide::audio::playWavFile;
//     Mistral "mp3" -> music::streamMp3File (vendored minimp3). The provider decides
//     the format (see core::speakerTtsFormat); a Mistral-only device speaks via MP3.
//   - Telegram voice replies ("mp3" -> telegram::sendMedia(audio)).
// Fail-open: returns 0 on any failure (the caller falls back to text).
namespace agent {
namespace tts {

bool available();   // an OpenAI key is configured

// Synthesize `text` to `outPath` on LittleFS. `format` ("mp3" | "wav") is honored
// only by OpenAI; Mistral (Voxtral) always returns MP3, so a "wav" request on a
// Mistral device returns 0 rather than writing MP3 into a .wav. `voice` nullptr
// picks the provider default (Mistral en_paul_neutral / OpenAI alloy). Returns
// audio bytes written (0 = fail). The on-device speaker plays WAV directly and MP3
// through the vendored minimp3 decoder, so both formats reach the speaker.
size_t synthesizeToFile(const String& text, const char* outPath,
                        const char* format = "mp3", const char* voice = nullptr);

}  // namespace tts
}  // namespace agent
