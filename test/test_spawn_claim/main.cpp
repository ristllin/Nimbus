// The spawn class of the claim check: replies that tell the owner sub-agents
// were started. Positives are shaped like the six 17-18 Oct phantom-spawn
// replies (synthetic text); negatives are the shapes that must never draw a
// correction (offers, questions, admissions, earlier work, quotes, code).
#include <unity.h>

#include <string>

#include "nimbus/orch/spawn_claim.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static void test_spawn_reports_are_claims() {
  for (const char* s : {
           "I have just started it properly now: six sub-agents are on it.",
           "Research started - three sub-agents are looking into the lineup.",
           "Spawning a quick research task, I'll report back.",
           "I've kicked off 4 background searches on the prices.",
           "Launched two agents to compare the caravans.",
           "Six research agents were started this turn.",
           "I'll start three sub-agents on this now.",
           "Done. Spawned 3 subagents: rail, weather and news.",
           "Here is the plan.\nI fired off a research job for each city.",
           "I've started the first half of the research now.",
           "I have just put the six sub-agents into this turn's spawn field, the part I got wrong before.",
           "I've restarted the agent for the publications; that result will land later.",
       })
    TEST_ASSERT_TRUE_MESSAGE(replyClaimsSpawn(s), s);
}

static void test_offers_questions_admissions_and_quotes_are_not_claims() {
  for (const char* s : {
           "Want me to start a sub-agent for that?",
           "Should I spawn three agents to compare them?",
           "I can launch agents if you like.",
           "The sub-agents did not start.",
           "The three research sub-agents I said I had started are not running.",
           "job0015 is still running; I started it earlier.",
           "I already spawned those agents yesterday.",
           "Starting tomorrow, the travel agent opens at nine.",
           "Paste this: `start three sub-agents now` into the console.",
           "You wrote \"I started the agents\" in your note.",
           "Here is the summary of the results.",
           "I put your notes in the scratchpad.",
           "I'll put the research task on hold for now.",
           "I'll put your research task in the queue for later.",
           "I put the sub-agents on hold until you reply.",
           "Research shows the seat is safer rear-facing.",
           "I started drafting the proposal while three agents from the vendor reviewed it.",
           "I started the draft, and two agents of the seller called.",
           "",
       })
    TEST_ASSERT_FALSE_MESSAGE(replyClaimsSpawn(s), s);
}

static void test_correction_lines_are_plain_and_dash_free() {
  for (const char* s : {kSpawnNotStartedLine, kSpawnSomeNotStartedLine}) {
    const std::string line = s;
    TEST_ASSERT_TRUE(line.find("\xE2\x80\x94") == std::string::npos);   // no em dash
    TEST_ASSERT_TRUE(line.find("did not start") != std::string::npos);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_spawn_reports_are_claims);
  RUN_TEST(test_offers_questions_admissions_and_quotes_are_not_claims);
  RUN_TEST(test_correction_lines_are_plain_and_dash_free);
  return UNITY_END();
}
