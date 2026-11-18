// test_fold_driver_vn - the Virtual Nimbus runs the same fold driver as the device
// (lane F of the history fix, 16 Nov 2026; owner ruling 2). Offline: every provider
// answer is scripted (FakeHttpTransport), no keys leave the process (test values only).
//
//   (a) an automatic cycle on the hosted instance whose every rung answers with no
//       summary: Pending n-1 times, then CycleFailed once, with no notice to the owner,
//       and the same outcomes and (host, model) order as the device's driver over a
//       plain engine with the same ladder and the same answers;
//   (b) /compact and Summarize now (POST /api/fold/run) queue a manual catch-up that
//       answers once; GET /api/fold/status names the plan's fields; an unknown chat is
//       refused.
#include <ArduinoJson.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../test/support/fake_config.h"
#include "../../test/support/fake_http.h"
#include "../../test/support/fake_platform.h"
#include "engine_thread.h"
#include "nimbus/harness/fold_driver.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;
namespace orch = nimbus::orch;

namespace {

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY", "CUMULO_API_KEY",
                        "Z_AI_TOKEN", "TELEGRAM_BOT_TOKEN"})
    unsetenv(k);
}

NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.toolLoop = false;
  o.foldTriggerBytes = 24 * 1024;
  return o;
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

// One Responses answer (OpenAI direct and the Cumulo router's OpenAI upstream alike).
Exchange responses(const std::string& text) {
  Exchange e;
  e.body = "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
           "\"output_text\",\"text\":\"" + esc(text) + "\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
  return e;
}
Exchange turnAnswer(const std::string& reply) { return responses("{\"reply\":\"" + reply + "\",\"memory\":\"\"}"); }
const char kEmptyFold[] = "{\"reply\":\"\",\"memory\":\"\"}";
const char kSummary[] =
    "1. Decisions & preferences\n- Notes stay short.\n2. Pending items & next steps\n- Plan the week.\n"
    "3. Facts worth remembering long-term\n- Walks to the market most days.";

std::string padded(int i) {
  std::string s = "note " + std::to_string(i) + " ";
  s.append(3000, (char)('a' + i % 26));
  return s;
}

// The device's side of the comparison: a plain engine over hosts that answer from the
// same list, the same ladder, and the shared driver with the quiet gate open.
std::vector<agent::FoldTick> deviceRun(const std::vector<agent::FoldRung>& rungs, std::vector<std::string>& asked) {
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  cfg.keyed.clear();
  cfg.priority.clear();
  for (const agent::FoldRung& r : rungs) {
    if (r.kind == agent::FoldRungKind::SubModel) {
      cfg.subModels[r.host] = r.model;
    } else {
      cfg.orchModels[r.host] = r.model;
      if (!cfg.subModels.count(r.host)) cfg.subModels[r.host] = r.model;   // no Sub rung unless the VN has one
    }
    if (r.kind != agent::FoldRungKind::SubModel) {
      cfg.keyed.insert(r.host);
      cfg.priority += (cfg.priority.empty() ? "" : ",") + r.host;
    }
  }
  agent::TurnEngine::Deps d;
  d.cfg = cfg.contract();
  d.platform = plat.contract();
  for (const agent::FoldRung& r : rungs) {
    const std::string host = r.host;
    d.hosts.add(host, [&asked, &cfg, host](std::string&, const std::string&, const std::string&, std::string& out,
                                          std::string&, const agent::HeadTools*, orch::TokenUsage* u) {
      const std::string pin = agent::providers::modelOverrideFor(host.c_str());
      asked.push_back(host + "/" + (pin.empty() ? cfg.orchModels[host] : pin));
      if (u) u->responses = 1;
      out = kEmptyFold;
      return true;
    });
  }
  agent::TurnEngine eng(std::move(d));
  orch::FoldStore folds;
  struct IO : orch::FoldStoreIO {
    std::string b;
    std::string load() override { return b; }
    void save(const std::string& v) override { b = v; }
  } io;
  folds.begin(&io);
  agent::FoldDriverHooks h;
  h.slice = [](const std::string&, size_t, bool) {
    agent::FoldSlice s;
    s.digest = std::string(30 * 1024, 'd');
    s.thru = "m00000100";
    s.rows = 20;
    return s;
  };
  folds.setSoftStart("owner", "m00000080");
  agent::FoldDriver drv(eng, folds, orch::FoldCtlIO(), h);
  drv.begin();
  std::vector<agent::FoldTick> out;
  for (size_t i = 0; i < rungs.size(); i++) out.push_back(drv.tick(1000 + (uint32_t)i, 1794787200));
  return out;
}

void testAutomaticCycleMatchesTheDevice(ndtest::Ctx& c) {
  std::printf("  -- (a) a failing automatic cycle: Pending n-1 times, one CycleFailed, no notice --\n");
  clearEnv();
  setenv("OPENAI_API_KEY", "sk-test-FOLDVN", 1);           // test values: the transport is scripted
  setenv("CUMULO_API_KEY", "cumulo_sk_test_FOLDVN", 1);
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("fold-vn-a"), &tx);
  std::vector<std::string> notices;
  rig.setDeliver([&notices](const std::string&, const std::string& text) {
    if (!has(text, "noted")) notices.push_back(text);
  });
  const std::vector<agent::FoldRung> rungs = rig.engine().foldRungs();
  c.ok(rungs.size() >= 2, "the hosted ladder has more than one rung (" + std::to_string(rungs.size()) + ")");
  std::vector<agent::FoldTick> vn;
  std::vector<std::string> asked;
  const auto noBusy = [](bool) {};
  for (int i = 0; i < 80 && (vn.empty() || vn.back() != agent::FoldTick::CycleFailed); i++) {
    tx.script.push_back(turnAnswer("noted"));
    rig.say("owner", padded(i));
    rig.noteFoldPumpEnd();
    const size_t before = tx.seen.size();
    tx.script.push_back(responses(kEmptyFold));
    const agent::FoldTick t = rig.foldTick(noBusy);
    if (tx.seen.size() > before) {
      const std::string& body = tx.seen.back().body;
      const size_t at = body.find("\"model\":\"");
      asked.push_back(tx.seen.back().host + "/" + body.substr(at + 9, body.find('"', at + 9) - at - 9));
    } else {
      tx.script.pop_back();
    }
    if (t != agent::FoldTick::Deferred && t != agent::FoldTick::Idle) vn.push_back(t);
  }
  c.ok(vn.size() == rungs.size(), "one outcome per rung (" + std::to_string(vn.size()) + ")");
  for (size_t i = 0; i + 1 < vn.size(); i++) c.ok(vn[i] == agent::FoldTick::Pending, "Pending after rung " + std::to_string(i + 1));
  c.ok(!vn.empty() && vn.back() == agent::FoldTick::CycleFailed, "CycleFailed once, after the last rung");
  c.ok(notices.empty(), "no notice to the owner for an automatic cycle");
  c.ok(rig.foldState("owner").failedCycles == 1, "one failed cycle counted");
  c.ok(rig.foldState("owner").nextTry > (uint32_t)time(nullptr), "the next try is scheduled");
  c.ok(has(NimbusdRig::logTail(64 * 1024, false), "fold(owner): cycle failed, next try"), "the log says when");

  std::vector<std::string> devAsked;
  const std::vector<agent::FoldTick> dev = deviceRun(rungs, devAsked);
  c.ok(dev == vn, "the device's driver comes to the same outcomes on the same ladder and answers");
  c.ok(devAsked.size() == asked.size(), "and asks as many rungs");
  for (size_t i = 0; i < devAsked.size() && i < asked.size(); i++) {
    const std::string vnModel = asked[i].substr(asked[i].find('/') + 1);
    c.ok(has(devAsked[i], vnModel), "rung " + std::to_string(i + 1) + " on the same model: " + vnModel);
  }
  rig.setDeliver(nullptr);
  clearEnv();
}

// A Cumulo head on a routed forced reasoner ("mistral/mistral-large-4-0"): the fold asks
// the router for Mistral Small, on the wire (lane W's live legs: Large 4 folding on itself
// is cut at its fit), and the head's own pick is untouched.
void testRoutedForcedReasonerFoldsOnSmall(ndtest::Ctx& c) {
  std::printf("  -- (c) a Cumulo head on routed Large 4 folds on mistral/mistral-small-latest --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_FOLDVN", 1);   // a test value: the transport is scripted
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig::Options o = opts("fold-vn-c");
  o.models["cumulo"] = "mistral/mistral-large-4-0";
  NimbusdRig rig(cfg, o, &tx);
  const std::vector<agent::FoldRung> rungs = rig.engine().foldRungs();
  bool onSmall = false;
  for (const agent::FoldRung& r : rungs) {
    c.ok(r.model.find("large-4") == std::string::npos, "no rung on Large 4 (" + r.host + "/" + r.model + ")");
    if (r.host == "cumulo" && r.model == "mistral/mistral-small-latest") onSmall = true;
  }
  c.ok(onSmall, "the Cumulo rung is mistral/mistral-small-latest");
  const std::string chat = "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"";
  const std::string usage = "\"}}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";
  const auto noBusy = [](bool) {};
  std::string folded;
  for (int i = 0; i < 40 && folded.empty(); i++) {
    Exchange turn;
    turn.body = chat + esc("{\"reply\":\"noted\",\"memory\":\"\"}") + usage;
    tx.script.push_back(turn);
    rig.say("owner", padded(i));
    rig.noteFoldPumpEnd();
    const size_t before = tx.seen.size();
    Exchange fold;
    fold.body = chat + esc(kSummary) + usage;
    tx.script.push_back(fold);
    rig.foldTick(noBusy);
    if (tx.seen.size() > before) folded = tx.seen.back().body;
    else tx.script.pop_back();
  }
  c.ok(!folded.empty(), "a fold request went out");
  c.ok(has(folded, "\"model\":\"mistral-small-latest\""), "the fold asked the router for Mistral Small");
  c.ok(!has(folded, "large-4"), "and never for Large 4");
  c.ok(rig.cumuloModel() == "mistral/mistral-large-4-0", "the head's own pick is unchanged");
  // Lane CTXR1 (device parity): the pinned fold leaves the last good selector alone.
  c.ok(rig.lastGoodCumuloSelector() != "mistral/mistral-small-latest",
       "the pinned fold is not the head's last good model");
  clearEnv();
}

struct Web {
  explicit Web(NimbusdRig& r) : rig(r), eng(&r), api(&r, &eng, &replies) { eng.start(); }
  ~Web() { eng.stop(); }
  ApiResp call(const std::string& m, const std::string& path, const std::string& body = "") {
    for (int i = 0; i < 100; i++) {
      ApiResp out;
      api.handle(m, path, body, out);
      if (out.status != 503) return out;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return ApiResp{503, "", ""};
  }
  NimbusdRig& rig;
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

void testSummarizeNowRoutes(ndtest::Ctx& c) {
  std::printf("  -- (b) Summarize now: the route queues a catch-up, the status names its fields --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_FOLDVN", 1);
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("fold-vn-b"), &tx);
  for (int i = 0; i < 3; i++) {
    tx.script.push_back(turnAnswer("noted"));
    rig.say("web", "web note " + std::to_string(i));
  }
  std::mutex mu;
  std::vector<std::string> told;
  rig.setDeliver([&](const std::string& chat, const std::string& text) {
    std::lock_guard<std::mutex> lk(mu);
    if (chat == "web") told.push_back(text);
  });
  {
    Web web(rig);   // its engine thread runs the fold ticks, as on a live instance
    const ApiResp st = web.call("GET", "/api/fold/status?chat=web");
    c.ok(st.status == 200, "GET /api/fold/status answers");
    for (const char* k : {"\"summaryAgeH\":", "\"thru\":", "\"lastOk\":", "\"failedCycles\":", "\"nextTry\":",
                          "\"paidToday\":", "\"cap\":12", "\"rung\":", "\"state\":", "\"canRun\":true", "\"chats\":["})
      c.ok(has(st.body, k), std::string("status names ") + k);
    c.ok(!has(st.body, "null"), "no field is null");
    c.ok(web.call("GET", "/api/fold/status?chat=stranger").status == 404, "an unknown chat is refused");
    c.ok(web.call("POST", "/api/fold/run", "chat=stranger").status == 404, "and cannot be summarized");
    tx.script.push_back(responses("{\"reply\":\"" + esc(kSummary) + "\",\"memory\":\"\"}"));
    const ApiResp run = web.call("POST", "/api/fold/run", "chat=web");
    c.ok(run.status == 202 && has(run.body, "\"queued\":true"), "POST /api/fold/run queues it: 202");
    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
      landed = has(web.call("GET", "/api/fold/status?chat=web").body, "\"state\":\"fresh\"");
      if (!landed) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    c.ok(landed, "the manual catch-up lands and the badge turns fresh");
  }
  c.ok(has(rig.foldState("web").summary, "Walks to the market"), "the summary is stored");
  c.ok(rig.foldDriver().ctl().manualPendingChat.empty(), "the request is answered and dropped");
  {
    std::lock_guard<std::mutex> lk(mu);
    c.ok(told.size() == 1 && has(told[0], "\xE2\x9C\x93 Compacted. Summary updated: covered"),
         "one confirmation, naming what it covered");
  }
  rig.setDeliver(nullptr);
  clearEnv();
}

// Lane CTXR1: the hosted owner is found by role, not by position. A hosted instance approves
// chats as User, so the first allow-listed chat is often a guest: Summarize now's default
// chat and the daily alert go to the first Admin chat, else the web chat.
void testTheHostedOwnerIsTheAdminChat(ndtest::Ctx& c) {
  std::printf("  -- (c) the owner's chat is the Admin one, not the first approved --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_FOLDVN", 1);
  setenv("TELEGRAM_BOT_TOKEN", "123456:test-token-not-real", 1);
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("fold-vn-d"), &tx);
  std::string err;
  c.ok(rig.telegramAccess().approve("111111", "Guest", err), "a guest is approved first (User)");
  c.ok(rig.telegramAccess().approve("222222", "Owner", err), "then the owner's own chat");
  std::string out;
  c.ok(rig.foldStatusJson("", out) && has(out, std::string("\"chat\":\"") + kWebChatId + "\""),
       "with no Admin chat: the web chat, not the first approved guest");
  c.ok(rig.telegramAccess().setRole("222222", orch::Role::Admin, err), "the owner's chat is made Admin");
  c.ok(rig.foldStatusJson("", out) && has(out, "\"chat\":\"222222\""), "Summarize now defaults to the Admin chat");
  c.ok(!has(out, "\"chat\":\"111111\""), "never the first approved guest");
  clearEnv();
}

}  // namespace

int main() {
  ndtest::Ctx c;
  std::printf("=== the hosted fold driver is the device's (offline) ===\n");
  testAutomaticCycleMatchesTheDevice(c);
  testSummarizeNowRoutes(c);
  testRoutedForcedReasonerFoldsOnSmall(c);
  testTheHostedOwnerIsTheAdminChat(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf(c.failures == 0 ? "PASSED\n" : "FAILED\n");
  return c.failures == 0 ? 0 : 1;
}
