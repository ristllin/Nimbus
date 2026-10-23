#include "scenarios_g1.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "nimbus/harness/dream.h"    // buildEpisodicDigest - the device's digest builder
#include "nimbus/orch/compact.h"     // capSummary / kChatSummaryMax (L19-27)

namespace lab {
namespace caching {

namespace {

// A synthetic chat of the shape the device keeps for weeks: meals logged, small
// questions, plans. No real person's data.
struct Exchange {
  const char* user;
  const char* assistant;
};
const Exchange kSeed[] = {
    {"morning! oats with banana and a coffee for breakfast",
     "Morning. Logged breakfast: oats with banana and a coffee, about 390 kcal and 11 g protein. "
     "Today so far: 390 kcal, 11 g protein."},
    {"what's a quick lunch with more protein?",
     "A tuna and bean salad (about 450 kcal, 35 g protein) or two eggs on rye with spinach (about "
     "380 kcal, 22 g protein). Both take under ten minutes."},
    {"had the tuna salad", "Logged lunch: tuna and bean salad, about 450 kcal and 35 g protein. "
                           "Today so far: 840 kcal, 46 g protein."},
    {"remind me what the library book is called?",
     "You mentioned 'The Overstory' last week, due back on the 30th."},
    {"thanks. snack: an apple and some almonds",
     "Logged a snack: an apple and about 20 g of almonds, roughly 210 kcal and 5 g protein. Today so "
     "far: 1050 kcal, 51 g protein."},
    {"is it going to rain this afternoon?",
     "I can check a forecast if you want - say the word and I will look it up for your area."},
    {"no need. dinner was salmon, rice and broccoli",
     "Logged dinner: salmon with rice and broccoli, about 640 kcal and 42 g protein. Today: 1690 kcal "
     "and 93 g protein, so you are 210 kcal and 47 g protein under your targets."},
    {"good night", "Good night. Everything for today is logged."},
    {"new day. yogurt with berries and granola",
     "Logged breakfast: yogurt with berries and granola, about 350 kcal and 18 g protein."},
    {"can you add milk and coffee beans to the shopping list?",
     "Added milk and coffee beans to your shopping list. It now holds: milk, coffee beans, rye bread, "
     "spinach."},
    {"lunch was a chicken wrap", "Logged lunch: chicken wrap, about 520 kcal and 34 g protein. Today so "
                                 "far: 870 kcal, 52 g protein."},
    {"how many steps did I say I walked yesterday?",
     "You said about 9,400 steps yesterday evening."},
    {"dinner: veggie chili, one bowl",
     "Logged dinner: one bowl of vegetable chili, about 480 kcal and 21 g protein. Today: 1350 kcal and "
     "73 g protein."},
    {"what should I cook tomorrow?",
     "Something quick and high in protein: a lentil and spinach curry, or baked cod with potatoes. "
     "Both use what is already on your shopping list."},
    {"lentil curry it is", "Noted - lentil and spinach curry tomorrow. I will remind you to soak "
                           "nothing: red lentils cook in 20 minutes."},
    {"thanks, that's all for today", "You're welcome. Sleep well."},
};
const char* const kLive[] = {"ok", "thanks", "what's my protein so far today?", "and calories?", "ok",
                             "thanks", "ok", "noted"};

int envInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::atoi(v) : fallback;
}

void seed(LabRig& rig, const std::string& chat, int n) {
  for (int i = 0; i < n && i < (int)(sizeof(kSeed) / sizeof(kSeed[0])); i++) {
    LabRig::Row u;
    u.chat = chat;
    u.text = kSeed[i].user;
    u.tags = "from:Owner";
    const std::string id = rig.addRow(u);
    LabRig::Row a;
    a.chat = chat;
    a.role = "assistant";
    a.text = kSeed[i].assistant;
    a.tags = "fx:none,out:reply,turn:" + id;
    rig.addRow(a);
  }
}

}  // namespace

double inputEquivalent(uint32_t in, uint32_t cacheRead, uint32_t cacheWrite, uint32_t cacheWrite1h,
                       bool excludesCache) {
  const double w1h = cacheWrite1h < cacheWrite ? cacheWrite1h : cacheWrite;
  const double w5m = cacheWrite - w1h;
  double fresh = in;
  if (!excludesCache) fresh = in > cacheRead + cacheWrite ? in - cacheRead - cacheWrite : 0;
  return fresh + 0.1 * cacheRead + 1.25 * w5m + 2.0 * w1h;
}

void cacheGaps(LabRig& rig, const history::Report& r) {
  const int gapS = envInt("NIMBUS_G1_GAP_S", 60);
  const int turns = envInt("NIMBUS_G1_TURNS", 4);
  const std::string chat = "gaps";
  seed(rig, chat, envInt("NIMBUS_G1_SEED_TURNS", 16));
  std::string series;
  int answered = 0;
  const int nLive = (int)(sizeof(kLive) / sizeof(kLive[0]));
  for (int k = 0; k < turns; k++) {
    if (k) std::this_thread::sleep_for(std::chrono::seconds(gapS));
    const TurnRecord t = rig.say(chat, kLive[k % nLive]);
    answered += t.ok && !t.reply.empty();
    const bool excl = t.host.rfind("anthropic", 0) == 0;
    const double ie = inputEquivalent(t.tokensIn, t.cacheRead, t.cacheWrite, 0, excl);
    char line[256];
    std::snprintf(line, sizeof line, "turn %d (%s): in=%u cacheRead=%u cacheWrite=%u out=%u -> %.0f input-equivalent",
                  k + 1, t.host.c_str(), t.tokensIn, t.cacheRead, t.cacheWrite, t.tokensOut, ie);
    r.note(line);
    char cell[32];
    std::snprintf(cell, sizeof cell, "%s%.0f", k ? "," : "", ie);
    series += cell;
  }
  r.note("cache-gaps gap=" + std::to_string(gapS) + "s input-equivalent per turn: [" + series + "]");
  r.check(answered == turns, "every turn was answered (" + std::to_string(answered) + "/" +
                                 std::to_string(turns) + ")");
}

// L19-27 live: one fold on the pinned head over a recorded long history - the
// seeded day, three weeks of it. The REAL engine runFold (the device's
// compactTick call, minus its device-only storage) must produce a summary that
// stays inside the stated budget (capSummary, kChatSummaryMax) and keeps the
// durable section headers a tail-cut would have eaten (2, 4, 5).
void foldBudget(LabRig& rig, const history::Report& r) {
  const std::string chat = "foldleg";
  std::vector<nimbus::orch::EpisodicMessage> rows;
  for (int rep = 0; rep < 3; rep++) {
    for (int i = 0; i < (int)(sizeof(kSeed) / sizeof(kSeed[0])); i++) {
      nimbus::orch::EpisodicMessage u, a;
      u.sessionId = chat;
      u.role = "user";
      u.text = kSeed[i].user;
      a.sessionId = chat;
      a.role = "assistant";
      a.text = kSeed[i].assistant;
      rows.push_back(u);
      rows.push_back(a);
    }
  }
  const std::string digest = agent::dream::buildEpisodicDigest(rows, 16384);
  r.note("fold digest: " + std::to_string(digest.size()) + " B over " +
         std::to_string(rows.size()) + " recorded rows");
  std::string summary;
  size_t produced = 0;
  const auto res = rig.runFold(chat, "", digest, summary, &produced);
  if (res != agent::TurnEngine::FoldResult::Ok) {
    r.check(false, "the fold ran on the pinned head (result " + std::to_string((int)res) + ")");
    return;
  }
  r.check(true, "the fold ran on the pinned head");
  r.note("fold produced " + std::to_string(produced) + " B, kept " +
         std::to_string(summary.size()) + " B (cap " +
         std::to_string(nimbus::orch::kChatSummaryMax) + " B)");
  r.check(summary.size() <= nimbus::orch::kChatSummaryMax,
          "the kept summary stays inside the stated budget");
  r.note("summary head: " + summary.substr(0, 400));
  r.check(summary == nimbus::orch::capSummary(summary),
          "capSummary keeps it whole (no re-trim, so the anchored fold re-digests from it)");
  // Header titles, number-agnostic - the same rule capSummary's section
  // splitter uses (a head may number or bold them differently).
  for (const char* h : {"Pending items", "Owner intent", "Device"})
    r.check(summary.find(h) != std::string::npos, std::string("section header kept: ") + h);
}

}  // namespace caching
}  // namespace lab
