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
// Native is decided per SERVING host at request time, never at intake: the channel
// keeps every photo it can (the limits below) whatever head is configured, so a turn
// that fails over from a blind host to a seeing one still sends the pixels, and a
// transcription is made only when a blind host actually renders the turn.
//
// image.look (an earlier photo, by its ref) puts that photo in front of the head
// again as an image (`looked`): Anthropic in the tool result, OpenAI and Mistral in a
// user message after the round's results. Only a blind host reads a caption of it
// (lookAnswer, made on first use). image.note lets the head write the photo's record
// itself in the turn it arrived (TurnImage::note); settledRow() is what the photo's
// stored row says once the turn is over, so no caption call is made for a photo the
// head saw and no history message is rewritten after it was sent.
//
// Single task by design (the poll task runs the whole turn), like turn_inbox.
// Arduino-free, host-tested (test_turn_images).
namespace nimbus {
namespace orch {

// Bounds, raised only after a bench heap trace (lane E: the 3- and 10-photo turns).
constexpr size_t kTurnImageMax = 10;                    // Telegram's album maximum
constexpr size_t kImagePartBytes = 512u * 1024;         // one photo (tg::kPhotoBudget)
constexpr size_t kTurnImageBytes = 2u * 1024u * 1024u;  // every photo of one turn, raw
                                                        // (looked photos included)
constexpr size_t kTurnLookMax = 4;                      // photos image.look shows again per turn
constexpr size_t kImageNoteMax = 270;                   // bytes of the head's note of one photo:
                                                        // with " (seen; note: ...)" it fits the
                                                        // history's 300-character photo row

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
// provider's own model list says decides when it says (the image-input lookup below:
// Mistral's and Anthropic's lists carry it per model, yes or no); a model the list has
// no row for (an alias, a new model) and a device with no list fall back to the name
// rule (a wrong guess is a 400/422 the provider path answers with the transcriptions):
//   anthropic: every claude- model; openai: gpt-4o, gpt-4.1, gpt-5 and newer, o3,
//   o4; mistral: mistralVisionByName (model_catalog.h: small, medium, large,
//   ministral, magistral, pixtral; not codestral, devstral, voxtral or the OCR models);
//   cumulo: the "<upstream>/<model>" it names; zai and custom: no (GLM vision
//   models are not on the device's list).
bool headTakesImages(const std::string& host, const std::string& model);

// The device's image-input lookup: 1 = the provider's stored list says `model` takes
// images, 0 = it says it does not, -1 = no row for it or the list does not say, -2 = the
// device holds no list of that provider. Only 1 and 0 decide; -1 and -2 leave it to the
// name rule. Unset (host tests, the hosted daemon): the name rule alone.
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
  std::string note;       // the head's own note of it (image.note, this turn; "" = none)
  std::string context;    // the sender's words with it (a blind host's reading uses them)
  bool retryRead = false; // its fallback reading failed for a passing reason (re-read later)
};

// A photo image.look put in front of the head again this turn.
struct LookedImage {
  TurnImage img;          // ref, mime, bytes (freed by the set's release, like the photos), tag
  std::string question;   // what the head asked: a blind host's reading answers it
  std::string answer;     // that reading ("" = not made yet)
};

class TurnImages {
 public:
  TurnImages() = default;
  TurnImages(const TurnImages&) = delete;
  TurnImages& operator=(const TurnImages&) = delete;
  ~TurnImages();

  std::vector<TurnImage> items;
  std::vector<LookedImage> looked;   // photos image.look showed again this turn
  std::string nonce;   // per turn, random on the device: markers cannot be typed by a sender
  std::string chatId;  // the chat the photos came from (their stored rows)

  // The fallback for one photo: its segment for a head that cannot see it (the
  // captioner's machine transcription, or a typed "not read" label). Device: the
  // fallback captioner. Unset = the attached segment's tag with "(not read: ...)".
  std::function<std::string(TurnImage&)> transcribe;
  // A blind host's reading of a looked photo: the answer to `question` (device: the
  // caption walk, CaptionKind::Question). Unset = a typed "not read" line.
  std::function<std::string(const TurnImage&, const std::string& question)> answer;
  // Frees the bytes (device: PSRAM). Run once, from the destructor.
  std::function<void(TurnImages&)> release;

  // A provider refused the image parts this turn (HTTP 400 naming the image, 413):
  // every later request to that host reads the transcription instead; another host
  // still gets the pixels.
  bool rejected = false;
  std::string rejectedBy;   // the host that refused ("" = every host: an old caller)
  // Does `host` refuse the parts this turn?
  bool refusedBy(const std::string& host) const { return rejected && (rejectedBy.empty() || rejectedBy == host); }
  // A request of this turn carried the photos as image parts: the head has them in
  // front of it, so image.look on one of them makes no second provider call.
  bool shown = false;
  // Some request of this turn that carried the photos as image parts was answered (a
  // 2xx: the head saw them), whatever host served the last round; never cleared.
  bool sent = false;
  // Is `ref` one of this turn's photos?
  bool holds(const std::string& ref) const;
  // The head's note for this turn's photo `ref` (image.note): one line, capped at
  // kImageNoteMax on a character boundary. false when `ref` is not one of this turn's
  // photos or the note is empty.
  bool setNote(const std::string& ref, const std::string& note);
  // The looked photo `ref` (-1 = none).
  int lookedIndex(const std::string& ref) const;
  // Upload once (a provider file reference, the Anthropic Files API): `uploadOnce`
  // asks for it (the device's switch; on by default), `uploadTried` records that the
  // first round of the turn already tried, so a failed upload is not retried per round.
  bool uploadOnce = true;
  bool uploadTried = false;

  bool empty() const { return items.empty(); }
  // Raw bytes held: the photos and the looked photos.
  size_t bytes() const;
  // The marker the renderer writes where image i's base64 belongs.
  std::string marker(size_t i) const;
  // The marker of looked photo j ("nlook:" + nonce + ":" + j + ":").
  std::string lookMarker(size_t j) const;
  // A blind host's text for looked photo j: its reading (made once, on first use), or
  // why there is none.
  const std::string& lookAnswer(size_t j);
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
// The same for a photo image.look shows again: at most kTurnLookMax per turn, the same
// per-photo and per-turn byte limits.
const char* admitLook(const TurnImages& set, const uint8_t* bytes, size_t len, const char*& why);

// image.look's result in the turn's record for a photo shown again. Each renderer
// writes its own words over it (the photo attached, or a blind host's reading), so the
// record names only the ref: "[image ref=<16 hex>] shown to you again".
std::string lookResultText(const std::string& ref);
// The ref such a result names ("" when `text` is not one).
std::string lookResultRef(const std::string& text);

// What photo i's stored row says once its turn is over. When an answered request
// showed it (sent): the head's note ("[image 1/2 ref=...] (seen; note: ...)"), or "seen
// in its turn" with no note. Otherwise a blind host's reading (its transcription or why
// there is none), or "" when the turn never showed or read it (the row keeps what
// intake stored).
std::string settledRow(const TurnImages& set, size_t i);
// What settledRow chose for photo i, in the log's words ("the head's note", "seen", ...).
const char* settledHow(const TurnImages& set, size_t i);
// Photo i's row is a blind host's reading that failed for a passing reason: read it
// again once a provider is open. Never for a photo the head saw (its row is its own).
bool settledNeedsReread(const TurnImages& set, size_t i);

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
// turn by ActiveTurnImages; the head loops seed their Transcript from it. An owner
// turn that brought no photo still gets an (empty) set, so image.look can show it an
// earlier photo.
TurnImages* activeTurnImages();
class ActiveTurnImages {
 public:
  // nullptr: the turn brought no photo; an empty set is made for image.look and freed
  // (its release hook run) when the turn ends.
  explicit ActiveTurnImages(TurnImages* imgs);
  ~ActiveTurnImages();
  ActiveTurnImages(const ActiveTurnImages&) = delete;
  ActiveTurnImages& operator=(const ActiveTurnImages&) = delete;
 private:
  TurnImages* prev_;
  std::unique_ptr<TurnImages> own_;
};

}  // namespace orch
}  // namespace nimbus
