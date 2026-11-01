#include "scenarios_ovf.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/msg_batch.h"
#include "nimbus/orch/turn_images.h"

namespace lab {
namespace ovf {
namespace {

namespace orch = nimbus::orch;

const char kSwell[] = "OVFSWELL";

bool isHead(const agent::HttpRequest& q) {
  return isProviderHost(q.host) && q.path.find("/embeddings") == std::string::npos;
}

// A full turn's answer with its orch_turn reply swapped, in the provider's own shape: the
// model's answer is replaced on its way back, everything else on the wire is real. True when
// the body held an orch_turn.
bool swapFinalReply(std::string& body, const std::string& reply) {
  JsonDocument d;
  if (deserializeJson(d, body)) return false;
  JsonDocument args;
  args["reply"] = reply;
  args["memory"] = "";
  args["ask"] = "";
  std::string argStr;
  serializeJson(args, argStr);
  bool swapped = false;
  for (JsonObject c : d["content"].as<JsonArray>())   // anthropic
    if (std::string(c["name"] | "") == "orch_turn") {
      c["input"] = args;
      swapped = true;
    }
  for (JsonObject o : d["output"].as<JsonArray>())   // openai
    if (std::string(o["name"] | "") == "orch_turn") {
      o["arguments"] = argStr;
      swapped = true;
    }
  for (JsonObject t : d["choices"][0]["message"]["tool_calls"].as<JsonArray>())   // mistral
    if (std::string(t["function"]["name"] | "") == "orch_turn") {
      t["function"]["arguments"] = argStr;
      swapped = true;
    }
  if (!swapped) return false;
  body.clear();
  serializeJson(d, body);
  return true;
}

// Every provider exchange of this run, one line each: status and size. The raw record.
void printCalls(LabRig& rig, const Report& r) {
  for (const auto& c : rig.http().calls) {
    if (!isProviderHost(c.host) || c.path.find("/embeddings") != std::string::npos) continue;
    r.note(c.host + c.path + " -> " + std::to_string(c.status) + " body=" + std::to_string(c.reqBody.size()) + " B " +
           (c.status >= 400 ? c.respBody.substr(0, 200) : std::string()));
  }
}

int refusedCalls(LabRig& rig) {
  int n = 0;
  for (const auto& c : rig.http().calls) n += isProviderHost(c.host) && c.status >= 400 ? 1 : 0;
  return n;
}

int headCalls(LabRig& rig) {
  int n = 0;
  for (const auto& c : rig.http().calls)
    n += isProviderHost(c.host) && c.path.find("/embeddings") == std::string::npos ? 1 : 0;
  return n;
}

}  // namespace

void overflowRefusal(LabRig& rig, const Report& r) {
  const std::string chat = "ovf-overflow";
  LabRig::Row oldQ;
  oldQ.chat = chat;
  oldQ.text = std::string("Note this for later: ") + kSwell;
  rig.addRow(oldQ);
  LabRig::Row oldA;
  oldA.chat = chat;
  oldA.role = "assistant";
  oldA.text = "Noted.";
  rig.addRow(oldA);
  LabRig::Row q2;
  q2.chat = chat;
  q2.text = "And what is the capital of France?";
  rig.addRow(q2);
  LabRig::Row a2;
  a2.chat = chat;
  a2.role = "assistant";
  a2.text = "Paris.";
  rig.addRow(a2);
  // The marker swells to 1.5 M words where it rides: past every current model's window.
  const std::string pad = [] {
    std::string p;
    p.reserve(7500000);
    for (int i = 0; i < 1500000; i++) p += "word ";
    return p;
  }();
  rig.shim().onRequest = [&pad](agent::HttpRequest& q) {
    if (!isHead(q)) return;
    const size_t at = q.body.find(kSwell);
    if (at != std::string::npos) q.body.replace(at, sizeof(kSwell) - 1, pad);
  };
  auto t = rig.say(chat, "What is 2 + 2? Answer with the number only.");
  rig.shim().onRequest = nullptr;
  printCalls(rig, r);
  r.note("reply: " + t.reply);
  r.check(refusedCalls(rig) >= 1, "the provider really refused the swollen request");
  r.check(t.reply.find("4") != std::string::npos, "the turn answered after the trim");
  r.check(t.reply.find("didn't finish") == std::string::npos, "no failure reply");
}

void synthesisOverBudget(LabRig& rig, const Report& r) {
  const std::string chat = "ovf-round0";
  // What the request's own estimate cannot see: text the wire writes at twice its size.
  std::string heavy;
  while (heavy.size() < 2400) heavy += "\"x\"\n";
  for (int i = 0; i < 6; i++) {
    LabRig::Row q;
    q.chat = chat;
    q.text = "old question " + std::to_string(i) + " " + heavy;
    rig.addRow(q);
    LabRig::Row a;
    a.chat = chat;
    a.role = "assistant";
    a.text = "old answer " + std::to_string(i) + " " + heavy;
    rig.addRow(a);
  }
  // The history's weight alone does not reach 85 KB on this rig's short tool list; a long
  // standing rules block rides the same prompt, so the first body really crosses it.
  rig.registry();
  size_t largest = 0;
  rig.shim().onRequest = [&largest](agent::HttpRequest& q) {
    if (isHead(q) && q.body.size() > largest) largest = q.body.size();
  };
  rig.jobs().addFreshResult("job0008", "openai/gpt-5.2",
                            std::string("The soup is pumpkin, about 180 kcal a bowl. ") + std::string(7000, 'r'));
  auto t = rig.synthesize(chat);
  rig.shim().onRequest = nullptr;
  printCalls(rig, r);
  r.note("largest request " + std::to_string(largest) + " B; reply: " + t.reply.substr(0, 300));
  r.check(largest <= agent::providers::kWireBodyMax, "every request sent was inside the wire budget");
  r.check(!t.reply.empty() && t.reply.find("didn't finish") == std::string::npos, "the synthesis answered");
  r.check(t.reply.find("raw findings") == std::string::npos, "not the raw fallback");
}

void cutContinues(LabRig& rig, const Report& r) {
  agent::providers::setTestCutRounds(1, 48);
  auto t = rig.say("ovf-cut",
                   "List the planets of the solar system with one short fact about each. Then say how many there are.");
  agent::providers::setTestCutRounds(0, 0);
  printCalls(rig, r);
  r.note("reply: " + t.reply.substr(0, 400));
  r.check(headCalls(rig) >= 3, "two attempts were cut, and the round ran again");
  r.check(!t.reply.empty() && t.reply.find("cut off") == std::string::npos, "an answer, not the cut marker");
  r.check(t.reply.find("next turn") == std::string::npos, "no next-turn promise");
}

void forcedPlaceholder(LabRig& rig, const Report& r) {
  int swapped = 0;
  rig.shim().onResponse = [&swapped](const agent::HttpRequest& q, agent::HttpResponse& out) {
    if (!isHead(q) || swapped || out.status != 200) return;
    if (swapFinalReply(out.body, "placeholder")) swapped++;
  };
  auto t = rig.say("ovf-placeholder", "What is the capital of Japan? One word.");
  rig.shim().onResponse = nullptr;
  printCalls(rig, r);
  r.note("reply: " + t.reply);
  r.check(swapped == 1, "the first final answer was filler on its way back");
  r.check(refusedCalls(rig) == 0, "the provider accepted the correction pair");
  r.check(t.reply.find("placeholder") == std::string::npos, "the owner never saw it");
  r.check(t.reply.find("Tokyo") != std::string::npos, "the real answer reached the owner");
}

void promiseCorrected(LabRig& rig, const Report& r) {
  int swapped = 0;
  rig.shim().onResponse = [&swapped](const agent::HttpRequest& q, agent::HttpResponse& out) {
    if (!isHead(q) || swapped || out.status != 200) return;
    if (swapFinalReply(out.body, "I'll work out the capital of Japan and tell you in the next turn.")) swapped++;
  };
  auto t = rig.say("ovf-promise", "What is the capital of Japan? One word.");
  rig.shim().onResponse = nullptr;
  printCalls(rig, r);
  r.note("reply: " + t.reply);
  r.check(swapped == 1, "the first final answer was the promise on its way back");
  r.check(refusedCalls(rig) == 0, "the provider accepted the correction pair");
  r.check(t.reply.find("next turn") == std::string::npos ||
              t.reply.find("will not continue on its own") != std::string::npos,
          "no bare promise reached the owner");
}

void albumOfSix(LabRig& rig, const Report& r) {
  const char* dir = std::getenv("NIMBUS_LAB_PHOTOS");
  if (!dir || !*dir) {
    r.note("NIMBUS_LAB_PHOTOS is not set: skipped");
    return;
  }
  JsonDocument truth;
  {
    std::ifstream t(std::string(dir) + "/truth.json");
    const std::string json((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
    if (deserializeJson(truth, json)) {
      r.check(false, "a readable truth.json in the photo directory");
      return;
    }
  }
  const std::string chat = "ovf-album";
  static std::vector<std::vector<uint8_t>> bytes;   // the set's images point at these
  bytes.clear();
  auto set = std::unique_ptr<orch::TurnImages>(new orch::TurnImages());
  orch::MessageBatcher b;
  const size_t want = 6;
  JsonArrayConst list = truth["photos"].as<JsonArrayConst>();
  const size_t have = list.size();
  if (!have) {
    r.check(false, "photos in truth.json");
    return;
  }
  bytes.resize(want);
  for (size_t k = 0; k < want; ++k) {   // the set repeats when the directory holds fewer
    std::ifstream f(std::string(dir) + "/" + (const char*)(list[k % have]["file"] | ""), std::ios::binary);
    bytes[k].assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    b.addMedia(chat, "Owner", k == 0 ? std::string("Dinner tonight") : std::string(), (int)k, (int32_t)(k + 1));
  }
  b.add(chat, "Owner",
        "Log everything in these 6 photos with a full nutrition analysis of each one, every item, and the day total.",
        (int32_t)(want + 1));
  set->nonce = "ovf0nonce";
  set->uploadOnce = true;
  for (size_t k = 0; k < want; ++k) {
    orch::MediaLabel l;
    l.k = (int)k + 1;
    l.n = (int)want;
    std::string seen(bytes[k].begin(), bytes[k].end());
    seen += std::to_string(k);   // a repeated file is still its own photo
    l.ref = orch::blobHash(seen);
    l.cls = orch::MediaClass::Attached;
    b.setMediaText(0, k, orch::mediaSegment(l));
    set->items.push_back(orch::attachedImage(l, bytes[k].data(), bytes[k].size(), "image/jpeg"));
  }
  const std::string turn = b.render(0).c_str();
  orch::offerTurnImages(chat, turn, std::move(set));
  auto t = rig.say(chat, turn);
  printCalls(rig, r);
  r.note("seconds " + std::to_string((int)t.seconds) + " tokens in/out " + std::to_string(t.tokensIn) + "/" +
         std::to_string(t.tokensOut) + "; reply: " + t.reply.substr(0, 500));
  r.check(!t.reply.empty(), "the album got an answer");
  r.check(t.reply.find("didn't finish") == std::string::npos, "no failure reply");
  r.check(t.reply.find("next turn") == std::string::npos ||
              t.reply.find("will not continue on its own") != std::string::npos,
          "no bare next-turn promise");
}

}  // namespace ovf
}  // namespace lab
