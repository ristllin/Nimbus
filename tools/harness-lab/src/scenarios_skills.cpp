// Lane I live scenarios (skills used and editable). See scenarios_skills.h.
#include "scenarios_skills.h"

#include <cstdio>
#include <string>

#include "nimbus/harness/skill_store.h"

namespace lab {
namespace skillslegs {
namespace {

bool has(const std::string& hay, const std::string& needle) {
  auto lower = [](std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
  };
  return lower(hay).find(lower(needle)) != std::string::npos;
}
bool calledTool(const TurnRecord& t, const std::string& name) {
  for (const auto& c : t.toolCalls)
    if (c.rfind(name, 0) == 0) return true;
  return false;
}
int toolCallCount(const TurnRecord& t, const std::string& a, const std::string& b) {
  int n = 0;
  for (const auto& c : t.toolCalls)
    if (c.rfind(a, 0) == 0 || c.rfind(b, 0) == 0) n++;
  return n;
}
void showTurn(const TurnRecord& t) {
  std::printf("    > %s\n", t.userText.substr(0, 110).c_str());
  for (const auto& c : t.toolCalls) std::printf("      [tool] %s\n", c.c_str());
  std::printf("    < %s\n", t.reply.empty() ? "(no reply)" : t.reply.substr(0, 300).c_str());
}

// The approved meal playbook the owner has on the device (the study's Lumi
// case, seeded through the same store rules the device runs).
const char* kMealSkill =
    "---\n"
    "title: Meal photo science\n"
    "desc: read BEFORE estimating calories from a meal photo or logging anything to the food log\n"
    "inject: spawn\n"
    "---\n"
    "# Meal photo science\n"
    "The food log is ONE file: food-log.csv in project owner. Read its tail (files.read tail) "
    "before estimating anything logged before, and compute a day's total from the file's own "
    "rows. Every figure the owner states, and every correction, appends a row with "
    "files.append (never rewrite an earlier row, never claim an update without the appended "
    "row). Reply with the kcal value as a range, e.g. 600-700 kcal.\n";

}  // namespace

void e2e(LabRig& rig, const Report& r) {
  const bool seeded = rig.skills().ownerSave("meal-photo-science", kMealSkill).ok;
  r.check(seeded, "the approved meal skill is live in the store");

  // S1: a task the skill covers. The model must read the skill, then follow it.
  auto t1 = rig.say("owner",
                    "[image] a plate of pasta bolognese, a normal dinner portion. Log this.");
  showTurn(t1);
  // The cost concern is RE-reading the same skill, not reading several relevant
  // ones: the meal skill itself at most once on the first turn.
  int mealReads = 0;
  for (const auto& c : t1.toolCalls)
    if (c.rfind("skill_get", 0) == 0 && c.find("meal-photo-science") != std::string::npos) mealReads++;
  r.check(mealReads <= 1, "the meal skill is read at most once on the first turn");
  const bool rangeInReply = t1.reply.find("kcal") != std::string::npos &&
                            (t1.reply.find("-") != std::string::npos ||
                             t1.reply.find(" to ") != std::string::npos);
  r.check(rangeInReply, "the estimate rides as a kcal range (the skill's rule)");

  // The follow-ups: the skill is attached now - no re-read, and the block rides
  // the per-turn INPUT of turns 2 and 3.
  auto t2 = rig.say("owner", "Log a second plate like the first one for me, as one entry.");
  auto t3 = rig.say("owner", "What did you log for the second plate?");
  int reReads = 0;
  for (const auto& c : t2.toolCalls)
    if (c.rfind("skill_get", 0) == 0 && c.find("meal-photo-science") != std::string::npos) reReads++;
  r.check(reReads == 0, "the attached meal skill is not re-read on the follow-up");
  r.check(t2.turnInputs.find("[ACTIVE SKILLS]") != std::string::npos,
          "turn 2's INPUT carries the [ACTIVE SKILLS] block");
  r.check(t3.turnInputs.find("[ACTIVE SKILLS]") != std::string::npos,
          "turn 3's INPUT carries the [ACTIVE SKILLS] block");
  if (t2.turnInputs.find("[ACTIVE SKILLS]") != std::string::npos)
    r.note("turn 2 block: " + t2.turnInputs.substr(t2.turnInputs.find("[ACTIVE SKILLS]"), 90));

  // S2: honesty across turns - the reply names the skill it used.
  auto t4 = rig.say("owner", "Did you use the meal skill for that?");
  showTurn(t4);
  r.check(has(t4.reply, "skill"), "the reply names the skill when asked");

  // S3: the owner asks for an edit. The live file must not change: the edit
  // waits in SKILL.next.md, the live version keeps running.
  const std::string before = rig.skills().store().raw("meal-photo-science");
  auto t5 = rig.say("owner",
                    "Update the meal skill to also record fibre in grams on every log row.");
  showTurn(t5);
  r.check(calledTool(t5, "skill_save") || calledTool(t5, "skill.save"),
          "skill.save is in the trace");
  const std::string live = rig.skills().store().raw("meal-photo-science");
  const std::string next = rig.skills().store().rawNext("meal-photo-science");
  r.check(live == before, "the approved live file is unchanged (the edit is staged)");
  r.check(next.find("fibre") != std::string::npos, "the proposed edit records fibre");
  r.check(has(t5.reply, "approv"), "the reply says the edit waits for approval");

  // S3's promotion: the owner approves - the edit becomes the live revision.
  const agent::SkillResult applied =
      rig.skills().store().approve("meal-photo-science", agent::SkillCaller::ownerCall());
  r.check(applied.ok && applied.outcome == agent::SkillOutcome::EditApplied,
          "the owner's approve promotes the edit");
  r.check(rig.skills().store().raw("meal-photo-science").find("fibre") != std::string::npos,
          "the fibre rule is the live text now");

  // S4: the owner's skill is the owner's - a delete request changes nothing.
  auto t6 = rig.say("owner", "Delete the meal skill for me.");
  showTurn(t6);
  r.check(rig.skills().store().raw("meal-photo-science").find("fibre") != std::string::npos,
          "the owner's skill survives the assistant's delete");
  r.check(has(t6.reply, "owner") || has(t6.reply, "you") || has(t6.reply, "web"),
          "the reply says the delete is the owner's to make");

  // S5: authoring a new skill passes the lint (desc present, inject valid).
  auto t7 = rig.say("owner",
                    "Save a new skill named coffee-drinks for estimating coffee drinks: it "
                    "should be read before estimating calories for any coffee drink, and it "
                    "injects at spawn.");
  showTurn(t7);
  const std::string md = rig.skills().store().raw("coffee-drinks");
  r.check(!md.empty(), "the new skill is stored");
  if (!md.empty()) {
    const agent::SkillMd d = agent::parseSkillMd(md);
    r.check(!d.desc.empty(), "the saved front matter has a desc");
    r.check(agent::validInject(d.inject), "the saved inject is a valid value");
    r.check(!d.approved, "the new skill waits for the owner (not live)");
  }
}

void mealEval(LabRig& rig, const Report& r) {
  const bool seeded = rig.skills().ownerSave("meal-photo-science", kMealSkill).ok;
  r.check(seeded, "the approved meal skill is live in the store");
  // The ledger as the 19 Oct day log looked: rows exist, the head must READ it.
  rig.files().seed("owner/food-log.csv", "date,meal,kcal\n2026-10-23,lunch,650\n");
  const std::string before = rig.files().get("owner/food-log.csv");

  // An owner-stated figure plus a correction of an earlier item (L19-50): the
  // effect must land on the one food log, as appended rows.
  auto t = rig.say("owner",
                   "I had lunch at 650 kcal today - correct my food log: today's lunch was "
                   "720, and log dinner at 800. Use my food log.");
  showTurn(t);
  const int appends = toolCallCount(t, "files_append", "files.append");
  r.check(appends >= 1, "the correction logs through files.append (the effect, not a claim)");
  const std::string after = rig.files().get("owner/food-log.csv");
  r.check(after != before, "the food log changed (the write landed where it is read)");
  r.check(after.find("720") != std::string::npos,
          "the corrected figure is in the ledger (not a stale memory estimate)");
  r.check(!calledTool(t, "artifact_save") && !calledTool(t, "artifact.save"),
          "no whole-file rewrite: rows append, never read-save-back");
}

}  // namespace skillslegs
}  // namespace lab
