// test_harness_claim_check - the engine's claim check: the final-answer check
// (one correction round, with the turn's own fields as pending effects) and the
// owner tail after apply, against the turn's real outcomes.
#include <unity.h>

#include <string>

#include "nimbus/harness/claim_check.h"

using agent::ClaimCheck;
using nimbus::orch::FxKind;

void setUp() {}
void tearDown() {}

static std::string turnJson(const std::string& reply, const std::string& extra = "") {
  std::string j = "{\"reply\":\"" + reply + "\"";
  if (!extra.empty()) j += "," + extra;
  return j + "}";
}

// Counter-test: a reply consistent with what the turn did costs zero extra rounds.
static void test_consistent_replies_need_no_correction() {
  struct Case { const char* reply; const char* fields; FxKind tool; bool useTool; };
  const Case cases[] = {
      {"Logged.", "\"mem_write\":[{\"content\":\"ran 5 km\",\"importance\":0.6}]", FxKind::Read, false},
      {"Noted in my notes.", "\"scratchpad\":{\"active\":\"plan\",\"short\":null,\"mid\":null,\"long\":null}",
       FxKind::Read, false},
      {"I have started two research sub-agents.",
       "\"session_ops\":[{\"op\":\"spawn\",\"task\":\"a\"},{\"op\":\"spawn\",\"task\":\"b\"}]", FxKind::Read, false},
      {"Reading it aloud now.", "\"device\":[{\"type\":\"tts\",\"text\":\"hi\"}]", FxKind::Read, false},
      {"The plan is saved as plan.md.", "", FxKind::FileWrite, true},
      {"I checked the timetable.", "", FxKind::Read, true},
      {"Here is the plan you asked for.", "", FxKind::Read, false},
  };
  for (const Case& c : cases) {
    ClaimCheck cc;
    cc.beginTurn();
    if (c.useTool) cc.noteTool(c.tool, false, c.tool == FxKind::FileWrite ? "saved plan.md (1 KB)" : "ok");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", cc.finalCheck(turnJson(c.reply, c.fields)).c_str(), c.reply);
    TEST_ASSERT_FALSE(cc.corrected());
  }
}

static void test_unbacked_claim_gets_one_correction_with_the_record() {
  ClaimCheck cc;
  cc.beginTurn();
  cc.noteTool(FxKind::MemWrite, false, "stored memory mb2accd88");
  const uint32_t before = ClaimCheck::correctedCount();
  const std::string fix =
      cc.finalCheck(turnJson("Run logged. The strength block is written and waiting whenever you want it."));
  TEST_ASSERT_EQUAL(0, (int)fix.find("[CLAIM CHECK]"));
  TEST_ASSERT_TRUE(fix.find("written and waiting") != std::string::npos);
  TEST_ASSERT_TRUE(fix.find("mb2accd88") != std::string::npos);
  TEST_ASSERT_TRUE(cc.corrected());
  TEST_ASSERT_EQUAL(before + 1, ClaimCheck::correctedCount());
  // A turn that cannot be read is the shape check's case, not this one's.
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck("{not json").c_str());
}

static void test_tail_after_apply_uses_the_real_outcomes() {
  // The final check saw a mem_write item (pending), so "Logged" passed; apply
  // then recorded the write as refused: the owner gets the correction line.
  ClaimCheck cc;
  cc.beginTurn();
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck(turnJson("Logged.", "\"mem_write\":[{\"content\":\"x\"}]")).c_str());
  cc.effects().add(FxKind::MemWrite, false);
  const uint32_t before = ClaimCheck::tailedCount();
  const std::string tail = cc.tailAfterApply("Logged.");
  TEST_ASSERT_EQUAL_STRING("\n\n(Correction: that has not been saved.)", tail.c_str());
  TEST_ASSERT_EQUAL(before + 1, ClaimCheck::tailedCount());
  // The same reply with the write stored: no tail.
  ClaimCheck ok;
  ok.beginTurn();
  ok.effects().add(FxKind::MemWrite, true, "m1");
  TEST_ASSERT_EQUAL_STRING("", ok.tailAfterApply("Logged.").c_str());
  TEST_ASSERT_EQUAL_STRING("", ok.tailAfterApply("  ").c_str());
}

static void test_scratchpad_durability_fresh_device_and_full_nvs() {
  const char* reply = "Put it in my notes, which are loaded into every single turn, reboot or not.";
  ClaimCheck fresh;   // fresh device: the save landed
  fresh.beginTurn();
  fresh.effects().add(FxKind::Scratch, true);
  TEST_ASSERT_EQUAL_STRING("", fresh.tailAfterApply(reply).c_str());
  ClaimCheck full;    // full NVS: kept in RAM only
  full.beginTurn();
  full.effects().add(FxKind::ScratchRamOnly, true);
  TEST_ASSERT_TRUE(full.tailAfterApply(reply).find("until the next restart") != std::string::npos);
}

static void test_hosted_tools_leave_sent_checked_and_changed_unchecked() {
  for (int hosted = 0; hosted < 2; hosted++) {
    for (const char* reply : {"I sent the email to Sam.", "I checked your calendar.", "I updated the event."}) {
      ClaimCheck cc;
      cc.beginTurn();
      cc.setHostedTools(hosted != 0);
      const bool flagged = !cc.finalCheck(turnJson(reply)).empty();
      TEST_ASSERT_EQUAL_MESSAGE(!hosted, flagged, reply);
    }
    // Device-side claims are checked either way.
    ClaimCheck cc;
    cc.beginTurn();
    cc.setHostedTools(hosted != 0);
    TEST_ASSERT_FALSE(cc.finalCheck(turnJson("Logged.")).empty());
  }
}

static void test_skip_spawned_leaves_the_spawn_class_to_the_shape_check() {
  ClaimCheck cc;
  cc.beginTurn();
  TEST_ASSERT_FALSE(cc.finalCheck(turnJson("Six sub-agents started this turn.")).empty());
  ClaimCheck skip;
  skip.beginTurn();
  TEST_ASSERT_EQUAL_STRING("", skip.finalCheck(turnJson("Six sub-agents started this turn."), true).c_str());
}

static void test_unsourced_fresh_result_needs_a_hedge() {
  ClaimCheck cc;
  cc.beginTurn();
  cc.noteFresh(0, 1);
  TEST_ASSERT_FALSE(cc.finalCheck(turnJson("Central line: no disruptions reported.")).empty());
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck(turnJson("The line status was not checked live.")).c_str());
  TEST_ASSERT_TRUE(cc.tailAfterApply("Central line: no disruptions reported.").find("not checked live") !=
                   std::string::npos);
  ClaimCheck src;
  src.beginTurn();
  src.noteFresh(1, 0);
  TEST_ASSERT_EQUAL_STRING("", src.finalCheck(turnJson("Central line: no disruptions reported.")).c_str());
}

static void test_figures_against_the_turn_sources() {
  ClaimCheck cc;
  cc.beginTurn();
  cc.setSources("[USER]\n[image] the label reads 20 grams of protein, 3 grams of net carbs", "");
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck(turnJson("| Bar (label) | 20 g protein | 3 g carbs |")).c_str());
  const std::string fix = cc.finalCheck(turnJson("| Bar (label) | 60 g | 200 | 21 g |"));
  TEST_ASSERT_TRUE(fix.find("60, 200, 21") != std::string::npos);
  // A tool result is a source too.
  cc.noteTool(FxKind::Read, false, "label lookup: 200 kcal, 60 g bar, 21 g protein");
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck(turnJson("| Bar (label) | 60 g | 200 | 21 g |")).c_str());
}

static void test_device_actions_back_their_claims_only_when_allowed() {
  FxKind k;
  TEST_ASSERT_TRUE(ClaimCheck::deviceActionEffect("{\"type\":\"tts\",\"text\":\"hi\"}", k));
  TEST_ASSERT_EQUAL((int)FxKind::Speak, (int)k);
  TEST_ASSERT_TRUE(ClaimCheck::deviceActionEffect("{\"type\":\"lights\",\"value\":\"off\"}", k));
  TEST_ASSERT_EQUAL((int)FxKind::Config, (int)k);
  TEST_ASSERT_FALSE(ClaimCheck::deviceActionEffect("{\"type\":\"nope\"}", k));
  TEST_ASSERT_FALSE(ClaimCheck::deviceActionEffect("not json", k));
}

// Live-eval shapes: a scratchpad save that landed backs "saved"; a RAM-only one
// does not (but still backs "noted"); a memory written with permanent:true
// through the tool backs "pinned" and "never decays".
static void test_scratch_outcomes_and_permanent_tool_writes() {
  const std::string saved = turnJson("Noted, it's saved on the device.");
  ClaimCheck ok;
  ok.beginTurn();
  ok.noteTool(FxKind::Scratch, false, "added to short");
  TEST_ASSERT_EQUAL_STRING("", ok.finalCheck(saved).c_str());
  ClaimCheck ram;
  ram.beginTurn();
  ram.noteTool(FxKind::Scratch, false, "added to short; kept until restart only (saving failed)");
  TEST_ASSERT_FALSE(ram.finalCheck(saved).empty());
  TEST_ASSERT_EQUAL_STRING("", ram.finalCheck(turnJson("Noted.")).c_str());
  TEST_ASSERT_EQUAL((int)nimbus::orch::kFxPermanent,
                    (int)ClaimCheck::toolCallFlags("memory.write", R"({"content":"x","permanent":true})"));
  TEST_ASSERT_EQUAL(0, (int)ClaimCheck::toolCallFlags("memory.write", R"({"content":"x"})"));
  TEST_ASSERT_EQUAL(0, (int)ClaimCheck::toolCallFlags("files.read", R"({"permanent":true})"));
  ClaimCheck pin;
  pin.beginTurn();
  pin.noteTool(FxKind::MemWrite, false, "stored memory m52ff9c86",
               ClaimCheck::toolCallFlags("memory.write", R"({"content":"x","permanent":true})"));
  TEST_ASSERT_EQUAL_STRING("", pin.finalCheck(turnJson("Saved as a permanent note. It is pinned, so it can never decay.")).c_str());
}

static void test_begin_turn_clears_everything() {
  ClaimCheck cc;
  cc.beginTurn();
  cc.noteTool(FxKind::MemWrite, false, "stored memory m1");
  cc.setHostedTools(true);
  cc.finalCheck(turnJson("I sent it, and the block is written and waiting."));
  TEST_ASSERT_TRUE(cc.corrected());
  cc.beginTurn();
  TEST_ASSERT_TRUE(cc.effects().empty());
  TEST_ASSERT_FALSE(cc.corrected());
  TEST_ASSERT_FALSE(cc.finalCheck(turnJson("I sent it.")).empty());   // hosted flag reset
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_consistent_replies_need_no_correction);
  RUN_TEST(test_unbacked_claim_gets_one_correction_with_the_record);
  RUN_TEST(test_tail_after_apply_uses_the_real_outcomes);
  RUN_TEST(test_scratchpad_durability_fresh_device_and_full_nvs);
  RUN_TEST(test_hosted_tools_leave_sent_checked_and_changed_unchecked);
  RUN_TEST(test_skip_spawned_leaves_the_spawn_class_to_the_shape_check);
  RUN_TEST(test_unsourced_fresh_result_needs_a_hedge);
  RUN_TEST(test_figures_against_the_turn_sources);
  RUN_TEST(test_device_actions_back_their_claims_only_when_allowed);
  RUN_TEST(test_scratch_outcomes_and_permanent_tool_writes);
  RUN_TEST(test_begin_turn_clears_everything);
  return UNITY_END();
}
