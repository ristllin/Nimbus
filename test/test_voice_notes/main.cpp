// test_voice_notes - how a spoken reply is sized (nimbus/orch/voice_notes.h): the read
// window per synthesis, the voice-note split, the caps and the tool's words.
// The class: every planned note fits its cap and its read window at the slowest
// measured rate, no text is lost or reordered, a cut never splits a character, and
// text over the cap is refused with the numbers before anything is synthesized.
#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/voice_notes.h"

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
  return UNITY_END();
}
