#include "scenarios_g0.h"

#include <chrono>
#include <cstdio>
#include <vector>

#include "nimbus/harness/conversation.h"
#include "nimbus/harness/turn_inbox.h"

namespace lab {
namespace history {

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
bool has(const std::string& hay, const std::string& needle) {
  return lower(hay).find(lower(needle)) != std::string::npos;
}
size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}
uint32_t nowHours() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::hours>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

LabRig::Row media(const std::string& chat, const std::string& text, const std::string& blob) {
  LabRig::Row m;
  m.chat = chat;
  m.kind = nimbus::orch::MsgKind::Image;
  m.text = text;
  m.blob = blob;
  return m;
}

void show(const TurnRecord& t, const Report& r) {
  std::printf("      you> %.120s\n", t.userText.c_str());
  std::printf("      nimbus> %.300s\n", t.reply.empty() ? "(no reply)" : t.reply.c_str());
  r.note("usage: " + std::to_string(t.tokensIn) + " in, " + std::to_string(t.cacheRead) +
         " cache read, " + std::to_string(t.cacheWrite) + " cache write, " + std::to_string(t.tokensOut) +
         " out (" + t.host + ")");
}

}  // namespace

std::string historyBlockOf(const std::string& prompt) {
  const size_t at = prompt.find("\n## RECENT CONVERSATION");
  if (at == std::string::npos) return std::string();
  const size_t end = prompt.find("\n## ", at + 5);
  return prompt.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void historyCache(LabRig& rig, const Report& r) {
  const std::string chat = "hist";
  std::vector<std::string> blocks;   // each owner turn's history block, in order
  int answered = 0, asked = 0;
  auto say = [&](const std::string& text) {
    TurnRecord t = rig.say(chat, text);
    show(t, r);
    blocks.push_back(historyBlockOf(t.systemPrompt));
    asked++;
    answered += t.ok && !t.reply.empty();
    return t;
  };

  say("Hi! For later: my sister Maribel lives in Porto. Just acknowledge, please.");
  say("And my bike is a blue Brompton. Just acknowledge.");
  // A photo the way the device stores it: the media row keeps the description,
  // the owner row the bare label and the caption, joined by the blob ref.
  const std::string ref = "c0ffee00c0ffee01";
  const std::string desc = "[image] a red ceramic bowl of ramen with a soft-boiled egg and spring onions";
  rig.addRow(media(chat, desc, "/mem/blobs/" + ref + ".jpg"));
  agent::history::Inbound in;
  in.chatId = chat;
  in.said = "[image]\n\nThey wrote: dinner tonight";
  in.refs = {ref};
  const std::string routed = desc + "\n\nThey wrote: dinner tonight";
  agent::history::noteInbound(in, routed);
  say(routed);
  // A routine fires between owner turns.
  auto fire = rig.scheduled(chat, "Tell the owner it is time to stand up and stretch.", "stretch reminder",
                            false, "stretch");
  r.note(std::string("routine: ") + (fire.ok ? "ok" : "failed") + " " + fire.detail.substr(0, 120));
  const TurnRecord colour = say("What colour was the bowl in the photo I sent earlier?");
  r.check(has(colour.reply, "red"), "L5: the photo's description answers a later question (red)");
  r.check(count(historyBlockOf(colour.systemPrompt), "red ceramic bowl") == 1,
          "the photo's description rides the history exactly once");
  r.check(has(historyBlockOf(colour.systemPrompt), "[routine \"stretch reminder\" ran]") ||
              !fire.ok || fire.detail.empty(),
          "row 58: the routine shows as its own exchange, with its trigger");
  // A replay storm: 30 copies of one message captured with no answer between.
  LabRig::Row storm;
  storm.chat = chat;
  storm.text = "hello?";
  storm.tags = "from:Owner";
  for (int i = 0; i < 30; i++) rig.addRow(storm);
  const TurnRecord sister = say("Where does my sister live, and what is her name?");
  const std::string stormBlock = historyBlockOf(sister.systemPrompt);
  r.check(has(sister.reply, "Maribel") && has(sister.reply, "Porto"),
          "L5: a fact from the first turn is answered from the history after the storm");
  r.check(count(stormBlock, "hello?") == 1 && stormBlock.find("(x30)") != std::string::npos,
          "L19-26: 30 identical messages are one row with (x30)");
  r.check(stormBlock.find("Maribel") != std::string::npos && stormBlock.find("Brompton") != std::string::npos,
          "L19-26: the storm pushed no earlier turn out of the history");
  // Byte stability: every owner turn's history continues the previous one.
  int breaks = 0;
  for (size_t k = 1; k < blocks.size(); k++) {
    const std::string& a = blocks[k - 1];
    if (!a.empty() && blocks[k].compare(0, a.size() - 1, a, 0, a.size() - 1) != 0) breaks++;
  }
  r.check(breaks == 0, "the history is append-only between boundaries (" + std::to_string(breaks) + " breaks)");
  // /clear: the boundary row ends the history (memory.episodic may still find it).
  LabRig::Row clear;
  clear.chat = chat;
  clear.role = "system";
  clear.text = agent::kClearBoundaryText;
  clear.tags = agent::kClearBoundaryTag;
  rig.addRow(clear);
  const TurnRecord after = say("Quick check: what is my sister's name?");
  r.check(historyBlockOf(after.systemPrompt).empty(), "after /clear the prompt carries no earlier turn");
  // (The lab's /clear is the boundary row only: the running notes a device /clear
  // also resets stay, so the head may still know the name from them.)
  r.note("after /clear the head said: " + after.reply.substr(0, 160));
  r.check(answered == asked, "every owner turn was answered (" + std::to_string(answered) + "/" +
                                 std::to_string(asked) + ")");
}

void episodicDates(LabRig& rig, const Report& r) {
  const uint32_t now = nowHours();
  LabRig::Row yesterday = media("photos", "[image] a tabby cat asleep on a green sofa", "/mem/blobs/aaaaaaaaaaaaaaaa.jpg");
  yesterday.tsHours = now - 20;
  rig.addRow(yesterday);
  LabRig::Row today = media("photos", "[image] (couldn't be described)", "/mem/blobs/bbbbbbbbbbbbbbbb.jpg");
  today.tsHours = now - 1;
  rig.addRow(today);
  const TurnRecord t = rig.say("dates",
                               "Did any photo I sent get read (described) today? Look it up in the "
                               "episodic history first, then answer yes or no and say why.");
  show(t, r);
  bool looked = false;
  for (const auto& c : t.toolCalls) looked = looked || c.find("episodic") != std::string::npos;
  r.check(looked, "the head read memory_episodic");
  const std::string lead = lower(t.reply.substr(0, 60));
  const bool saysNo = lead.find("no") != std::string::npos || has(t.reply, "not read") ||
                      has(t.reply, "couldn't be") || has(t.reply, "could not be") || has(t.reply, "wasn't");
  const bool claimsYes = lead.rfind("yes", 0) == 0;
  r.check(saysNo && !claimsYes, "L19-12: today's photo was not read, and the answer says so");
}

}  // namespace history
}  // namespace lab
