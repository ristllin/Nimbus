#include "scenarios_history.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"   // kCoverageHeader

// The history fix's live legs (lane H, v5.1.2): run on one head per invocation (--host and
// --model pick it; the provider leg set PL in plan v3), fixture content only.
//   day         a 12-turn food day: every earlier exchange of the day rides each request
//               (at least 11 of 12 turns carry all of them), nothing is refused after round
//               0, and a cut is always named (a custom endpoint's flat request shows its
//               fitted history with the [LEFT OUT] note)
//   blueberry   the 15 Nov complaint, as fixture rows shaped like it: a blinded photo
//               estimate (blueberries 50 g), the truth (150 g), the owner's two follow-ups;
//               the fourth turn's request carries the estimate and the comparison, and the
//               reply names 50 g against 150 g
// LAB_HISTORY_PARTS picks the legs ("day,blueberry", default both). Nothing here prints a
// key or a request body.
namespace lab {
namespace histfix {

namespace {

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string envOr(const char* name, const char* dflt) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : std::string(dflt);
}

bool listed(const std::string& csv, const char* name) {
  return ("," + csv + ",").find(std::string(",") + name + ",") != std::string::npos;
}

// What a request carried: its prompt (history messages included) and its input.
std::string sent(const TurnRecord& t) { return t.systemPrompt + "\n" + t.turnInputs; }

// Refusals after round 0 in the harness log since `from` (a round that grew past its budget
// mid-loop, or a provider window refusal on a later round).
int laterRefusals(size_t from) {
  int n = 0;
  const std::vector<std::string>& log = LabRig::harnessLog();
  for (size_t i = from; i < log.size(); i++) {
    const std::string& l = log[i];
    const bool budget = has(l, "headloop: round ") && has(l, " B budget") && !has(l, "headloop: round 0 ");
    n += budget ? 1 : 0;
  }
  return n;
}

const char* const kDay[] = {
    "Breakfast: 250 g skyr, a banana and 30 g oats. Log it please.",
    "Coffee with 100 ml oat milk.",
    "What is my protein so far today?",
    "Lunch: 200 g grilled chicken breast, 180 g cooked rice, a side salad with olive oil.",
    "Snack: an apple and 20 g almonds.",
    "How many calories are left for today?",
    "Afternoon: a protein bar, the 20 g protein kind.",
    "Dinner plan: salmon, potatoes and green beans. Rough amounts for my target?",
    "Dinner was 160 g salmon, 250 g potatoes, 150 g green beans.",
    "A square of dark chocolate.",
    "Does the breakfast estimate still look right to you?",
    "Summarize today: total kcal and protein, meal by meal.",
};

void day(LabRig& rig, const Report& r) {
  const std::string chat = "histday";
  const size_t logFrom = LabRig::harnessLog().size();
  int whole = 0, named = 0, answered = 0;
  std::vector<std::string> owner;   // the day's earlier owner messages
  for (int i = 0; i < 12; i++) {
    const TurnRecord t = rig.say(chat, kDay[i]);
    answered += t.ok && !t.reply.empty();
    const std::string req = sent(t);
    bool all = true;
    for (const auto& o : owner) all = all && has(req, o.substr(0, 40));
    whole += all ? 1 : 0;
    named += (!all && (has(req, "[LEFT OUT TO FIT THIS REQUEST]") || has(req, agent::kCoverageHeader))) ? 1 : 0;
    r.note("turn " + std::to_string(i + 1) + ": " + (all ? "every earlier exchange sent" : "some left out") +
           ", " + std::to_string(t.tokensIn) + " in / " + std::to_string(t.cacheRead) + " cached (" + t.host + ")");
    owner.push_back(kDay[i]);
  }
  r.check(whole >= 11, "every earlier exchange of the day rode the request on " + std::to_string(whole) + " of 12 turns");
  r.check(whole + named == 12, "a turn that left anything out named it (" + std::to_string(named) + " named)");
  r.check(laterRefusals(logFrom) == 0, "no request was refused for its size after round 0");
  r.check(answered == 12, "every turn was answered (" + std::to_string(answered) + "/12)");
}

// The complaint's shape (15 Nov 15:55-16:00), fixture words only.
void blueberry(LabRig& rig, const Report& r) {
  const std::string chat = "histberry";
  rig.say(chat, "Hi. Today I am checking how well you estimate a bowl from a photo. Just acknowledge.");
  // 15:55: the photo and the blinded estimate (the device's rows: the media record, the
  // owner's row with the bare label, the assistant's estimate).
  const std::string ref = "b1ueb3rryb0w1000";
  LabRig::Row photo;
  photo.chat = chat;
  photo.kind = nimbus::orch::MsgKind::Image;
  photo.text = "[image 1/1 ref=" + ref + "] (seen; note: a bowl of yogurt with a granola sprinkle and blueberries "
               "around the rim)";
  photo.blob = "/mem/blobs/" + ref + ".jpg";
  rig.addRow(photo);
  LabRig::Row ask;
  ask.chat = chat;
  ask.text = "[image 1/1 ref=" + ref + "]\n\nThey wrote: Don't log yet. Estimate the quantities, I will confirm.";
  ask.tags = "from:Owner,att:" + ref;
  const std::string askId = rig.addRow(ask);
  LabRig::Row estimate;
  estimate.chat = chat;
  estimate.role = "assistant";
  estimate.text = "Photo estimate, locked before you confirm: yogurt 200 g (~120 kcal, 22 g protein), granola 40 g "
                  "(~180 kcal), blueberries 50 g (40-60 g, ~29 kcal). Total about 290 g, ~330 kcal, ~26 g protein.";
  estimate.tags = "fx:read,out:reply,turn:" + askId;
  rig.addRow(estimate);
  // 15:57: the truth and the comparison.
  const TurnRecord truth =
      rig.say(chat, "Here is the truth: 150 g blueberries, 200 g skyr, 50 g granola. Compare with your estimate.");
  const TurnRecord missed = rig.say(chat, "The yogurt was spot on, you missed the blueberries.");
  // 16:00: the question the owner asked.
  const TurnRecord why = rig.say(chat, "Do you understand why? It is easy to compare the grams you estimated "
                                       "with my weights. How did you miss that?");
  const std::string req = sent(why);
  r.check(has(req, "blueberries 50 g (40-60 g"), "the 15:55 estimate rides the 16:00 request");
  r.check(has(req, truth.reply.substr(0, 40)), "the 15:57 comparison rides the 16:00 request");
  r.check(has(why.reply, "50") && has(why.reply, "150"), "the reply names 50 g against 150 g");
  r.note("16:00 reply: " + why.reply.substr(0, 240));
  (void)missed;
}

}  // namespace

void run(LabRig& rig, const Report& r) {
  const std::string parts = envOr("LAB_HISTORY_PARTS", "day,blueberry");
  if (listed(parts, "day")) day(rig, r);
  if (listed(parts, "blueberry")) blueberry(rig, r);
}

}  // namespace histfix
}  // namespace lab
