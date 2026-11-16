// fold_replay - replay one offline owner-regression fold scenario through a hosted
// instance (NimbusdRig: the Virtual Nimbus fold path over the shared fold store and the
// shared engine fold, the same calls the device's compactTick makes). Every provider
// answer is scripted (FakeHttpTransport), so it needs no network and no keys, and the
// same scenario runs on any release: the replay touches only what a hosted instance has
// had since v5.0 (say, foldTick, noteFoldPumpEnd, foldState, the fold-store file).
//
//   fold_replay --scenario <file.json> --out <result.json>
//
// Scenario keys (mode "offline", runner "fold_replay"; see run_offline.py):
//   plant        a fold-store record written to the instance volume before it starts,
//                the shape an older release left behind: bytesSinceFold, msgsSinceFold,
//                lastFoldDaysAgo, turnsSinceFold, breakerFails, paused (0, 1 or 2), summary
//   turns        owner turns of about 3 KB each, every one answered by the head
//   quietMs      real time to wait after the last turn (the owner-first quiet bound)
//   ticks        fold pump passes after the wait, no turn in between (a quiet evening)
//   foldAnswers  the provider's answer to each fold request, in order ("" = an empty
//                reply); requests past the list get an empty reply
// The result names every request a pump pass made (pass, host, model, path), the summary
// before and after, every message the instance sent the owner, and the fold-store file it
// left (blob_after). Grading is run_offline.py's.
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../../../test/support/fake_http.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

namespace {

const char* const kChat = "owner";

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (c == '\n') {
      o += "\\n";
      continue;
    }
    o += c;
  }
  return o;
}

// One answer through the Cumulo router's OpenAI upstream (the hosted default head).
Exchange responsesAnswer(const std::string& text) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.body = "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
           "\"output_text\",\"text\":\"" + esc(text) + "\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
  return e;
}

Exchange headTurn(const std::string& reply) {
  return responsesAnswer("{\"reply\":\"" + esc(reply) + "\",\"memory\":\"\"}");
}

std::string ownerText(int i) {
  std::string s = "Diary note " + std::to_string(i) + ": ";
  while (s.size() < 3000) s += "walked to the market, bought bread and apples, and planned the week's meals. ";
  return s;
}

// The v5.0-v5.1.1 fold-store record (FoldStore::encodeSlot): the fields an older
// release persists, unit separators between them, a record separator at the end.
std::string plantedRecord(const JsonObjectConst& p) {
  const uint32_t epoch = (uint32_t)std::time(nullptr) - (uint32_t)(p["lastFoldDaysAgo"] | 0) * 86400u;
  const char us = '\x1F';
  std::string r = kChat;
  r += us + std::to_string(p["bytesSinceFold"] | 0u);
  r += us + std::to_string(p["msgsSinceFold"] | 0u);
  r += us + std::to_string(epoch);
  r += us + std::to_string(p["turnsSinceFold"] | 0u);
  r += us + std::to_string(p["breakerFails"] | 0u);
  r += us + std::to_string(p["paused"] | 0u);
  r += us + std::string(p["summary"] | "");
  r += '\x1E';
  return r;
}

std::string field(const std::string& body, const char* key) {
  const std::string k = std::string("\"") + key + "\":\"";
  const size_t at = body.find(k);
  if (at == std::string::npos) return "";
  const size_t end = body.find('"', at + k.size());
  return body.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
}

struct Replay {
  FakeHttpTransport tx;
  std::vector<std::string> answers;
  size_t nextAnswer = 0;
  JsonDocument out;

  // One pump pass with no turn before it: the next scripted fold answer for its first
  // request, empty replies for any further request the pass makes (a pass that walks
  // several rungs), the unused ones dropped after it, and every request recorded.
  void tick(NimbusdRig& rig, int pass, JsonArray reqs) {
    const size_t before = tx.seen.size();
    const size_t first = tx.idx;
    tx.script.resize(first);
    tx.script.push_back(responsesAnswer(nextAnswer < answers.size() ? answers[nextAnswer] : std::string()));
    for (int i = 0; i < 4; i++) tx.script.push_back(responsesAnswer(""));
    rig.noteFoldPumpEnd();
    rig.foldTick([](bool) {});
    if (tx.idx > first) nextAnswer++;
    tx.script.resize(tx.idx);
    for (size_t i = before; i < tx.seen.size(); i++) {
      JsonObject r = reqs.add<JsonObject>();
      r["pass"] = pass;
      r["host"] = tx.seen[i].host;
      r["path"] = tx.seen[i].path;
      r["model"] = field(tx.seen[i].body, "model");
    }
  }
};

void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY", "CUMULO_API_KEY",
                        "Z_AI_TOKEN", "TELEGRAM_BOT_TOKEN"})
    unsetenv(k);
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenarioPath, outPath;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--scenario") scenarioPath = argv[i + 1];
    else if (k == "--out") outPath = argv[i + 1];
  }
  JsonDocument sc;
  if (scenarioPath.empty() || outPath.empty() || deserializeJson(sc, readFile(scenarioPath))) {
    std::fputs("usage: fold_replay --scenario <file.json> --out <result.json>\n", stderr);
    return 2;
  }
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_FOLDREPLAY", 1);   // a test value: the router is scripted
  const std::string scratch = ndtest::scratchDir("fold-replay");
  ndtest::rmTree(scratch);
  NimbusdRig::Options opt;
  opt.dataDir = scratch + "/data";
  opt.embeddings = false;
  opt.embedDims = 64;
  opt.toolLoop = false;
  const JsonObjectConst plant = sc["plant"].as<JsonObjectConst>();
  if (!plant.isNull()) {
    std::string cmd = "mkdir -p '" + opt.dataDir + "/mem'";
    if (std::system(cmd.c_str()) != 0) return 2;
    std::ofstream(opt.dataDir + "/mem/chatsum.txt", std::ios::binary) << plantedRecord(plant);
  }

  Replay rp;
  for (JsonVariantConst a : sc["foldAnswers"].as<JsonArrayConst>()) rp.answers.push_back(a.as<std::string>());
  std::vector<std::string> sent;
  {
    Config cfg;
    NimbusdRig rig(cfg, opt, &rp.tx);
    rig.setDeliver([&sent](const std::string&, const std::string& text) { sent.push_back(text); });
    rp.out["summary_before"] = rig.foldState(kChat).summary;
    const int turns = sc["turns"] | 0;
    for (int i = 0; i < turns; i++) {
      rp.tx.script.push_back(headTurn("Noted."));
      rig.say(kChat, ownerText(i));
    }
    rp.out["turn_requests"] = (int)rp.tx.seen.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(sc["quietMs"] | 0));
    JsonArray reqs = rp.out["fold_requests"].to<JsonArray>();
    const int ticks = sc["ticks"] | 0;
    for (int t = 1; t <= ticks; t++) rp.tick(rig, t, reqs);
    rp.out["summary_after"] = rig.foldState(kChat).summary;
    rig.setDeliver(nullptr);
  }
  // The fold-store file the instance left (the release's own blob encoding): the sample
  // check_fold_health.py reads under the v5.1.1 parser for the rollback round trip.
  rp.out["blob_after"] = readFile(opt.dataDir + "/mem/chatsum.txt");
  JsonArray msgs = rp.out["messages"].to<JsonArray>();
  for (const auto& m : sent) msgs.add(m);
  rp.out["scenario"] = sc["id"].as<std::string>();
  rp.out["log_tail"] = NimbusdRig::logTail(16 * 1024, false);
  std::string text;
  serializeJsonPretty(rp.out, text);
  std::ofstream(outPath, std::ios::binary) << text;
  ndtest::rmTree(scratch);
  return 0;
}
