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
// The characters of `s` a speech engine reads out: ASCII letters and digits, and 2- and 3-byte
// letters (accents, Cyrillic, CJK). Punctuation, dividers, markdown marks and 4-byte symbols
// (emoji) are not spoken, so they do not count toward how much audio a text must produce.
size_t speechChars(const std::string& s);

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
// A raw body with no stated length ends by the server's close, which a reset or a proxy cut
// looks just like. Its audio then says whether it is whole (clip_probe.h): the last MP3
// frame or the WAV data size is complete, and the audio is not far shorter than its text
// takes to say. The fields below carry what the probe found; a stream it could not follow
// (shapeReadable false) gets no verdict, so an odd but whole clip is never refused.
struct ClipRead {
  bool   mistral = false;
  bool   endSeen = false;        // the closing quote, or the server's close / the full length
  bool   writesOk = false;       // every decoded byte was written to flash
  long   contentLength = -1;     // the raw body's stated length (-1: none; unused for Mistral)
  size_t bytes = 0;              // audio bytes written
  bool     shapeReadable = false; // a ClipProbe followed the raw body's framing cleanly from the first byte
  bool     shapeEnds = false;     // ...to a whole frame / the declared data size, with audio
  uint32_t clipMs = 0;            // the audio the probe counted (0: unknown)
  size_t   textChars = 0;         // spoken characters (speechChars) of the text the clip voices
};
bool clipWhole(const ClipRead& r);
// The failure for a cut clip: "truncated at 543843 B (the speech audio did not arrive whole)".
std::string clipTruncatedText(size_t bytes);
// The failure for a clip clipWhole refused, with the reason the facts give: a flash write that
// failed, a stream that ended mid-frame, audio far shorter than its text, else the plain
// truncated text.
std::string clipCutText(const ClipRead& r);
// The least audio (ms) a text of `chars` SPOKEN characters (speechChars) can take, at 80
// characters a second, four times the normal speaking rate: audio under it is a cut, whatever
// the framing says. Texts under kClipDurationMinChars have no floor.
constexpr size_t kClipDurationMinChars = 40;
uint32_t clipMinMs(size_t chars);

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
// When the upload had gone out and only Telegram's answer was missing or unreadable
// (voiceReasonUnconfirmed), the words say the message MAY have arrived instead:
//   "the voice message may not have arrived: Telegram did not confirm it (<reason>). ..."
//   "only 1 of the 3 voice messages is confirmed by Telegram (message_id 811); message 2 may not have arrived: ..."
std::string voiceFailedText(const std::vector<VoiceNoteSent>& sent, size_t total,
                            const std::string& reason);

// True when a voice failure's reason is one where the note was fully uploaded and
// Telegram's receipt is what is missing: no status line inside the read window ("HTTP 0"),
// or an answer that was not a usable receipt. Telegram may well have accepted that note,
// so the failure text must not say "nothing was delivered". A refusal Telegram described
// ("Bad Request: chat not found"), a failed synthesis and a call that ran out of its
// budget are not unconfirmed: nothing was uploaded, or Telegram said no.
bool voiceReasonUnconfirmed(const std::string& reason);

// The words an unconfirmed voice failure carries (the claim ledger reads them to
// record the send as unconfirmed rather than refused).
constexpr const char* kVoiceMayNotHaveArrived = "may not have arrived";
// The reason without voice_reply.cpp's wrapper: "Telegram did not accept it (HTTP 0)" gives
// "HTTP 0"; any other text comes back as it is.
std::string voiceReasonDetail(const std::string& reason);

// reply.speak over kSpeakMaxChars: the refusal with the numbers.
std::string speakTooLongText(size_t chars);

// The end-of-turn `tts` action bounds what it voices (the synthesis and upload): at most
// this many BYTES of text. Text over it is cut, never mid-character, and the reply says so.
constexpr size_t kTtsActionMaxBytes = 400;

struct SpeechCut {
  std::string text;          // what to voice: the whole text, or its first part
  bool        shortened = false;
  size_t      spokenChars = 0;   // characters in `text`
  size_t      totalChars = 0;    // characters in the trimmed original
};

// Cut `text` to at most `maxBytes` bytes for speech: whole when it fits, else at the
// last sentence end in the back two thirds, else at the last space there, else at the
// last character boundary that fits. A multi-byte character is never split. Empty or
// blank text comes back as an empty, not shortened, cut.
SpeechCut cutForSpeech(const std::string& text, size_t maxBytes = kTtsActionMaxBytes);

// The reply line for a shortened cut:
//   "(Note: the spoken version was shortened to the first 212 of its 900 characters.)"
std::string speechShortenedNote(const SpeechCut& cut);

}  // namespace orch
}  // namespace nimbus
