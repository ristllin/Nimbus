#include "scenarios_c2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace lab {
namespace c2 {

namespace {

// The journal's store over RAM (the device keeps the same slots in one flash file).
struct RamStore : orch::JournalStore {
  std::map<int, std::string> slots;
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  bool put(int slot, const std::string& v) override {
    slots[slot] = v;
    return true;
  }
  bool remove(int slot) override {
    slots.erase(slot);
    return true;
  }
  void clearNs() override { slots.clear(); }
};

const char* kChat = "c2fin";

// What the previous boot left: two finished research jobs, saved in their project.
void seedFinished(RamStore& st) {
  const char* tags[] = {"job0003", "job0004"};
  const char* names[] = {"tide-research", "moon-phase"};
  for (int i = 0; i < 2; ++i) {
    orch::FinishedJob f{};
    std::snprintf(f.tag, sizeof(f.tag), "%s", tags[i]);
    std::snprintf(f.name, sizeof(f.name), "%s", names[i]);
    std::snprintf(f.prj, sizeof(f.prj), "tides");
    std::snprintf(f.artifact, sizeof(f.artifact), "tides/%s-%s.md", names[i], tags[i]);
    std::snprintf(f.finishedAt, sizeof(f.finishedAt), "2026-10-22 Thu 0%d:4%d BST", 8, i);
    std::snprintf(f.chatId, sizeof(f.chatId), "%s", kChat);
    f.seq = (uint32_t)i;
    char buf[orch::kJournalSerializeBuf];
    orch::serializeFinished(f, buf, sizeof(buf));
    st.slots[orch::kJournalFinishedSlot0 + i] = buf;
  }
}

// The conversation before the restart: the ask, the head's ack, and a working note
// that still says it is waiting (the state the owner's device restarted in).
void seedConversation(LabRig& rig) {
  LabRig::Row ask;
  ask.chat = kChat;
  ask.text = "Please research why the sea has two high tides a day, and what the moon phase "
             "is tonight. Save both to the tides project and tell me when they're done.";
  rig.addRow(ask);
  LabRig::Row ack;
  ack.chat = kChat;
  ack.role = "assistant";
  ack.text = "On it - I started two research helpers (tide-research and moon-phase) and will "
             "report back as soon as they finish.";
  rig.addRow(ack);
  // NIMBUS_C2_FILES=1: the results are also on the card, in both legs. With them, the
  // head can find them itself (files.list) and the counter-leg rarely re-spawns; without
  // them (the default) the counter-leg reproduces the re-run the ring prevents.
  const char* files = std::getenv("NIMBUS_C2_FILES");
  if (!files || std::strcmp(files, "1") != 0) {
    (void)rig.scratchpad().setActiveTask(
        "Waiting for the tide-research and moon-phase helpers; report both results to the owner.");
    return;
  }
  rig.files().seed("tides/tide-research-job0003.md",
                   "Two high tides a day: the Moon's pull raises a bulge of water on the near side "
                   "of the Earth, and inertia leaves a second bulge on the far side; the Earth turns "
                   "through both about every 24 h 50 min.");
  rig.files().seed("tides/moon-phase-job0004.md", "Tonight's moon: waxing gibbous, about 78% lit.");
  (void)rig.scratchpad().setActiveTask(
      "Waiting for the tide-research and moon-phase helpers; report both results to the owner.");
}

struct Leg {
  int spawns = 0;
  bool listed = false;
  bool answered = false;
  uint32_t in = 0, out = 0, cacheR = 0, cacheW = 0;
};

Leg runLeg(LabRig& rig, bool withRing, const std::string& msg) {
  static RamStore st;   // the engines keep a pointer to the journal for the rig's life
  static orch::Journal journal;
  st.slots.clear();
  if (withRing) seedFinished(st);
  journal.begin(&st);   // the restart: the journal loads what the previous boot left
  rig.attachJournal(&journal);
  seedConversation(rig);
  const size_t before = rig.http().calls.size();
  TurnRecord t = rig.say(kChat, msg);
  Leg leg;
  leg.spawns = rig.jobs().pendingCount();
  // The turn context rides the request (the dynamic block of the input), not the
  // system prompt: read what the provider was actually sent this turn.
  for (size_t i = before; i < rig.http().calls.size(); ++i) {
    const std::string& body = rig.http().calls[i].reqBody;
    leg.listed = leg.listed || (body.find("Finished before the device last restarted") != std::string::npos &&
                                body.find("tide-research (job0003, project tides") != std::string::npos &&
                                body.find("moon-phase (job0004") != std::string::npos);
  }
  leg.answered = t.ok && !t.reply.empty();
  leg.in = t.tokensIn;
  leg.out = t.tokensOut;
  leg.cacheR = t.cacheRead;
  leg.cacheW = t.cacheWrite;
  std::printf("      [%s] you> %s\n", withRing ? "A ring" : "B none", msg.c_str());
  std::printf("      nimbus> %.300s\n", t.reply.empty() ? "(no reply)" : t.reply.c_str());
  std::printf("      spawned=%d listed=%d usage in=%u out=%u cacheR=%u cacheW=%u (%s)\n", leg.spawns,
              (int)leg.listed, t.tokensIn, t.tokensOut, t.cacheRead, t.cacheWrite, t.host.c_str());
  return leg;
}

}  // namespace

void finishedJobs(LabRig& rig, const Report& r) {
  // The spec's unrelated message, and the follow-up an owner sends after a restart
  // (the case where the head re-ran the jobs on the owner's device).
  const std::string msgs[] = {"Unrelated question: what's a good name for a grey kitten?",
                              "Did that research ever finish?"};
  int reps = 2;
  if (const char* e = std::getenv("NIMBUS_C2_REPS")) reps = std::atoi(e) > 0 ? std::atoi(e) : reps;
  int bSpawns = 0, bTurns = 0;
  for (int rep = 1; rep <= reps; ++rep) {
    for (const std::string& msg : msgs) {
      const std::string tag = "rep " + std::to_string(rep) + (msg == msgs[0] ? " unrelated" : " follow-up");
      std::printf("    -- %s --\n", tag.c_str());
      const Leg a = runLeg(rig, /*withRing=*/true, msg);
      r.check(a.listed, tag + " A: the turn context lists the finished jobs");
      r.check(a.answered, tag + " A: the message is answered");
      r.check(a.spawns == 0, tag + " A: the head starts nothing (" + std::to_string(a.spawns) + " spawn(s))");
      const Leg b = runLeg(rig, /*withRing=*/false, msg);
      r.check(!b.listed, tag + " B: no finished jobs listed (the ring removed)");
      bSpawns += b.spawns;
      bTurns++;
    }
  }
  r.note("counter-leg B (no finished record): " + std::to_string(bSpawns) + " re-spawn(s) over " +
         std::to_string(bTurns) + " turn(s)");
}

}  // namespace c2
}  // namespace lab
