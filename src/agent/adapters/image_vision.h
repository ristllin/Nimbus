#pragma once
#include <Arduino.h>

#include <memory>

#include "nimbus/harness/vision_caption.h"
#include "nimbus/orch/turn_images.h"

namespace fs { class FS; }

// image_vision - the device side of photos (owner ruling R1).
//
// The main path sends a photo to the head model as an image part
// (nimbus/orch/turn_images.h): the bytes stay in one PSRAM buffer for the turn and
// whichever host serves a request decides, at that request, whether it sees the
// pixels or reads a transcription. image.look puts an earlier photo in front of the
// head again as an image the same way. This adapter is the FALLBACK reader over the
// portable caption walk (nimbus/harness/vision_caption.h): keys from the store, the
// one device transport (its work slot, 4,096 B slices and exchange trace), the owner
// notice, and the bytes read into PSRAM. A caption request is made only for a host
// that cannot see images (or when the bytes could not be kept, or a provider refused
// the image); a photo the head saw keeps the head's own note (image.note) as its
// record, written at the turn's end, never a second model's caption.
namespace agent {
namespace vision {

// Is a provider with a key open to read a photo now? (One that the park closed, out
// of credit or refused, is skipped with no request.)
bool available();

// Read the photo in `bytes` (`kind` says what to write). Returns the typed result.
CaptionResult read(const uint8_t* bytes, size_t len, const char* mime, const String& context,
                   CaptionKind kind = CaptionKind::Fallback, const String& question = String());

// Describe the image at `path`, reading through `sourceFs` (nullptr = LittleFS):
// the file read into PSRAM, then read(). "" on any failure (failWords() says why).
String describeImage(const char* path, const char* mime, ::fs::FS* sourceFs, const String& caption);

// Why the last describeImage/read gave no text, in the label's plain words ("openai
// out of credit (HTTP 429)", "too large", ...); "" after a success.
String failWords();

// image.look (R1): look again at the earlier photo stored as `ref`. In an owner turn
// the photo goes back in front of the head as an image (returns the result text the
// renderers turn into the photo); otherwise, or past the turn's photo limits, one
// caption request answers `question`. A caller that is not the owner may look only at
// photos of its own chat (`ns`). Returns "" with `err` set on failure (the tool's words).
std::string look(const std::string& ref, const std::string& question, const std::string& ns, bool owner,
                 std::string& err);

// image.note (R1): the head's own short note of a photo of the current message; it
// becomes that photo's stored row at the turn's end ("noted"), so no caption call is
// made for it. "" with `err` set when `ref` is not a photo of this turn.
std::string note(const std::string& ref, const std::string& text, std::string& err);

// Read a file into a new PSRAM buffer (nullptr when it is empty, over `maxBytes`, or
// PSRAM is short; `why` then says which, in the label's words). Free with free().
uint8_t* loadToPsram(const char* path, ::fs::FS& fsys, size_t maxBytes, size_t& len, const char*& why);

// The photo set of the turn being built (Telegram, one turn at a time): each photo
// the head can see is appended with its PSRAM bytes; the set is offered to the engine
// when the turn is routed (offer) and freed with it.
nimbus::orch::TurnImages* turnSet(bool create);
// Offer the set to the turn about to run for `chatId` with `routedText` (the engine
// takes it), or free it when no photo was attached.
void offerTurnSet(const String& chatId, const String& routedText);
// Free a set that never reached a turn (the page ended, the turn failed early).
void dropTurnSet();

// Does the configured head read images (IMGMODE? reports it)? Never the intake's
// decision: every photo is kept for the turn and the serving host decides per request.
// setNative(false) (RAM only, the bench) sends every photo through the caption step.
bool nativeHead();
void setNative(bool on);
bool native();

// Upload once (the Files API) for this boot: the bench compares it against inline
// photos; RAM only, on by default.
void setUploadOnce(bool on);
bool uploadOnce();

// The idle step of the poll task (between cycles, never inside a turn): a kept photo
// upload unused for an hour queued for deletion, one queued deletion of an uploaded
// photo, or one re-read of a photo a blind host could not read.
bool idleDue();
void idleStep();

// device.status visionLastErr: the last failure ("" when none since boot), as
// {"atS":<uptime s>,"provider":"openai","class":"billing","status":429}.
String lastErrJson();

}  // namespace vision
}  // namespace agent
