#include "scenarios_foldladder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"   // historyTurns + foldSlice: the device's own digest
#include "nimbus/harness/fold_ladder.h"    // foldAnswerThin, foldSectionsNamed, rung reports
#include "nimbus/harness/providers.h"      // orchTurnCustom: the custom flat wire
#include "nimbus/orch/compact.h"           // ORCH_COMPACT_PROMPT, buildCompactInputs, capSummary
#include "nimbus/orch/turn.h"              // parseHeadReply

// The fold ladder's live legs (lane F of the history fix, plan v3 PL): one real fold per
// leg on a Lumi-shaped digest of that host's fold step (53.9 KB Mistral and OpenAI,
// 35.4 KB Claude, 26.1 KB Z.ai, a flat string on the custom wire), through the engine's
// own rung (the head's fold model: Large 4 folds on mistral-small-latest). Each leg
// records the model the rung ran, output tokens and time, and must pass the fold's
// quality gate. FOLDLADDER_SAVE=<dir> writes each summary as a matcher fixture
// (<leg>_<digest bytes>.txt, test/support/fixtures/fold_outputs_2026-11). PAID: about
// 0.5 USD for the whole set; a leg without its key is skipped, never a pass.
namespace lab {
namespace foldladder {

namespace {

struct Leg {
  const char* name;
  const char* host;
  const char* model;      // the head model the rig runs
  const char* subModel;   // "" = the rig's default; the leg folds on its Sub rung when set
  size_t stepBytes;       // the host's fold step (plan v3 provider matrix)
};

const Leg kLegs[] = {
    {"large4-head", "mistral", "mistral-large-4-0", "", 53900},
    {"mistral-medium", "mistral", "mistral-medium-latest", "", 12300},
    {"openai-luna", "openai", "gpt-5.6-luna", "", 53900},
    {"openai-gpt-5.6", "openai", "gpt-5.6", "", 53900},
    {"anthropic-sonnet", "anthropic", "claude-sonnet-5-5", "", 35400},
    {"anthropic-haiku-sub", "anthropic", "claude-sonnet-5-5", "claude-haiku-5-5", 35400},
    {"zai-glm-5.3", "zai", "glm-5.3", "", 26100},
};

// A Lumi-shaped conversation: two weeks of meals, weigh-ins, runs and plans, an owner
// line and a reply per turn, a few hours apart.
std::vector<nimbus::orch::EpisodicMessage> lumiRows(size_t bytes) {
  static const char* const kOwner[] = {
      "Breakfast: 60 g oats with 200 ml semi-skimmed milk, a banana and black coffee.",
      "Lunch was the chicken and lentil soup again, about 400 g, plus a slice of rye bread.",
      "Weighed in at 82.4 kg this morning. Down 0.3 from last week.",
      "Ran 7 km along the canal in 41 minutes, legs felt heavy after the hill.",
      "Dinner: salmon fillet 140 g, roast potatoes 200 g and green beans. What's the total today?",
      "Remind me the plan for Sunday's long run, I think it was 14 km at easy pace.",
      "Snack: Greek yoghurt 170 g with honey. The label says 9 g protein per 100 g.",
      "Bought bread, apples and eggs at the market. Planning meals for the week.",
  };
  static const char* const kReply[] = {
      "Logged breakfast: about 390 kcal and 17 g protein. Day total so far 390 kcal.",
      "Logged lunch: about 520 kcal and 31 g protein from the soup and bread.",
      "Noted 82.4 kg, 0.3 kg down on last Monday; the trend is steady.",
      "Nice run. 7 km in 41 minutes is a 5:51 pace; keep Sunday easy.",
      "Logged dinner: about 610 kcal and 38 g protein. Today: 1,860 kcal and 112 g protein.",
      "Sunday: 14 km at easy pace (around 6:15 per km), then stretch and refuel.",
      "Logged with the label figure: about 160 kcal and 15 g protein.",
      "Noted. With eggs and apples in, breakfasts are covered through Thursday.",
  };
  std::vector<nimbus::orch::EpisodicMessage> rows;
  size_t total = 0;
  for (int i = 0; total < 2 * bytes; i++) {
    for (int half = 0; half < 2; half++) {
      nimbus::orch::EpisodicMessage m;
      char id[16];
      std::snprintf(id, sizeof(id), "m%08x", 0x17da + (int)rows.size());
      m.id = id;
      m.sessionId = "owner";
      m.role = half ? "assistant" : "user";
      m.kind = nimbus::orch::MsgKind::Message;
      m.text = half ? kReply[i % 8] : kOwner[i % 8];
      m.tsHours = 493000u + (uint32_t)(i * 3);
      total += m.text.size() + 40;
      rows.push_back(m);
    }
  }
  return std::vector<nimbus::orch::EpisodicMessage>(rows.rbegin(), rows.rend());   // newest first
}

// The digest the device would fold: the oldest turns of the conversation, up to `step`.
std::string lumiDigest(size_t step) {
  agent::HistoryOptions o;
  const auto turns = agent::historyTurns(lumiRows(step), o);
  return agent::foldSlice(turns, 0, step, /*manual=*/true, 0).digest;
}

void save(const std::string& dir, const std::string& name, size_t digestBytes, const std::string& summary) {
  if (dir.empty()) return;
  std::ofstream(dir + "/" + name + "_" + std::to_string(digestBytes) + ".txt", std::ios::binary) << summary;
}

void judge(const Report& r, const std::string& leg, const agent::FoldRungReport& rep, const std::string& summary,
           size_t digestBytes) {
  r.note(leg + ": " + rep.host + "/" + rep.model + ", " + std::to_string(digestBytes) + " B digest, " +
         std::to_string(rep.promptTokens) + " in / " + std::to_string(rep.completionTokens) + " out tokens, " +
         std::to_string(rep.elapsedMs) + " ms, " + std::to_string(rep.producedBytes) + " B produced, " +
         std::to_string(agent::foldSectionsNamed(summary)) + " of 5 sections");
  r.check(rep.result == agent::FoldResult::Ok,
          leg + ": the rung answered with a summary" +
              (rep.result == agent::FoldResult::Ok ? std::string() : std::string(" (") + agent::foldFailKindName(rep.fail) +
                                                                       ": " + rep.detail + ")"));
  if (rep.result != agent::FoldResult::Ok) return;
  r.check(!agent::foldAnswerThin(summary, digestBytes, 0), leg + ": the quality gate takes it");
  r.check(summary.size() <= nimbus::orch::kChatSummaryMax, leg + ": inside the 6,144 B cap");
}

void runLeg(const Env& env, const Leg& leg, const Report& r, const std::string& saveDir) {
  if (env.providerKey(leg.host).empty()) {
    r.note(std::string(leg.name) + ": no " + leg.host + " key - leg not run");
    return;
  }
  LabRig::Options opt;
  opt.priority = leg.host;
  opt.models[leg.host] = leg.model;
  opt.embeddings = false;
  opt.pins = WirePins{"", 0, 0};   // the shipping effort and caps
  LabRig rig(env, opt);
  std::vector<agent::FoldRung> rungs = rig.engine().foldRungs();
  if (rungs.empty()) {
    r.check(false, std::string(leg.name) + ": the ladder has a rung");
    return;
  }
  agent::FoldRung rung = rungs.front();
  if (*leg.subModel) {
    rung.model = leg.subModel;
    rung.kind = agent::FoldRungKind::SubModel;
  }
  const std::string digest = lumiDigest(leg.stepBytes);
  std::string summary;
  agent::FoldRungReport rep;
  rig.engine().runFoldRung("owner", "", digest, rung, summary, &rep);
  judge(r, leg.name, rep, summary, digest.size());
  if (rep.result == agent::FoldResult::Ok) save(saveDir, leg.name, digest.size(), summary);
}

uint32_t nowMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The custom wire (an OpenAI-compatible endpoint the owner names) against OpenAI's own
// /v1/chat/completions with gpt-5.6-luna, the plan's stand-in: the fold is one flat
// string there, in prose mode.
void runCustomLeg(LabRig& rig, const Report& r, const std::string& saveDir) {
  const Env& env = rig.env();
  if (env.providerKey("openai").empty()) {
    r.note("custom: no openai key - leg not run");
    return;
  }
  CurlHttpTransport http;
  agent::providers::ProviderDeps pd;
  pd.http = &http;
  pd.nowMs = [] { return nowMs(); };
  pd.customBase = [] { return std::string("api.openai.com"); };
  pd.customPathPrefix = [] { return std::string("/v1"); };
  pd.customKey = [env] { return env.providerKey("openai"); };
  pd.customConv = [] { return std::string("openai"); };
  pd.customModel = [] { return std::string("gpt-5.6-luna"); };
  const std::string digest = lumiDigest(20 * 1024);
  const std::string inputs = nimbus::orch::buildCompactInputs("", digest);
  std::string conv, out, err;
  nimbus::orch::TokenUsage u;
  const uint32_t t0 = nowMs();
  const bool ok = agent::providers::orchTurnCustom(pd, conv, nimbus::orch::ORCH_COMPACT_PROMPT, inputs, out, err,
                                                   nullptr, &u);
  agent::FoldRungReport rep;
  rep.host = "custom";
  rep.model = "gpt-5.6-luna";
  rep.elapsedMs = nowMs() - t0;
  rep.promptTokens = u.promptTokens;
  rep.completionTokens = u.completionTokens;
  nimbus::orch::Turn t;
  nimbus::orch::ParseError perr;
  const bool parsed = ok && nimbus::orch::parseHeadReply(out, t, perr) != nimbus::orch::ReplyShape::Unusable &&
                      !t.reply.empty();
  rep.result = parsed ? agent::FoldResult::Ok : agent::FoldResult::Failed;
  rep.detail = ok ? "no summary in reply" : err;
  rep.producedBytes = parsed ? t.reply.size() : 0;
  const std::string summary = parsed ? nimbus::orch::capSummary(t.reply) : std::string();
  judge(r, "custom", rep, summary, digest.size());
  if (parsed) save(saveDir, "custom-luna", digest.size(), summary);
}

}  // namespace

void run(LabRig& rig, const Report& r) {
  const char* dir = std::getenv("FOLDLADDER_SAVE");
  const std::string saveDir = dir ? dir : "";
  const char* only = std::getenv("FOLDLADDER_LEGS");   // a comma list of leg names, "" = all
  for (const Leg& leg : kLegs)
    if (!only || std::string(",") .append(only).append(",").find(std::string(",") + leg.name + ",") != std::string::npos)
      runLeg(rig.env(), leg, r, saveDir);
  if (!only || std::string(only).find("custom") != std::string::npos) runCustomLeg(rig, r, saveDir);
}

}  // namespace foldladder
}  // namespace lab
