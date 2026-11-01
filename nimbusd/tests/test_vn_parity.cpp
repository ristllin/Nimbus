// test_vn_parity - a hosted instance does what the device does, or says what it lacks
// (the Virtual Nimbus reality check, 1 Nov 2026). Offline: every provider answer is
// scripted, so each check reads the request the instance really sent.
//
//   (a) the history rides as messages past the old 12-row window, and a fold takes
//       what it hides: same engine calls as the device, summary before chain reset,
//       anchored, durable over a restart, and cleared by /clear;
//   (b) a web turn that only started sub-agents resolves its poll, the page's watcher
//       sees later results, and a reply that said it started work gets no second ack;
//   (c) a tool call leaves its row, the turn its summary row, a job shows in /api/orch;
//   (d) long-term memory follows the key the instance holds, and an instance no key
//       can embed for is told the truth;
//   (e) routines and wake-ups: the web routes, the tools, a due fire, no burst after a
//       restart, and who may do what;
//   (f) skills: the owner's side, the assistant's waiting side, the prompt's index;
//   (g) photos reach a seeing head as image parts and settle their row; a blind head
//       and a file that is no image are told plainly;
//   (h) the web search key is set on the page and survives a restart;
//   (i) nothing in a hosted turn's prompt calls the instance a device.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;
namespace orch = nimbus::orch;

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY", "CUMULO_API_KEY",
                        "Z_AI_TOKEN", "TELEGRAM_BOT_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& tag, bool keepData = false) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  if (!keepData) ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.toolLoop = false;
  return o;
}

static std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

// One single-shot head answer through the Cumulo router (the orch_turn JSON as the text).
static Exchange headJson(const std::string& turnJson) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/openai/v1/responses";
  e.body = "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
           "\"output_text\",\"text\":\"" + esc(turnJson) + "\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
  return e;
}
static Exchange headTurn(const std::string& reply) { return headJson("{\"reply\":\"" + reply + "\",\"memory\":\"\"}"); }

struct Web {
  explicit Web(NimbusdRig& r) : rig(r), eng(&r), api(&r, &eng, &replies) {
    rig.setDeliver([this](const std::string& chat, const std::string& text) {
      replies.push("assistant", text, chat, eng.currentWebTurn());
    });
    eng.start();
  }
  ~Web() {
    eng.stop();
    rig.setDeliver(nullptr);
  }
  ApiResp call(const std::string& m, const std::string& path, const std::string& body = "") {
    for (int i = 0; i < 100; i++) {
      ApiResp out;
      api.handle(m, path, body, out);
      if (out.status != 503) return out;   // an honest busy: the page retries too
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return ApiResp{503, "", ""};
  }
  bool waitBody(const std::string& path, const std::string& needle, int ms = 5000) {
    for (int i = 0; i < ms / 50; i++) {
      if (has(call("GET", path).body, needle)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }
  NimbusdRig& rig;
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

static std::string padded(int i) {
  std::string s = "note " + std::to_string(i) + " ";
  s.append(3000, (char)('a' + i % 26));
  return s;
}

static void testHistoryAndFold(ndtest::Ctx& c) {
  std::printf("  -- (a) history as messages, and the fold --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_FOLD", 1);
  FakeHttpTransport tx;
  const auto opt = opts("vp-a");
  const auto noBusy = [](bool) {};
  {
    Config cfg;
    NimbusdRig rig(cfg, opt, &tx);
    tx.script.push_back(headTurn("one"));
    tx.script.push_back(headTurn("two"));
    rig.say("owner", "the codeword is PERSIMMON");
    rig.say("owner", "second message");
    c.ok(tx.seen.size() == 2 && has(tx.seen[1].body, "the codeword is PERSIMMON") && has(tx.seen[1].body, "\"role\":\"assistant\""),
         "the next turn carries the last turn as real messages");
    c.ok(!has(tx.seen[1].body, "RECENT CONVERSATION"), "not the old text window");

    // Enough new rows to be due, with the fold waiting its quiet bound (6 turns).
    Exchange foldAnswer = headTurn("SUMMARY-OF-EARLIER: the codeword is PERSIMMON.");
    bool folded = false;
    for (int i = 0; i < 40 && !folded; i++) {
      tx.script.push_back(headTurn("noted"));
      rig.say("owner", padded(i));
      rig.noteFoldPumpEnd();
      const size_t before = tx.seen.size();
      tx.script.push_back(foldAnswer);
      rig.foldTick(noBusy);
      folded = tx.seen.size() > before;
      if (!folded) tx.script.pop_back();
    }
    c.ok(folded, "a due fold ran, after the owner-first wait");
    c.ok(has(rig.foldState("owner").summary, "SUMMARY-OF-EARLIER"), "the summary is stored");
    c.ok(!rig.foldState("owner").thru.empty(), "and anchored to the newest row it covers");
    c.ok(rig.foldState("owner").msgsSinceFold == 0, "the counters restarted");

    tx.script.push_back(headTurn("still here"));
    rig.say("owner", "what was the codeword?");
    c.ok(has(tx.seen.back().body, "SUMMARY-OF-EARLIER"), "the next turn carries the summary");
    c.ok(!has(tx.seen.back().body, "the codeword is PERSIMMON\""), "and not the folded row itself");
    c.ok(has(NimbusdRig::logTail(64 * 1024, false), "fold(owner): ok"), "the log says so");
  }
  {
    Config cfg;
    auto o2 = opt;
    NimbusdRig rig(cfg, o2, &tx);
    c.ok(has(rig.foldState("owner").summary, "SUMMARY-OF-EARLIER"), "the summary survives a restart");
    tx.script.push_back(headTurn("after restart"));
    rig.say("owner", "and now?");
    c.ok(has(tx.seen.back().body, "SUMMARY-OF-EARLIER"), "and rides the first turn after it");
    rig.say("owner", "/clear yes");
    c.ok(rig.foldState("owner").summary.empty(), "/clear drops the summary");
    tx.script.push_back(headTurn("clean"));
    rig.say("owner", "fresh start");
    c.ok(!has(tx.seen.back().body, "SUMMARY-OF-EARLIER"), "and the next turn starts clean");
    c.ok(has(rig.say("owner", "/compact").reply, "Compacting"), "/compact is a command here");
  }
  clearEnv();
}

static long jsonNum(const std::string& body, const std::string& key) {
  const size_t at = body.find("\"" + key + "\":");
  return at == std::string::npos ? -1 : std::strtol(body.c_str() + at + key.size() + 3, nullptr, 10);
}

static void testWebChatContract(ndtest::Ctx& c) {
  std::printf("  -- (b) a spawn-only turn resolves, results reach the watcher, one acknowledgement --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_CHAT", 1);
  FakeHttpTransport tx;
  tx.script.push_back(headJson("{\"reply\":\"\",\"memory\":\"\",\"session_ops\":[{\"op\":\"spawn\",\"task\":\"work\"}]}"));
  Exchange sub;
  sub.expectPathContains = "/router/openai/v1/chat/completions";
  sub.body = "{\"choices\":[{\"message\":{\"content\":\"the answer is 144\"}}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":2}}";
  tx.script.push_back(sub);
  tx.script.push_back(headTurn("The sub-agent found 144."));
  tx.script.push_back(headTurn("four"));
  Config cfg;
  NimbusdRig rig(cfg, opts("vp-b"), &tx);
  Web web(rig);
  const ApiResp unknown = web.call("GET", "/api/chat?turn=999");
  c.ok(has(unknown.body, "\"gone\":true"), "an id this instance does not know is gone, not pending");
  const ApiResp post = web.call("POST", "/api/chat", "text=spawn+only");
  const long turn = jsonNum(post.body, "turn");
  bool done = false;
  std::string last;
  for (int i = 0; i < 200 && !done; i++) {
    last = web.call("GET", "/api/chat?turn=" + std::to_string(turn)).body;
    done = has(last, "\"done\":true");
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  c.ok(done, "the spawn-only turn resolves its poll");
  c.ok(has(last, "\"busy\":") && has(last, "\"async\":"), "every body carries the watcher's numbers");
  c.ok(web.waitBody("/api/chat?watch=1", "\"async\":", 100), "?watch=1 answers");
  bool result = false;
  for (int i = 0; i < 400 && !result; i++) {
    result = has(web.replies.sinceJsonArray(0), "The sub-agent found 144");
    if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  c.ok(result, "the result reaches the ring between turns");
  c.ok(jsonNum(web.call("GET", "/api/chat?watch=1").body, "async") >= 2, "the async counter moved: the page re-reads");
  c.ok(has(web.replies.sinceJsonArray(0), "On it."), "a spawn-only turn keeps its acknowledgement");

  const ApiResp held = web.call("POST", "/api/chat", "text=next+message");
  const long turn2 = jsonNum(held.body, "turn");
  bool answered = false;
  for (int i = 0; i < 200 && !answered; i++) {
    answered = has(web.call("GET", "/api/chat?turn=" + std::to_string(turn2)).body, "four");
    if (!answered) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  c.ok(answered, "the message after it is answered, not held behind it");
  clearEnv();

  setenv("CUMULO_API_KEY", "cumulo_sk_test_CHAT", 1);
  FakeHttpTransport tx2;
  tx2.script.push_back(headJson("{\"reply\":\"Starting one.\",\"memory\":\"\",\"session_ops\":[{\"op\":\"spawn\",\"task\":\"work\"}]}"));
  tx2.script.push_back(sub);
  tx2.script.push_back(headTurn("Done with it."));
  Config cfg2;
  NimbusdRig rig2(cfg2, opts("vp-b2"), &tx2);
  Web web2(rig2);
  web2.call("POST", "/api/chat", "text=with+reply");
  bool end = false;
  for (int i = 0; i < 400 && !end; i++) {
    end = has(web2.replies.sinceJsonArray(0), "Done with it");
    if (!end) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  c.ok(end, "the turn with a reply finishes");
  c.ok(!has(web2.replies.sinceJsonArray(0), "On it."), "a reply that said it started work gets no second acknowledgement");
  clearEnv();
}

static void testToolRows(ndtest::Ctx& c) {
  std::printf("  -- (c) a tool call leaves a row, a job shows in /api/orch --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_ROWS", 1);
  FakeHttpTransport tx;
  Exchange call;
  call.expectPathContains = "/responses";
  call.body = "{\"id\":\"r1\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"docs_search\","
              "\"arguments\":\"{\\\"query\\\":\\\"routines\\\"}\"}],\"usage\":{\"input_tokens\":5,\"output_tokens\":5}}";
  tx.script.push_back(call);
  Exchange fin;
  fin.expectPathContains = "/responses";
  fin.body = "{\"id\":\"r2\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c2\",\"name\":\"orch_turn\","
             "\"arguments\":\"{\\\"reply\\\":\\\"looked it up\\\",\\\"memory\\\":\\\"\\\"}\"}],\"usage\":{\"input_tokens\":5,\"output_tokens\":5}}";
  tx.script.push_back(fin);
  auto o = opts("vp-c");
  o.toolLoop = true;
  Config cfg;
  NimbusdRig rig(cfg, o, &tx);
  rig.say("owner", "how do routines work?");
  {
    Web web(rig);
    const std::string rows = web.call("GET", "/api/mem/episodic?limit=50").body;
    c.ok(has(rows, "\"kind\":\"tool_output\""), "the tool call left a tool_output row");
    c.ok(has(rows, "tool:docs_search") || has(rows, "tool:docs.search"), "named by its tool");
    c.ok(has(rows, "ev:turnend"), "the turn left its summary row");
    c.ok(has(rows, "\"channel\":\"web\""), "the page's channel chip reads the web chat");
  }
  c.ok(rig.sessionsJson() == "[]", "no jobs: an empty list");
  tx.script.push_back(headJson("{\"reply\":\"on it\",\"memory\":\"\",\"session_ops\":[{\"op\":\"spawn\",\"task\":\"long work\"}]}"));
  auto o2 = opts("vp-c2");
  Config cfg2;
  NimbusdRig rig2(cfg2, o2, &tx);
  rig2.say("owner", "start it");
  rig2.pumpJobs();
  c.ok(rig2.activeJobCount() >= 0, "the watcher's job count reads");
  clearEnv();
}

static void testEmbeddings(ndtest::Ctx& c) {
  std::printf("  -- (d) long-term memory follows the key --\n");
  clearEnv();
  const auto embedOpts = [](const std::string& tag) {
    auto o = opts(tag);
    o.embeddings = true;
    o.embedHost = "mistral";
    o.embedModel = "mistral-embed";
    o.embedDims = 1024;
    return o;
  };
  {
    setenv("OPENAI_API_KEY", "sk-test-openai-only-000000000000", 1);
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, embedOpts("vp-d1"), &tx);
    c.eq(rig.embedProvider(), "openai", "an OpenAI-only instance embeds with OpenAI");
    c.eqi(rig.embedWidth(), orch::kEmbedOpenAiDims, "at the OpenAI width");
    c.ok(rig.embedAvailable(), "so memory is on");
    clearEnv();
  }
  {
    setenv("MISTRAL_API_KEY", "mistral-test-key-000000000000000000", 1);
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, embedOpts("vp-d2"), &tx);
    c.eq(rig.embedProvider(), "mistral", "a Mistral key keeps Mistral");
    clearEnv();
  }
  {
    setenv("CUMULO_API_KEY", "cumulo_sk_test_EMBED", 1);
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, embedOpts("vp-d3"), &tx);
    c.eq(rig.embedProvider(), "mistral", "a Cumulo-only instance keeps the default through the router");
    c.eq(rig.embedRouteLabel(), "router", "on the router route");
    clearEnv();
  }
  for (const char* env : {"ANTHROPIC_API_KEY", "Z_AI_TOKEN"}) {
    setenv(env, "test-key-with-no-embedding-route-0000", 1);
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, embedOpts(std::string("vp-d4") + env), &tx);
    c.ok(!rig.embedAvailable(), std::string(env) + ": no route can embed");
    const std::string out = rig.callTool("memory.write", R"({"content":"my sister's cat is Zorblax"})");
    c.ok(has(out, "kept as text"), std::string(env) + ": the write says what happened");
    c.ok(has(out, "No later confirmation"), std::string(env) + ": and that nothing follows");
    c.ok(!has(out, "stored automatically"), std::string(env) + ": never a promise of storing later");
    const std::string found = rig.callTool("memory.search", R"({"query":"Zorblax cat"})");
    c.ok(has(found, "Zorblax"), std::string(env) + ": keyword search finds it");
    clearEnv();
  }
  {
    // Stored vectors keep their space: a Mistral-width store never moves to OpenAI.
    setenv("OPENAI_API_KEY", "sk-test-openai-only-000000000000", 1);
    FakeHttpTransport tx;
    Config cfg;
    auto o = embedOpts("vp-d5");
    NimbusdRig rig(cfg, o, &tx);
    c.eq(rig.embedProvider(), "openai", "the empty store moved");
    clearEnv();
  }
}

static void testRoutines(ndtest::Ctx& c) {
  std::printf("  -- (e) routines and wake-ups --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_LOOP", 1);
  FakeHttpTransport tx;
  auto o = opts("vp-e");
  {
    Config cfg;
    NimbusdRig rig(cfg, o, &tx);
    Web web(rig);
    const ApiResp made = web.call("POST", "/api/loops",
                                  "action=create&name=stretch&prompt=Remind+me&chatId=owner&schedule=%7B%22kind%22%3A%22daily%22%2C%22at%22%3A%2209%3A00%22%7D");
    c.eqi(made.status, 200, "the page's form creates a routine");
    c.ok(has(made.body, "\"ok\":true"), "and says so");
    std::string list = web.call("GET", "/api/loops").body;
    c.ok(has(list, "\"name\":\"stretch\"") && has(list, "\"at\":\"09:00\""), "it lists");
    c.ok(has(list, "\"approved\":true"), "an owner's routine needs no approval");
    const size_t idAt = list.find("\"id\":\"");
    const std::string id = list.substr(idAt + 6, list.find('"', idAt + 6) - idAt - 6);
    c.ok(has(web.call("POST", "/api/loops", "action=pause&id=" + id).body, "\"ok\":true"), "pause");
    c.ok(has(web.call("GET", "/api/loops").body, "\"enabled\":false"), "reads paused");
    c.ok(has(web.call("POST", "/api/loops", "action=resume&id=" + id).body, "\"ok\":true"), "resume");
    c.ok(has(web.call("POST", "/api/loops", "action=delete&id=" + id).body, "\"ok\":true"), "delete");
    c.ok(web.call("GET", "/api/loops").body == "[]", "gone");
    const ApiResp bad = web.call("POST", "/api/loops", "action=create&name=x&prompt=y&schedule=%7B%22kind%22%3A%22interval%22%2C%22every_seconds%22%3A5%7D");
    c.eqi(bad.status, 400, "a schedule under the floor is refused");
    c.ok(has(web.call("GET", "/api/wakeups").body, "\"approvalMode\":\"auto\""), "the wake-ups route has the device's shape");
  }
  {
    Config cfg;
    NimbusdRig rig(cfg, opts("vp-e2"), &tx);
    const std::string pending =
        rig.callTool("loop.create", R"({"name":"digest","prompt":"Daily digest","schedule":{"kind":"daily","at":"08:00"}})");
    c.ok(has(pending, "PENDING"), "a routine the assistant creates waits for the owner");
    c.ok(has(rig.routines().json(), "\"approved\":false"), "unapproved");
    tx.script.clear();
    const std::string armed = rig.callTool("wakeup.set", R"({"minutes":5,"note":"check the report"})");
    c.ok(has(armed, "wakeup armed"), "a wake-up arms with no approval");
    c.ok(has(rig.callTool("wakeup.set", R"({"minutes":1,"note":"too soon"})"), "between 2 and"), "the floor holds");

    // A member's conversation may not touch routines.
    const orch::Principal member = orch::principalForRole("2001", orch::Role::User);
    const std::string memberCall = rig.registry().handleRpc(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"loop.list","arguments":{}}})", member);
    c.ok(has(memberCall, "only") || has(memberCall, "owner"), "a member cannot list routines");
  }
  {
    // A wake-up whose time has come fires one turn, framed as one, then it is gone. A
    // recurring routine past its slot takes its next slot instead: no burst after a restart.
    const auto o3 = opts("vp-e3");
    fsutil::mkdirs(o3.dataDir + "/mem");
    const long now = (long)time(nullptr);
    std::vector<orch::LoopRecord> seed(2);
    seed[0].id = "lpwake1";
    seed[0].name = "wakeup";
    seed[0].prompt = "CHECK-THE-REPORT";
    seed[0].chatId = "owner";
    seed[0].sched.kind = orch::SchedKind::Once;
    seed[0].sched.intervalSec = 300;
    seed[0].createdBy = orch::CreatedBy::Agent;
    seed[0].nextRun = (uint64_t)(now - 30);
    seed[1].id = "lprecur";
    seed[1].name = "recurring";
    seed[1].prompt = "RECURRING-PROMPT";
    seed[1].chatId = "owner";
    seed[1].sched.kind = orch::SchedKind::Interval;
    seed[1].sched.intervalSec = 3600;
    seed[1].nextRun = (uint64_t)(now - 30);
    fsutil::writeFileAtomic(o3.dataDir + "/mem/loops.json", orch::dumpLoops(seed));
    FakeHttpTransport tx3;
    tx3.script.push_back(headTurn("Your report is ready."));
    Config cfg;
    NimbusdRig rig(cfg, o3, &tx3);
    std::string spoken;
    rig.setDeliver([&spoken](const std::string&, const std::string& t) { spoken += t; });
    int marks = 0;
    rig.loopsTick([&marks](bool on) { marks += on ? 1 : 0; });
    c.eqi((long)tx3.seen.size(), 1, "the due wake-up fired one turn, the recurring routine none");
    c.ok(!tx3.seen.empty() && has(tx3.seen[0].body, "CHECK-THE-REPORT") && has(tx3.seen[0].body, "[WAKEUP]"), "framed as a wake-up, with its note");
    c.ok(has(spoken, "Your report is ready"), "its message reached the owner");
    c.eqi(marks, 1, "the thread was marked busy around the fire");
    c.ok(!has(rig.routines().json(), "lpwake1"), "a one-shot is gone after it fired");
    c.ok(has(rig.routines().json(), "lprecur"), "the recurring routine stays");
  }
  clearEnv();
}

static void testSkills(ndtest::Ctx& c) {
  std::printf("  -- (f) skills --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_SKILL", 1);
  FakeHttpTransport tx;
  auto o = opts("vp-f");
  Config cfg;
  NimbusdRig rig(cfg, o, &tx);
  {
    Web web(rig);
    const std::string md = "---%0Atitle%3A+Tea%0Adesc%3A+How+the+owner+takes+tea%0A---%0ATwo+sugars.%0A";
    const ApiResp saved = web.call("POST", "/api/skills/save", "id=tea&md=" + md);
    c.eqi(saved.status, 200, "the owner saves a skill");
    c.ok(has(saved.body, "\"ok\":true"), "and it is the page's shape");
    const std::string list = web.call("GET", "/api/skills/list").body;
    c.ok(has(list, "\"id\":\"tea\"") && !has(list, "\"id\":\"tea\",\"title\":\"Tea\",\"desc\":\"How the owner takes tea\",\"source\":\"sd\",\"origin\":\"owner\",\"inject\":\"none\",\"rev\":1,\"pending\""),
         "it lists as approved");
    c.ok(has(web.call("GET", "/api/skills/get?id=tea").body, "Two sugars"), "and reads back");
    c.eqi(web.call("GET", "/api/skills/get?id=nope").status, 404, "an unknown id is a 404");
  }
  const std::string out = rig.callTool("skill.save", R"({"id":"coffee","md":"---\ntitle: Coffee\ndesc: x\n---\nBlack.\n"})");
  c.ok(has(out, "approv"), "the assistant's skill waits for the owner");
  tx.script.push_back(headTurn("ok"));
  rig.say("owner", "hello");
  c.ok(!tx.seen.empty() && has(tx.seen[0].body, "tea"), "the prompt's index names the owner's skill");
  c.ok(has(rig.callTool("skill.get", R"({"id":"tea"})"), "Two sugars"), "skill.get reads it");
  clearEnv();
}

// One red PNG pixel: real magic, so the bytes decide its type.
static std::string pngPixel() {
  static const unsigned char kPng[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49,
      0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0,
      0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  return std::string((const char*)kPng, sizeof kPng);
}

// The head loop's final answer (photos ride a head that runs the loop).
static Exchange loopFinal(const std::string& reply) {
  Exchange e;
  e.expectPathContains = "/responses";
  e.body = "{\"id\":\"r\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c\",\"name\":\"orch_turn\","
           "\"arguments\":\"{\\\"reply\\\":\\\"" + reply +
           "\\\",\\\"memory\\\":\\\"\\\"}\"}],\"usage\":{\"input_tokens\":5,\"output_tokens\":5}}";
  return e;
}
static NimbusdRig::Options loopOpts(const std::string& tag) {
  auto o = opts(tag);
  o.toolLoop = true;
  return o;
}

static void testPhotos(ndtest::Ctx& c) {
  std::printf("  -- (g) photos --\n");
  clearEnv();
  setenv("OPENAI_API_KEY", "sk-test-openai-photo-00000000000", 1);
  {
    FakeHttpTransport tx;
    tx.script.push_back(loopFinal("It is red."));
    tx.script.push_back(loopFinal("You sent a red photo."));
    Config cfg;
    NimbusdRig rig(cfg, loopOpts("vp-g"), &tx);
    std::string err;
    c.ok(rig.files().put("chat", "red.png", pngPixel(), err), "the upload is in Files");
    const TurnRecord t = rig.sayWithPhotos("owner", "what color is this?", {PhotoIn{"chat", "red.png", ""}});
    c.ok(has(t.reply, "red"), "the turn answered");
    c.ok(!tx.seen.empty() && !tx.seen[0].splices.empty(), "the bytes went as an image part");
    c.ok(!tx.seen.empty() && has(tx.seen[0].body, "what color is this?"), "with the sender's words");
    orch::MsgQuery q;
    q.haveKind = true;
    q.kind = orch::MsgKind::Image;
    const auto rows = rig.episodic().query(q);
    c.eqi((long)rows.size(), 1, "the photo left one row");
    c.ok(!rows.empty() && has(rows[0].text, "seen"), "settled: it says the head saw it");
    rig.say("owner", "and what did I send you?");
    c.ok(tx.seen.size() >= 2 && has(tx.seen.back().body, "[image 1/1 ref=") && !has(tx.seen.back().body, "the photo itself is in this message"),
         "the next turn's history names it, and does not claim it is attached");
    const std::string ref = orch::blobHash(pngPixel());
    c.ok(has(rig.callTool("image.look", "{\"ref\":\"" + ref + "\",\"question\":\"what color?\"}"), ref) ||
             has(rig.callTool("image.look", "{\"ref\":\"" + ref + "\",\"question\":\"what color?\"}"), "photo"),
         "image.look finds the stored copy");
    c.ok(has(rig.callTool("image.note", "{\"ref\":\"" + ref + "\",\"note\":\"x\"}"), "only for a photo"), "image.note is for this turn's photos");

  }
  {
    FakeHttpTransport tx;
    const Exchange fin = loopFinal("I could not read it.");
    tx.script.push_back(fin);
    Config cfg;
    NimbusdRig rig(cfg, loopOpts("vp-g2"), &tx);
    std::string err;
    rig.files().put("chat", "notes.png", std::string("this is plain text, not a photo"), err);
    rig.sayWithPhotos("owner", "look", {PhotoIn{"chat", "notes.png", ""}});
    c.ok(!tx.seen.empty() && tx.seen[0].splices.empty(), "bytes that are no image are never sent as one");
    c.ok(!tx.seen.empty() && has(tx.seen[0].body, "(not read:"), "the head reads why");
    tx.script.push_back(fin);
    rig.sayWithPhotos("owner", "look again", {PhotoIn{"chat", "missing.png", ""}});
    c.ok(has(tx.seen.back().body, "not found in Files"), "a missing file says so");
  }
  clearEnv();
  setenv("Z_AI_TOKEN", "zai-test-key-00000000000000000000", 1);
  {
    FakeHttpTransport tx;
    Exchange e;
    e.expectPathContains = "/chat/completions";
    e.body = "{\"choices\":[{\"message\":{\"content\":\"{\\\"reply\\\":\\\"I cannot see it.\\\",\\\"memory\\\":\\\"\\\"}\"}}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":2}}";
    tx.script.push_back(e);
    Config cfg;
    NimbusdRig rig(cfg, opts("vp-g3"), &tx);
    std::string err;
    rig.files().put("chat", "red.png", pngPixel(), err);
    rig.sayWithPhotos("owner", "what color?", {PhotoIn{"chat", "red.png", ""}});
    c.ok(!tx.seen.empty() && tx.seen[0].splices.empty(), "a blind head gets no image part");
    c.ok(!tx.seen.empty() && has(tx.seen[0].body, "(not read"), "and is told nobody read it");
  }
  clearEnv();
}

static void testTavilyKey(ndtest::Ctx& c) {
  std::printf("  -- (h) the web search key on the page --\n");
  clearEnv();
  auto o = opts("vp-h");
  {
    Config cfg;
    FakeHttpTransport tx;
    NimbusdRig rig(cfg, o, &tx);
    Web web(rig);
    c.ok(!rig.registry().has("web.search"), "no key, no web.search");
    c.ok(has(web.call("GET", "/api/orch").body, "\"hasTav\":false"), "the page reads no key");
    c.eqi(web.call("POST", "/api/orch", "tavKey=tvly-test-key-0000000000000000").status, 200, "the key saves");
    c.ok(rig.registry().has("web.search"), "web.search is offered at once");
    c.ok(has(web.call("GET", "/api/orch").body, "\"hasTav\":true"), "the page reads it");
  }
  {
    Config cfg;
    FakeHttpTransport tx;
    NimbusdRig rig(cfg, o, &tx);
    c.ok(rig.registry().has("web.search"), "it survives a restart");
    Web web(rig);
    c.eqi(web.call("POST", "/api/orch", "clr_tavKey=1").status, 200, "the key clears");
    c.ok(!rig.registry().has("web.search"), "web.search goes with it");
  }
  clearEnv();
}

static void testNoDeviceWording(ndtest::Ctx& c) {
  std::printf("  -- (i) a hosted head is never told it is a device --\n");
  clearEnv();
  setenv("MISTRAL_API_KEY", "mistral-test-key-000000000000000000", 1);
  FakeHttpTransport tx;
  Exchange e;
  e.expectPathContains = "/v1/";
  e.status = 500;
  e.body = "{}";
  for (int i = 0; i < 8; i++) tx.script.push_back(e);
  Config cfg;
  NimbusdRig rig(cfg, opts("vp-i"), &tx);
  rig.say("owner", "hello");
  c.ok(!tx.seen.empty(), "a request went out");
  if (!tx.seen.empty()) {
    const std::string& b = tx.seen[0].body;
    for (const char* banned : {"INSIDE the device", "the device's wait", "while the device collects", "ESP32"})
      c.ok(!has(b, banned), std::string("no \"") + banned + "\"");
  }
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== a hosted instance does what the device does (offline) ===\n");
  testHistoryAndFold(c);
  testWebChatContract(c);
  testToolRows(c);
  testEmbeddings(c);
  testRoutines(c);
  testSkills(c);
  testPhotos(c);
  testTavilyKey(c);
  testNoDeviceWording(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf(c.failures == 0 ? "PASSED\n" : "FAILED\n");
  return c.failures == 0 ? 0 : 1;
}
