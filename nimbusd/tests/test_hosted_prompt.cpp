// test_hosted_prompt - CUM-542: every hosted head is told the truth about the
// instance it runs on.
//
// The composer's identity block said "Current date-time: UNKNOWN (device clock not
// synced)" on every hosted turn (nimbusd never set ComposeInputs.now) and described
// an ESP32 desk device with "Connectivity: offline", so a Virtual Nimbus refused
// time questions and called itself offline. The class rule, over EVERY head this
// daemon registers (openai, anthropic, mistral, cumulo, zai): the instructions carry
// the real clock in the owner's time zone, the engine version and a hosted identity,
// and never the desk-device or offline text.
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <cstdio>
#include <string>

#include <chrono>
#include <thread>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "tg_inbound.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY",
                        "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.toolLoop = false;
  return o;
}

// One turn on a rig whose ONLY key is `env`; returns the first request body.
static std::string firstHeadBody(const char* env, const std::string& tag) {
  clearEnv();
  setenv(env, "test_key_PROMPT", 1);
  FakeHttpTransport tx;
  for (int i = 0; i < 4; i++) tx.script.push_back(Exchange{"", "", 500, "{}"});   // the turn fails; we read the request
  Config cfg;
  NimbusdRig rig(cfg, opts(tag), &tx);
  rig.say("owner", "what time is it?");
  clearEnv();
  return tx.seen.empty() ? std::string() : tx.seen[0].body;
}

static void testEveryHeadGetsTheHostedTruth(ndtest::Ctx& c) {
  std::printf("  -- every head: real clock, version, hosted identity --\n");
  const char* kEnvs[][2] = {{"OPENAI_API_KEY", "openai"},   {"ANTHROPIC_API_KEY", "anthropic"},
                            {"MISTRAL_API_KEY", "mistral"}, {"CUMULO_API_KEY", "cumulo"},
                            {"Z_AI_TOKEN", "zai"}};
  for (const auto& e : kEnvs) {
    const std::string body = firstHeadBody(e[0], std::string("hp-") + e[1]);
    const std::string who = std::string(e[1]) + " head: ";
    c.ok(!body.empty(), who + "dispatched");
    // The date rides the cached static block, the time of day the per-turn one.
    c.ok(has(body, "Current date: 20"), who + "the prompt carries the real date");
    c.ok(has(body, "Current time: "), who + "the prompt carries the time of day");
    c.ok(!has(body, "date-time: UNKNOWN"), who + "never the unsynced-clock line");
    c.ok(has(body, "Firmware: " NIMBUS_FW_VERSION), who + "the engine version");
    c.ok(has(body, "hosted cloud instance that is always online"), who + "the hosted identity");
    c.ok(!has(body, "ESP32"), who + "no desk-device (ESP32) text");
    c.ok(!has(body, "Connectivity: offline"), who + "never told it is offline");
    c.ok(!has(body, "-LED ring") && !has(body, "color touchscreen"), who + "no ring or screen claimed");
  }
}

static void testClockFollowsTheOwnersZone(ndtest::Ctx& c) {
  std::printf("  -- the prompt's clock follows the saved time zone --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_PROMPT", 1);
  FakeHttpTransport tx;
  for (int i = 0; i < 4; i++) tx.script.push_back(Exchange{"", "", 500, "{}"});
  Config cfg;
  NimbusdRig rig(cfg, opts("hp-tz"), &tx);
  {
    EngineThread eng(&rig);
    eng.start();
    ReplyBuffer replies;
    WebApi api(&rig, &eng, &replies);
    ApiResp out;
    api.handle("POST", "/api/config", "devTz=QQT-4", out);
    c.eqi(out.status, 200, "the zone saves");
    eng.stop();
  }
  rig.say("owner", "what time is it?");
  // RB-REC5: the per-turn line is the shared stamp; the zone's name rides the date line.
  c.ok(!tx.seen.empty() && has(tx.seen[0].body, ", time zone QQT ("), "the next turn's clock is in the owner's zone");
  {
    EngineThread eng(&rig);
    eng.start();
    ReplyBuffer replies;
    WebApi api(&rig, &eng, &replies);
    ApiResp out;
    api.handle("POST", "/api/config", "devTz=", out);   // back to UTC for the rest of the run
    eng.stop();
  }
  clearEnv();
}

// Joiner-VN3: a message sent from the hosted web page was told "[CHANNEL] This
// message arrived via Telegram; your reply is delivered as a Telegram message". The
// page's turn (POST /api/chat) names the web UI, as the device's page does, and a
// real Telegram message from an approved chat still says Telegram.

// RB-REC1 (owner ruling 41): no stored file rides a hosted turn by itself. Whatever table the
// owner keeps (a food log, an expenses log, a training log: the same expectations for each),
// neither the owner's turn nor an approved member's carries its rows, its header or its
// name; the assistant reads a file with a tool when it needs one.
static void testNoStoredTableRidesTheHostedTurn(ndtest::Ctx& c) {
  std::printf("  -- the VN turn carries no stored table, for any domain --\n");
  struct Table {
    const char* path;
    const char* header;
    const char* row;
    const char* message;
  };
  const Table tables[] = {
      {"food/food-log.csv", "date,time,item,qty,kcal", "2026-11-19,12:40,soup,1,310", "I had soup for lunch, log it"},
      {"money/expenses.csv", "date,time,item,qty,amount", "2026-11-19,12:40,train ticket,1,24.30", "I paid 24.30 for the train"},
      {"gym/training.csv", "date,time,exercise,reps,kg", "2026-11-19,12:40,squat,5,80", "5 squats at 80 kg, log it"},
  };
  int n = 0;
  for (const Table& t : tables) {
    for (const char* who : {"owner", "444"}) {
      clearEnv();
      const std::string key = "cumulo_sk_test_TBL" + std::to_string(++n);
      setenv("CUMULO_API_KEY", key.c_str(), 1);
      FakeHttpTransport tx;
      for (int i = 0; i < 4; i++) tx.script.push_back(Exchange{"", "", 500, "{}"});
      Config cfg;
      NimbusdRig rig(cfg, opts(("hp-tbl" + std::to_string(n)).c_str()), &tx);
      rig.files().seed(t.path, std::string(t.header) + "\n" + t.row + "\n");
      if (std::string(who) == "444") {
        EngineThread eng(&rig);
        eng.start();
        ReplyBuffer replies;
        WebApi api(&rig, &eng, &replies);
        ApiResp out;
        api.handle("POST", "/api/orch", "tgAllow=444", out);
        c.eqi(out.status, 200, "the member chat is approved");
        eng.stop();
      }
      rig.say(who, t.message);
      const std::string what = std::string(t.path) + " / " + who;
      c.ok(!tx.seen.empty(), ("the turn dispatched: " + what).c_str());
      if (tx.seen.empty()) continue;
      c.ok(!has(tx.seen[0].body, t.header), ("no header row rides: " + what).c_str());
      c.ok(!has(tx.seen[0].body, t.row), ("no stored row rides: " + what).c_str());
      c.ok(!has(tx.seen[0].body, t.path), ("the turn never names the file: " + what).c_str());
      clearEnv();
    }
  }
}

// RB-Q3a: the instance's effect rings feed the hosted turn's [RECENT ACTIONS] block: the
// owner's turn shows the owner chat's receipt; another chat's never rides it.
static void testRecentActionsRideTheHostedTurn(ndtest::Ctx& c) {
  std::printf("  -- the VN turn carries its chat's recent actions --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_FX", 1);
  FakeHttpTransport tx;
  for (int i = 0; i < 4; i++) tx.script.push_back(Exchange{"", "", 500, "{}"});
  Config cfg;
  NimbusdRig rig(cfg, opts("hp-fx"), &tx);
  rig.effects().record("owner", 1794920520, "files.edit", "added row R3K9QZ to log/day.csv; 2 rows");
  rig.effects().record("444", 1794920520, "files.edit", "added row ROTHER to log/day.csv; 1 row");
  rig.say("owner", "make that 200");
  c.ok(!tx.seen.empty(), "the turn dispatched");
  c.ok(has(tx.seen[0].body, "[RECENT ACTIONS - "), "the recent-actions block rides the VN turn");
  c.ok(has(tx.seen[0].body, "files.edit: added row R3K9QZ"), "with the owner chat's receipt");
  c.ok(!has(tx.seen[0].body, "ROTHER"), "never another chat's");
  clearEnv();
}

static void testWebPageIsTheWebChannel(ndtest::Ctx& c) {
  // The hosted rig opts in to the page alias at construction (a device never does).
  nimbus::orch::setHostedWebChatAlias(false);
  {
    Config cfgA;
    NimbusdRig probe(cfgA, opts("hp-alias"));
    c.ok(nimbus::orch::deliveryChannelOf(nimbus::orch::kHostedWebChatId) == nimbus::orch::DeliveryChannel::Web,
         "constructing the hosted rig turns the page alias on");
  }
  std::printf("  -- a page message is the web UI, a Telegram message is Telegram --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_PROMPT", 1);
  FakeHttpTransport tx;
  for (int i = 0; i < 8; i++) tx.script.push_back(Exchange{"", "", 500, "{}"});   // we read the requests
  Config cfg;
  NimbusdRig rig(cfg, opts("hp-chan"), &tx);
  {
    EngineThread eng(&rig);
    eng.start();
    ReplyBuffer replies;
    WebApi api(&rig, &eng, &replies);
    ApiResp out;
    api.handle("POST", "/api/chat", "text=hello+from+the+page", out);
    c.eqi(out.status, 200, "the page's message is accepted");
    for (int i = 0; i < 250 && tx.seen.empty(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    eng.stop();
  }
  c.ok(!tx.seen.empty(), "the page's turn dispatched");
  if (!tx.seen.empty()) {
    const std::string& b = tx.seen[0].body;
    c.ok(has(b, "[CHANNEL] This message arrived via the web UI."), "[CHANNEL] names the web UI");
    c.ok(!has(b, "arrived via Telegram"), "never Telegram for a page message");
  }
  std::string err;
  c.ok(rig.telegramAccess().approve("424242", "Ana", err), "a Telegram chat is approved");
  const size_t before = tx.seen.size();
  nimbusd::TgInboundIo io;
  io.turn = [&rig](const std::string& chat, const std::string& text) { rig.say(chat, text); };
  nimbus::tg::Update u;
  u.chatId = "424242";
  u.from = "Ana";
  u.text = "hello from Telegram";
  nimbusd::routeTelegramUpdates({u}, rig.telegramAccess(), io, time(nullptr));
  c.ok(tx.seen.size() > before, "the Telegram message ran a turn");
  if (tx.seen.size() > before)
    c.ok(has(tx.seen[before].body, "[CHANNEL] This message arrived via Telegram;"),
         "a real Telegram message still says Telegram");
  clearEnv();
}

// The hosted head runs a single host's tool loop, never the fabric: on a direct key and on the
// router. A provider's window refusal after a tool ran is trimmed and answered on the same turn,
// the tool does not run again, and the refitted prompt is still the hosted one.
static void testOverflowAfterAToolIsTrimmedInTheLoop(ndtest::Ctx& c) {
  struct Head {
    const char* env;
    const char* key;
    const char* tool;
    const char* answer;
    const char* refusal;
  };
  const Head heads[] = {
      {"MISTRAL_API_KEY", "test_key_PROMPT",
       "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"toolcall1\","
       "\"function\":{\"name\":\"memory_search\",\"arguments\":\"{\\\"query\\\":\\\"x\\\"}\"}}]}}]}",
       "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"abcdefghi\","
       "\"function\":{\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"answered after the trim\\\"}\"}}]}}]}",
       "{\"object\":\"error\",\"message\":\"Prompt 1100000 > 1048576 maximum context length\",\"type\":"
       "\"invalid_request_prompt_too_long\",\"code\":\"3059\"}"},
      {"CUMULO_API_KEY", "cumulo_sk_test_PROMPT",
       "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"toolcall1\","
       "\"name\":\"memory_search\",\"arguments\":\"{\\\"query\\\":\\\"x\\\"}\"}]}",
       "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c\","
       "\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"answered after the trim\\\"}\"}]}",
       "{\"error\":{\"message\":\"Your input exceeds the context window of this model.\",\"type\":"
       "\"invalid_request_error\",\"code\":\"context_length_exceeded\"}}"},
  };
  for (const Head& h : heads) {
    std::printf("  -- %s: a window refusal after a tool ran --\n", h.env);
    clearEnv();
    setenv(h.env, h.key, 1);
    FakeHttpTransport tx;
    tx.script.push_back(Exchange{"", "", 200, h.tool});
    tx.script.push_back(Exchange{"", "", 400, h.refusal});
    tx.script.push_back(Exchange{"", "", 200, h.answer});
    Config cfg;
    NimbusdRig::Options o = opts(std::string("hp-trim-") + h.env);
    o.toolLoop = true;
    NimbusdRig rig(cfg, o, &tx);
    const TurnRecord t = rig.say("owner", "Use a web lookup and compare the three plans in detail");
    c.eqi((int)tx.seen.size(), 3, "the tool round, the refusal, the trimmed retry");
    c.ok(has(t.reply, "answered after the trim"), "the owner gets the answer");
    if (tx.seen.size() == 3) {
      const std::string& b = tx.seen[2].body;
      c.ok(has(b, "LEFT OUT"), "the retry says what it left out");
      c.ok(has(b, "THIS TURN RUNS ON"), "the retry still names the host it runs on");
      c.ok(has(b, "hosted Virtual Nimbus") || has(b, "a Virtual Nimbus"), "the refitted prompt is the hosted one");
      c.ok(!has(b, "ESP32"), "never the desk device's words");
    }
    clearEnv();
  }
}

int main() {
  ndtest::Ctx c;
  std::printf("=== hosted prompt truth (CUM-542, offline) ===\n");
  testEveryHeadGetsTheHostedTruth(c);
  testClockFollowsTheOwnersZone(c);
  testNoStoredTableRidesTheHostedTurn(c);
  testRecentActionsRideTheHostedTurn(c);
  testWebPageIsTheWebChannel(c);
  testOverflowAfterAToolIsTrimmedInTheLoop(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
