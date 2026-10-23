#include "nimbus/orch/voice_notes.h"

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

// Where to end a note that must stop by byte `limit` (a code-point boundary): just past
// the last break of the wanted kind in the back two thirds, else at `limit` itself.
size_t cutPoint(const std::string& s, size_t limit, Cut kind) {
  const size_t floor = limit / 3;
  if (kind != Cut::Hard) {
    for (size_t p = limit; p-- > floor;) {
      if (kind == Cut::Sentence && sentenceEndAt(s, p)) return p + 1;
      if (kind == Cut::Space && isSpace(s[p])) return p;
    }
  }
  return limit;
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

bool clipWhole(const ClipRead& r) {
  if (r.bytes == 0 || !r.writesOk || !r.endSeen) return false;
  return r.mistral || r.contentLength < 0 || (unsigned long long)r.contentLength == (unsigned long long)r.bytes;
}

std::string clipTruncatedText(size_t bytes) {
  return "truncated at " + std::to_string((unsigned long long)bytes) +
         " B (the speech audio did not arrive whole)";
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

std::string voiceFailedText(const std::vector<VoiceNoteSent>& sent, size_t total,
                            const std::string& reason) {
  const std::string next = ". Reply in text and say the voice message could not be sent";
  if (sent.empty())
    return "the voice message did not reach Telegram: " + reason + "; nothing was delivered" + next;
  return "only " + std::to_string((unsigned long long)sent.size()) + " of the " +
         std::to_string((unsigned long long)total) + " voice messages reached Telegram (message_id " +
         idList(sent) + "); message " + std::to_string((unsigned long long)sent.size() + 1) +
         " did not: " + reason + next + " for the rest";
}

std::string speakTooLongText(size_t chars) {
  return "the text is " + groupedNumber(chars) + " characters, over the " +
         groupedNumber(kSpeakMaxChars) +
         "-character limit for one spoken reply on the speaker; nothing was played. Read "
         "out a shorter summary, or reply in text";
}

}  // namespace orch
}  // namespace nimbus
