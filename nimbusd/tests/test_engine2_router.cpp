// test_engine2_router - the hosted engine 2 on every provider slot and through the Cumulo router (lane E2-R8b),
// offline against the scripted provider (no request leaves the process):
//   (a) one owner turn with a tool call on each direct slot (Anthropic, OpenAI, Mistral, Z.ai): the request goes
//       to that provider's host in its own format, the tool runs once, the reply leaves, and the spend is booked
//       to that provider and model with its cache counted the ledger's way;
//   (b) the same turn on an instance with only a Cumulo router key, for every upstream: the request goes to the
//       router under /router/<upstream>/, with the bearer key, the deadline header and the bare model, and the
//       spend is booked to the router's row; a routed refusal is decided by its reason (no retry on a refused key);
//   (c) a summarizer pass on a non-Claude head runs on that head's own wire and is booked to its provider;
//   (d) a head change in the settings is seen by the next turn without a restart (the conversation carried);
//   (e) the monthly ceiling reached during a turn stops it: the next request is not sent, the person gets the
//       pause line, and no retry waits;
//   (f) a member's message on a router-only instance goes to the router (the platform moderates routed traffic);
//   (g) the engine-2 host names no provider by a literal.
#include <string>
#include <vector>

#include "../../test/support/scripted_provider/scripted_provider.h"
#include "engine2_host.h"
#include "posix_fs.h"
#include "test_util.h"

namespace sp = harness_test::scripted;

namespace {

constexpr const char* kRouter = "router.test";

// Calls a hook after each request is answered (the ceiling case flips the pause there).
class Model : public sp::ScriptedProvider {
 public:
  std::function<void(size_t)> after;
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    const bool ok = sp::ScriptedProvider::exec(req, out, err);
    if (after) after(requests().size());
    return ok;
  }
};

struct Rig {
  std::string dir;
  nimbusd::PosixFiles files;
  Model model;
  std::string slot, modelId, sub;
  bool paused = false;
  std::vector<std::pair<std::string, std::string>> sent;   // chat, text
  std::vector<std::pair<nimbus::orch::TurnSpend, std::string>> spends;
  std::function<nimbus::orch::Role(const std::string&)> roleOf;
  std::unique_ptr<nimbusd::Engine2Host> host;

  Rig(const std::string& d, std::string s, std::string m) : dir(d), files(d + "/mem/files"), slot(std::move(s)), modelId(std::move(m)) {}

  std::string start() {
    nimbusd::Engine2Options o;
    o.dataDir = dir;
    o.files = &files;
    o.head = [this] { return slot; };
    o.model = [this] { return modelId; };
    o.summarizerModel = [this] { return sub; };
    o.key = [] { return std::string("k-fake-e2r8b"); };
    o.routerHost = kRouter;
    o.utcOffsetMin = [](uint32_t) { return 0; };
    o.dayStartMin = [] { return uint16_t(240); };
    o.deliver = [this](const std::string& chat, const std::string& text) { sent.push_back({chat, text}); };
    o.transport = &model;
    o.spend = [this](const nimbus::orch::TurnSpend& s, const std::string& tag) { spends.push_back({s, tag}); };
    o.paused = [this] { return paused; };
    o.roleOf = roleOf;
    host.reset(new nimbusd::Engine2Host(o));
    return host->start();
  }
};

bool hasHeader(const agent::HttpRequest& r, const std::string& name) {
  for (const auto& h : r.headers) {
    std::string n = h.first;
    for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
    if (n == name) return true;
  }
  return false;
}

std::string tailOf(agent::plain::Dialect d) {
  return d == agent::plain::Dialect::AnthropicMessages ? "/messages"
         : d == agent::plain::Dialect::OpenAIResponses ? "/responses"
                                                       : "/chat/completions";
}

sp::ChatShape shapeOf(const std::string& up) {
  return up == "mistral" ? sp::ChatShape::Mistral : up == "zai" ? sp::ChatShape::Zai : sp::ChatShape::OpenAI;
}

// One owner turn with a write_file call, then the reply; true when every check held.
void toolTurn(ndtest::Ctx& c, Rig& r, const std::string& label) {
  sp::Step first = sp::call("write_file", R"({"path":"r8b/a.txt","content":"one\n"})");
  sp::Step done = sp::say("Saved.");
  done.usage.cacheRead = 40;   // the ledger's count of the cache differs by provider
  const std::string m = r.modelId.substr(r.modelId.find('/') + 1);   // the model the provider answers with
  first.model = done.model = m;
  r.model.push(first);
  r.model.push(done);
  c.eq(r.start(), "", label + ": the host starts");
  const nimbusd::PlainTurn t = r.host->say("owner", "Save one to r8b/a.txt", "web");
  c.ok(t.ending == nimbusd::PlainEnding::Reply && t.stated == "Saved.", label + ": the turn replies");
  std::string body;
  c.ok(nimbusd::fsutil::readFile(r.dir + "/mem/files/r8b/a.txt", body) && body == "one\n", label + ": the tool ran");
  std::string why;
  c.ok(r.model.verify(why), label + ": both scripted steps asked, no failure: " + why);
  c.ok(r.model.allPaired(why), label + ": every call has its result: " + why);
}

const nimbus::orch::SpendSlot* turnSlot(const Rig& r) {
  for (const auto& s : r.spends)
    if (s.second == "turn" && s.first.n) return &s.first.slot[0];
  return nullptr;
}

// Every request of a direct head: to the provider's host, in its own format, with its key header and no router
// deadline.
void directRequests(ndtest::Ctx& c, const Rig& r, const std::string& slot) {
  const nimbusd::Engine2Head& h = r.host->head();
  const std::string tail = tailOf(h.slot.dialect);
  const auto& reqs = r.model.requests();
  bool hostOk = !reqs.empty() && !h.slot.endpoint.host.empty(), pathOk = !reqs.empty(), keyOk = !reqs.empty();
  for (const auto& q : reqs) {
    hostOk = hostOk && q.req.host == h.slot.endpoint.host && q.req.host.find(slot == "zai" ? "z.ai" : slot) != std::string::npos;
    pathOk = pathOk && q.req.path.size() > tail.size() && q.req.path.compare(q.req.path.size() - tail.size(), tail.size(), tail) == 0;
    keyOk = keyOk && !hasHeader(q.req, "x-cumulo-deadline-ms") && (hasHeader(q.req, "x-api-key") || hasHeader(q.req, "authorization"));
  }
  c.ok(hostOk, slot + ": every request goes to " + h.slot.endpoint.host);
  c.ok(pathOk, slot + ": in its own format (" + (reqs.empty() ? std::string() : reqs[0].req.path) + ")");
  c.ok(keyOk, slot + ": with the provider's key header and no router deadline");
}

void caseDirect(ndtest::Ctx& c) {
  std::printf("  (a) one tool turn on each direct slot\n");
  const std::vector<std::pair<std::string, std::string>> heads = {
      {"anthropic", "claude-haiku-5-5"}, {"openai", "gpt-5.6"}, {"mistral", "mistral-large-4-0"}, {"zai", "glm-5.3"}};
  for (const auto& hm : heads) {
    const std::string dir = ndtest::scratchDir("e2r8b-direct");
    Rig r(dir, hm.first, hm.second);
    r.model.chatShape = shapeOf(hm.first);
    toolTurn(c, r, hm.first);
    const nimbusd::Engine2Head& h = r.host->head();
    c.ok(!h.slot.endpoint.routed && h.upstream == hm.first, hm.first + ": a direct head");
    directRequests(c, r, hm.first);
    const nimbus::orch::SpendSlot* s = turnSlot(r);
    c.ok(s && std::string(s->host) == hm.first, hm.first + ": the turn is booked to " + hm.first);
    c.ok(s && s->billed == 2, hm.first + ": two billed responses");
    // Anthropic's ledger counts the input without the cache; every other provider counts it inside.
    const uint32_t wantIn = hm.first == "anthropic" ? 200 : 240;
    c.ok(s && s->in == wantIn && s->cacheR == 40, hm.first + ": input " + std::to_string(s ? s->in : 0) + " (want " +
                                                      std::to_string(wantIn) + "), cache read 40");
    ndtest::rmTree(dir);
  }
}

void caseRouted(ndtest::Ctx& c) {
  std::printf("  (b) the same turn through the Cumulo router, every upstream\n");
  const std::vector<std::pair<std::string, std::string>> sels = {{"anthropic", "anthropic/claude-sonnet-5-5"},
                                                                 {"openai", "openai/gpt-5.6"},
                                                                 {"mistral", "mistral/mistral-large-4-0"},
                                                                 {"zai", "zai/glm-5.3"}};
  for (const auto& us : sels) {
    const std::string dir = ndtest::scratchDir("e2r8b-routed");
    Rig r(dir, "cumulo", us.second);
    r.model.chatShape = shapeOf(us.first);
    toolTurn(c, r, us.second);
    const auto& reqs = r.model.requests();
    const std::string bare = us.second.substr(us.second.find('/') + 1);
    bool where = !reqs.empty(), auth = !reqs.empty(), deadline = !reqs.empty(), model = !reqs.empty();
    for (const auto& q : reqs) {
      where = where && q.req.host == kRouter && q.req.path.rfind("/router/" + us.first + "/", 0) == 0;
      auth = auth && hasHeader(q.req, "authorization") && !hasHeader(q.req, "x-api-key");
      deadline = deadline && hasHeader(q.req, "x-cumulo-deadline-ms");
      model = model && q.body.find("\"model\":\"" + bare + "\"") != std::string::npos;
    }
    c.ok(where, us.second + ": to the router under /router/" + us.first + "/ (" + (reqs.empty() ? std::string() : reqs[0].req.path) + ")");
    c.ok(auth, us.second + ": with the bearer key only");
    c.ok(deadline, us.second + ": with the deadline header");
    c.ok(model, us.second + ": asking the bare model in the body");
    const nimbus::orch::SpendSlot* s = turnSlot(r);
    c.ok(s && std::string(s->host) == "cumulo" && std::string(s->model).find(bare) != std::string::npos,
         us.second + ": booked to the router's row (" + (s ? std::string(s->host) + " " + s->model : std::string("none")) + ")");
    ndtest::rmTree(dir);
  }
  // The refusal contract: a refused router key is decided by its reason, once, with no retry.
  const std::string dir = ndtest::scratchDir("e2r8b-refusal");
  Rig r(dir, "cumulo", "mistral/mistral-large-4-0");
  r.model.chatShape = sp::ChatShape::Mistral;
  r.model.push(sp::httpError(401, R"({"reason":"invalid_router_key","retryable":false})"));
  c.eq(r.start(), "", "refusal: the host starts");
  const nimbusd::PlainTurn t = r.host->say("owner", "Hello", "web");
  c.ok(t.ending != nimbusd::PlainEnding::Reply && r.model.requests().size() == 1, "a refused router key: one request, no retry");
  c.ok(!r.sent.empty(), "...and the owner is told");
  ndtest::rmTree(dir);
}

void caseSummarizer(ndtest::Ctx& c) {
  std::printf("  (c) a summarizer pass on a non-Claude head\n");
  const std::string dir = ndtest::scratchDir("e2r8b-sum");
  Rig r(dir, "mistral", "mistral-large-4-0");
  r.sub = "mistral-small-latest";
  r.model.chatShape = sp::ChatShape::Mistral;
  for (int i = 0; i < 9; ++i) r.model.push(sp::say("Reply " + std::to_string(i) + "."));
  c.eq(r.start(), "", "the host starts on Mistral");
  for (int i = 0; i < 9; ++i) (void)r.host->say("owner", "Message " + std::to_string(i) + " about the garden.", "web");
  sp::Step s = sp::say("+F The person talks about the garden.");
  s.usage.input = 5000;
  s.usage.cacheRead = 1000;
  r.model.push(s);
  c.ok(r.host->requestCompaction(), "/compact is queued");
  auto compacts = [&r] {
    size_t n = 0;
    for (const auto& sp : r.spends) n += sp.second == "compact";
    return n;
  };
  for (int i = 0; i < 4 && compacts() == 0; ++i) (void)r.host->idle(false);
  c.ok(compacts() == 1, "the summarizer's request is counted once");
  const auto& last = r.model.last();
  c.ok(last.req.host == "api.mistral.ai" && last.req.path.find("/chat/completions") != std::string::npos,
       "the summarizer runs on the head's own wire: " + last.req.host + last.req.path);
  c.ok(last.body.find("mistral-small-latest") != std::string::npos, "...on the sub-agent model");
  for (const auto& sp : r.spends)
    if (sp.second == "compact")
      c.ok(std::string(sp.first.slot[0].host) == "mistral" && sp.first.slot[0].in == 6000,
           "booked to mistral, the cache inside the input (" + std::to_string(sp.first.slot[0].in) + ")");
  std::string why;
  c.ok(r.model.verify(why), "every scripted step asked: " + why);
  ndtest::rmTree(dir);
}

void caseHeadChange(ndtest::Ctx& c) {
  std::printf("  (d) a head change is seen without a restart\n");
  const std::string dir = ndtest::scratchDir("e2r8b-change");
  Rig r(dir, "anthropic", "claude-haiku-5-5");
  r.model.push(sp::say("One."));
  r.model.push(sp::say("Two."));
  r.model.push(sp::say("Three."));
  c.eq(r.start(), "", "the host starts on Anthropic");
  (void)r.host->say("owner", "first message", "web");
  r.slot = "openai";
  r.modelId = "gpt-5.6";
  const nimbusd::PlainTurn t = r.host->say("owner", "second message", "web");
  c.ok(t.ending == nimbusd::PlainEnding::Reply, "the turn after the change replies");
  c.ok(r.model.last().req.host == "api.openai.com" && r.model.last().req.path.find("/responses") != std::string::npos,
       "the new provider is asked: " + r.model.last().req.host + r.model.last().req.path);
  c.ok(r.model.last().body.find("first message") != std::string::npos, "...with the conversation before the change");
  const nimbus::orch::SpendSlot* s = r.spends.empty() ? nullptr : &r.spends.back().first.slot[0];
  c.ok(s && std::string(s->host) == "openai", "the turn is booked to openai");
  r.slot = "cumulo";
  r.modelId = "zai/glm-5.3";
  (void)r.host->say("owner", "third message", "web");
  c.ok(r.model.last().req.host == kRouter && r.model.last().req.path.rfind("/router/zai/", 0) == 0,
       "a change to the router is seen too: " + r.model.last().req.path);
  const size_t asked = r.model.requests().size();
  r.slot = "custom";
  r.modelId = "my-model";
  (void)r.host->say("owner", "fourth message", "web");
  c.ok(r.model.requests().size() == asked, "a head no route serves sends no request");
  c.ok(!r.sent.empty() && r.sent.back().second == nimbusd::kNoHeadLine, "...and the owner is told in one line");
  ndtest::rmTree(dir);
}

void caseCeiling(ndtest::Ctx& c) {
  std::printf("  (e) the monthly ceiling stops a running turn\n");
  const std::string dir = ndtest::scratchDir("e2r8b-ceiling");
  Rig r(dir, "openai", "gpt-5.6");
  r.model.push(sp::call("write_file", R"({"path":"r8b/b.txt","content":"two\n"})"));
  r.model.push(sp::say("never sent"));
  r.model.after = [&r](size_t n) {
    if (n == 1) r.paused = true;   // the ceiling is reached while the first answer's tool runs
  };
  c.eq(r.start(), "", "the host starts");
  const uint32_t t0 = nimbusd::vclock::steadyMs();
  const nimbusd::PlainTurn t = r.host->say("owner", "Save two", "web");
  c.ok(r.model.requests().size() == 1, "the request after the ceiling was not sent");
  std::string body;
  c.ok(nimbusd::fsutil::readFile(dir + "/mem/files/r8b/b.txt", body), "the call answered before it ran (stored once)");
  c.ok(r.host->ceilingRefusals() >= 1 && t.stated == "plain-rig:paused", "the turn ended at the ceiling: " + t.stated);
  size_t paused = 0, other = 0;
  for (const auto& s : r.sent) (s.second == nimbusd::kPausedLine ? paused : other)++;
  c.ok(paused == 1 && other == 0, "the person gets the pause line once and no failure line");
  c.ok(nimbusd::vclock::steadyMs() - t0 < 1500, "no retry waited while paused");
  const nimbusd::PlainTurn t2 = r.host->say("owner", "Again", "web");
  c.ok(t2.stated == "plain-rig:paused" && r.model.requests().size() == 1, "the next message is refused before any request");
  ndtest::rmTree(dir);
}

void caseMember(ndtest::Ctx& c) {
  std::printf("  (f) a member's message on a router-only instance goes through the router\n");
  const std::string dir = ndtest::scratchDir("e2r8b-member");
  Rig r(dir, "cumulo", "anthropic/claude-sonnet-5-5");
  r.roleOf = [](const std::string& chat) {
    return chat == "555" ? nimbus::orch::Role::User : nimbus::orch::Role::Admin;
  };
  r.model.push(sp::say("Hi."));
  c.eq(r.start(), "", "the host starts on the router");
  const nimbusd::PlainTurn t = r.host->say("555", "hello from a member", "tg");
  c.ok(t.ending == nimbusd::PlainEnding::Reply, "the member's turn replies");
  c.ok(r.model.requests().size() == 1 && r.model.last().req.host == kRouter &&
           r.model.last().req.path.rfind("/router/anthropic/", 0) == 0 && hasHeader(r.model.last().req, "authorization"),
       "the member's message reaches only the router, where the platform moderates it");
  ndtest::rmTree(dir);
}

void caseNoLiteral(ndtest::Ctx& c) {
  std::printf("  (g) the engine-2 host names no provider by a literal\n");
  std::string f = __FILE__;
  f = f.substr(0, f.rfind('/')) + "/../src/engine2_host.h";
  std::string src;
  c.ok(nimbusd::fsutil::readFile(f, src) && !src.empty(), "the host's source is read");
  c.ok(src.find("anthropic") == std::string::npos, "no \"anthropic\" in engine2_host.h");
}

void caseHeadTable(ndtest::Ctx& c) {
  std::printf("  (h) the head table\n");
  const nimbusd::Engine2Head a = nimbusd::engine2Head("anthropic", "claude-sonnet-5-5", kRouter);
  c.ok(a.ok() && a.slot.endpoint.host == "api.anthropic.com" && a.slot.dialect == agent::plain::Dialect::AnthropicMessages, "anthropic: direct Messages");
  const nimbusd::Engine2Head z = nimbusd::engine2Head("zai", "glm-5.3", kRouter);
  c.ok(z.ok() && z.slot.dialect == agent::plain::Dialect::ChatCompletions && !z.slot.endpoint.routed, "zai: direct Chat");
  const nimbusd::Engine2Head b = nimbusd::engine2Head("cumulo", "gpt-5.6", kRouter);
  c.ok(b.ok() && b.slot.endpoint.routed && b.upstream == "openai" && b.slot.dialect == agent::plain::Dialect::OpenAIResponses,
       "a bare router selector runs on the router's default upstream");
  for (const auto& hm : std::vector<std::pair<std::string, std::string>>{
           {"custom", "x"}, {"", ""}, {"nobody", "m"}, {"cumulo", ""}, {"cumulo", "nobody/m"}})
    c.ok(!nimbusd::engine2Head(hm.first, hm.second, kRouter).ok(), "no route: '" + hm.first + "' '" + hm.second + "'");
}

}  // namespace

int main() {
  ndtest::Ctx c;
  std::printf("test_engine2_router\n");
  caseDirect(c);
  caseRouted(c);
  caseSummarizer(c);
  caseHeadChange(c);
  caseCeiling(c);
  caseMember(c);
  caseNoLiteral(c);
  caseHeadTable(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  return c.failures ? 1 : 0;
}
