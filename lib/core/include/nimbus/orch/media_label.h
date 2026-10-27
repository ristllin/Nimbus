#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// media_label - the one label for an inbound photo or file wherever the assistant
// reads it: the turn text, the episodic row and the history window. One format, one
// place, so the turn, the stored row, the history renderer and the image tools can
// never drift apart again (three formats existed for the same thing: "[image] ...",
// "[photo k of N]" and "[image ref=..]").
//
//   [image 2/3 ref=0123456789abcdef] a plate of pasta with ...
//   [image 3/3 ref=fedcba9876543210, sent 09:58] (couldn't be described)
//   [image 1/1] (could not be fetched: too large)
//   [file 1/2 ref=0011223344556677] menu.pdf
//   [image 1/2 ref=0123456789abcdef] (attached: the photo itself is in this message)
//   [image 2/2 ref=fedcba9876543210] (transcribed by openai) Nutrition facts ...
//   [image 1/1 ref=0123456789abcdef] (not read: anthropic out of credit (HTTP 429))
//   [image 1/1 ref=0123456789abcdef] (seen; note: receipt, 3 items, total 12.40)
//   [image 1/1 ref=0123456789abcdef] (seen in its turn; image.look shows it again)
//
// - k/N: the item's position among the media of its turn (album order), and the count.
// - ref: the item's content hash (the 16 lowercase hex characters of the blob store's
//   FNV-1a digest, nimbus/orch/blob_store.h). The same hash names the blob sidecar
//   and is what the files store dedupes on; it is absent when nothing was fetched.
// - sent: the original Telegram send time, only for a late or redelivered item, so a
//   replayed photo never reads as new (the time is the device's local time).
// - the class suffix after the bracket: a description, or why there is none. The
//   classes below are the receive path's and the image lane's (R1: the photo goes to
//   the head as an image; a transcription is the fallback and says so; a photo nobody
//   could read says why, one class per cause, never in the sender's words).
//
// Portable and Arduino-free (host-tested in test_media_label, which also checks that
// every label in the device's receive path comes from here).
namespace nimbus {
namespace orch {

enum class MediaKind : uint8_t { Image, File };

enum class MediaClass : uint8_t {
  Described,     // the description follows the bracket
  NotDescribed,  // kept, but nothing could describe it
  FetchFailed,   // the download failed; `detail` says why
  Stored,        // a file kept without a description (documents): `detail` is its name
  Attached,      // R1: the photo itself rides this turn's message as an image part
  Transcribed,   // another model's text from the photo (`by` names it): the fallback,
                 // and a photo's short record in memory; `detail` is the text
  NotRead,       // nobody could read it: `detail` is the typed reason (vision::ReadFail
                 // words: "anthropic out of credit (HTTP 429)", "too large", ...)
  Pending,       // a stored photo whose turn has not shown or read it yet (kept for
                 // image.look); its turn rewrites the row before it is ever history
  Seen,          // the head saw the photo itself in its turn and wrote no note
  Noted,         // the head saw it and wrote its own note (image.note): `detail`
  Count,         // sentinel: the class test walks every value below it
};

// Bytes of description one item may carry in a turn. A longer description is cut
// on a character boundary and marked, so ten photos stay a bounded turn.
constexpr size_t kMediaTextMax = 2048;
constexpr const char* kMediaCutMark = " [description cut]";

struct MediaLabel {
  MediaKind   kind = MediaKind::Image;
  int         k = 0;        // 1-based position among the turn's media (0: unnumbered)
  int         n = 0;        // media in the turn (0: unnumbered)
  std::string ref;          // validRef() or "" (no content)
  std::string sentAt;       // "09:58" for a late item, "" otherwise
  MediaClass  cls = MediaClass::Described;
  std::string detail;       // the description (Described), the reason (FetchFailed,
                            // NotRead), the file name (Stored) or the text (Transcribed)
  std::string by;           // Transcribed: the provider that wrote it ("" = unnamed)
};

// Exactly 16 lowercase hex characters (a blob hash).
bool validRef(const std::string& ref);

// The bracket alone: "[image 2/3 ref=0123456789abcdef, sent 09:58]".
std::string mediaTag(const MediaLabel& l);

// The whole segment: the bracket plus its class suffix. A description longer than
// kMediaTextMax is cut on a UTF-8 boundary and ends with kMediaCutMark.
std::string mediaSegment(const MediaLabel& l);
// The class suffix alone (what follows the bracket, its leading space included), so
// a holder of the bracket text can write the segment for another class.
std::string mediaSuffix(const MediaLabel& l);
// The suffix of an image the head sees itself (Attached), word for word.
extern const char kAttachedSuffix[];
// The suffix of a photo seen in its turn with no note (Seen), word for word.
extern const char kSeenSuffix[];

// A late or redelivered item: its Telegram date is more than kLateItemS before the
// device clock (false while the clock is unset, before 2020). Such an item carries
// its send time in the turn (the label's "sent", a text's "(sent 09:58)") and its
// episodic row is stamped with that date, so a replay never reads as new.
constexpr int64_t kLateItemS = 120;
bool lateItem(int64_t nowEpoch, int64_t date);

// For readers of stored text: the kind a label at the start of `text` names (true
// when `text` starts with one of the formats above, or the pre-label "[image]" and
// "[file]" heads older rows carry).
bool mediaLabelHead(const std::string& text, MediaKind& kind);

}  // namespace orch
}  // namespace nimbus
