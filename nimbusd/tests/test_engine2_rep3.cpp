// test_engine2_rep3 - lane E2-REP3, offline: what the hosted daemon's engine 2 lacked beside the device, each
// through the device's binding and the scripted provider (no model call, no Telegram):
//   (1) the instance's connectors ride a direct Anthropic head's request (the device's provider-side attach);
//   (2) send: a second message to this chat, a stored file and a photo to Telegram, a voice note as text;
//   (3) the docs group (docs_list, docs_search, docs_read) and device_status, which names the instance's truth;
//   (4) the monthly ceiling is checked before each request of a turn with the spend of its earlier requests;
//   (5) on its own provider key the instance says in /api/state that members are not moderated.
#include <cstdlib>
#include <string>
#include <vector>

#include "../../test/support/scripted_provider/scripted_provider.h"
#include "daemon_http.h"
#include "engine2_route.h"
#include "http_control.h"
#include "reply_buffer.h"
#include "test_util.h"

namespace sp = harness_test::scripted;

namespace {

bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

struct Out {
  std::string chat, text;
};
struct Upload {
  std::string chat, kind, name, bytes, caption;
};

struct Host {
  std::string dir;
  nimbusd::PosixFiles files;
  sp::ScriptedProvider model{sp::Dialect::Anthropic};
  std::vector<Out> sent;
  std::vector<Upload> uploads;
  std::unique_ptr<nimbusd::Engine2Host> host;
  nimbusd::Engine2Options o;

  explicit Host(const std::string& d) : dir(d), files(d + "/mem/files") {
    o.dataDir = dir;
    o.files = &files;
    o.head = [] { return std::string("anthropic"); };
    o.model = [] { return std::string("claude-haiku-5-5"); };
    o.summarizerModel = [] { return std::string("claude-haiku-5-5"); };
    o.key = [] { return std::string("sk-fake-rep3"); };
    o.utcOffsetMin = [](uint32_t) { return 0; };
    o.dayStartMin = [] { return uint16_t(240); };
    o.deliver = [this](const std::string& chat, const std::string& text) { sent.push_back({chat, text}); };
    o.transport = &model;
    o.send.allowed = [](const std::string& chat) { return chat == "555"; };
    o.send.ownerChat = [] { return std::string("555"); };
    o.send.media = [this](const std::string& chat, const std::string& kind, const std::string& name,
                          const std::string& bytes, const std::string& caption) {
      uploads.push_back({chat, kind, name, bytes, caption});
      return true;
    };
  }
  std::string start() {
    host.reset(new nimbusd::Engine2Host(o));
    return host->start();
  }
  // The body of request i (0-based).
  std::string body(size_t i) const { return i < model.requests().size() ? model.requests()[i].body : std::string(); }
};

void caseConnectors(ndtest::Ctx& c) {
  std::printf("  (1) the instance's connectors on a direct head\n");
  const std::string dir = ndtest::scratchDir("e2rep3conn");
  Host h(dir);
  h.o.connectors = [] {
    nimbus::orch::ConnectorInfo ci;
    ci.name = ci.type = "docs";
    ci.prov = "anthropic";
    ci.kind = "mcp";
    ci.url = "https://mcp.docs.example.test/mcp";
    ci.enabled = true;
    return std::vector<nimbus::orch::ConnectorInfo>{ci};
  };
  h.o.bearer = [](const nimbus::orch::ConnectorInfo&) { return std::string("tok-docs"); };
  h.model.push(sp::say("Hi."));
  c.eq(h.start(), "", "the host starts");
  (void)h.host->say("owner", "hello", "web");
  const std::string b = h.body(0);
  c.ok(has(b, "\"mcp_servers\"") && has(b, "https://mcp.docs.example.test/mcp") && has(b, "mcp_toolset"),
       "the owner's Anthropic tool server rides the head's request with its toolset");
  c.ok(has(b, "tok-docs"), "...with its stored bearer");
  ndtest::rmTree(dir);
}

void caseSend(ndtest::Ctx& c) {
  std::printf("  (2) send\n");
  const std::string dir = ndtest::scratchDir("e2rep3send");
  Host h(dir);
  std::string err;
  c.ok(h.files.put("notes", "list.txt", "eggs, milk", err), "a stored file");
  const std::string jpeg("\xFF\xD8\xFF\xE0" "0000JFIF", 12);
  c.ok(h.files.put("pics", "cat.jpg", jpeg, err), "a stored photo");
  h.model.push(sp::call("send", R"({"kind":"text","text":"A second message."})"));
  h.model.push(sp::call("send", R"({"kind":"file","path":"notes/list.txt"})"));
  h.model.push(sp::call("send", R"({"kind":"photo","path":"pics/cat.jpg","caption":"the cat"})"));
  h.model.push(sp::call("send", R"({"kind":"voice","text":"Said aloud."})"));
  h.model.push(sp::say("Done."));
  c.eq(h.start(), "", "the host starts");
  (void)h.host->say("owner", "send me things", "web");
  c.ok(has(h.body(0), "\"name\":\"send\""), "send is offered");
  c.ok(h.sent.size() >= 3 && h.sent[0].chat == "owner" && h.sent[0].text == "A second message.",
       "a second message reaches the web chat");
  c.ok(has(h.body(1), "queued for chat owner"), "...and the model is told it went");
  c.ok(h.uploads.size() == 2 && h.uploads[0].chat == "555" && h.uploads[0].kind == "document" &&
           h.uploads[0].bytes == "eggs, milk" && h.uploads[0].caption == "notes/list.txt",
       "a stored file goes to the owner's Telegram chat as a document");
  c.ok(has(h.body(2), "queued for chat 555"), "...and its result names the chat");
  c.ok(h.uploads.size() == 2 && h.uploads[1].kind == "photo" && h.uploads[1].bytes == jpeg &&
           h.uploads[1].caption == "the cat",
       "a stored image goes as a photo with its caption");
  c.ok(has(h.body(4), "voice replies are off: sent as text") && h.sent.size() >= 2 && h.sent[1].text == "Said aloud.",
       "a voice note arrives as text, and the model is told so (the instance has no speech path)");
  h.model.push(sp::call("send", R"({"kind":"file","path":"notes/list.txt","chat":"999"})"));
  h.model.push(sp::say("No."));
  (void)h.host->say("owner", "send it to a stranger", "web");
  c.ok(has(h.body(6), "not sent: no allowed Telegram chat") && h.uploads.size() == 2,
       "a chat that is not approved gets nothing");
  std::string why;
  c.ok(h.model.verify(why), "the script was used exactly (" + why + ")");
  ndtest::rmTree(dir);
}

void caseDocsAndStatus(ndtest::Ctx& c) {
  std::printf("  (3) the docs group and device_status\n");
  const std::string dir = ndtest::scratchDir("e2rep3docs");
  nimbusd::Config cfg;
  nimbusd::NimbusdRig::Options opt;
  opt.dataDir = dir + "/rig";
  opt.embeddings = false;
  opt.embedDims = 64;
  nimbusd::NimbusdRig rig(cfg, opt);
  Host h(dir);
  h.o.e1Tools = &rig.registry();
  h.o.status = [&rig](const std::string& p, const std::string& m) { return nimbusd::engine2StatusJson(rig, p, m); };
  c.ok(nimbus::docs::sectionCount() > 0, "the docs pack rides the daemon");
  const std::string id = nimbus::docs::section(0).id;   // the orchestrator world's opening section
  h.model.push(sp::call("more", R"({"name":"docs_list","args":{}})"));
  h.model.push(sp::call("more", R"({"name":"docs_search","args":{"query":"telegram"}})"));
  h.model.push(sp::call("more", "{\"name\":\"docs_read\",\"args\":{\"id\":\"" + id + "\"}}"));
  h.model.push(sp::call("more", R"({"name":"device_status","args":{}})"));
  h.model.push(sp::say("Done."));
  c.eq(h.start(), "", "the host starts");
  (void)h.host->say("owner", "what are you", "web");
  c.ok(has(h.body(1), "\\\"sections\\\"") && has(h.body(1), nimbus::docs::file(0).slug),
       "docs_list answers the pack's file index");
  c.ok(has(h.body(2), "\\\"snippet\\\""), "docs_search answers sections with a snippet (engine 1's handler)");
  c.ok(has(h.body(3), "\"content\":\"## ") && has(h.body(3), "The cognitive architecture"),
       "docs_read answers the section in full (engine 1's handler)");
  const std::string st = h.body(4);
  c.ok(has(st, "\\\"host\\\":\\\"nimbusd\\\"") && has(st, "\\\"fw\\\":\\\"" NIMBUS_FW_VERSION_ENGINE2 "\\\"") &&
           has(st, "\\\"provider\\\":\\\"anthropic\\\"") && has(st, "\\\"limitCents\\\""),
       "device_status names the instance, engine 2's version, its head and the monthly ceiling");
  c.ok(!has(st, "batt") && !has(st, "wifi") && !has(st, "Wi-Fi"), "...and no battery or Wi-Fi it does not have");
  std::string why;
  c.ok(h.model.verify(why), "the script was used exactly (" + why + ")");
  h.host.reset();
  ndtest::rmTree(dir);
}

void caseCeiling(ndtest::Ctx& c) {
  std::printf("  (4) the ceiling before each request of a turn\n");
  const std::string dir = ndtest::scratchDir("e2rep3ceil");
  Host h(dir);
  uint32_t pendingTokens = 0;
  const uint32_t ceilingTokens = 150;   // the first answer's 120 tokens fit; the turn's spend then crosses it
  h.o.pendingSpend = [&pendingTokens](const nimbus::orch::TurnSpend& s) {
    const nimbus::orch::TokenUsage u = s.total();
    pendingTokens += u.promptTokens + u.completionTokens;
  };
  h.o.paused = [&pendingTokens, ceilingTokens] { return pendingTokens >= ceilingTokens; };
  h.model.push(sp::call("calc", R"({"expression":"2+2"})"));
  h.model.push(sp::call("calc", R"({"expression":"3+3"})"));
  h.model.push(sp::say("Four and six."));
  c.eq(h.start(), "", "the host starts");
  const nimbusd::PlainTurn t = h.host->say("owner", "add things", "web");
  c.ok(h.model.requests().size() == 2, "the request after the spend crossed the ceiling is not sent");
  c.ok(!h.sent.empty() && h.sent.back().text == nimbusd::kPausedLine, "the person gets the pause line");
  c.eq(t.stated, std::string("plain-rig:paused"), "the turn ends paused");
  ndtest::rmTree(dir);
}

struct Resp {
  int status = 0;
  std::string body;
};
Resp get(nimbusd::DaemonHttpTransport& cl, int port, const std::string& path) {
  agent::HttpRequest r;
  r.method = "GET";
  r.host = "127.0.0.1";
  r.port = port;
  r.tls = false;
  r.path = path;
  r.timeoutMs = 8000;
  r.headers.push_back({"X-Nimbus-Token", "rep3-token"});
  agent::HttpResponse out;
  std::string err;
  cl.exec(r, out, err);
  return Resp{out.status, out.body};
}

void caseModeration(ndtest::Ctx& c) {
  std::printf("  (5) member moderation on the instance's own key\n");
  for (bool router : {false, true}) {
    if (router) setenv("CUMULO_API_KEY", "cumulo_sk_test_REP3", 1);
    else setenv("ANTHROPIC_API_KEY", "sk-ant-fake-rep3", 1);
    const std::string dir = ndtest::scratchDir("e2rep3mod");
    nimbusd::Config cfg;
    nimbusd::NimbusdRig::Options opt;
    opt.dataDir = dir;
    opt.embeddings = false;
    opt.embedDims = 64;
    nimbusd::NimbusdRig rig(cfg, opt);
    nimbusd::EngineThread eng(&rig);
    eng.start();
    nimbusd::ReplyBuffer replies;
    nimbusd::HttpControl http(&eng, "127.0.0.1", 0, "rep3-token", &replies, &rig);
    const int port = http.start();
    nimbusd::DaemonHttpTransport cl;
    const Resp s = get(cl, port, "/api/state");
    if (!router)
      c.ok(s.status == 200 && has(s.body, "\"memberModeration\":\"Members' messages and the replies to them are not "
                                          "checked by a moderation service"),
           "own key (" + rig.hostBadge() + "): /api/state says members are not moderated");
    else
      c.ok(s.status == 200 && !has(s.body, "memberModeration"), "the Cumulo router: no such line");
    http.stop();
    eng.stop();
    unsetenv("CUMULO_API_KEY");
    unsetenv("ANTHROPIC_API_KEY");
    ndtest::rmTree(dir);
  }
}

}  // namespace

int main() {
  ndtest::Ctx c;
  std::printf("test_engine2_rep3\n");
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY", "NIMBUSD_ENGINE",
                        "CUMULO_API_KEY"})
    unsetenv(k);
  caseConnectors(c);
  caseSend(c);
  caseDocsAndStatus(c);
  caseCeiling(c);
  caseModeration(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  return c.failures ? 1 : 0;
}
