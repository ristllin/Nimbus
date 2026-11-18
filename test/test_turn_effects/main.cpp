// test_turn_effects - the turn's effect ledger (turn_effects.h), the tts summary's record
// (speak_outcome.h), the voice send's receipt (voice_notes.h) and a sub-agent result's
// provenance (sub_provenance.h). These tests lived in test_orch_claims with the claim
// scanner; the scanner is gone (lane RB-Q1b), the ledger and its readers stay: the old
// engine's empty-answer check, the history row's fx: tag and apply read it, and RB-FX's
// stored ring is fed from it.
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/speak_outcome.h"
#include "nimbus/orch/sub_provenance.h"
#include "nimbus/orch/turn_effects.h"
#include "nimbus/orch/voice_notes.h"

using namespace nimbus::orch;

// A Telegram voice message send, as the voice tools record it.
static const uint8_t kVoiceSend = (uint8_t)(kFxTelegram | kFxVoice);

void setUp() {}
void tearDown() {}

static bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > s.size()) return false;
    for (size_t k = 1; k < n; k++)
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

static void test_fx_tag_is_bounded_and_names_whole_groups() {
  TurnEffects none;
  TEST_ASSERT_EQUAL_STRING("fx:none", none.fxTag().c_str());
  TurnEffects all;
  for (int k = 0; k < (int)FxKind::kCount; k++) all.add((FxKind)k, true);
  const std::string tag = all.fxTag();
  TEST_ASSERT_LESS_OR_EQUAL(24, (int)tag.size());
  TEST_ASSERT_EQUAL(0, (int)tag.find("fx:mem"));
  TEST_ASSERT_NOT_EQUAL(',', tag.back());
  TurnEffects failed;
  failed.add(FxKind::MemWrite, false);
  TEST_ASSERT_EQUAL_STRING("fx:none", failed.fxTag().c_str());
}

static void test_every_fx_kind_has_a_slug_and_a_summary_noun() {
  std::set<std::string> seen;
  for (int k = 0; k < (int)FxKind::kCount; k++) {
    const std::string slug = fxSlug((FxKind)k);
    TEST_ASSERT_NOT_EQUAL(0, (int)slug.size());
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(slug).second, slug.c_str());
    TurnEffects fx;
    fx.add((FxKind)k, true);
    TEST_ASSERT_NOT_EQUAL(0, (int)fx.summary().size());
  }
  TEST_ASSERT_EQUAL_STRING("unknown", fxSlug(FxKind::kCount));
}

static void test_tool_results_refine_their_effect_class() {
  struct Row { FxKind base; const char* out; FxKind want; const char* ref; };
  const Row rows[] = {
      {FxKind::MemWrite, "stored memory m1a2b3c4 (importance 0.8)", FxKind::MemWrite, "m1a2b3c4"},
      {FxKind::MemPin, "pin ok (m77)", FxKind::MemPin, "m77"},
      {FxKind::MemPin, "already pinned (m77), no change.", FxKind::MemPinNoop, "m77"},
      {FxKind::MemPin, "unpin ok (m77)", FxKind::MemUpdate, "m77"},
      {FxKind::MemPin, "delete ok (m77)", FxKind::MemDelete, "m77"},
      {FxKind::Scratch, "added to long", FxKind::Scratch, ""},
      {FxKind::Scratch, "added to long; kept until restart only (saving failed)", FxKind::ScratchRamOnly, ""},
      {FxKind::Scratch, "## SCRATCHPAD (your own working notes)", FxKind::Read, ""},
      {FxKind::Config, "updated retrieval_count", FxKind::Config, ""},
      {FxKind::Config, "retrieval_count=10 relevance_threshold=0.3", FxKind::Read, ""},
      {FxKind::FileWrite, "saved notes/plan.md (2 KB)", FxKind::FileWrite, "notes/plan.md"},
  };
  for (const Row& r : rows) {
    TurnEffects fx;
    recordToolEffect(fx, r.base, false, r.out);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)fx.size(), r.out);
    TEST_ASSERT_EQUAL_MESSAGE((int)r.want, (int)fx.at(0).kind, r.out);
    TEST_ASSERT_TRUE(fx.at(0).ok);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r.ref, fx.at(0).ref, r.out);
  }
  // A permanent write pins only when it stored; a duplicate reports the pin state.
  TurnEffects dupPinned;
  recordToolEffect(dupPinned, FxKind::MemWrite, false,
                   "duplicate of an existing memory (importance bumped; it is already pinned)", kFxPermanent);
  TEST_ASSERT_TRUE(dupPinned.anyOk(FxKind::MemPinNoop));
  TEST_ASSERT_FALSE(dupPinned.anyOkWith(FxKind::MemWrite, kFxPermanent));
  TurnEffects storedPerm;
  recordToolEffect(storedPerm, FxKind::MemWrite, false, "stored memory m1", kFxPermanent);
  TEST_ASSERT_TRUE(storedPerm.anyOkWith(FxKind::MemWrite, kFxPermanent));
  TurnEffects err;
  recordToolEffect(err, FxKind::MemWrite, true, "memory not stored: embedding failed");
  TEST_ASSERT_FALSE(err.at(0).ok);
  // A long ref keeps its tail (the file name), valid UTF-8.
  TurnEffects lng;
  lng.add(FxKind::FileWrite, true, "a-very-long-project-name/\xC3\xA9t\xC3\xA9-notes-final.md");
  TEST_ASSERT_TRUE(validUtf8(lng.at(0).ref));
  TEST_ASSERT_NOT_EQUAL(nullptr, std::strstr(lng.at(0).ref, "final.md"));
}

static void test_live_task_phrases() {
  const char* live[] = {"Check the Central line status", "Any delays on my train today?",
                        "Weather forecast for London", "latest news headlines", "What's the price of gold",
                        "Is the pharmacy open now", "Traffic on the M11 right now"};
  const char* notLive[] = {"Summarize the history of the Roman empire", "Draft a strength block",
                           "Research car seats for a newborn", "Explain transformers simply"};
  for (const char* t : live) TEST_ASSERT_TRUE_MESSAGE(liveTask(t), t);
  for (const char* t : notLive) TEST_ASSERT_FALSE_MESSAGE(liveTask(t), t);
}

static void test_provenance_property_over_asked_live_and_tool_runs() {
  // unverified = (connectorAsked && calls == 0) || (liveTask && toolRuns == 0)
  for (int asked = 0; asked < 2; asked++)
    for (uint16_t calls : {0, 1, 3})
      for (int live = 0; live < 2; live++)
        for (uint16_t runs : {0, 1, 3}) {
          const bool want = (asked && calls == 0) || (live && runs == 0);
          TEST_ASSERT_EQUAL(want, subResultUnverified(asked != 0, calls, live != 0, runs));
        }
  TEST_ASSERT_EQUAL_STRING(" [no tools ran]", freshProvenanceTag(0, 0).c_str());
  TEST_ASSERT_EQUAL_STRING(" [tools: 3, web: 1]", freshProvenanceTag(3, 1).c_str());
  TEST_ASSERT_EQUAL_STRING(" [tools: 2]", freshProvenanceTag(2, 0).c_str());
  TEST_ASSERT_EQUAL(0, (int)liveDataNotCheckedNote().find("[NOT CHECKED]"));
}

// CUM-782: the log-row flag a files.append call carries survives only when its own
// result counts a row that landed.
static void test_log_row_needs_a_counted_row() {
  struct Case { const char* out; bool kept; };
  const Case cases[] = {
      {"appended 2 rows to food/2026-10-27.csv (now 310 bytes)", true},
      {"appended 1 row to notes/run.log (now 40 bytes)", true},
      {"replaced 1 row, added 0 rows in food/2026-10-27.csv\n- lunch: 450 kcal", true},
      {"replaced 0 rows, added 2 rows in food/2026-10-27.csv", true},
      {"replaced 0 rows, added 0 rows in food/2026-10-27.csv (no change: the rows were already as sent)", false},
      {"appended 0 rows to food/2026-10-27.csv (now 310 bytes)", false},
      // A whole-file save of a text document that landed is the free-text log's
      // rewrite (the review r2 K1 shape): the save's own receipt keeps the flag.
      {"saved notes/plan.md (120 bytes)", true},
      {"saved notes/daylog.txt (214 bytes)", true},
  };
  for (const Case& c : cases) {
    TurnEffects fx;
    recordToolEffect(fx, FxKind::FileWrite, false, c.out, kFxLogRow);
    TEST_ASSERT_TRUE_MESSAGE(fx.anyOk(FxKind::FileWrite), c.out);
    TEST_ASSERT_EQUAL_MESSAGE(c.kept, fx.anyOkWith(FxKind::FileWrite, kFxLogRow), c.out);
  }
  TurnEffects failed;
  recordToolEffect(failed, FxKind::FileWrite, true, "SD open failed", kFxLogRow);
  TEST_ASSERT_FALSE(failed.anyOkWith(FxKind::FileWrite, kFxLogRow));
  TEST_ASSERT_EQUAL(3, fxAppendedRows("replaced 1 row, added 2 rows in x/y.csv"));
  TEST_ASSERT_EQUAL(0, fxAppendedRows("stored memory m1"));
}

// CUM-824: a sub-agent's tools by name, with counts, the idle ones at 0.
static void test_tool_tally_names_counts_and_idle_tools() {
  ToolTally t;
  t.offered("web_search");
  t.ran("notion_search");
  t.ran("notion_search");
  t.offered("notion_search");
  TEST_ASSERT_EQUAL_STRING("notion_search x2, web_search x0", t.render(95).c_str());
  ToolTally dirty;
  dirty.ran("bad,na]me\n[x");
  TEST_ASSERT_EQUAL_STRING("badnamex x1", dirty.render(95).c_str());
  ToolTally empty;
  empty.ran("");
  TEST_ASSERT_TRUE(empty.empty());
  TEST_ASSERT_EQUAL_STRING("", empty.render(95).c_str());
  // Bounded: at most kMaxNames names, a cut list says how many more, never past the cap,
  // whole names only.
  for (size_t cap : {20u, 40u, 60u, 95u}) {
    ToolTally many;
    for (int i = 0; i < 11; i++) many.ran("tool_number_" + std::to_string(i));
    const std::string r = many.render(cap);
    TEST_ASSERT_TRUE(r.size() <= cap);
    TEST_ASSERT_TRUE_MESSAGE(r.empty() || r.find(" more") != std::string::npos, r.c_str());
    TEST_ASSERT_TRUE(r.empty() || r.rfind("tool_number_0 x1", 0) == 0);
  }
  ToolTally lots;   // past kMaxNames, the runs still count, as other tools
  for (int i = 0; i < 11; i++) lots.ran("t" + std::to_string(i));
  lots.ran("t10");
  TEST_ASSERT_TRUE(lots.render(400).find(", other tools x4") != std::string::npos);
  char field[24];
  lots.copyTo(field);
  TEST_ASSERT_TRUE(std::strlen(field) <= 23 && std::string(field).find(" more") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING(" [ran: web_search x2]", toolRunsTag("web_search x2").c_str());
  TEST_ASSERT_EQUAL_STRING("", toolRunsTag("").c_str());
}

// What a tts device action's summary records: the speaker is a Speak, a Telegram delivery
// is a receipted voice Send, and every other summary spoke nothing. A tool error always
// records the failed Speak. The same through the device.control tool's "applied: " line.
static void test_a_tts_summary_records_what_really_happened() {
  { TurnEffects fx; recordSpeakSummary(fx, "tts ");
    TEST_ASSERT_EQUAL(1, (int)fx.size()); TEST_ASSERT_TRUE(fx.at(0).kind == FxKind::Speak && fx.at(0).ok); }
  { TurnEffects fx; recordSpeakSummary(fx, "tts(telegram message_id 41) ");
    TEST_ASSERT_EQUAL(1, (int)fx.size());
    TEST_ASSERT_TRUE(fx.at(0).kind == FxKind::Send && fx.at(0).ok);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(kVoiceSend | kFxReceipt), fx.at(0).flags);
    TEST_ASSERT_EQUAL_STRING("41", fx.at(0).ref);
    TEST_ASSERT_TRUE(voiceDelivered(fx)); }
  { TurnEffects fx; recordSpeakSummary(fx, "tts(telegram message_id ) ");   // no id: no receipt
    TEST_ASSERT_TRUE(fx.at(0).kind == FxKind::Send && !fx.at(0).ok);
    TEST_ASSERT_FALSE(voiceDelivered(fx)); }
  for (const char* sum : {"tts(not-spoken: the speaker volume is 0, so speech would play as silence) ",
                          "tts(voice-off) ", "tts(no-sink) ", "tts", ""}) {
    TurnEffects fx; recordSpeakSummary(fx, sum);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)fx.size(), sum);
    TEST_ASSERT_TRUE_MESSAGE(fx.at(0).kind == FxKind::Speak && !fx.at(0).ok, sum);
  }
  {   // an upload Telegram never confirmed may have arrived: a failed Speak, and a failed unconfirmed voice Send
    TurnEffects fx;
    recordSpeakSummary(fx, std::string("tts(not-spoken: ") + kTtsUnconfirmedLead +
                           "; the speaker is marked faulty; Telegram sent no confirmation (HTTP 0)) ");
    TEST_ASSERT_EQUAL(2, (int)fx.size());
    TEST_ASSERT_TRUE(fx.at(0).kind == FxKind::Speak && !fx.at(0).ok);
    TEST_ASSERT_TRUE(fx.at(1).kind == FxKind::Send && !fx.at(1).ok && (fx.at(1).flags & kFxUnconfirmed));
    TEST_ASSERT_FALSE(voiceDelivered(fx));
  }
  {   // a refused one is only a failed Speak
    TurnEffects fx;
    recordSpeakSummary(fx, "tts(not-spoken: the speaker is marked faulty; the Telegram voice message did not "
                           "arrive: Telegram did not accept it (Bad Request: chat not found)) ");
    TEST_ASSERT_EQUAL(1, (int)fx.size());
  }
  { TurnEffects fx; recordSpeakSummary(fx, "tts ", /*isError=*/true);
    TEST_ASSERT_TRUE(fx.at(0).kind == FxKind::Speak && !fx.at(0).ok); }
  { TurnEffects fx; recordSpeakSummary(fx, "tts(telegram message_id 41) ", /*isError=*/true);
    TEST_ASSERT_TRUE(fx.at(0).kind == FxKind::Speak && !fx.at(0).ok); }
  // A config summary that merely STARTS with the letters is a setting, not speech: "ttsOn cfg " is
  // a changed stored value (a Config that landed), "ttsOn cfg(no change) " changed nothing.
  for (const char* sum : {"ttsOn cfg ", "ttsOff cfg ", "ttsOn-refused(scheduled) cfg(no change) "}) {
    const bool landed = std::strstr(sum, "no change") == nullptr;
    TurnEffects fx;
    recordToolEffect(fx, FxKind::Config, false, std::string("applied: ") + sum);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)fx.size(), sum);
    TEST_ASSERT_TRUE_MESSAGE(fx.at(0).kind == FxKind::Config, sum);
    TEST_ASSERT_EQUAL_MESSAGE(landed, fx.at(0).ok, sum);
  }
  // Through device.control's tool result: the first line is the summary, a note may follow.
  { TurnEffects fx; recordToolEffect(fx, FxKind::Config, false, "applied: tts \n(Note: the spoken version was shortened.)");
    TEST_ASSERT_TRUE(fx.at(0).kind == FxKind::Speak && fx.at(0).ok); }
  { TurnEffects fx; recordToolEffect(fx, FxKind::Config, false, "applied: tts(telegram message_id 9) ");
    TEST_ASSERT_TRUE(voiceDelivered(fx)); }
  { TurnEffects fx; recordToolEffect(fx, FxKind::Config, false, "applied: tts(not-spoken: the speaker is marked faulty) ");
    TEST_ASSERT_TRUE(fx.at(0).kind == FxKind::Speak && !fx.at(0).ok); }
  { TurnEffects fx; recordToolEffect(fx, FxKind::Config, true, "applied: tts ");
    TEST_ASSERT_FALSE(fx.at(0).ok); }
}

// A provider's or Telegram's error text must never read as a delivery receipt: a failure whose
// reason says "message_id 7" (a hostile router, an odd description) records no delivered send.
static void test_a_failure_reason_cannot_forge_a_receipt() {
  const char* reasons[] = {"the router said message_id 7", "Telegram did not accept it (message_id 812 invalid)",
                           "Message_id 5 and message_id 6", "HTTP 0 message_id 9"};
  for (const char* why : reasons) {
    TurnEffects fx;
    recordToolEffect(fx, FxKind::Send, true, voiceFailedText({}, 1, why), kVoiceSend);
    TEST_ASSERT_FALSE_MESSAGE(voiceDelivered(fx), why);
  }
}

// A voice send whose upload went out but whose answer Telegram never gave usably may have
// arrived: it is recorded as unconfirmed, not as undelivered. A refusal and a failed
// synthesis are plain failures.
static void test_an_unconfirmed_voice_send_is_not_called_undelivered() {
  const std::string unconfirmed =
      voiceFailedText({}, 1, "Telegram did not accept it (HTTP 0)");
  const std::string refused =
      voiceFailedText({}, 1, "Telegram did not accept it (Bad Request: chat not found)");
  const std::string noClip = voiceFailedText({}, 1, "text-to-speech failed: no audio came back");
  TurnEffects a;
  recordToolEffect(a, FxKind::Send, true, unconfirmed, kVoiceSend);
  TEST_ASSERT_EQUAL(1, (int)a.size());
  TEST_ASSERT_TRUE(!a.at(0).ok && (a.at(0).flags & kFxUnconfirmed));
  for (const std::string& why : {refused, noClip}) {
    TurnEffects b;
    recordToolEffect(b, FxKind::Send, true, why, kVoiceSend);
    TEST_ASSERT_FALSE(b.at(0).ok);
    TEST_ASSERT_FALSE(b.at(0).flags & kFxUnconfirmed);
  }
  // A partial send whose later note is unconfirmed still records the delivered one.
  TurnEffects p;
  recordToolEffect(p, FxKind::Send, true, voiceFailedText({{811, 1000}}, 3, "Telegram did not accept it (HTTP 0)"),
                   kVoiceSend);
  TEST_ASSERT_TRUE(voiceDelivered(p));
  // The flag is only ever set on a voice send.
  TurnEffects t;
  recordToolEffect(t, FxKind::Send, true, unconfirmed, kFxTelegram);
  TEST_ASSERT_FALSE(t.at(0).flags & kFxUnconfirmed);
}


int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fx_tag_is_bounded_and_names_whole_groups);
  RUN_TEST(test_every_fx_kind_has_a_slug_and_a_summary_noun);
  RUN_TEST(test_tool_results_refine_their_effect_class);
  RUN_TEST(test_live_task_phrases);
  RUN_TEST(test_provenance_property_over_asked_live_and_tool_runs);
  RUN_TEST(test_log_row_needs_a_counted_row);
  RUN_TEST(test_tool_tally_names_counts_and_idle_tools);
  RUN_TEST(test_a_tts_summary_records_what_really_happened);
  RUN_TEST(test_a_failure_reason_cannot_forge_a_receipt);
  RUN_TEST(test_an_unconfirmed_voice_send_is_not_called_undelivered);
  return UNITY_END();
}
