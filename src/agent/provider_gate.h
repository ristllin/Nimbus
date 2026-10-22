#pragma once
#include <Arduino.h>

#include "nimbus/orch/provider_park.h"

// provider_gate - the device side of the one per-provider park
// (nimbus/orch/provider_park.h): the millis/wall-clock glue, the flash file the
// park lives in (/data/provpark.txt, written whole through writeFileAtomic; never
// NVS), and the two calls every device seam makes around a paid request.
//
// Every paid seam on the device asks check() before it opens a connection and
// tells noteAnswer() what came back: embeddings, the photo caption step, speech to
// text, text to speech, the download scan (and image generation). The chat fabric
// and the sub-agent picker ask the same park through the harness (providers.h,
// jobs.cpp), so a refusal learned on any of them closes the provider for all.
namespace agent::gate {

// May `seam` send a request to `provider` now, carrying `key`? (The key only
// names the park: a changed key is a new start.) A closed verdict means: send
// nothing, and say parkWords(provider, verdict) where a reason is shown.
nimbus::orch::ParkVerdict check(const char* provider, nimbus::orch::PaidSeam seam, const String& key);

// The answer to a request this seam sent: `status` is the HTTP status (0 = no
// answer: network, timeout), `text` everything the provider said. A billing or key
// refusal parks the provider for every seam; a 2xx ends an episode; anything else
// re-arms an open re-check. Returns the kind it parked for (None = nothing parked).
nimbus::orch::ParkKind noteAnswer(const char* provider, nimbus::orch::PaidSeam seam, const String& key,
                                  int status, const char* text);

// Plain words for a closed verdict ("" for an open one).
String words(const char* provider, const nimbus::orch::ParkVerdict& v);

// L19-2: the daily spend warning's delivery. ownerReachable: an owner chat exists and
// this is not a quiet background turn (a dream, a routine) - only then is the warning
// checked, so the latch is never taken for a notice nobody received. sendOwnerNotice
// sends it to the owner's chat (non-blocking).
bool ownerReachable();
void sendOwnerNotice(const char* provider, const String& text);

}  // namespace agent::gate
