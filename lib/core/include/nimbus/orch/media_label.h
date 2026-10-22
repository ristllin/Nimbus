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
//
// - k/N: the item's position among the media of its turn (album order), and the count.
// - ref: the item's content hash (the 16 lowercase hex characters of the blob store's
//   FNV-1a digest, nimbus/orch/blob_store.h). The same hash names the blob sidecar
//   and is what the files store dedupes on; it is absent when nothing was fetched.
// - sent: the original Telegram send time, only for a late or redelivered item, so a
//   replayed photo never reads as new (the time is the device's local time).
// - the class suffix after the bracket: a description, or why there is none. The
//   classes below are the receive path's; the image lane adds its typed failure
//   classes here, never a new format.
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
  std::string detail;       // the description (Described), the reason (FetchFailed) or
                            // the file name (Stored)
};

// Exactly 16 lowercase hex characters (a blob hash).
bool validRef(const std::string& ref);

// The bracket alone: "[image 2/3 ref=0123456789abcdef, sent 09:58]".
std::string mediaTag(const MediaLabel& l);

// The whole segment: the bracket plus its class suffix. A description longer than
// kMediaTextMax is cut on a UTF-8 boundary and ends with kMediaCutMark.
std::string mediaSegment(const MediaLabel& l);

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
