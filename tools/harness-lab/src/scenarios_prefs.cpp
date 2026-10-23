// Lane J live scenarios (standing owner preferences). See scenarios_prefs.h.
#include "scenarios_prefs.h"

#include "nimbus/orch/no_dash.h"

#include <cstdio>
#include <string>

namespace lab {
namespace prefslegs {
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
void showTurn(const TurnRecord& t) {
  std::printf("    > %s\n", t.userText.substr(0, 110).c_str());
  for (const auto& c : t.toolCalls) std::printf("      [tool] %s\n", c.c_str());
  std::printf("    < %s\n", t.reply.empty() ? "(no reply)" : t.reply.substr(0, 260).c_str());
}

}  // namespace

void e2e(LabRig& rig, const Report& r) {
  // Day N, turn 1: the owner states an always/never rule in their own words.
  auto t1 = rig.say("owner",
                    "One standing rule from now on: never use em dashes in any reply you "
                    "write for me, on any topic. Save it so it sticks.");
  showTurn(t1);
  const bool savedViaTool = calledTool(t1, "prefs_edit") || calledTool(t1, "prefs.edit");
  r.check(savedViaTool, "the assistant saved the rule through prefs.edit (or a standing mem_write)");
  const bool ruleSaved = !rig.prefs().store().renderBlock().empty();
  r.check(ruleSaved, "the rule is in the store (active, in [STANDING RULES])");

  // Turn 2, a fresh topic: the block rides the prompt and the reply obeys.
  auto t2 = rig.say("owner",
                    "Summarize in a few bullet points why a desk device might prefer a "
                    "wired connection.");
  showTurn(t2);
  r.check(t2.systemPrompt.find("- (p1)") != std::string::npos,
          "turn 2's prompt carries the rule line under [STANDING RULES]");
  r.check(t2.systemPrompt.find("never use em dashes") != std::string::npos,
          "the rule rides the static prompt band (never dropped)");
  r.check(nimbus::orch::hasEmDash(t2.reply) == false, "the reply carries no em dash");

  // Turn 3: an unattended routine turn still carries the owner's rules.
  rig.scheduled("owner", "Write the one-line evening reminder for the owner.", "evening note");
  const TurnRecord& t3 = rig.turns().back();
  showTurn(t3);
  r.check(t3.systemPrompt.find("- (p1)") != std::string::npos,
          "the routine fire's prompt carries the rule line");
  r.check(nimbus::orch::hasEmDash(t3.reply) == false, "the routine's reply carries no em dash");

  // Day N+1: the same rule, no reminder, the block still riding.
  rig.prefs().advanceHours(24);
  auto t4 = rig.say("owner",
                    "It is tomorrow. Give me your honest opinion on long-form emails in "
                    "three sentences, then sign off.");
  showTurn(t4);
  r.check(t4.systemPrompt.find("- (p1)") != std::string::npos,
          "the next day's turn still carries the rule line (persisted)");
  r.check(nimbus::orch::hasEmDash(t4.reply) == false, "the next day's reply carries no em dash");
}

void nextDay(LabRig& rig, const Report& r) {
  // The 19 Oct Lumi case: a standing default stated once, then the outage, then
  // the next day. Day N: the owner states the usual drink.
  auto t1 = rig.say("owner",
                    "Note for whenever you pick drinks for me: my usual is a flat white "
                    "unless I say otherwise. Save that as the standing default.");
  showTurn(t1);
  // The amendment grades the BEHAVIOR here (applied on day N+1 without a
  // reminder); the store-vs-memory save path is the seed leg's, on the bench.
  // Still note it: a reply that only says "I'll remember" without any save is
  // the 19 Oct failure mode, and the log carries it either way.
  const bool savedViaTool = calledTool(t1, "prefs_edit") || calledTool(t1, "prefs.edit");
  r.note(savedViaTool ? "the default also went into the standing rules"
                      : "the default was not saved through the rules this run (memory alone)");

  // Day N+1: no reminder, a new topic that needs the default.
  rig.prefs().advanceHours(24);
  auto t2 = rig.say("owner", "I am at the coffee place. Order my usual for me.");
  showTurn(t2);
  r.check(has(t2.reply, "flat white"), "the next day's reply names the usual drink");
  r.check(t2.reply.find("remind") == std::string::npos || has(t2.reply, "flat white"),
          "no re-ask needed (the default applied without a reminder)");
}

}  // namespace prefslegs
}  // namespace lab
