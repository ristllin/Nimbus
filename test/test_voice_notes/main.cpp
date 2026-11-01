// test_voice_notes - how a spoken reply is sized (nimbus/orch/voice_notes.h): the read
// window per synthesis, the voice-note split, the caps and the tool's words.
// The class: every planned note fits its cap and its read window at the slowest
// measured rate, no text is lost or reordered, a cut never splits a character, and
// text over the cap is refused with the numbers before anything is synthesized.
#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/net/tg_receipt.h"
#include "nimbus/orch/voice_notes.h"
#include "nimbus/stt_result.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static std::string nonSpace(const std::string& s) {
  std::string out;
  for (char c : s)
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') out += c;
  return out;
}

static void assertValidPlan(const std::string& text, const VoicePlan& p, const char* what) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", p.refusal.c_str(), what);
  TEST_ASSERT_TRUE_MESSAGE(!p.notes.empty() && p.notes.size() <= kVoiceMaxNotes, what);
  std::string joined;
  for (const std::string& n : p.notes) {
    TEST_ASSERT_FALSE_MESSAGE(n.empty(), what);
    TEST_ASSERT_TRUE_MESSAGE(textChars(n) <= kVoiceNoteMaxChars, what);
    TEST_ASSERT_FALSE_MESSAGE(((unsigned char)n[0] & 0xC0) == 0x80, what);   // starts on a character
    TEST_ASSERT_FALSE_MESSAGE(n[0] == ' ' || n.back() == ' ', what);
    joined += n;
  }
  TEST_ASSERT_TRUE_MESSAGE(nonSpace(joined) == nonSpace(text), what);   // nothing lost, nothing moved
}

static void test_chars_and_numbers() {
  TEST_ASSERT_EQUAL(0, (int)textChars(""));
  TEST_ASSERT_EQUAL(5, (int)textChars("hello"));
  TEST_ASSERT_EQUAL(5, (int)textChars("caf\xC3\xA9 "));
  TEST_ASSERT_EQUAL(2, (int)textChars("\xE4\xBD\xA0\xE5\xA5\xBD"));
  TEST_ASSERT_EQUAL_STRING("0", groupedNumber(0).c_str());
  TEST_ASSERT_EQUAL_STRING("600", groupedNumber(600).c_str());
  TEST_ASSERT_EQUAL_STRING("2,400", groupedNumber(2400).c_str());
  TEST_ASSERT_EQUAL_STRING("1,234,567", groupedNumber(1234567).c_str());
}

// The 22 Oct cut: about 1,500 characters in one 25 s window. Every note now fits its
// window at the slowest rate the bench has shown (20 KB/s for the base64 body, 360 B
// of MP3 a character, 5 s to the first byte), and the window never shrinks below the
// old 25 s or grows past 60 s.
static void test_the_read_window_fits_every_note() {
  TEST_ASSERT_EQUAL_UINT32(25000, ttsDeadlineMs(0));
  TEST_ASSERT_EQUAL_UINT32(25000, ttsDeadlineMs(100));
  TEST_ASSERT_EQUAL_UINT32(50000, ttsDeadlineMs(kVoiceNoteMaxChars));
  TEST_ASSERT_EQUAL_UINT32(60000, ttsDeadlineMs(kSpeakMaxChars));
  TEST_ASSERT_EQUAL_UINT32(60000, ttsDeadlineMs(100000));
  uint32_t prev = 0;
  for (size_t c = 0; c <= kSpeakMaxChars; c++) {
    const uint32_t d = ttsDeadlineMs(c);
    TEST_ASSERT_TRUE(d >= prev);   // never shorter for longer text
    prev = d;
    if (c > kVoiceNoteMaxChars) continue;
    const uint64_t b64 = (uint64_t)c * 360 * 4 / 3;
    const uint64_t needMs = 5000 + b64 * 1000 / (20 * 1024);
    TEST_ASSERT_TRUE_MESSAGE(d >= needMs, std::to_string(c).c_str());
  }
}

static void test_short_and_boundary_texts() {
  VoicePlan p = planVoiceNotes("  Good night.  ");
  TEST_ASSERT_EQUAL(1, (int)p.notes.size());
  TEST_ASSERT_EQUAL_STRING("Good night.", p.notes[0].c_str());
  const std::string atCap(kVoiceNoteMaxChars, 'a');
  TEST_ASSERT_EQUAL(1, (int)planVoiceNotes(atCap).notes.size());
  TEST_ASSERT_EQUAL(2, (int)planVoiceNotes(atCap + "b").notes.size());
  for (const char* empty : {"", "   ", "\n\t "}) {
    p = planVoiceNotes(empty);
    TEST_ASSERT_TRUE(p.notes.empty());
    TEST_ASSERT_EQUAL_STRING("the text is empty", p.refusal.c_str());
  }
}

static void test_over_the_cap_is_refused_with_the_numbers() {
  const std::string full(kVoiceNoteMaxChars * kVoiceMaxNotes, 'w');
  assertValidPlan(full, planVoiceNotes(full), "at the cap");
  const VoicePlan p = planVoiceNotes(full + "x");
  TEST_ASSERT_TRUE(p.notes.empty());
  TEST_ASSERT_EQUAL_STRING(
      "the text is 2,401 characters, over the 2,400-character limit for one voice reply "
      "(4 voice messages of up to 600 characters)",
      p.refusal.c_str());
}

static void test_cuts_prefer_sentence_ends() {
  // Three 280-character sentences: two notes, cut after a sentence, not mid-word.
  std::string s1 = "One" + std::string(272, 'a') + " end.", s2 = "Two" + std::string(272, 'b') + " end.",
              s3 = "Six" + std::string(272, 'c') + " end.";
  const std::string text = s1 + " " + s2 + " " + s3;
  const VoicePlan p = planVoiceNotes(text);
  assertValidPlan(text, p, "sentences");
  TEST_ASSERT_EQUAL(2, (int)p.notes.size());
  TEST_ASSERT_EQUAL_STRING((s1 + " " + s2).c_str(), p.notes[0].c_str());
  TEST_ASSERT_EQUAL_STRING(s3.c_str(), p.notes[1].c_str());
}

// No spaces at all (one long token, or CJK text where every byte run is 3 bytes): hard
// cuts at character boundaries, still within the note count.
static void test_hard_cuts_keep_characters_whole() {
  const std::string word(kVoiceNoteMaxChars * kVoiceMaxNotes, 'z');
  VoicePlan p = planVoiceNotes(word);
  assertValidPlan(word, p, "one word");
  TEST_ASSERT_EQUAL((int)kVoiceMaxNotes, (int)p.notes.size());
  std::string cjk;
  for (size_t i = 0; i < kVoiceNoteMaxChars * kVoiceMaxNotes; i++) cjk += "\xE4\xBD\xA0";
  p = planVoiceNotes(cjk);
  assertValidPlan(cjk, p, "cjk");
  TEST_ASSERT_EQUAL((int)kVoiceMaxNotes, (int)p.notes.size());
}

// The property over many texts: random words, sentence marks, line breaks and multibyte
// characters, every length up to the cap. A text at or under the cap is always planned
// (never refused), whatever its shape.
static void test_every_text_under_the_cap_is_planned_whole() {
  uint32_t seed = 0x5EED1234u;
  auto rnd = [&seed](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % n;
  };
  static const char* const kPieces[] = {"word", "a", "voice", "\xC3\xA9t\xC3\xA9", "\xE4\xBD\xA0\xE5\xA5\xBD",
                                        ".", "!", "?", ",", "\n", "supercalifragilistic"};
  for (int run = 0; run < 400; run++) {
    std::string text;
    const size_t want = 1 + rnd(kVoiceNoteMaxChars * kVoiceMaxNotes);
    while (textChars(text) < want) {
      text += kPieces[rnd(sizeof(kPieces) / sizeof(kPieces[0]))];
      if (rnd(3)) text += ' ';
    }
    while (textChars(text) > kVoiceNoteMaxChars * kVoiceMaxNotes) {
      size_t e = text.size() - 1;
      while (e > 0 && ((unsigned char)text[e] & 0xC0) == 0x80) e--;
      text.erase(e);
    }
    if (nonSpace(text).empty()) continue;
    assertValidPlan(text, planVoiceNotes(text), std::to_string(run).c_str());
  }
}

// One call has one time budget: the first note always starts, a later one only when its
// whole synthesis window plus the upload allowance still fits; a full-size note at the
// cap fits when the earlier ones went at an ordinary pace.
static void test_the_whole_call_has_one_time_budget() {
  TEST_ASSERT_TRUE(voiceNoteFits(kVoiceReplyBudgetMs * 2, 0, kVoiceNoteMaxChars));
  const uint32_t perNote = ttsDeadlineMs(kVoiceNoteMaxChars) + kVoiceUploadAllowMs;
  TEST_ASSERT_TRUE(voiceNoteFits(kVoiceReplyBudgetMs - perNote, 1, kVoiceNoteMaxChars));
  TEST_ASSERT_FALSE(voiceNoteFits(kVoiceReplyBudgetMs - perNote + 1, 1, kVoiceNoteMaxChars));
  // Four notes at the measured pace (5 s to the first byte, 15 s for the upload) fit.
  uint32_t t = 0;
  for (size_t i = 0; i < kVoiceMaxNotes; i++) {
    TEST_ASSERT_TRUE(voiceNoteFits(t, i, kVoiceNoteMaxChars));
    t += 20000;
  }
  TEST_ASSERT_EQUAL_STRING("the voice reply ran out of its 150 s time budget (the network is slow)",
                           voiceBudgetText().c_str());
}

// A clip is used only when it arrived whole. The 22 Oct clip: Mistral, 543,843 B
// decoded, the 25 s window passed before the closing quote: refused, with the size.
static void test_a_cut_clip_is_refused() {
  ClipRead incident;
  incident.mistral = true;
  incident.bytes = 543843;
  incident.writesOk = true;
  incident.endSeen = false;
  TEST_ASSERT_FALSE(clipWhole(incident));
  TEST_ASSERT_EQUAL_STRING("truncated at 543843 B (the speech audio did not arrive whole)",
                           clipTruncatedText(incident.bytes).c_str());
  // Every way a download can end, both wire shapes: whole only with the end seen,
  // every byte written, some audio, and (raw body) a stated length met exactly.
  for (int mistral = 0; mistral < 2; mistral++)
    for (int end = 0; end < 2; end++)
      for (int writes = 0; writes < 2; writes++)
        for (long cl : {-1L, 1000L, 999L})
          for (size_t bytes : {(size_t)0, (size_t)1000}) {
            ClipRead r;
            r.mistral = mistral;
            r.endSeen = end;
            r.writesOk = writes;
            r.contentLength = cl;
            r.bytes = bytes;
            const bool lengthOk = mistral || cl < 0 || (size_t)cl == bytes;
            TEST_ASSERT_EQUAL(end && writes && bytes > 0 && lengthOk, clipWhole(r));
          }
}

static void test_the_tool_words() {
  std::vector<VoiceNoteSent> one = {{812, 215433}};
  TEST_ASSERT_EQUAL_STRING("delivered to Telegram as a voice message (message_id 812, 210 KB)",
                           voiceDeliveredText(one).c_str());
  std::vector<VoiceNoteSent> three = {{811, 200000}, {812, 220000}, {813, 230000}};
  TEST_ASSERT_EQUAL_STRING(
      "delivered to Telegram as 3 voice messages (message_id 811, 812, 813; 635 KB in all)",
      voiceDeliveredText(three).c_str());
  TEST_ASSERT_EQUAL_STRING("delivered to Telegram as a voice message (message_id 7, 512 B)",
                           voiceDeliveredText({{7, 512}}).c_str());
  TEST_ASSERT_EQUAL_STRING("delivered to Telegram as 2 voice messages (message_id 7, 8; 1.4 MB in all)",
                           voiceDeliveredText({{7, 734003}, {8, 734003}}).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "the voice message did not reach Telegram: Bad Request: chat not found; nothing was "
      "delivered. Reply in text and say the voice message could not be sent",
      voiceFailedText({}, 1, "Bad Request: chat not found").c_str());
  const std::string partial = voiceFailedText({{811, 1000}}, 3, "truncated at 4096 B");
  TEST_ASSERT_EQUAL_STRING(
      "only 1 of the 3 voice messages reached Telegram (message_id 811); message 2 did not: "
      "truncated at 4096 B. Reply in text and say the voice message could not be sent for the rest",
      partial.c_str());
  TEST_ASSERT_EQUAL_STRING(
      "the text is 1,500 characters, over the 1,200-character limit for one spoken reply on the "
      "speaker; nothing was played. Read out a shorter summary, or reply in text",
      speakTooLongText(1500).c_str());
}

// What voice_reply.cpp makes of a failed send: "Telegram did not accept it (<reason>)".
static std::string reasonFor(bool transportOk, const std::string& transportErr, const std::string& body) {
  const nimbus::net::TgReceipt r = nimbus::net::parseTgSendAnswer(transportOk, transportErr, body);
  TEST_ASSERT_FALSE(r.ok);
  return "Telegram did not accept it (" + r.error + ")";
}

// QW6 (CODEREVIEW voice-model-seams): a failure that is only a missing or unreadable
// ANSWER (the whole file went out) must not say "nothing was delivered": Telegram may have
// accepted the note. The class: every transport state the receipt parser can report, and
// every other failure the voice path can name, goes through the one classifier.
static void test_an_unconfirmed_upload_is_not_called_undelivered() {
  const std::string noStatus = std::string("HTTP ") + "0";   // httpmp::post: the status line never came
  struct Row { const char* what; std::string reason; bool unconfirmed; };
  const Row rows[] = {
      {"no status line in the window", reasonFor(false, noStatus, ""), true},
      {"no transport text at all", reasonFor(false, "", ""), true},
      {"a 2xx answer that is not JSON", reasonFor(true, "", "<html>proxy</html>"), true},
      {"a 2xx answer without a message id", reasonFor(true, "", R"({"ok":true,"result":{}})"), true},
      {"Telegram refused with a description",
       reasonFor(false, "HTTP 400", R"({"ok":false,"error_code":400,"description":"Bad Request: chat not found"})"),
       false},
      {"a refusal without a body", reasonFor(false, "HTTP 403", ""), false},
      {"a server error", reasonFor(false, "HTTP 502", ""), false},
      {"a status that only starts with 0", std::string("Telegram did not accept it (HTTP 04)"), false},
      {"connect failed", std::string("Telegram did not accept it (") + nimbus::voice::kErrConnectFailed + ")", false},
      {"the slot was busy", std::string("Telegram did not accept it (") + nimbus::voice::kErrSlotBusy + ")", false},
      {"synthesis failed", "text-to-speech failed: the voice provider did not answer in time", false},
      {"the call budget", voiceBudgetText(), false},
      {"a refusal of the text", "the text is 2,500 characters, over the limit", false},
  };
  for (const Row& row : rows) {
    TEST_ASSERT_EQUAL_MESSAGE(row.unconfirmed, voiceReasonUnconfirmed(row.reason), row.what);
    for (size_t total : {(size_t)1, (size_t)3}) {
      for (size_t got = 0; got < total && got < 2; got++) {
        std::vector<VoiceNoteSent> sent;
        for (size_t i = 0; i < got; i++) sent.push_back({(int64_t)(811 + i), 1000});
        const std::string text = voiceFailedText(sent, total, row.reason);
        const bool saysMayNot = text.find(kVoiceMayNotHaveArrived) != std::string::npos;
        const bool saysNothing = text.find("nothing was delivered") != std::string::npos;
        TEST_ASSERT_EQUAL_MESSAGE(row.unconfirmed, saysMayNot, row.what);
        // "nothing was delivered" only when nothing was uploaded OR Telegram said no, and nothing arrived before.
        TEST_ASSERT_EQUAL_MESSAGE(!row.unconfirmed && got == 0, saysNothing, row.what);
        // The reason is kept (an unconfirmed one without voice_reply.cpp's wrapper).
        TEST_ASSERT_TRUE_MESSAGE(text.find(row.unconfirmed ? voiceReasonDetail(row.reason) : row.reason) !=
                                     std::string::npos,
                                 row.what);
      }
    }
  }
  TEST_ASSERT_EQUAL_STRING(
      "the voice message may not have arrived: Telegram did not confirm it (HTTP 0). Do not say it was "
      "delivered or that it failed; say it may not have arrived, and give the text in your reply",
      voiceFailedText({}, 1, "Telegram did not accept it (HTTP 0)").c_str());
  TEST_ASSERT_EQUAL_STRING(
      "only 1 of the 3 voice messages is confirmed by Telegram (message_id 811); message 2 may not have "
      "arrived: Telegram did not confirm it (HTTP 0). Say it may not have arrived and give the text of "
      "the rest in your reply",
      voiceFailedText({{811, 1000}}, 3, "Telegram did not accept it (HTTP 0)").c_str());
}

// QW6: the end-of-turn tts action's bound. Whole text when it fits; otherwise a prefix cut
// at a sentence end, else a space, else a character boundary - never mid-character, never
// longer than the bound, never empty for non-empty text.
static void test_speech_cut_keeps_whole_characters() {
  SpeechCut whole = cutForSpeech("  Hello there.  ");
  TEST_ASSERT_FALSE(whole.shortened);
  TEST_ASSERT_EQUAL_STRING("Hello there.", whole.text.c_str());
  TEST_ASSERT_EQUAL(12, (int)whole.spokenChars);
  TEST_ASSERT_EQUAL(12, (int)whole.totalChars);
  TEST_ASSERT_EQUAL_STRING("", cutForSpeech("  \n ").text.c_str());
  TEST_ASSERT_FALSE(cutForSpeech("").shortened);
  // Exactly at the bound: whole. One byte over: cut.
  TEST_ASSERT_FALSE(cutForSpeech(std::string(400, 'a')).shortened);
  TEST_ASSERT_TRUE(cutForSpeech(std::string(401, 'a')).shortened);
  // Sentence end preferred.
  std::string prose;
  while (prose.size() < 500) prose += "One sentence here. ";
  SpeechCut s = cutForSpeech(prose);
  TEST_ASSERT_TRUE(s.shortened);
  TEST_ASSERT_EQUAL_CHAR('.', s.text.back());
  TEST_ASSERT_TRUE(s.text.size() <= kTtsActionMaxBytes);
  // No sentence end: a word boundary, not a word cut in half.
  std::string words;
  while (words.size() < 500) words += "alpha beta gamma ";
  s = cutForSpeech(words);
  TEST_ASSERT_TRUE(s.shortened);
  TEST_ASSERT_TRUE(s.text.back() == 'a' && words.compare(0, s.text.size() + 1, s.text + " ") == 0);
  // A multi-byte run with no break at all: the hard cut stays on a character.
  std::string cjk;
  for (int i = 0; i < 300; i++) cjk += "\xE4\xBD\xA0";   // 900 bytes
  s = cutForSpeech(cjk);
  TEST_ASSERT_TRUE(s.shortened);
  TEST_ASSERT_EQUAL(399, (int)s.text.size());   // 133 characters, the last whole one that fits
  TEST_ASSERT_EQUAL(133, (int)s.spokenChars);
  TEST_ASSERT_EQUAL(300, (int)s.totalChars);
  TEST_ASSERT_EQUAL_STRING("(Note: the spoken version was shortened to the first 133 of its 300 characters.)",
                           speechShortenedNote(s).c_str());
  // The same property at every offset: 4-byte characters with a varying prefix.
  for (int pad = 0; pad < 8; pad++) {
    std::string e(pad, 'q');
    for (int i = 0; i < 200; i++) e += "\xF0\x9F\x98\x80";
    s = cutForSpeech(e);
    TEST_ASSERT_TRUE(s.text.size() <= 400 && !s.text.empty());
    TEST_ASSERT_EQUAL(0, (int)((s.text.size() - pad) % 4));   // whole emoji only
    TEST_ASSERT_TRUE(e.compare(0, s.text.size(), s.text) == 0);
  }
}

// QW6 (CODEREVIEW voice-model-seams): a raw body that ended by the server's close, with no
// stated length, is judged by its own framing. The class: with no length, a readable stream
// that stops mid-frame (or holds far less audio than its text takes to say) is cut; a
// stream the probe could not follow, a stated length, and Mistral's marker decide as before.
static void test_a_close_delimited_clip_is_judged_by_its_framing() {
  ClipRead base;
  base.endSeen = true;
  base.writesOk = true;
  base.bytes = 150000;
  base.shapeReadable = true;
  base.shapeEnds = true;
  base.clipMs = 20000;
  base.textChars = 400;
  TEST_ASSERT_TRUE(clipWhole(base));
  // Ends mid-frame: cut.
  ClipRead mid = base;
  mid.shapeEnds = false;
  TEST_ASSERT_FALSE(clipWhole(mid));
  TEST_ASSERT_EQUAL_STRING("truncated at 150000 B (the speech audio ended in the middle of a frame)",
                           clipCutText(mid).c_str());
  // Far less audio than 400 characters take (5 s at four times the normal rate): cut,
  // even on a frame boundary.
  ClipRead brief = base;
  brief.clipMs = 4900;
  TEST_ASSERT_FALSE(clipWhole(brief));
  TEST_ASSERT_EQUAL_STRING("truncated at 150000 B (only 4.9 s of audio for 400 spoken characters)",
                           clipCutText(brief).c_str());
  brief.clipMs = 5000;
  TEST_ASSERT_TRUE(clipWhole(brief));
  // Short texts have no duration floor; unknown duration (0) is not "short".
  ClipRead tiny = base;
  tiny.textChars = kClipDurationMinChars - 1;
  tiny.clipMs = 10;
  TEST_ASSERT_TRUE(clipWhole(tiny));
  ClipRead unknownMs = base;
  unknownMs.clipMs = 0;
  TEST_ASSERT_TRUE(clipWhole(unknownMs));
  // A stream the probe could not follow gets no verdict: as before.
  ClipRead lost = mid;
  lost.shapeReadable = false;
  TEST_ASSERT_TRUE(clipWhole(lost));
  TEST_ASSERT_EQUAL_STRING(clipTruncatedText(lost.bytes).c_str(), clipCutText(lost).c_str());
  // A stated length decides, whatever the probe saw (a trailer the probe does not know
  // must not refuse a clip the server says arrived whole); a short one is cut as before.
  ClipRead stated = mid;
  stated.contentLength = 150000;
  TEST_ASSERT_TRUE(clipWhole(stated));
  stated.contentLength = 150001;
  TEST_ASSERT_FALSE(clipWhole(stated));
  TEST_ASSERT_EQUAL_STRING(clipTruncatedText(stated.bytes).c_str(), clipCutText(stated).c_str());
  // Mistral's closing quote decides.
  ClipRead mistral = mid;
  mistral.mistral = true;
  TEST_ASSERT_TRUE(clipWhole(mistral));
  // The old conditions still hold with a clean probe: no end seen, no bytes, a failed write.
  for (int i = 0; i < 3; i++) {
    ClipRead r = base;
    if (i == 0) r.endSeen = false;
    if (i == 1) r.bytes = 0;
    if (i == 2) r.writesOk = false;
    TEST_ASSERT_FALSE(clipWhole(r));
  }
  // A flash write that failed is named as such, not as a cut stream.
  ClipRead flash = base;
  flash.writesOk = false;
  TEST_ASSERT_EQUAL_STRING("the speech audio could not be saved (a flash write failed after 150000 B)",
                           clipCutText(flash).c_str());
  TEST_ASSERT_EQUAL(0, (int)clipMinMs(0));
  TEST_ASSERT_EQUAL(5000, (int)clipMinMs(400));
  TEST_ASSERT_EQUAL(60000, (int)clipMinMs(4800));
}

// The reason without voice_reply.cpp's wrapper, so no sentence says "did not confirm it (Telegram
// did not accept it (HTTP 0))".
static void test_the_reason_detail_unwraps_the_telegram_wrapper() {
  TEST_ASSERT_EQUAL_STRING("HTTP 0", voiceReasonDetail("Telegram did not accept it (HTTP 0)").c_str());
  TEST_ASSERT_EQUAL_STRING("Bad Request: chat not found",
                           voiceReasonDetail("Telegram did not accept it (Bad Request: chat not found)").c_str());
  TEST_ASSERT_EQUAL_STRING("no answer", voiceReasonDetail("no answer").c_str());
  TEST_ASSERT_EQUAL_STRING("Telegram did not accept it (", voiceReasonDetail("Telegram did not accept it (").c_str());
  TEST_ASSERT_EQUAL_STRING("", voiceReasonDetail("").c_str());
}

// Only what a speech engine reads counts toward how much audio a text must produce: dividers,
// markdown marks, punctuation and emoji do not, so a text that is mostly such marks never gets
// a clip refused as "too short" (and a long symbol line has no floor at all).
static void test_only_spoken_characters_set_the_audio_floor() {
  TEST_ASSERT_EQUAL(0, (int)speechChars(""));
  TEST_ASSERT_EQUAL(5, (int)speechChars("Hello!"));            // the "!" is not read
  TEST_ASSERT_EQUAL(0, (int)speechChars(std::string(120, '-')));
  TEST_ASSERT_EQUAL(0, (int)speechChars("**  __  ..."));
  TEST_ASSERT_EQUAL(6, (int)speechChars("caf\xC3\xA9 ok"));    // the accented letter is read
}

static void test_emoji_and_symbols_are_not_spoken_but_scripts_are() {
  TEST_ASSERT_EQUAL(0, (int)speechChars("\xF0\x9F\x98\x80\xF0\x9F\x98\x80"));   // two emoji
  TEST_ASSERT_EQUAL(2, (int)speechChars("\xE4\xBD\xA0\xE5\xA5\xBD"));            // two CJK characters
  TEST_ASSERT_EQUAL(3, (int)speechChars("\xD0\x9F\xD1\x80\xD0\xB8"));             // three Cyrillic letters
  TEST_ASSERT_EQUAL(0, (int)speechChars("\x80\x80\xBF"));                          // stray continuation bytes
  // The floor follows: 100 divider characters are 0 spoken characters, no floor; 100 words' worth is.
  TEST_ASSERT_EQUAL(0, (int)clipMinMs(speechChars(std::string(100, '='))));
  TEST_ASSERT_EQUAL(5000, (int)clipMinMs(speechChars(std::string(400, 'a'))));
}

// What the owner/log is told when a clip is refused: a read window that ran out, and no audio at
// all, are the plain "did not arrive whole", not a claim about frames; tenths of a second for a
// short clip.
static void test_clip_cut_words_match_the_facts() {
  ClipRead base;
  base.endSeen = true;
  base.writesOk = true;
  base.bytes = 100000;
  base.shapeReadable = true;
  base.shapeEnds = false;
  base.textChars = 400;
  base.clipMs = 9000;
  TEST_ASSERT_EQUAL_STRING("truncated at 100000 B (the speech audio ended in the middle of a frame)",
                           clipCutText(base).c_str());
  ClipRead timedOut = base;   // the read window ran out: the stream did not reach its end
  timedOut.endSeen = false;
  TEST_ASSERT_EQUAL_STRING(clipTruncatedText(100000).c_str(), clipCutText(timedOut).c_str());
  ClipRead empty = base;
  empty.bytes = 0;
  TEST_ASSERT_EQUAL_STRING(clipTruncatedText(0).c_str(), clipCutText(empty).c_str());
  ClipRead brief = base;
  brief.shapeEnds = true;
  brief.clipMs = 800;
  TEST_ASSERT_EQUAL_STRING("truncated at 100000 B (only 0.8 s of audio for 400 spoken characters)",
                           clipCutText(brief).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_chars_and_numbers);
  RUN_TEST(test_the_read_window_fits_every_note);
  RUN_TEST(test_short_and_boundary_texts);
  RUN_TEST(test_over_the_cap_is_refused_with_the_numbers);
  RUN_TEST(test_cuts_prefer_sentence_ends);
  RUN_TEST(test_hard_cuts_keep_characters_whole);
  RUN_TEST(test_every_text_under_the_cap_is_planned_whole);
  RUN_TEST(test_the_whole_call_has_one_time_budget);
  RUN_TEST(test_a_cut_clip_is_refused);
  RUN_TEST(test_the_tool_words);
  RUN_TEST(test_an_unconfirmed_upload_is_not_called_undelivered);
  RUN_TEST(test_speech_cut_keeps_whole_characters);
  RUN_TEST(test_a_close_delimited_clip_is_judged_by_its_framing);
  RUN_TEST(test_the_reason_detail_unwraps_the_telegram_wrapper);
  RUN_TEST(test_only_spoken_characters_set_the_audio_floor);
  RUN_TEST(test_emoji_and_symbols_are_not_spoken_but_scripts_are);
  RUN_TEST(test_clip_cut_words_match_the_facts);
  return UNITY_END();
}
