#pragma once
#include <cstdint>
#include <set>
#include <string>

// Portable, host-testable transform for Mistral's GET /v1/audio/voices catalog.
// No Arduino deps (ArduinoJson v7 is header-only, host-safe).
//
// Why this exists: Mistral's voices endpoint is PAGINATED and ignores the `page`
// query field entirely - only `offset`/`limit` advance the window, while the
// response body still echoes a misleading `page`/`page_size`/`total_pages`. The
// device used to read one response (the first 10 of 30 voices) and stop, so the
// picker only ever saw a third of the catalog (e.g. Jane's sole "sarcasm" of her
// nine emotions, and Marie/French not at all). The device now paginates via
// `offset` and feeds each page here; this same code path is asserted by
// test/test_tts_voices so the device and the test can't drift.
namespace core {

// Parse ONE page body ({"items":[...],"total":N}) and append a UI row
// {"value","label","name","gender","lang","emotion"} for every voice whose slug is
// not already in `seen`. Rows are appended to `outRows` as comma-separated JSON
// objects with NO enclosing brackets (the caller wraps once with [ ]); the leading
// comma is handled internally so pages concatenate cleanly. Each emitted slug is
// added to `seen` (dedup key) so a server that ignores `offset` and re-serves the
// same page can't inflate the list. `name`/`emotion` are split from Mistral's
// "Paul - Sad" name so the web cascade groups by persona.
//
// Returns the number of raw items PROCESSED from the page (the items[] length,
// pre-dedup) - the caller advances `offset` by this. `*added` (if non-null)
// receives the count of NEW unique rows; when it is 0 the page contributed nothing
// new and the caller stops. `*total` (if non-null) receives the response's
// advertised catalog size - a loop hint only, never trusted for the final count.
int mergeMistralVoicesPage(const char* pageJson, std::set<std::string>& seen,
                           std::string& outRows, int* added = nullptr,
                           int* total = nullptr);

// ---- on-device speaker playback format -------------------------------------
// Which audio format the on-device speaker path should synthesize for a given TTS
// provider, and how it must be played back. The speaker plays canonical PCM WAV
// directly (solide::audio::playWavFile); MP3 is decoded by the vendored minimp3
// (music::streamMp3File). OpenAI's /v1/audio/speech can return WAV; Mistral
// (Voxtral) returns MP3 only. So the provider decides the format: pick the one the
// provider actually emits, never force a WAV the provider can't produce (that was
// the field bug - a Mistral device wrote MP3 into a .wav and playback rejected it).
//
// Returns the response_format to request: "wav" for a WAV-capable provider,
// otherwise "mp3". `*playAsMp3` (if non-null) is set true when the result must be
// decoded as MP3 for the speaker (i.e. the format is not WAV). Unknown providers
// default to MP3 (the safe, decodable path). Pure and host-tested so the device
// (orchestrator::speakOnDevice) and the test can never drift.
const char* speakerTtsFormat(const std::string& provider, bool* playAsMp3 = nullptr);

// The TTS provider that will actually voice a request, given the configured provider
// and which provider keys are present. Prefers the configured provider; falls back to
// the OTHER provider when the configured one has no key but the other does, so the
// device still speaks instead of going silent (the field-bug regression: dropping the
// old WAV->OpenAI reroute left a Mistral-default device holding only an OpenAI key
// silent). Returns "openai" or "mistral"; anything other than "openai" is treated as
// Mistral. With NO keys at all it returns the configured provider (the caller then
// fails at the key check with a clear log). Pure + host-tested so the device
// (agent::tts::activeProvider) and the test cannot drift.
std::string ttsActiveProvider(const std::string& configured,
                              bool hasOpenaiKey, bool hasMistralKey);

// Downmix interleaved stereo LE16 PCM to mono by averaging L+R per frame, writing
// `frames` mono samples to `out`. Split out of the MP3 speaker feed so the mixing
// math is host-tested independent of the I2S sink. `interleaved` holds 2*frames
// samples; `out` holds `frames`. A no-op when frames <= 0.
void downmixStereoToMono(const int16_t* interleaved, int frames, int16_t* out);

}  // namespace core
