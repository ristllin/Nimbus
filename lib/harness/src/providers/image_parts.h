#pragma once
#include <ArduinoJson.h>

#include <string>

#include "nimbus/harness/http.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/transcript.h"
#include "nimbus/orch/turn_images.h"

// image_parts - the head renderers' half of R1 (photos to the head as images,
// nimbus/orch/turn_images.h). INTERNAL to the provider files, like wire.h.
//
// One seeded user message carries the photos: the newest user message of the
// request (the later user messages of a tool loop carry tool results only), so a
// photo is sent with the turn it arrived in and never re-attached to a later turn
// (an older photo is re-read only through image.look). Each photo is preceded by a
// one-line text part naming its label, so the head can tell which ref is which, and
// the turn text comes last. No image part carries cache_control: the moving cache
// point stays on the last text block, after the photos, so a later round reads them
// from the provider's cache as part of the prefix.
//
// The bytes never become a JSON string: the part carries a marker (TurnImages::
// marker) and spliceMarkers() cuts it out of the serialized body and records a splice
// (http.h BodySplice) that the transport base64-encodes slice by slice at send time.
namespace agent {
namespace providers {
namespace imageparts {

enum class Dialect : uint8_t { Anthropic, OpenAIResponses, MistralChat };

// OpenAI's image detail for the head: labels, receipts and screens need the high one.
inline constexpr const char* kOpenAIDetail = "high";

// The image set this request shows as parts: the transcript's, when this host's
// model takes image input and no provider refused the parts this turn. nullptr =
// render the turn blind (userText below).
nimbus::orch::TurnImages* shown(const nimbus::orch::Transcript& tr, const std::string& host,
                                const std::string& model);

// The seeded user text a request renders: unchanged when it shows the photos (or
// the turn has none), else with each photo's attached label replaced by its machine
// transcription (made once, on first use).
std::string userText(const nimbus::orch::Transcript& tr, const std::string& text, bool showing);

// Rewrite the seeded user message `m` (its "content" a string, or text blocks) as
// [label, image, label, image, ..., the turn text] in the dialect's part shapes.
void putParts(ArduinoJson::JsonObject m, const nimbus::orch::TurnImages& imgs, Dialect d);

// After serialization: cut every inline image's marker out of `body` and record its
// splice. false (err set) when an inline image's marker is missing: the request
// would carry the photo's label but not the photo.
bool spliceMarkers(std::string& body, const nimbus::orch::TurnImages& imgs, BodySplices& out,
                   std::string& err);

// A refusal of the image parts themselves (HTTP 400/422 naming the image, or 413:
// the request was too large to take): the round is re-sent with the transcriptions.
bool refusedImages(int httpStatus, const std::string& errText);

// The step's one blind retry after refusedImages: marks the set rejected for the
// rest of the turn and logs it. Returns true when the caller should re-send.
bool rejectAndRetry(nimbus::orch::TurnImages* imgs, const char* who, int httpStatus);

// The turn input for a request path that cannot show photos (a single-shot head):
// the photos of the turn in flight read as their machine transcriptions.
std::string blindInputs(const std::string& inputs);

// One head loop's photos: seeds `tr` with the turn's set (activeTurnImages) and, when
// the loop ends, queues the deletion of what it uploaded (deleted later, one at a
// time, by deletePendingUploads on the device's idle step, so no reply waits on it).
class LoopImages {
 public:
  explicit LoopImages(nimbus::orch::Transcript& tr);
  ~LoopImages();
  LoopImages(const LoopImages&) = delete;
  LoopImages& operator=(const LoopImages&) = delete;
 private:
  nimbus::orch::Transcript& tr_;
};

// ---- upload once (Anthropic Files API) ---------------------------------------------
// The beta the Files API and a file image source need.
inline constexpr const char* kAntFilesBeta = "files-api-2025-04-14";
// Upload every inline photo of `imgs` once (POST /v1/files, multipart, the raw
// bytes spliced): each round then references it by file_id instead of re-sending
// it. Direct keys only (a router head stays inline). A failed upload leaves that
// photo inline (logged). Returns the count uploaded.
int antUploadImages(const ProviderDeps& pd, nimbus::orch::TurnImages& imgs);
// Any photo of the set rides as a file reference (the request needs kAntFilesBeta).
bool anyFileRef(const nimbus::orch::TurnImages* imgs);

}  // namespace imageparts
}  // namespace providers
}  // namespace agent
