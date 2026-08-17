#pragma once
#include <Arduino.h>

// audio_stt - speech-to-text via a provider transcription endpoint (OpenAI
// /v1/audio/transcriptions, model gpt-4o-mini-transcribe). Uploads an audio file
// straight off LittleFS through the shared multipart uploader (TLS on PSRAM).
// Used by (a) the Telegram voice-note path (setSttSink) and (b) the on-device
// long-press voice input. Returns the transcript, or "" on any failure (fail-open:
// the caller nudges the owner to send text / retry).
namespace agent {
namespace stt {

// True if a transcription provider key is configured (OpenAI).
bool available();

// Transcribe the audio at `localPath` (LittleFS) with the given mime type. Returns
// the transcript text ("" on failure). Signature matches telegram::SttSink so it
// can be passed directly to telegram::setSttSink.
String transcribe(const char* localPath, const char* mime);

// Wrap a raw 16-bit mono PCM file (what solide::audio::recordToFile writes - it
// emits headerless PCM) into a canonical WAV so a transcription API can decode it.
// Streams pcmPath -> wavPath with a 44-byte header. Returns false on I/O error.
// Transcribe a HEADERLESS 16-bit mono PCM capture (recordToFile's output) by
// streaming a 44-byte RIFF/WAVE header inline ahead of the file bytes in the
// multipart upload. Replaces the old pcmToWav two-file dance - the second full
// copy on LittleFS halved the recordable length for nothing but 44 bytes.
String transcribePcm(const char* pcmPath, uint32_t sampleRate);

}  // namespace stt
}  // namespace agent
