#pragma once
// voice_notes - how a spoken reply is sized: the read window one text-to-speech
// request gets, how long text is cut into Telegram voice notes, the caps, and the
// words the reply tools answer with.
//
// One synthesis is one clip, downloaded whole to the device's flash and then played
// or uploaded. Speech runs about 19 characters a second, and a Mistral MP3 is about
// 360 B a character (measured 22 Oct: 400 characters, 20.8 s, 145 KB, 3 s to the
// first byte). The old fixed 25 s read window cut a reply of about 1,500 characters
// (543,843 B, 22 Oct) and the cut clip was used as if it were whole. So each request
// now gets a window sized to its text, long Telegram text is split into notes of at
// most kVoiceNoteMaxChars, and text over the caps is refused with the numbers.
//
// Portable and host-tested (test_voice_notes).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nimbus {
namespace orch {

constexpr size_t kVoiceNoteMaxChars = 600;    // one Telegram voice note: ~31 s, ~220 KB
constexpr size_t kVoiceMaxNotes = 4;          // one reply.telegram call: 2,400 characters
constexpr size_t kSpeakMaxChars = 1200;       // one speaker clip: ~1 minute

// Characters (UTF-8 code points) in `s`.
size_t textChars(const std::string& s);

// "2,400" (thousands grouped, for the numbers in refusals).
std::string groupedNumber(size_t n);

// The read window for one synthesis of `chars` characters: 25 s up to 100
// characters, then 20 s plus 50 ms a character, at most 60 s.
uint32_t ttsDeadlineMs(size_t chars);

// One reply.telegram voice call holds the poll task (no Telegram polling, no other
// turn) while it runs, so the whole call has one time budget. A note starts only when
// its synthesis window plus an upload allowance still fits (the first always starts);
// otherwise the reply stops there and says how many messages arrived.
constexpr uint32_t kVoiceReplyBudgetMs = 150000;
constexpr uint32_t kVoiceUploadAllowMs = 20000;
bool voiceNoteFits(uint32_t elapsedMs, size_t noteIndex, size_t noteChars);
// The reason a reply stopped on its budget.
std::string voiceBudgetText();

// How one synthesis download ended. Mistral answers {"audio_data":"<base64>"}: the
// clip is whole only when the value's closing quote arrived. OpenAI and the router send
// the raw audio: whole only when the server closed the connection inside the window, or
// a stated Content-Length was met exactly. Either way every byte must have reached flash.
struct ClipRead {
  bool   mistral = false;
  bool   endSeen = false;        // the closing quote, or the server's close / the full length
  bool   writesOk = false;       // every decoded byte was written to flash
  long   contentLength = -1;     // the raw body's stated length (-1: none; unused for Mistral)
  size_t bytes = 0;              // audio bytes written
};
bool clipWhole(const ClipRead& r);
// The failure for a cut clip: "truncated at 543843 B (the speech audio did not arrive whole)".
std::string clipTruncatedText(size_t bytes);

struct VoicePlan {
  std::vector<std::string> notes;   // in order; empty when refused
  std::string refusal;              // why the text is not voiced ("" when planned)
};

// Cut `text` into notes of at most `noteMax` characters: at sentence ends where that
// fits the note count, else at spaces, else at character boundaries. Refuses empty
// text and text over noteMax * maxNotes characters.
VoicePlan planVoiceNotes(const std::string& text, size_t noteMax = kVoiceNoteMaxChars,
                         size_t maxNotes = kVoiceMaxNotes);

// One delivered note: Telegram's message id and the clip's size.
struct VoiceNoteSent {
  int64_t messageId = 0;
  size_t  bytes = 0;
};

// reply.telegram voice results (model-facing; the claim check reads "message_id"):
//   "delivered to Telegram as a voice message (message_id 812, 210 KB)"
//   "delivered to Telegram as 3 voice messages (message_id 811, 812, 813; 640 KB in all)"
std::string voiceDeliveredText(const std::vector<VoiceNoteSent>& sent);
//   "the voice message did not reach Telegram: <reason>; nothing was delivered. ..."
//   "only 1 of the 3 voice messages reached Telegram (message_id 811); message 2 did not: <reason>. ..."
std::string voiceFailedText(const std::vector<VoiceNoteSent>& sent, size_t total,
                            const std::string& reason);

// reply.speak over kSpeakMaxChars: the refusal with the numbers.
std::string speakTooLongText(size_t chars);

}  // namespace orch
}  // namespace nimbus
