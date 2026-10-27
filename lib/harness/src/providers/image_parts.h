#pragma once
#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/transcript.h"
#include "nimbus/orch/turn_images.h"

// image_parts - the head renderers' half of R1 (photos to the head as images,
// nimbus/orch/turn_images.h). INTERNAL to the provider files, like wire.h.
//
// One seeded user message carries the photos: the newest user message of the
// request (the later user messages of a tool loop carry tool results only). Each
// photo is preceded by a one-line text part naming its label, so the head can tell
// which ref is which, and the turn text comes last. No image part carries
// cache_control: the moving cache point stays on the last text block, after the
// photos, so a later round reads them from the provider's cache as part of the prefix.
//
// An earlier photo comes back as pixels through image.look (the turn's `looked`
// photos): Anthropic in the tool result itself ([text, image]), OpenAI and Mistral in
// one user message right after the round's results ([label, image] per photo). A host
// that cannot see images reads a caption answering the head's question instead (made
// once, on first use), so a failover between hosts never shows a blind host a photo
// it cannot read or hides one from a host that can.
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
// The same over the whole set: the seeded photos when `photos` (strict, as above),
// and every looked photo whose marker the body carries (a round folded away shows none).
bool spliceAll(std::string& body, const nimbus::orch::TurnImages* imgs, bool photos, BodySplices& out,
               std::string& err);

// ---- image.look: an earlier photo shown again -----------------------------------
// Does a request on this host show images this turn (its model takes image input and
// no provider refused them)? The looked photos follow the same rule as the seeded ones.
bool sees(const nimbus::orch::Transcript& tr, const std::string& host, const std::string& model);
// The looked photo a transcript entry shows again (-1 = none): an image.look result
// (lookResultText) naming a photo of the set's `looked`.
int lookedFor(const nimbus::orch::TurnImages* imgs, const nimbus::orch::TranscriptItem& result);
// The text a request renders for that result: where the photo is (`showing`, in the
// dialect's words), else a blind host's reading of it (made once).
std::string lookText(nimbus::orch::TurnImages& imgs, size_t j, bool showing, Dialect d);
// Anthropic: the tool_result's content as [text, image].
void putLookResult(ArduinoJson::JsonObject toolResult, nimbus::orch::TurnImages& imgs, size_t j);
// OpenAI and Mistral: one user message after a round's results, [label, image] for
// each looked photo `js` of the round.
void putLookMessage(ArduinoJson::JsonArray msgs, const nimbus::orch::TurnImages& imgs, const std::vector<size_t>& js,
                    Dialect d);

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
// A serialized request carries a file image source (it needs kAntFilesBeta): the
// turn's photos, a re-attached history photo or a looked photo by file_id.
bool bodyHasFileRef(const std::string& body);

// ---- re-attach (Anthropic): recent photos ride their history message -------------
// A photo uploaded once in its turn keeps its file id after the turn (RAM, bounded),
// and while its label is in the history window it rides its ORIGINAL history message
// as a file reference: [label, image, ..., the message text], the text block last (it
// keeps the message's cache point), so every later turn sends the same bytes for that
// message and the cached prefix holds across turns. A photo whose id is no longer kept
// (a restart, the bound) renders as its label only; the head can still image.look it.
// An id unused for kFileRefIdleTurns head loops is deleted (the idle step).
inline constexpr size_t kMaxFileRefs = 16;
inline constexpr uint8_t kFileRefIdleTurns = 24;
// The chat the head loop in flight serves (its prompt's per-chat cache key): an upload
// is kept for that chat and re-attached only in its turns, so a label typed in another
// chat never shows that chat a photo it was not sent.
void setLoopChat(const std::string& chatKey);
// The kept file id of photo `ref` for the loop's chat ("" = none).
std::string fileRefFor(const std::string& ref);
// Re-attach the kept photos of the first `count` messages (the history). Returns the
// images added.
int reattachHistory(ArduinoJson::JsonArray messages, size_t count);
// A looked photo with a kept id rides as that file reference (no bytes re-sent).
void useFileRef(nimbus::orch::TurnImage& it);
// The provider refused a request that carried kept ids (a changed key, an expired
// file): forget them all (nothing to delete) so the round is re-sent without them.
void forgetFileRefs();
// Queue the deletion of every kept id (re-attach switched off).
void dropFileRefs();
size_t fileRefCount();
// A refusal of a file reference itself (HTTP 400/404 naming a file).
bool refusedFileRefs(int httpStatus, const std::string& errText);

}  // namespace imageparts
}  // namespace providers
}  // namespace agent
