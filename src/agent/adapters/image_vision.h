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
// the head reasons over the pixels. This adapter is the FALLBACK reader and the
// photo's memory writer, over the portable caption walk (nimbus/harness/
// vision_caption.h): keys from the store, the one device transport (its work slot,
// 4,096 B slices and exchange trace), the owner notice, and the bytes read into PSRAM.
// It is used only when native input is not possible (the head has no image input,
// the bytes could not be kept, a provider refused the image), for a photo's short
// record in memory, and for image.look.
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

// image.look (R1): look again at the earlier photo stored as `ref` and answer
// `question`. A caller that is not the owner may look only at photos of its own chat
// (`ns`). Returns the answer, or "" with `err` set (the tool's words).
std::string look(const std::string& ref, const std::string& question, const std::string& ns, bool owner,
                 std::string& err);

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

// Does the head the next turn runs read images (and is native input on)? The
// configured head and its model, by the one rule (nimbus::orch::headTakesImages).
// setNative(false) (RAM only, the bench) sends every photo through the caption step.
bool nativeHead();
void setNative(bool on);
bool native();

// Upload once (the Files API) for this boot: the bench compares it against inline
// photos; RAM only, on by default.
void setUploadOnce(bool on);
bool uploadOnce();

// The idle step of the poll task (between cycles, never inside a turn): one queued
// deletion of an uploaded photo, or one pending memory record. True when it ran one.
bool idleDue();
void idleStep();

// device.status visionLastErr: the last failure ("" when none since boot), as
// {"atS":<uptime s>,"provider":"openai","class":"billing","status":429}.
String lastErrJson();

}  // namespace vision
}  // namespace agent
