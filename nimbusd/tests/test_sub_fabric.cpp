// test_sub_fabric - offline (T2) proof that a hosted instance runs sub-agents the
// way the device does, so a Mistral Studio connector actually executes: the head
// spawns a Mistral sub, the sub's Conversations call carries the Studio
// connectors and the configured sub model, and the result comes back to the owner
// through the synthesis turn. Before the fabric was wired the JobEngine dropped
// every spawn SILENTLY (no adapter, no message) - the class rule here is "a spawn
// always ends in a result or an honest failure message, never silence".
//
//   (a) Mistral spawn end to end, driven by the real EngineThread pump;
//   (b) a spawn on an unkeyed provider says so (no silent drop);
//   (c) the durable journal re-attaches an unfinished job after a restart;
//   (d) background engine work is not a turn: a web write queued behind it is
//       applied, not refused busy.
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "sub_fabric.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY",
                        "TAVILY_API_KEY", "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.priority = "mistral";
  o.toolLoop = false;   // single-shot head: one scripted exchange per turn
  o.models["mistral"] = "mistral-small-latest";
  return o;
}

// A single-shot Mistral head turn whose orch_turn JSON is `turnJson`.
static Exchange headTurn(const std::string& turnJson) {
  std::string esc;
  for (char ch : turnJson) {
    if (ch == '"' || ch == '\\') esc += '\\';
    esc += ch;
  }
  Exchange e;
  e.expectHost = "api.mistral.ai";
  e.expectPathContains = "/v1/conversations";
  e.body = "{\"conversation_id\":\"head1\",\"outputs\":[{\"type\":\"message.output\",\"content\":\"" +
           esc + "\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";
  return e;
}

static const char* kGcal =
    "[{\"name\":\"gcal\",\"type\":\"gcal\",\"prov\":\"mistral\",\"kind\":\"connector\","
    "\"cid\":\"connector_googlecalendar\",\"en\":1}]";

struct Inbox {
  std::mutex mu;
  std::vector<std::string> msgs;
  void push(const std::string& t) {
    std::lock_guard<std::mutex> lk(mu);
    msgs.push_back(t);
  }
  bool waitFor(const std::string& needle, int ms) {
    for (int i = 0; i < ms / 20; i++) {
      {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& m : msgs)
          if (has(m, needle)) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
};

// (a) the device's connector path, end to end on the hosted composition.
static void testMistralSpawnEndToEnd(ndtest::Ctx& c) {
  std::printf("  -- (a) head spawns a Mistral sub; the sub carries gcal; synthesis delivers --\n");
  clearEnv();
  FakeHttpTransport tx;
  tx.script.push_back(headTurn(
      "{\"reply\":\"Checking your calendar.\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":\"spawn\","
      "\"id\":null,\"task\":\"List every event on my calendar today\",\"provider\":\"mistral\","
      "\"model\":null,\"skill\":\"gcal\"}]}"));
  Exchange sub;
  sub.expectHost = "api.mistral.ai";
  sub.expectPathContains = "/v1/conversations";
  sub.body = "{\"conversation_id\":\"sub1\",\"outputs\":[{\"type\":\"tool.execution\","
             "\"name\":\"google_calendar_list_events\"},{\"type\":\"message.output\","
             "\"content\":\"Standup at 10:00; Dentist at 15:00\"}]}";
  tx.script.push_back(sub);
  tx.script.push_back(headTurn(
      "{\"reply\":\"Today: Standup at 10:00 and Dentist at 15:00.\",\"memory\":\"\",\"ask\":\"\"}"));

  Config cfg;
  NimbusdRig rig(cfg, opts("fab-a"), &tx);
  rig.applyProviderKey("mistral", "mk_TEST_FAB_1");
  c.eq(rig.connectors().replaceBlob(kGcal), "", "a gcal Studio connector is configured");
  Inbox inbox;
  rig.setDeliver([&inbox](const std::string&, const std::string& t) { inbox.push(t); });
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("web", "What is on my calendar today?");

  c.ok(inbox.waitFor("Checking your calendar.", 5000), "the head turn replied and spawned");
  c.ok(inbox.waitFor("Dentist at 15:00.", 15000),
       "the synthesis turn delivered the sub-agent's calendar result (no silent drop)");
  eng.stop();

  c.eqi((long)tx.seen.size(), 3, "exactly three provider calls: head, sub, synthesis");
  if (tx.seen.size() >= 2) {
    const std::string& b = tx.seen[1].body;
    c.ok(has(b, "\"connector_id\":\"google_calendar\""),
         "the sub-agent's Conversations call carries the gcal Studio connector");
    c.ok(has(b, "\"model\":\"mistral-small-latest\""), "the sub runs the configured sub model");
    c.ok(has(b, "List every event on my calendar today"), "the sub carries the spawned task");
    c.ok(has(b, "[Current date-time: "), "the sub brief is anchored to the instance clock");
  }
  if (tx.seen.size() >= 3)
    c.ok(has(tx.seen[2].body, "Standup at 10:00; Dentist at 15:00"),
         "the synthesis turn sees the sub-agent's result");
  c.eqi(rig.jobs().activeCount(), 0, "the job drained (nothing left queued or running)");
  clearEnv();
}

// (b) no silent drop: a spawn that cannot run says so.
static void testUnkeyedSpawnSaysSo(ndtest::Ctx& c) {
  std::printf("  -- (b) a spawn on an unkeyed provider gets an honest message --\n");
  clearEnv();
  FakeHttpTransport tx;
  tx.script.push_back(headTurn(
      "{\"reply\":\"Starting a researcher.\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":\"spawn\","
      "\"id\":null,\"task\":\"research X\",\"provider\":\"openai\",\"model\":null}]}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("fab-b"), &tx);
  rig.applyProviderKey("mistral", "mk_TEST_FAB_2");
  Inbox inbox;
  rig.setDeliver([&inbox](const std::string&, const std::string& t) { inbox.push(t); });
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("web", "research X on openai");
  c.ok(inbox.waitFor("Couldn't start that agent on openai.", 8000),
       "the unkeyed openai spawn is reported, not dropped");
  eng.stop();
  c.eqi((long)tx.seen.size(), 1, "no request was sent to the unkeyed provider");
  clearEnv();
}

// (c) the journal is durable: an unfinished job survives a restart.
static void testJournalReattaches(ndtest::Ctx& c) {
  std::printf("  -- (c) the job journal re-attaches after a restart --\n");
  const std::string dir = ndtest::scratchDir("fab-c") + "/journal";
  ndtest::rmTree(ndtest::scratchDir("fab-c"));
  {
    FileJournalStore store(dir);
    nimbus::orch::Journal j;
    j.begin(&store);
    nimbus::orch::JobRecord r{};
    std::snprintf(r.tag, sizeof r.tag, "job0007");
    std::snprintf(r.jobId, sizeof r.jobId, "openai:resp_abc123");
    std::snprintf(r.backend, sizeof r.backend, "openai");
    std::snprintf(r.chatId, sizeof r.chatId, "web");
    r.state = nimbus::orch::JobState::Running;
    c.ok(j.write(r), "a running job is journaled");
  }
  FileJournalStore store2(dir);
  nimbus::orch::Journal j2;
  j2.begin(&store2);
  nimbus::orch::JobRecord back{};
  c.eqi(j2.count(), 1, "a fresh process sees the unfinished job");
  c.ok(j2.get(0, back) && std::string(back.jobId) == "openai:resp_abc123",
       "its re-attach key survived byte-identical");
  j2.markSeen("job0007");
  j2.gc();
  FileJournalStore store3(dir);
  nimbus::orch::Journal j3;
  j3.begin(&store3);
  c.eqi(j3.count(), 0, "a delivered + compacted job is gone after the next restart");
  ndtest::rmTree(ndtest::scratchDir("fab-c"));
}

// (d) background engine work is not a turn.
static void testBackgroundWorkIsNotATurn(ndtest::Ctx& c) {
  std::printf("  -- (d) a web write behind background work is applied, not refused --\n");
  clearEnv();
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("fab-d"), &tx);
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);
  eng.postWork([] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));   // the work is running now
  c.ok(!eng.snapshot().turnInFlight, "background work does not read as a turn in flight");
  ApiResp out;
  api.handle("POST", "/api/orch", "oaiKey=sk_TEST_FAB_3", out);
  c.eqi(out.status, 200, "the key write queued behind it is applied (200), not refused busy");
  c.ok(has(out.body, "\"applied\":1"), "the write reports it applied");
  ApiResp orch;
  api.handle("GET", "/api/orch", "", orch);
  c.ok(has(orch.body, "\"orchLoop\":false"), "/api/orch reports the configured tool loop (off here)");
  eng.stop();
  clearEnv();
}

// (e) an engine rebuild (a key/model save) keeps queued spawns.
static void testRebuildKeepsJobs(ndtest::Ctx& c) {
  std::printf("  -- (e) a key/model save does not drop queued sub-agent work --\n");
  clearEnv();
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("fab-e"), &tx);
  rig.applyProviderKey("mistral", "mk_TEST_FAB_4");
  nimbus::orch::Spawn sp;
  sp.task = "look something up";
  sp.provider = "mistral";
  agent::JobEngine* before = &rig.jobs();
  rig.jobs().enqueueSpawn(sp, "owner", /*quiet=*/true);
  c.eqi(rig.jobs().pendingCount(), 1, "one spawn is queued (not yet dispatched)");
  c.ok(rig.applyOrchModel("mistral", "mistral-medium-latest"), "a model save rebuilds the engine");
  c.ok(&rig.jobs() == before, "the job engine survived the rebuild");
  c.eqi(rig.jobs().pendingCount(), 1, "the queued spawn is still queued");
  clearEnv();
}

// (f) an unauthenticated chat (Telegram with no chat lock) cannot spawn.
static void testUntrustedChatCannotSpawn(ndtest::Ctx& c) {
  std::printf("  -- (f) a chat the daemon could not authenticate gets no sub-agents --\n");
  clearEnv();
  FakeHttpTransport tx;
  tx.script.push_back(headTurn(
      "{\"reply\":\"On it.\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":\"spawn\","
      "\"id\":null,\"task\":\"read the calendar\",\"provider\":\"mistral\",\"model\":null}]}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("fab-f"), &tx);
  rig.applyProviderKey("mistral", "mk_TEST_FAB_5");
  rig.noteUntrustedChat("424242");
  Inbox inbox;
  rig.setDeliver([&inbox](const std::string&, const std::string& t) { inbox.push(t); });
  rig.say("424242", "what is on my calendar?");
  c.ok(inbox.waitFor("Sub-agents are off in this chat", 100), "the spawn is refused with the reason");
  c.eqi(rig.jobs().activeCount(), 0, "nothing was queued");
  c.eqi((long)tx.seen.size(), 1, "only the head turn reached a provider");
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  c.suite = "sub-agent fabric (device parity)";
  std::printf("=== %s ===\n", c.suite);
  testMistralSpawnEndToEnd(c);
  testUnkeyedSpawnSaysSo(c);
  testJournalReattaches(c);
  testBackgroundWorkIsNotATurn(c);
  testRebuildKeepsJobs(c);
  testUntrustedChatCannotSpawn(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
