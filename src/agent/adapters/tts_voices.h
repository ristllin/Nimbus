#pragma once
#include <Arduino.h>

// tts_voices - the TTS voice catalog for the web picker, kept accurate by fetching
// Mistral's live GET /v1/audio/voices (which carries gender/accent) once, on a
// background task, and caching it. OpenAI has no voices endpoint, so its list is a
// verified static set. The web asks GET /api/voices?provider=X and renders whatever
// comes back - so the picker can never silently drift from the provider's real list.
namespace agent {
namespace ttsvoices {

// Spawn the lazy-fetch task (idempotent). It fetches the Mistral catalog the first
// time voicesJson("mistral") is asked for while a Mistral key is set.
void begin();

// Compact JSON array [{"value":<slug>,"label":<human>}] for the picker. "openai" is
// the static verified set; "mistral" returns the LIVE catalog once fetched, else a
// static fallback (and requests the fetch so the next call is live).
String voicesJson(const String& provider);

}  // namespace ttsvoices
}  // namespace agent
