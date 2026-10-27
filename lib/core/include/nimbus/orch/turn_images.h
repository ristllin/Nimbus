#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/orch/media_label.h"   // MediaLabel - a photo's bracket and its attached segment
#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/turn_spend.h"    // TurnSpend - the caption step's spend rides the turn's record

// turn_images - the photos of the head turn in flight, sent to the head model as
// image parts (owner ruling R1: images go to the main model as media, a description
// is only the fallback).
//
// The channel (Telegram) fetches each photo once, keeps its bytes in one PSRAM
// buffer and offers the turn's set to a single slot bound to the chat and the routed
// turn text (the turn_inbox pattern: an offer whose turn never reached the engine is
// dropped by the next one). The engine takes it when the owner turn starts and holds
// it for the turn; every head loop seeds its Transcript with it, and each provider's
// renderer puts the images into the seeded user message, the newest user message of
// the request, as that provider's image parts. A head that cannot see them (no
// image input, a provider that refused the parts) reads the same text with each
// photo's segment replaced by its machine transcription, made on first use.
//
// Image bytes never become a JSON string: the renderer writes a marker (marker(i))
// where the base64 belongs and the body splices it at send time (http.h BodySplice).
//
// Single task by design (the poll task runs the whole turn), like turn_inbox.
// Arduino-free, host-tested (test_turn_images).
namespace nimbus {
namespace orch {

// Bounds, raised only after a bench heap trace (lane E: the 3- and 10-photo turns).
constexpr size_t kTurnImageMax = 10;                    // Telegram's album maximum
constexpr size_t kImagePartBytes = 512u * 1024;         // one photo (tg::kPhotoBudget)
constexpr size_t kTurnImageBytes = 2u * 1024u * 1024u;  // every photo of one turn, raw

// The MIME type a head takes as an image part ("image/jpeg", "image/png",
// "image/webp", "image/gif"), "" for anything else. "image/jpg" reads as JPEG.
const char* imagePartMime(const std::string& mime);
// HEIC/HEIF (an iPhone photo sent as a file): no provider takes it as an image part.
bool isHeic(const std::string& mime);
// An animated GIF (two or more frames): providers read only one frame, so it is not
// sent as a still image.
bool animatedGif(const uint8_t* bytes, size_t len);
// The image format of `bytes` by its magic ("image/jpeg", "image/png", "image/gif",
// "image/webp"), "" for anything else (a stored file kept without its type).
const char* sniffImageMime(const uint8_t* bytes, size_t len);

// Does this head model take image input? One rule for the telegram path (attach or
// caption) and every renderer (render the parts, or the transcription). What the
// provider's own model list says decides when the device holds it (the image-input
// lookup below: Mistral's and Anthropic's lists carry it per model); a direct Mistral
// head its list does not know yet takes no images (the photos go as transcriptions);
// otherwise the name rule:
//   anthropic: every claude- model; openai: gpt-4o, gpt-4.1, gpt-5 and newer, o3,
//   o4; mistral: chat models with vision (small, medium, large, ministral,
//   magistral, pixtral), not codestral, devstral, voxtral or the OCR models;
//   cumulo: the "<upstream>/<model>" it names; zai and custom: no (GLM vision
//   models are not on the device's list).
bool headTakesImages(const std::string& host, const std::string& model);

// The device's image-input lookup: 1 = the provider's stored list says `model` takes
// images, 0 = it says it does not, -1 = no row for it or the list does not say, -2 = the
// device holds no list of that provider (the name rule decides). Unset (host tests, the
// hosted daemon): the name rule alone.
using ImageInputLookup = int (*)(const char* host, const char* model);
void installImageInputLookup(ImageInputLookup f);

struct TurnImage {
  std::string ref;        // the blob hash (16 lowercase hex)
  std::string mime;       // imagePartMime()
  const uint8_t* bytes = nullptr;   // held by the channel for the turn (PSRAM)
  size_t len = 0;
  int k = 0, n = 0;       // its place among the turn's media (the label's k/N)
  std::string tag;        // its bracket, "[image 2/3 ref=...]"
  std::string segment;    // its segment in the routed turn text (the attached label)
  std::string fallback;   // its segment for a head that cannot see it ("" = not made yet)
  std::string fileId;     // an upload-once provider file id ("" = sent inline)
  std::string fileHost;   // the provider that file lives at
};

class TurnImages {
 public:
  TurnImages() = default;
  TurnImages(const TurnImages&) = delete;
  TurnImages& operator=(const TurnImages&) = delete;
  ~TurnImages();

  std::vector<TurnImage> items;
  std::string nonce;   // per turn, random on the device: markers cannot be typed by a sender

  // The fallback for one photo: its segment for a head that cannot see it (the
  // captioner's machine transcription, or a typed "not read" label). Device: the
  // fallback captioner. Unset = the attached segment's tag with "(not read: ...)".
  std::function<std::string(const TurnImage&)> transcribe;
  // Frees the bytes (device: PSRAM). Run once, from the destructor.
  std::function<void(TurnImages&)> release;

  // A provider refused the image parts this turn (HTTP 400 naming the image): every
  // later request of the turn reads the transcription instead.
  bool rejected = false;
  // A request of this turn carried the photos as image parts: the head has them in
  // front of it, so image.look on one of them makes no second provider call.
  bool shown = false;
  // Is `ref` one of this turn's photos?
  bool holds(const std::string& ref) const;
  // Upload once (a provider file reference, the Anthropic Files API): `uploadOnce`
  // asks for it (the device's switch; on by default), `uploadTried` records that the
  // first round of the turn already tried, so a failed upload is not retried per round.
  bool uploadOnce = true;
  bool uploadTried = false;

  bool empty() const { return items.empty(); }
  size_t bytes() const;
  // The marker the renderer writes where image i's base64 belongs.
  std::string marker(size_t i) const;
  // `text` for a head that will not see the images: each photo's attached segment
  // replaced by its fallback segment (transcribed once, on first use). Idempotent:
  // blinding an already blind text changes nothing.
  std::string blindText(const std::string& text);
  // The fallback segment of image i (transcribed on first use).
  const std::string& fallbackOf(size_t i);
};

// Can a photo of `len` bytes join `set` as an image part? Returns its MIME type by its
// bytes, or "" with `why` set (the log words) when it goes to the caption step instead:
// the turn already holds kTurnImageMax photos, it is over kImagePartBytes, it would take
// the turn past kTurnImageBytes, its bytes are not a JPEG, PNG, WebP or GIF (a sender's
// declared type is never trusted), or it is an animated GIF.
const char* admitImage(const TurnImages& set, const uint8_t* bytes, size_t len, const char*& why);

// The image part for photo `label` (its bracket and its attached segment) over `bytes`.
TurnImage attachedImage(const MediaLabel& label, const uint8_t* bytes, size_t len, const std::string& mime);

// What the caption step (the fallback, a photo's record, image.look) billed since the
// last turn ended: filed into the next turn's per-host spend (lane T), one record per
// (provider, model), so a whole album's captions are one record and need no new key.
void addVisionSpend(const char* provider, const char* model, const TokenUsage& use);
// Move the pending caption spend into `spend` (the turn's record) and clear it.
void takeVisionSpend(TurnSpend& spend);

// ---- the slot (channel -> engine), single task ------------------------------------
// Offer the images of the turn about to be routed for `chatId` carrying `routedText`
// (replaces any older offer: a stale set is freed). nullptr/empty clears the slot.
void offerTurnImages(const std::string& chatId, const std::string& routedText,
                     std::unique_ptr<TurnImages> imgs);
// The offer for this chat's turn carrying `routedText`, consumed; nullptr when there
// is none or it was written for another turn (the slot is emptied either way).
std::unique_ptr<TurnImages> takeTurnImages(const std::string& chatId, const std::string& routedText);
// Free a set nobody took (the channel calls it once its turn returned).
void dropTurnImages();

// The images of the head turn in flight (nullptr = none). Set for the duration of a
// turn by ActiveTurnImages; the head loops seed their Transcript from it.
TurnImages* activeTurnImages();
class ActiveTurnImages {
 public:
  explicit ActiveTurnImages(TurnImages* imgs);
  ~ActiveTurnImages();
  ActiveTurnImages(const ActiveTurnImages&) = delete;
  ActiveTurnImages& operator=(const ActiveTurnImages&) = delete;
 private:
  TurnImages* prev_;
};

}  // namespace orch
}  // namespace nimbus
