#include "nimbus/orch/voice_notes.h"

#include <cctype>

namespace nimbus {
namespace orch {

namespace {

bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

std::string trimmed(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && isSpace(s[b])) b++;
  while (e > b && isSpace(s[e - 1])) e--;
  return s.substr(b, e - b);
}

// The byte offset just past the first `chars` code points of s (s.size() when shorter).
size_t byteOfChar(const std::string& s, size_t chars) {
  size_t p = 0, n = 0;
  while (p < s.size()) {
    if (!isContinuation((unsigned char)s[p])) {
      if (n == chars) return p;
      n++;
    }
    p++;
  }
  return s.size();
}

enum class Cut { Sentence, Space, Hard };

// A sentence ends at '.', '!' or '?' followed by whitespace, or at a line break.
bool sentenceEndAt(const std::string& s, size_t p) {
  if (s[p] == '\n') return true;
  return (s[p] == '.' || s[p] == '!' || s[p] == '?') && p + 1 < s.size() && isSpace(s[p + 1]);
}

// Where a note that must stop by byte `limit` (a code-point boundary) can end: just past the
// last break of the wanted kind in the back two thirds, npos when there is none.
size_t breakBefore(const std::string& s, size_t limit, Cut kind) {
  const size_t floor = limit / 3;
  for (size_t p = limit; p-- > floor;) {
    if (kind == Cut::Sentence && sentenceEndAt(s, p)) return p + 1;
    if (kind == Cut::Space && isSpace(s[p])) return p;
  }
  return std::string::npos;
}

// ...else `limit` itself (a hard cut, which always fits).
size_t cutPoint(const std::string& s, size_t limit, Cut kind) {
  const size_t at = kind == Cut::Hard ? std::string::npos : breakBefore(s, limit, kind);
  return at == std::string::npos ? limit : at;
}

std::vector<std::string> split(const std::string& text, size_t noteMax, Cut kind) {
  std::vector<std::string> notes;
  std::string rest = text;
  while (!rest.empty()) {
    const size_t limit = byteOfChar(rest, noteMax);
    if (limit >= rest.size()) {
      notes.push_back(rest);
      break;
    }
    // Never past `limit`: a sentence end there is the last byte the note may keep.
    size_t cut = cutPoint(rest, limit, kind);
    if (cut == 0) cut = limit;
    notes.push_back(trimmed(rest.substr(0, cut)));
    rest = trimmed(rest.substr(cut));
  }
  return notes;
}

std::string idList(const std::vector<VoiceNoteSent>& sent) {
  std::string out;
  for (const VoiceNoteSent& n : sent) {
    if (!out.empty()) out += ", ";
    out += std::to_string((long long)n.messageId);
  }
  return out;
}

// A size for the model: "512 B", "210 KB", "1.4 MB".
std::string sizeText(size_t bytes) {
  if (bytes < 1024) return std::to_string((unsigned long long)bytes) + " B";
  if (bytes < 1024u * 1024u) return std::to_string((unsigned long long)((bytes + 512) / 1024)) + " KB";
  const unsigned long long tenths = (bytes * 10ull + 512ull * 1024ull) / (1024ull * 1024ull);
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
}

size_t totalBytes(const std::vector<VoiceNoteSent>& sent) {
  size_t b = 0;
  for (const VoiceNoteSent& n : sent) b += n.bytes;
  return b;
}

}  // namespace

size_t textChars(const std::string& s) {
  size_t n = 0;
  for (char c : s)
    if (!isContinuation((unsigned char)c)) n++;
  return n;
}

size_t speechChars(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    size_t len = 1;
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    const bool ascii = c < 0x80;
    const bool spoken = ascii ? std::isalnum(c) != 0 : len <= 3 && !isContinuation(c);
    n += spoken ? 1 : 0;
    i += len;
  }
  return n;
}

std::string groupedNumber(size_t n) {
  std::string digits = std::to_string((unsigned long long)n), out;
  for (size_t i = 0; i < digits.size(); i++) {
    if (i && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

uint32_t ttsDeadlineMs(size_t chars) {
  const uint64_t ms = 20000ull + 50ull * chars;
  if (ms < 25000) return 25000;
  return ms > 60000 ? 60000u : (uint32_t)ms;
}

bool voiceNoteFits(uint32_t elapsedMs, size_t noteIndex, size_t noteChars) {
  if (noteIndex == 0) return true;
  const uint64_t need = (uint64_t)elapsedMs + ttsDeadlineMs(noteChars) + kVoiceUploadAllowMs;
  return need <= kVoiceReplyBudgetMs;
}

std::string voiceBudgetText() {
  return "the voice reply ran out of its " + std::to_string((unsigned)(kVoiceReplyBudgetMs / 1000)) +
         " s time budget (the network is slow)";
}

uint32_t clipMinMs(size_t chars) {
  return chars < kClipDurationMinChars ? 0 : (uint32_t)(chars * 1000u / 80u);
}

// What a close-delimited raw body's own framing says (clip_probe.h). None when a stated length
// or Mistral's closing quote decides instead, or when the framing could not be followed (an odd
// but whole clip is never refused).
enum class Fault { None, MidFrame, TooShort };

static Fault framingFault(const ClipRead& r) {
  if (r.mistral || r.contentLength >= 0 || !r.shapeReadable) return Fault::None;
  if (!r.shapeEnds) return Fault::MidFrame;
  return r.clipMs > 0 && r.clipMs < clipMinMs(r.textChars) ? Fault::TooShort : Fault::None;
}

static std::string tenthsOfSeconds(uint32_t ms) {
  return std::to_string((unsigned long long)(ms / 1000u)) + "." + std::to_string((unsigned long long)(ms % 1000u / 100u));
}

bool clipWhole(const ClipRead& r) {
  if (r.bytes == 0 || !r.writesOk || !r.endSeen) return false;
  if (r.mistral) return true;
  if (r.contentLength >= 0) return (unsigned long long)r.contentLength == (unsigned long long)r.bytes;
  return framingFault(r) == Fault::None;
}

void noteMistralValue(ClipRead& r, size_t bytes, bool clean, bool wroteAll) {
  r.bytes = bytes;
  r.endSeen = clean;
  r.writesOk = wroteAll;
}

std::string clipTruncatedText(size_t bytes) {
  return "truncated at " + std::to_string((unsigned long long)bytes) +
         " B (the speech audio did not arrive whole)";
}

std::string clipCutText(const ClipRead& r) {
  const std::string bytes = std::to_string((unsigned long long)r.bytes);
  if (r.bytes > 0 && !r.writesOk)
    return "the speech audio could not be saved (a flash write failed after " + bytes + " B)";
  // The framing's reason only for a stream that reached its end: a read window that ran out, or
  // no audio at all, is the plain "did not arrive whole".
  if (r.bytes > 0 && r.endSeen) {
    const Fault f = framingFault(r);
    if (f == Fault::MidFrame) return "truncated at " + bytes + " B (the speech audio ended in the middle of a frame)";
    if (f == Fault::TooShort)
      return "truncated at " + bytes + " B (only " + tenthsOfSeconds(r.clipMs) + " s of audio for " +
             std::to_string((unsigned long long)r.textChars) + " spoken characters)";
  }
  return clipTruncatedText(r.bytes);
}

VoicePlan planVoiceNotes(const std::string& text, size_t noteMax, size_t maxNotes) {
  VoicePlan plan;
  const std::string t = trimmed(text);
  const size_t chars = textChars(t);
  if (chars == 0) {
    plan.refusal = "the text is empty";
    return plan;
  }
  if (noteMax == 0 || maxNotes == 0 || chars > noteMax * maxNotes) {
    plan.refusal = "the text is " + groupedNumber(chars) + " characters, over the " +
                   groupedNumber(noteMax * maxNotes) + "-character limit for one voice reply (" +
                   std::to_string((unsigned long long)maxNotes) + " voice messages of up to " +
                   groupedNumber(noteMax) + " characters)";
    return plan;
  }
  // Sentence cuts read best; when they need more notes than allowed, spaces, then hard
  // cuts, which always fit: the cap above guarantees ceil(chars / noteMax) <= maxNotes.
  for (Cut kind : {Cut::Sentence, Cut::Space, Cut::Hard}) {
    plan.notes = split(t, noteMax, kind);
    if (plan.notes.size() <= maxNotes) return plan;
  }
  return plan;
}

std::string voiceDeliveredText(const std::vector<VoiceNoteSent>& sent) {
  if (sent.size() == 1)
    return "delivered to Telegram as a voice message (message_id " + idList(sent) + ", " +
           sizeText(sent[0].bytes) + ")";
  return "delivered to Telegram as " + std::to_string((unsigned long long)sent.size()) +
         " voice messages (message_id " + idList(sent) + "; " + sizeText(totalBytes(sent)) +
         " in all)";
}

std::string voiceReasonDetail(const std::string& reason) {
  static const char kWrap[] = "Telegram did not accept it (";
  const size_t lead = sizeof(kWrap) - 1;
  if (reason.compare(0, lead, kWrap) == 0 && reason.size() > lead && reason.back() == ')')
    return reason.substr(lead, reason.size() - lead - 1);
  return reason;
}

bool voiceReasonUnconfirmed(const std::string& reason) {
  // The reasons parseTgSendAnswer / httpmp::post give when the whole file went out and
  // the answer did not come back usable (test_voice_notes pins each against the parser).
  static const char* const kUnreadable[] = {"(no answer)", "Telegram's answer could not be read",
                                            "Telegram's answer had no message id"};
  for (const char* r : kUnreadable)
    if (reason.find(r) != std::string::npos) return true;
  static const char kNoStatus[] = "HTTP 0";   // httpmp::post: the status line never arrived
  const size_t at = reason.find(kNoStatus);
  if (at == std::string::npos) return false;
  const size_t after = at + sizeof(kNoStatus) - 1;
  return after >= reason.size() || reason[after] < '0' || reason[after] > '9';
}

// A reason is provider or Telegram text. The ledger reads a receipt as "message_id <digits>" in a
// failure's words, so the reason must not be able to write one: "message_id" becomes "message id".
static std::string withoutReceiptWords(std::string reason) {
  static const char kMark[] = "message_id";
  for (size_t at = reason.find(kMark); at != std::string::npos; at = reason.find(kMark, at))
    reason[at + 7] = ' ';
  return reason;
}

std::string voiceFailedText(const std::vector<VoiceNoteSent>& sent, size_t total,
                            const std::string& rawReason) {
  const std::string reason = withoutReceiptWords(rawReason);
  const bool unconfirmed = voiceReasonUnconfirmed(reason);
  const std::string got = std::to_string((unsigned long long)sent.size());
  const std::string of = std::to_string((unsigned long long)total);
  const std::string next = std::to_string((unsigned long long)sent.size() + 1);
  if (unconfirmed) {
    const std::string never = std::string("it ") + kVoiceMayNotHaveArrived;
    const std::string detail = voiceReasonDetail(reason);
    if (sent.empty())
      return std::string("the voice message ") + kVoiceMayNotHaveArrived + ": Telegram did not confirm it (" +
             detail + "). Do not say it was delivered or that it failed; say " + never +
             ", and give the text in your reply";
    return "only " + got + " of the " + of + " voice messages " + (sent.size() == 1 ? "is" : "are") +
           " confirmed by Telegram (message_id " + idList(sent) + "); message " + next + " " +
           kVoiceMayNotHaveArrived + ": Telegram did not confirm it (" + detail +
           "). Say " + never + " and give the text of the rest in your reply";
  }
  const std::string tail = ". Reply in text and say the voice message could not be sent";
  if (sent.empty())
    return "the voice message did not reach Telegram: " + reason + "; nothing was delivered" + tail;
  return "only " + got + " of the " + of + " voice messages reached Telegram (message_id " +
         idList(sent) + "); message " + next + " did not: " + reason + tail + " for the rest";
}

SpeechCut cutForSpeech(const std::string& text, size_t maxBytes) {
  SpeechCut out;
  const std::string t = trimmed(text);
  out.totalChars = textChars(t);
  if (t.size() <= maxBytes) {
    out.text = t;
    out.spokenChars = out.totalChars;
    return out;
  }
  // The largest prefix that fits and ends on a character boundary.
  size_t limit = maxBytes;
  while (limit > 0 && isContinuation((unsigned char)t[limit])) limit--;
  size_t cut = breakBefore(t, limit, Cut::Sentence);
  if (cut == std::string::npos) cut = cutPoint(t, limit, Cut::Space);
  out.text = trimmed(t.substr(0, cut));
  out.shortened = true;
  out.spokenChars = textChars(out.text);
  return out;
}

std::string speechShortenedNote(const SpeechCut& cut) {
  return "(Note: the spoken version was shortened to the first " + groupedNumber(cut.spokenChars) +
         " of its " + groupedNumber(cut.totalChars) + " characters.)";
}

std::string speakTooLongText(size_t chars) {
  return "the text is " + groupedNumber(chars) + " characters, over the " +
         groupedNumber(kSpeakMaxChars) +
         "-character limit for one spoken reply on the speaker; nothing was played. Read "
         "out a shorter summary, or reply in text";
}

}  // namespace orch
}  // namespace nimbus
