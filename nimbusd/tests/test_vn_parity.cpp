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
//   (i) nothing in a hosted turn's prompt calls the instance a device;
//   (j) a Telegram photo or album reaches one turn, and the rest gets an answer;
//   (k) activity recording is what the page says it is, and can be turned off;
//   (l) what a write says when the held key cannot embed, and when the text list is full;
//   (m) a chat photo leaves no Files entry behind, and photos waiting are bounded in RAM;
//   (n) the Telegram download itself; (o) the guards around held turns and routines.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "telegram.h"
#include "test_util.h"
#include "tg_inbound.h"
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
  Exchange done;
  done.expectPathContains = "/chat/completions";
  done.status = 0;   // the sub-agent's call fails: the job's own state is what is read
  tx.script.push_back(done);
  c.ok(rig2.jobsBusy(), "a started sub-agent is background work");
  clearEnv();
}

// The page's Home reads GET /api/orch "jobs": a real list while a job runs, empty after.
static void testJobsOnTheRoute(ndtest::Ctx& c) {
  std::printf("  -- (c2) /api/orch lists the job while it runs --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_JOBS", 1);
  // The sub-agent's provider does not answer until released, so the job is really running.
  struct Gate : agent::HttpTransport {
    FakeHttpTransport inner;
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;
    bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
      if (has(req.path, "/chat/completions")) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(20), [this] { return open; });
      }
      return inner.exec(req, out, err);
    }
    void release() {
      {
        std::lock_guard<std::mutex> lk(mu);
        open = true;
      }
      cv.notify_all();
    }
  } gate;
  gate.inner.script.push_back(headJson("{\"reply\":\"on it\",\"memory\":\"\",\"session_ops\":[{\"op\":\"spawn\",\"task\":\"long work\"}]}"));
  Exchange sub;
  sub.expectPathContains = "/chat/completions";
  sub.body = "{\"choices\":[{\"message\":{\"content\":\"done\"}}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":2}}";
  gate.inner.script.push_back(sub);
  gate.inner.script.push_back(headTurn("Reported."));
  Config cfg;
  NimbusdRig rig(cfg, opts("vp-c3"), &gate);
  {
    Web web(rig);
    c.ok(has(web.call("GET", "/api/orch").body, "\"jobs\":[]"), "no job: an empty list");
    web.call("POST", "/api/chat", "text=start+it");
    bool listed = false;
    for (int i = 0; i < 200 && !listed; i++) {
      const std::string b = web.call("GET", "/api/orch").body;
      listed = has(b, "\"jobs\":[{") && has(b, "\"tag\":");
      if (!listed) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    c.ok(listed, "the job lists on the route the page reads, with its tag");
    gate.release();
    bool empty = false;
    for (int i = 0; i < 400 && !empty; i++) {
      empty = has(web.call("GET", "/api/orch").body, "\"jobs\":[]") && has(web.replies.sinceJsonArray(0), "Reported");
      if (!empty) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    c.ok(empty, "and leaves it once reported");
  }
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
    auto o = embedOpts("vp-d5");
    {
      setenv("MISTRAL_API_KEY", "mistral-test-key-000000000000000000", 1);
      FakeHttpTransport tx;
      Config cfg;
      NimbusdRig rig(cfg, o, &tx);
      orch::VecEntry e;
      e.id = "k1";
      e.content = "kept";
      e.importance = 0.9f;
      e.ttlHours = -1;
      e.vec.assign(1024, 0);
      e.vec[3] = 127;
      c.ok(rig.vectors().add(e), "a Mistral-width memory is stored");
      rig.persist();
      clearEnv();
    }
    setenv("OPENAI_API_KEY", "sk-test-openai-only-000000000000", 1);
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, o, &tx);
    c.eq(rig.embedProvider(), "mistral", "stored vectors keep their provider");
    c.eqi(rig.embedWidth(), 1024, "and their width");
    c.eqi(rig.vectors().size(), 1, "and the memory");
    // That store with an OpenAI key: a key is held, it just cannot reach these memories.
    const std::string out = rig.callTool("memory.write", R"({"content":"my brother's dog is Quillfeather"})");
    c.ok(has(out, "kept as text"), "the write is kept as text");
    c.ok(!has(out, "holds no key"), "and never says it holds no key: it holds an OpenAI one");
    c.ok(has(out, "made with mistral"), "it names what the stored memories were made with");
    c.ok(has(out, "Mistral key"), "and the key that fixes it");
    c.ok(has(out, "No later confirmation"), "nothing is promised");
    clearEnv();
  }
  {
    // The 65th memory on a key that cannot embed: the same honest answer as the first 64.
    setenv("Z_AI_TOKEN", "test-key-with-no-embedding-route-0000", 1);
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, embedOpts("vp-d6"), &tx);
    std::string out;
    for (int i = 0; i < 65; i++)
      out = rig.callTool("memory.write", "{\"content\":\"fact number " + std::to_string(i) + " about the garden\"}");
    c.ok(has(out, "not stored"), "the 65th is refused");
    c.ok(has(out, "64"), "it says how many text memories are kept");
    c.ok(has(out, "holds no key"), "and why");
    c.ok(has(out, "OpenAI, Mistral or Cumulo Nimbus"), "and which keys would help");
    c.ok(!has(out, "device"), "with no device wording");
    c.ok(!has(out, "no embeddings key for"), "and no provider the owner never chose");
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
    {
      const agent::SkillTurnScope tainted(true);   // this turn read outside content
      c.ok(has(rig.callTool("wakeup.set", R"({"minutes":5,"note":"planted"})"), "pending the owner's approval"),
           "a wake-up from a turn that read outside content waits for the owner");
    }

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
    seed[0].chatId = "";   // the Routines form posts no chat: the owner's default
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
    orch::MsgQuery q;
    q.sessionId = kWebChatId;
    q.haveKind = true;
    q.kind = orch::MsgKind::Message;
    bool inPage = false;
    for (const auto& m : rig.episodic().query(q)) inPage = inPage || has(m.text, "Your report is ready");
    c.ok(inPage, "a routine with no chat lands in the page's own conversation");
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

static PhotoIn inFiles(const std::string& name) {
  PhotoIn p;
  p.project = "chat";
  p.name = name;
  return p;
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
  {
    Config cfg;
    FakeHttpTransport tx;
    NimbusdRig rig(cfg, opts("vp-g0"), &tx);
    Web web(rig);
    c.ok(has(web.call("POST", "/api/chat", "att=chat%2Fred.png").body, "\"turn\""), "a photo with no words is a message");
    c.eqi(web.call("POST", "/api/chat", "").status, 400, "nothing at all is still refused");
    c.eqi(web.call("POST", "/api/chat", "att=..%2F..%2Fsecrets.env").status, 400, "a path outside Files is no attachment");
  }
  setenv("OPENAI_API_KEY", "sk-test-openai-photo-00000000000", 1);
  {
    FakeHttpTransport tx;
    tx.script.push_back(loopFinal("It is red."));
    tx.script.push_back(loopFinal("You sent a red photo."));
    Config cfg;
    NimbusdRig rig(cfg, loopOpts("vp-g"), &tx);
    std::string err;
    c.ok(rig.files().put("chat", "red.png", pngPixel(), err), "the upload is in Files");
    const TurnRecord t = rig.sayWithPhotos("owner", "what color is this?", {inFiles("red.png")});
    c.ok(has(t.reply, "red"), "the turn answered");
    c.ok(!tx.spliced.empty() && !tx.spliced[0].empty(), "the bytes went as an image part");
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
    c.ok(has(rig.callTool("image.note", "{\"ref\":\"" + ref + "\",\"note\":\"x\"}"), "only for a photo"), "image.note is for this turn's photos");

    // image.look in a later turn: the stored bytes go to the head as an image part.
    Exchange look;
    look.expectPathContains = "/responses";
    look.body = "{\"id\":\"r1\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"image_look\","
                "\"arguments\":\"{\\\"ref\\\":\\\"" + ref + "\\\",\\\"question\\\":\\\"what color?\\\"}\"}],"
                "\"usage\":{\"input_tokens\":5,\"output_tokens\":5}}";
    tx.script.push_back(look);
    tx.script.push_back(loopFinal("It was red."));
    const size_t before = tx.spliced.size();
    const TurnRecord again = rig.say("owner", "look at that earlier photo again");
    c.ok(has(again.reply, "red"), "the turn that looked answered");
    c.ok(tx.spliced.size() == before + 2 && tx.spliced[before].empty(), "no image on the first request of that turn");
    c.ok(tx.spliced.size() == before + 2 && !tx.spliced[before + 1].empty(), "image.look brought the stored bytes back as an image part");
    c.ok(has(rig.callTool("image.look", R"({"ref":"0123456789abcdef","question":"what color?"})"), "no stored photo with that ref"),
         "a ref with no stored copy says so");
  }
  {
    FakeHttpTransport tx;
    const Exchange fin = loopFinal("I could not read it.");
    tx.script.push_back(fin);
    Config cfg;
    NimbusdRig rig(cfg, loopOpts("vp-g2"), &tx);
    std::string err;
    rig.files().put("chat", "notes.png", std::string("this is plain text, not a photo"), err);
    rig.sayWithPhotos("owner", "look", {inFiles("notes.png")});
    c.ok(!tx.spliced.empty() && tx.spliced[0].empty(), "bytes that are no image are never sent as one");
    c.ok(!tx.seen.empty() && has(tx.seen[0].body, "(not read:"), "the head reads why");
    tx.script.push_back(fin);
    // Past the route's check, the store's own gate: a real file beside Files is not read.
    fsutil::writeFileAtomic(rig.memDir() + "/secret-beside-files.png", pngPixel());
    PhotoIn outside;
    outside.project = "..";
    outside.name = "secret-beside-files.png";
    tx.script.push_back(fin);
    const size_t splicesBefore = tx.spliced.size();
    rig.sayWithPhotos("owner", "and this", {outside});
    c.ok(has(tx.seen.back().body, "not found in Files"), "a path outside Files reads nothing");
    c.ok(tx.spliced.size() > splicesBefore && tx.spliced.back().empty(), "and sends nothing");
    PhotoIn missing;
    missing.project = "chat";
    missing.name = "missing.png";
    rig.sayWithPhotos("owner", "look again", {missing});
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
    rig.sayWithPhotos("owner", "what color?", {inFiles("red.png")});
    c.ok(!tx.spliced.empty() && tx.spliced[0].empty(), "a blind head gets no image part");
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
    // The prompt and the turn's input. The reply schema's field notes and the tool
    // descriptions come from shared text and still say "the device".
    JsonDocument d;
    deserializeJson(d, tx.seen[0].body);
    const std::string b = std::string(d["instructions"] | "") + std::string(d["inputs"] | "");
    c.ok(!b.empty(), "the prompt is in the request");
    for (const char* banned :
         {"INSIDE the device", "the device's wait", "while the device collects", "ESP32", "personal-assistant device",
          "act on the device", "The device checks", "the device web page", "the device's web page",
          "the device inserts", "saved on the device", "the device refuses", "routines exist on this device",
          "saved to the device", "documentation is on the device", "the device's own job table",
          "the device does one network call", "queued on the device", "the device confirms"})
      c.ok(!has(b, banned), std::string("no \"") + banned + "\"");
  }
  clearEnv();
}

// One Telegram photo update as parsed from the wire.
static nimbus::tg::Update tgPhoto(const std::string& chat, const std::string& text, const std::string& group, const char* id) {
  nimbus::tg::Update u;
  u.chatId = chat;
  u.text = text;
  u.mediaGroupId = group;
  u.attachment.kind = nimbus::tg::Attachment::Kind::Photo;
  u.attachment.fileId = id;
  return u;
}

static void testTelegramPhotos(ndtest::Ctx& c) {
  std::printf("  -- (j) a Telegram photo or album reaches one turn --\n");
  clearEnv();
  const std::string dir = ndtest::scratchDir("vp-j");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  TelegramAccess access(dir);
  std::string err;
  access.setAllowCsv("123", err);
  TgInboundIo io;
  int turns = 0, fetched = 0;
  std::vector<PhotoIn> got;
  std::string caption, sent;
  io.turn = [&](const std::string&, const std::string&) { turns++; };
  io.send = [&](const std::string&, const std::string& t) { sent = t; };
  io.photoTurn = [&](const std::string&, const std::string& t, const std::vector<PhotoIn>& p) {
    turns++;
    caption = t;
    got = p;
  };
  io.fetchFile = [&](const std::string& id, std::string& bytes, std::string& e) {
    fetched++;
    if (id == "gone") {
      e = "too large";
      return false;
    }
    bytes = pngPixel();
    return true;
  };
  routeTelegramUpdates({tgPhoto("123", "dinner", "alb", "a"), tgPhoto("123", "", "alb", "b"), tgPhoto("123", "", "alb", "gone")},
                       access, io, 1000);
  c.eqi(turns, 1, "an album is one turn");
  c.eqi((long)got.size(), 3, "with all its photos");
  c.eq(caption, "dinner", "and its caption");
  c.ok(got.size() == 3 && got[2].why == "too large", "a photo that could not be fetched says why");
  turns = 0;
  fetched = 0;
  routeTelegramUpdates({tgPhoto("999", "not approved", "", "x")}, access, io, 1000);
  c.eqi(turns, 0, "an unapproved chat gets no turn");
  c.eqi(fetched, 0, "and its file is never downloaded");
  nimbus::tg::Update voice;
  voice.chatId = "123";
  voice.attachment.kind = nimbus::tg::Attachment::Kind::Voice;
  routeTelegramUpdates({voice}, access, io, 1000);
  c.ok(has(sent, "text and photos"), "a voice note is told what this bot reads, never silence");
}


static Exchange anyHead(const std::string& reply) { return loopFinal(reply); }

static void testChatPhotoLeavesNoEntry(ndtest::Ctx& c) {
  std::printf("  -- (m) a chat photo leaves no Files entry behind --\n");
  clearEnv();
  setenv("OPENAI_API_KEY", "sk-test-openai-photo-00000000000", 1);
  {
    FakeHttpTransport tx;
    tx.script.push_back(anyHead("One."));
    tx.script.push_back(anyHead("Two."));
    tx.script.push_back(anyHead("Three."));
    Config cfg;
    NimbusdRig rig(cfg, loopOpts("vp-m"), &tx);
    std::string err;
    const std::string uploaded = kChatPhotoPrefix + std::string("abc123-photo.png");
    c.ok(rig.files().put("chat", uploaded, pngPixel(), err), "the page's upload is in Files");
    c.eqi((long)rig.files().count(), 1, "one entry while it waits for its turn");
    rig.sayWithPhotos("owner", "what is this?", {inFiles(uploaded)});
    c.ok(!tx.spliced.empty() && !tx.spliced[0].empty(), "the turn took its bytes");
    c.eqi((long)rig.files().count(), 0, "and the Files entry is gone");
    const std::string ref = orch::blobHash(pngPixel());
    std::string blob;
    c.ok(fsutil::readFile(orch::blobPath(rig.memDir() + "/photos", ref, "bin"), blob), "the stored copy image.look reads stays");
    // A file the owner keeps in Files is never removed, even when a message names it.
    c.ok(rig.files().put("chat", "keep-me.png", pngPixel(), err), "an owner's own file");
    rig.sayWithPhotos("owner", "and this?", {inFiles("keep-me.png")});
    c.eqi((long)rig.files().count(), 1, "stays where it is");
    // One that never reaches a turn: the page closed before the message went.
    const std::string stale = kChatPhotoPrefix + std::string("old-unsent.png");
    c.ok(rig.files().put("chat", stale, pngPixel(), err), "an upload never sent");
    rig.files().sweepChatPhotos((uint32_t)time(nullptr) + 2 * 86400);
    c.ok(!rig.files().fetch("chat", stale, err), "is swept once it is days old");
    c.ok(rig.files().fetch("chat", "keep-me.png", err), "and the sweep leaves the owner's file");
  }
  {
    // 300 photos in a row never fill the store the assistant's own files share.
    FakeHttpTransport tx;
    Config cfg;
    auto o = loopOpts("vp-m2");
    NimbusdRig rig(cfg, o, &tx);
    std::string err;
    bool allStored = true;
    for (int i = 0; i < 300; i++) {
      const std::string n = kChatPhotoPrefix + std::to_string(i) + ".png";
      allStored = allStored && rig.files().put("chat", n, pngPixel(), err);
      tx.script.push_back(anyHead("seen"));
      rig.sayWithPhotos("owner", "photo " + std::to_string(i), {inFiles(n)});
    }
    c.ok(allStored, "photo 300 stores like photo 1");
    c.eqi((long)rig.files().count(), 0, "and nothing is left in Files");
    c.ok(rig.files().put("notes", "a.txt", "hello", err), "and the assistant's own files still have room");
  }
  clearEnv();
}

static void testHeldPhotoBytesBounded(ndtest::Ctx& c) {
  std::printf("  -- (m2) photos waiting for their turn are bounded in RAM --\n");
  clearEnv();
  const std::string dir = ndtest::scratchDir("vp-m3");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  TelegramAccess access(dir);
  std::string err;
  access.setAllowCsv("123", err);
  c.eqi((long)photoBudget().used(), 0, "nothing held at the start");
  TgInboundIo io;
  const std::string big(nimbus::orch::kImagePartBytes, 'x');
  std::vector<std::vector<PhotoIn>> waiting;   // the engine's queue while a long turn runs
  int fetched = 0;
  io.send = [](const std::string&, const std::string&) {};
  io.fetchFile = [&](const std::string&, std::string& bytes, std::string&) {
    fetched++;
    bytes = big;
    return true;
  };
  io.photoTurn = [&](const std::string&, const std::string&, const std::vector<PhotoIn>& p) { waiting.push_back(p); };
  size_t peak = 0;
  for (int album = 0; album < 40; album++) {
    std::vector<nimbus::tg::Update> ups;
    const std::string group = "alb" + std::to_string(album);
    for (int k = 0; k < 10; k++) ups.push_back(tgPhoto("123", k ? "" : "look", group, "f"));
    routeTelegramUpdates(ups, access, io, 1000 + album);
    peak = std::max(peak, photoBudget().used());
  }
  c.ok(peak <= kPhotoHoldMaxBytes, "the bytes held never pass the bound");
  c.ok(peak >= kPhotoHoldMaxBytes - 20 * nimbus::orch::kImagePartBytes, "and the bound is what stopped them");
  c.eqi((long)waiting.size(), 40, "every message still reaches a turn");
  c.ok(has(waiting.back()[0].why, "waiting"), "a photo past the bound reaches its turn as the reason");
  c.ok(fetched < 400, "and it is not downloaded");
  // A copy of the queue holds no second copy of the bytes.
  {
    const auto copy = waiting;
    c.eqi((long)photoBudget().used(), (long)peak, "copies share the bytes");
    c.ok(copy.size() == waiting.size(), "the copy is whole");
  }
  waiting.clear();
  c.eqi((long)photoBudget().used(), 0, "and a finished turn gives them back");
  clearEnv();
}

static void testTelegramDownload(ndtest::Ctx& c) {
  std::printf("  -- (n) the Telegram download itself --\n");
  clearEnv();
  const std::string dir = ndtest::scratchDir("vp-n");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  const std::string tok = "123456:TESTTOKEN";
  const auto getFile = [](const std::string& body) {
    Exchange e;
    e.expectHost = "api.telegram.org";
    e.expectPathContains = "/getFile?file_id=FID";
    e.body = body;
    return e;
  };
  {
    FakeHttpTransport tx;
    tx.script.push_back(getFile(R"({"ok":true,"result":{"file_id":"FID","file_size":5,"file_path":"photos/file_7.jpg"}})"));
    Exchange file;
    file.expectHost = "api.telegram.org";
    file.body = "BYTES";
    tx.script.push_back(file);
    TelegramChannel ch(tok, &tx, dir + "/o1");
    std::string out, err;
    c.ok(ch.fetchFile("FID", 100, out, err), "a photo downloads");
    c.eq(out, "BYTES", "with its bytes");
    c.ok(tx.seen.size() == 2 && tx.seen[1].path == "/file/bot" + tok + "/photos/file_7.jpg", "from the file host's own path");
  }
  {
    // Telegram writes the path with escaped slashes in some answers.
    FakeHttpTransport tx;
    tx.script.push_back(getFile(R"({"ok":true,"result":{"file_id":"FID","file_path":"photos\/file_8.jpg"}})"));
    Exchange file;
    file.body = "B";
    tx.script.push_back(file);
    TelegramChannel ch(tok, &tx, dir + "/o2");
    std::string out, err;
    c.ok(ch.fetchFile("FID", 100, out, err), "an escaped path downloads");
    c.ok(tx.seen.size() == 2 && tx.seen[1].path == "/file/bot" + tok + "/photos/file_8.jpg", "unescaped on the wire");
  }
  {
    FakeHttpTransport tx;
    tx.script.push_back(getFile(R"({"ok":true,"result":{"file_size":101,"file_id":"FID","file_path":"photos/big.jpg"}})"));
    TelegramChannel ch(tok, &tx, dir + "/o3");
    std::string out, err;
    c.ok(!ch.fetchFile("FID", 100, out, err), "a file over the limit by its reported size is refused");
    c.eq(err, "too large", "saying so");
    c.eqi((long)tx.seen.size(), 1, "before any bytes move");
  }
  {
    FakeHttpTransport tx;
    tx.script.push_back(getFile(R"({"ok":true,"result":{"file_id":"FID","file_path":"photos/unsized.jpg"}})"));
    Exchange file;
    file.body = std::string(101, 'x');
    tx.script.push_back(file);
    TelegramChannel ch(tok, &tx, dir + "/o4");
    std::string out, err;
    c.ok(!ch.fetchFile("FID", 100, out, err), "an unsized file over the limit is refused on arrival");
    c.eq(err, "too large", "saying so");
    c.ok(out.empty(), "and none of it is kept");
    c.ok(tx.seen.size() == 2 && tx.seen[1].maxResponseBytes > 0 && tx.seen[1].maxResponseBytes <= 100 + 1,
         "the transport is told to stop reading past the limit");
  }
  {
    FakeHttpTransport tx;
    Exchange gone = getFile(R"({"ok":false,"description":"file is too big"})");
    gone.status = 400;
    tx.script.push_back(gone);
    TelegramChannel ch(tok, &tx, dir + "/o5");
    std::string out, err;
    c.ok(!ch.fetchFile("FID", 100, out, err), "Telegram's own refusal is a failed download");
    c.eq(err, "the download failed", "in plain words");
  }
  {
    FakeHttpTransport tx;
    tx.script.push_back(getFile(R"({"ok":true,"result":{"file_id":"FID"}})"));
    TelegramChannel ch(tok, &tx, dir + "/o6");
    std::string out, err;
    c.ok(!ch.fetchFile("FID", 100, out, err), "an answer with no path is a failed download");
    c.eqi((long)tx.seen.size(), 1, "and no second request goes out");
  }
}

static void testTelegramRest(ndtest::Ctx& c) {
  std::printf("  -- (j2) an album past the cap, and a caption on what it cannot read --\n");
  clearEnv();
  const std::string dir = ndtest::scratchDir("vp-j2");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  TelegramAccess access(dir);
  std::string err;
  access.setAllowCsv("123", err);
  TgInboundIo io;
  std::vector<size_t> sizes;
  std::vector<std::string> sent, turnTexts;
  io.send = [&](const std::string&, const std::string& t) { sent.push_back(t); };
  io.turn = [&](const std::string&, const std::string& t) { turnTexts.push_back(t); };
  io.photoTurn = [&](const std::string&, const std::string&, const std::vector<PhotoIn>& p) { sizes.push_back(p.size()); };
  io.fetchFile = [&](const std::string&, std::string& b, std::string&) {
    b = pngPixel();
    return true;
  };
  std::vector<nimbus::tg::Update> album;
  for (size_t i = 0; i < nimbus::orch::kTurnImageMax + 2; i++) album.push_back(tgPhoto("123", "", "big", "f"));
  routeTelegramUpdates(album, access, io, 1000);
  c.ok(sizes.size() == 2 && sizes[0] == nimbus::orch::kTurnImageMax && sizes[1] == 2,
       "an album past the cap fills one turn, and the rest are the next: none lost");

  // A file, a voice note, a video with words: the words reach the head, and it knows.
  using K = nimbus::tg::Attachment::Kind;
  const struct { K kind; const char* word; } cases[] = {
      {K::Document, "file"},   {K::Voice, "voice note"},  {K::Video, "video"},      {K::Sticker, "sticker"},
      {K::Audio, "audio"},     {K::VideoNote, "video"},   {K::Animation, "animation"}, {K::Other, "attachment"}};
  for (const auto& kase : cases) {
    sent.clear();
    turnTexts.clear();
    nimbus::tg::Update u;
    u.chatId = "123";
    u.text = "what is the total on this invoice?";
    u.attachment.kind = kase.kind;
    u.attachment.fileId = "F";
    routeTelegramUpdates({u}, access, io, 2000);
    c.eqi((long)turnTexts.size(), 1, std::string(kase.word) + ": the words reach a turn");
    c.ok(!turnTexts.empty() && has(turnTexts[0], "what is the total") && has(turnTexts[0], kase.word),
         std::string(kase.word) + ": and the head is told what came with them");
    c.ok(!turnTexts.empty() && has(turnTexts[0], "did not reach you"), std::string(kase.word) + ": that it did not reach it");
    c.ok(sent.size() == 1 && has(sent[0], "text and photos"), std::string(kase.word) + ": and the sender hears what this bot reads");
  }
  // Plain words are plain words: nothing is added.
  sent.clear();
  turnTexts.clear();
  nimbus::tg::Update plain;
  plain.chatId = "123";
  plain.text = "hello";
  routeTelegramUpdates({plain}, access, io, 3000);
  c.ok(turnTexts.size() == 1 && turnTexts[0] == "hello", "a plain message is untouched");
  c.ok(sent.empty(), "and gets no extra line");
}

static void testActivityRecording(ndtest::Ctx& c) {
  std::printf("  -- (k) activity recording is what the page says --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_TRACE", 1);
  auto o = opts("vp-k");
  o.toolLoop = true;
  const auto toolTurn = [](FakeHttpTransport& tx) {
    Exchange call;
    call.expectPathContains = "/responses";
    call.body = "{\"id\":\"r1\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"docs_search\","
                "\"arguments\":\"{\\\"query\\\":\\\"routines\\\"}\"}],\"usage\":{\"input_tokens\":5,\"output_tokens\":5}}";
    tx.script.push_back(call);
    tx.script.push_back(loopFinal("looked it up"));
  };
  FakeHttpTransport tx;
  {
    Config cfg;
    NimbusdRig rig(cfg, o, &tx);
    {
      Web web(rig);
      c.ok(has(web.call("GET", "/api/orch").body, "\"orchTrace\":true"), "the page reads recording on, as it is");
    }
    toolTurn(tx);
    rig.say("owner", "first");
    Web web(rig);
    const std::string on = web.call("GET", "/api/mem/episodic?limit=50").body;
    c.ok(has(on, "\"kind\":\"tool_output\""), "on: the tool call left its row");
    c.eqi(web.call("POST", "/api/orch", "orchTrace=0").status, 200, "the box turns it off");
    c.ok(has(web.call("GET", "/api/orch").body, "\"orchTrace\":false"), "and the page reads off");
  }
  {
    Config cfg;
    NimbusdRig rig(cfg, o, &tx);
    {
      Web web(rig);
      c.ok(has(web.call("GET", "/api/orch").body, "\"orchTrace\":false"), "off survives a restart");
      const size_t before = web.call("GET", "/api/mem/episodic?limit=50").body.find("\"text\":\"second\"");
      c.ok(before == std::string::npos, "nothing yet from the next turn");
    }
    toolTurn(tx);
    rig.say("owner", "second");
    Web web(rig);
    const std::string off = web.call("GET", "/api/mem/episodic?limit=50").body;
    c.ok(has(off, "second"), "the message itself is kept");
    const size_t at = off.find("second");
    c.ok(off.find("tool:docs_search", 0) == off.rfind("tool:docs_search") || !has(off.substr(0, at), "tool:docs_search"),
         "off: this turn's tool call left no new row");
    int rows = 0;
    for (size_t p = off.find("\"kind\":\"tool_output\""); p != std::string::npos; p = off.find("\"kind\":\"tool_output\"", p + 1)) rows++;
    c.eqi(rows, 1, "only the row from when it was on");
    c.eqi(web.call("POST", "/api/orch", "orchTrace=1").status, 200, "and it turns back on");
  }
  clearEnv();
}

static void testHeldAndRoutineGuards(ndtest::Ctx& c) {
  std::printf("  -- (o) the guards around held turns and routines --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_GUARDS", 1);
  {
    // A routine due into a member's chat while the owner's work is unreported defers.
    const auto o = opts("vp-o");
    fsutil::mkdirs(o.dataDir + "/mem");
    std::vector<orch::LoopRecord> seed(1);
    seed[0].id = "lpmember";
    seed[0].name = "for sam";
    seed[0].prompt = "RUN-FOR-SAM";
    seed[0].chatId = "666";
    seed[0].sched.kind = orch::SchedKind::Once;
    seed[0].sched.intervalSec = 300;
    seed[0].createdBy = orch::CreatedBy::Owner;
    seed[0].approved = true;
    seed[0].nextRun = (uint64_t)time(nullptr) - 30;
    fsutil::writeFileAtomic(o.dataDir + "/mem/loops.json", orch::dumpLoops(seed));
    FakeHttpTransport tx;
    tx.script.push_back(headJson("{\"reply\":\"Starting.\",\"memory\":\"\",\"session_ops\":[{\"op\":\"spawn\",\"task\":\"work\"}]}"));
    Exchange sub;
    sub.expectPathContains = "/chat/completions";
    sub.body = "{\"choices\":[{\"message\":{\"content\":\"the owner's private answer\"}}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":2}}";
    tx.script.push_back(sub);
    Config cfg;
    NimbusdRig rig(cfg, o, &tx);
    std::string err;
    rig.telegramAccess().approve("555", "Roy", err);
    rig.telegramAccess().approve("666", "Sam", err);
    rig.telegramAccess().setRole("555", orch::Role::Admin, err);
    rig.say("555", "start the work");
    for (int i = 0; i < 200 && !rig.jobs().hasFreshResults(); i++) {
      rig.pumpJobs();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    c.ok(rig.turnMustWait("666"), "the owner's unreported work makes a member wait");
    const size_t seen = tx.seen.size();
    rig.loopsTick([](bool) {});
    c.eqi((long)tx.seen.size(), (long)seen, "a routine into that member's chat sends nothing while it waits");
    c.ok(has(rig.routines().json(), "lpmember"), "and is kept for its next try, not spent");
  }
  clearEnv();
  {
    // Past the bound the oldest waiting message goes, and whoever waits on it stops.
    EngineThread::HeldQueue q;
    std::atomic<int> firstEnded{0}, restEnded{0};
    EngineThread::Held first;
    first.chat = "666";
    first.text = "the oldest";
    first.ended = [&firstEnded] { firstEnded.fetch_add(1); };
    q.push(first);
    for (size_t i = 0; i < EngineThread::kHeldMax - 1; i++) {
      EngineThread::Held m;
      m.chat = "666";
      m.ended = [&restEnded] { restEnded.fetch_add(1); };
      q.push(m);
    }
    c.eqi(firstEnded.load(), 0, "nothing is dropped up to the bound");
    EngineThread::Held last;
    last.chat = "666";
    q.push(last);
    c.eqi(firstEnded.load(), 1, "past it the oldest waiter is released, not left hanging");
    c.eqi(restEnded.load(), 0, "and only that one");
    c.eqi((long)q.items.size(), (long)EngineThread::kHeldMax, "the queue stays at its bound");
  }
  {
    // A loop id that comes up twice is drawn again.
    setenv("CUMULO_API_KEY", "cumulo_sk_test_LOOPID", 1);
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, opts("vp-o2"), &tx);
    const uint32_t draws[] = {7, 7, 7, 9};
    size_t at = 0;
    rig.routines().setIdSource([&] { return draws[at++ % 4]; });
    JsonDocument d;
    deserializeJson(d, R"({"kind":"daily","at":"09:00"})");
    const auto a = rig.routines().create("a", "one", "owner", d.as<JsonObjectConst>(), false, false);
    const auto b = rig.routines().create("b", "two", "owner", d.as<JsonObjectConst>(), false, false);
    c.ok(a.ok && b.ok, "both routines are made");
    c.ok(a.id != b.id, "an id already in use is never reused");
    rig.routines().setIdSource(nullptr);
    clearEnv();
  }
}

int main() {
  ndtest::Ctx c;
  std::printf("=== a hosted instance does what the device does (offline) ===\n");
  testTelegramPhotos(c);
  testTelegramRest(c);
  testTelegramDownload(c);
  testHeldPhotoBytesBounded(c);
  testChatPhotoLeavesNoEntry(c);
  testActivityRecording(c);
  testHeldAndRoutineGuards(c);
  testHistoryAndFold(c);
  testWebChatContract(c);
  testToolRows(c);
  testJobsOnTheRoute(c);
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
