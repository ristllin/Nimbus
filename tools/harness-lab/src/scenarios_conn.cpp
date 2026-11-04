#include "scenarios_conn.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "nimbus/harness/fabric.h"
#include "nimbus/harness/http.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/journal.h"

namespace lab {
namespace conn {
namespace {

// ---- the tiny assertion harness (same shape as scenarios.cpp) ---------------
struct Ctx {
  std::vector<std::string> failures;
  void check(bool cond, const std::string& what) {
    if (cond) std::printf("    ok   %s\n", what.c_str());
    else { std::printf("    FAIL %s\n", what.c_str()); failures.push_back(what); }
  }
  void note(const std::string& n) { std::printf("    ...  %s\n", n.c_str()); }
};

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
void sleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
bool has(const std::string& hay, const std::string& needle) {
  return lower(hay).find(lower(needle)) != std::string::npos;
}
bool anyOf(const std::string& hay, std::initializer_list<const char*> needles) {
  for (const char* n : needles)
    if (has(hay, n)) return true;
  return false;
}

// A journal store in RAM (the C2 pattern): the engines keep the pointer for the
// rig's life.
struct RamStore : orch::JournalStore {
  std::map<int, std::string> slots;
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  bool put(int slot, const std::string& v) override { slots[slot] = v; return true; }
  bool remove(int slot) override { slots.erase(slot); return true; }
  void clearNs() override { slots.clear(); }
};

// A live mistral sub-agent adapter over the rig's own deps: the spawn the head
// asks for runs against the REAL Mistral API with the rig's connectors attached.
struct LiveMistralAdapter : agent::ManagedAgentAdapter {
  agent::providers::ProviderDeps pd;
  std::string model;
  const char* backendId() const override { return "mistral"; }
  agent::Capabilities capabilities() const override { return {}; }
  agent::FabricErr dispatch(const agent::Directive& d, char out[72]) override {
    return agent::providers::mistralDispatch(pd, model, d, out);
  }
  agent::FabricErr poll(const char* id, agent::ResultEnvelope& env) override {
    return agent::providers::mistralPoll(pd, id, env);
  }
  agent::FabricErr cancel(const char*) override { return agent::FabricErr::Unsupported; }
};

// The fake router (the success-path leg): a scripted transport that replays the
// RECORDED provider behavior - mistral-large-4-0 refuses the built-in connectors
// (HTTP 400, code 3004, 31 Oct), mistral-medium-latest serves the run.
struct ScriptedHttp : agent::HttpTransport {
  struct Step { int status; std::string body; };
  std::vector<Step> script;
  std::vector<std::string> bodies;   // every request body, in order
  size_t at = 0;
  bool exec(const agent::HttpRequest& r, agent::HttpResponse& out,
            std::string& err) override {
    if (at >= script.size()) { err = "script exhausted"; return false; }
    bodies.push_back(r.body);
    out.status = script[at].status;
    out.body = script[at].body;
    at++;
    return true;
  }
};

// The Mistral-only persona: the env keys only Mistral, the head is pinned to
// mistral-large-4-0 (the model that refuses the built-in connectors), and the
// connector catalog rides the turn exactly as the device wires it.
Env mistralOnlyEnv(const Env& base) {
  Env e = base;
  e.drop("OPENAI_API_KEY");
  e.drop("ANTHROPIC_API_KEY");
  e.drop("Z_AI_TOKEN");
  return e;
}
LabRig::Options mistralOnly(const LabRig::Options& base) {
  LabRig::Options o = base;
  o.priority = "mistral";
  o.models["mistral"] = "mistral-large-4-0";
  o.connectorsCatalog = true;
  return o;
}

// A Studio connector card as the device web page saves it.
nimbus::orch::ConnectorInfo studioCard(const char* type, const char* cid) {
  nimbus::orch::ConnectorInfo c;
  c.name = type;
  c.prov = "mistral";
  c.kind = "connector";
  c.type = type;
  c.connectorId = cid;
  c.enabled = true;
  return c;
}

// Every provider exchange of the run, one line each (the raw evidence).
void printCalls(LabRig& rig, Ctx& c) {
  for (const auto& call : rig.http().calls) {
    if (!isProviderHost(call.host)) continue;
    c.note(call.host + call.path + " -> " + std::to_string(call.status) +
           " body=" + std::to_string(call.reqBody.size()) + " B");
  }
}

// True when a digit-then-word count appears (an invented "12 unread" style fact).
bool inventedCount(const std::string& reply, const char* word) {
  const std::string low = lower(reply);
  const std::string w = word;
  for (size_t i = 0; i + w.size() < low.size(); i++) {
    if (isdigit((unsigned char)low[i]) && low.compare(i + 1, w.size(), w) == 0 &&
        (i == 0 || !isdigit((unsigned char)low[i - 1])))
      return true;
  }
  return false;
}

// ---- leg 1: Gmail with no connector configured -------------------------------
void legGmailAbsent(const Env& env, const LabRig::Options& base, Ctx& c) {
  LabRig::Options o = mistralOnly(base);
  o.connectors.clear();   // nothing configured: the honest "not set up" state
  LabRig rig(mistralOnlyEnv(env), o);
  const size_t before = rig.http().calls.size();
  auto t = rig.say("conn-gmail-none",
                   "Can you check my Gmail inbox and tell me how many unread emails I have?");
  c.note("reply: " + t.reply.substr(0, 400));
  printCalls(rig, c);

  c.check(!t.reply.empty(), "an answer came back");
  c.check(has(t.reply, "gmail") || has(t.reply, "email"), "the answer names the email service");
  c.check(!inventedCount(t.reply, " unread"), "no invented unread count");
  c.check(!inventedCount(t.reply, " emails"), "no invented email count");
  c.check(anyOf(t.reply, {"not set up", "not configured", "not connected", "isn't set up",
                         "isn't connected", "no gmail", "cannot check", "can't check",
                         "couldn't check", "could not check", "unable to check", "don't have",
                         "do not have", "isn't available", "not available"}),
          "the answer says Gmail is not set up (never a silent success)");
  c.check(has(t.reply, "mistral") || has(t.reply, "connector"),
          "the answer names where Gmail gets set up (Mistral or the connector page)");
  // The head's own prompt is honest about its model's limit and the two-step setup.
  c.check(has(t.systemPrompt, "refuses Mistral's built-in connectors"),
          "the stable prompt says the head model refuses the connectors");
  c.check(t.systemPrompt.find("Connectors on mistral are callable on YOUR OWN turns") ==
              std::string::npos,
          "the stable prompt never promises own-turn connector calls on this head");
  c.check(has(t.systemPrompt, "For the Mistral ones") && has(t.systemPrompt, "gmail"),
          "the stable prompt carries the two-step Gmail setup (console, then device page)");
  // A Mistral-only client: no other provider was ever asked.
  bool onlyMistral = true;
  for (size_t i = before; i < rig.http().calls.size(); ++i) {
    const auto& call = rig.http().calls[i];
    if (!isProviderHost(call.host)) continue;
    if (call.host != "api.mistral.ai") onlyMistral = false;
  }
  c.check(onlyMistral, "every provider request went to api.mistral.ai (no other provider asked)");
}

// ---- leg 2: a Mistral connector configured but unauthorized -------------------
// The connector id is a UUID no workspace can offer. The provider's REAL answer
// (recorded live, 4 Nov) is a silent no-run: the conversation is accepted
// (HTTP 200) and the connector never executes, so the device's provenance check
// is what surfaces it - the owner hears "the connector wasn't called, the text
// is a guess", never a silent success. No owner mailbox is ever reachable.
void legGmailUnauthorized(const Env& env, const LabRig::Options& base, Ctx& c) {
  const std::string fakeId = "019e721b-conn-lane-0000-4000-8000-00000000c0nn";
  LabRig::Options o = mistralOnly(base);
  o.connectors = {studioCard("gmail", fakeId.c_str())};
  o.mistralListed = fakeId;   // the fake probe lists it, so the attach proceeds
  LabRig rig(mistralOnlyEnv(env), o);
  static RamStore store;
  static orch::Journal journal;
  store.slots.clear();
  journal.begin(&store);
  static LiveMistralAdapter adapter;
  adapter.pd = rig.deps();
  adapter.model = rig.options().models.at("mistral");
  static agent::HeavyFabric fabric;
  fabric.registerAdapter(&adapter);
  rig.attachSubAgents(&fabric, &journal);

  const size_t callsBefore = rig.http().calls.size();
  const size_t deliveriesBefore = rig.lastTurn().deliveries.size();
  auto t = rig.say("conn-gmail-unauth",
                   "Search my Gmail for the newest message in my inbox and summarize it.");
  // The spawn is async (the device's pollJobs pumps it): pump until the job
  // finishes, then read what the owner was told.
  for (int i = 0; i < 180 && rig.jobs().activeCount() > 0; i++) {
    rig.jobs().pump();
    if (rig.jobs().activeCount() > 0) sleepMs(1000);
  }
  for (int i = 0; i < 10; i++) { rig.jobs().pump(); sleepMs(500); }   // let the reap run
  // The finished result is parked: the loop-closure synthesis turn delivers it.
  auto syn = rig.synthesize("conn-gmail-unauth");
  c.note("reply: " + t.reply.substr(0, 400));
  std::string delivered;
  for (size_t i = deliveriesBefore; i < rig.lastTurn().deliveries.size(); ++i)
    delivered += rig.lastTurn().deliveries[i] + "\n";
  c.note("delivered: " + delivered.substr(0, 600));
  c.note("synthesis: " + syn.reply.substr(0, 600));
  printCalls(rig, c);

  // The sub's dispatch carried the connector. The provider's REAL answer for a
  // connector the workspace does not offer is a silent no-run: the conversation
  // is accepted (HTTP 200) and the connector simply never executes - so the
  // device's provenance check is what keeps the owner honest.
  bool carried = false;
  for (size_t i = callsBefore; i < rig.http().calls.size(); ++i) {
    const auto& call = rig.http().calls[i];
    if (!isProviderHost(call.host) || call.path.find("/v1/conversations") == std::string::npos)
      continue;
    if (call.reqBody.find(fakeId) != std::string::npos) carried = true;
  }
  bool provenanceFired = false;
  for (const auto& l : LabRig::harnessLog())
    if (l.find("asked for Gmail but no connector ran") != std::string::npos) provenanceFired = true;
  c.check(carried, "a Mistral request carried the unauthorized connector id");
  c.check(provenanceFired, "the device saw the connector never ran (the provenance check fired)");
  c.check(!inventedCount(t.reply, " unread") && !inventedCount(t.reply, " messages"),
          "the head's own reply invents no mailbox data");
  c.check(anyOf(delivered, {"wasn't called", "was not called", "not called", "didn't connect",
                            "did not connect", "couldn't", "could not"}),
          "the owner hears that the connector did not run (never a silent success)");
  c.check(!has(delivered, "your newest") || anyOf(delivered, {"wasn't called", "was not called",
                                                             "not called", "guess"}),
          "any mailbox text the owner reads is marked as a guess, not presented as real");
}

// ---- leg 3: Notion via Mistral, the success path on a fake router -------------
void legNotionSuccess(const Env& env, const LabRig::Options& base, Ctx& c) {
  const std::string notionId = "019e721b-conn-lane-0000-4000-8000-000000n0710n";
  LabRig::Options o = mistralOnly(base);
  o.connectors = {studioCard("notion", notionId.c_str())};
  o.mistralListed = notionId;
  LabRig rig(mistralOnlyEnv(env), o);   // no live turn: the leg proves the spawn path itself

  // The recorded refusal (31 Oct, Lumi errlog), then the run on the tools model.
  ScriptedHttp http;
  http.script.push_back({400, "{\"object\":\"Error\",\"message\":\"Model mistral-large-4-0 "
                             "currently does not support builtin connectors.\","
                             "\"type\":\"invalid_request_error\",\"code\":3004}"});
  http.script.push_back({200, "{\"conversation_id\":\"c-conn-notion\",\"outputs\":["
                             "{\"type\":\"tool.execution\",\"name\":\"notion_create_page\"},"
                             "{\"type\":\"message.output\",\"content\":"
                             "\"Created the page Conn lane test in Notion: "
                             "https://notion.so/conn-lane-test\"}],"
                             "\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":30}}"});
  auto pd = rig.deps();
  pd.http = &http;
  agent::Directive d;
  d.category = "research";
  d.instruction = "Create a page titled 'Conn lane test' in my Notion workspace and reply "
                  "with its URL.";
  char job[72] = {};
  const agent::FabricErr e = agent::providers::mistralDispatch(pd, "mistral-large-4-0", d, job);
  c.note("dispatch err: " + std::to_string((int)e) + " job: " + job);
  std::string reply;
  unsigned connectorCalls = 0;
  if (e == agent::FabricErr::Ok) {
    static agent::ResultEnvelope env2;
    const agent::FabricErr pe = agent::providers::mistralPoll(pd, job, env2);
    c.note("poll err: " + std::to_string((int)pe));
    reply = env2.reply;
    connectorCalls = env2.connectorCalls;
  }
  c.note("reply: " + reply.substr(0, 400));

  c.check(e == agent::FabricErr::Ok, "the dispatch started");
  c.check(http.bodies.size() == 2, "the refusal was paid exactly once, then the run");
  if (http.bodies.size() == 2) {
    c.check(has(http.bodies[0], "mistral-large-4-0") && has(http.bodies[0], notionId),
            "the refused request carried the head model and the Notion connector");
    c.check(has(http.bodies[1], "mistral-medium-latest") && has(http.bodies[1], notionId),
            "the run landed on mistral-medium-latest with the connector still attached");
  }
  c.check(has(reply, "ran on mistral-medium-latest"),
          "the reply says which model actually ran the connector call");
  c.check(connectorCalls >= 1,
          "the provider's own record counts the connector run (" +
              std::to_string(connectorCalls) + ")");
  c.check(!has(reply, "was not called"), "no not-called note: the connector really ran");
  c.check(has(reply, "conn-lane-test"), "the owner gets the connector's real result");
}

struct LegFn { const char* name; void (*fn)(const Env&, const LabRig::Options&, Ctx&); };
const LegFn kLegs[] = {
    {"conn-gmail-none", legGmailAbsent},
    {"conn-gmail-unauth", legGmailUnauthorized},
    {"conn-notion-success", legNotionSuccess},
};

}  // namespace

int run(const Env& env, const LabRig::Options& opt, const std::vector<std::string>& names,
        int& ranOut) {
  int failed = 0, ran = 0;
  for (const auto& leg : kLegs) {
    if (!names.empty() &&
        std::find(names.begin(), names.end(), leg.name) == names.end())
      continue;
    std::printf("\n=== %s ===\n", leg.name);
    Ctx c;
    leg.fn(env, opt, c);
    ran++;
    if (!c.failures.empty()) {
      failed++;
      std::printf("    --> %zu failure(s)\n", c.failures.size());
    }
  }
  std::printf("\n%d conn leg(s) run, %d failed\n", ran, failed);
  ranOut = ran;
  return failed;
}

}  // namespace conn
}  // namespace lab
