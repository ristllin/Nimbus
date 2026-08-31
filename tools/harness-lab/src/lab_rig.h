#pragma once
#include <chrono>
#include <thread>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "curl_transport.h"
#include "lab_env.h"
#include "lab_files.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/websearch.h"
#include "nimbus/docs_pack.h"
#include "nimbus/orch/embedding.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/session_tools.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_memory.h"

// LabRig - a whole Nimbus orchestrator, on this machine.
//
// The device's own TurnEngine, tool registry, memory engines and provider
// adapters, wired to libcurl instead of WiFiClientSecure and to RAM instead of
// an SD card. Everything above the transport seam is the SAME CODE the board
// runs, so a bug reproduced here is a bug there.
//
// What is deliberately NOT simulated: the LEDs, the panel, Telegram, and
// the ~46 KB internal heap. `freeHeap` reports a large value so the device's
// memory gates never fire - the lab is for exercising harness LOGIC, and a
// scenario that depends on heap pressure belongs on hardware.
namespace lab {

namespace orch = nimbus::orch;

struct TurnRecord {
  std::string chatId, userText, reply;
  std::vector<std::string> toolCalls;    // "name(args)"
  std::vector<std::string> toolResults;  // truncated result text
  std::vector<std::string> deliveries;
  std::string host;
  uint32_t    tokensIn = 0, tokensOut = 0;
  double      seconds = 0;
  bool        ok = false;
  std::string systemPrompt;
};

class LabRig {
 public:
  struct Options {
    std::string priority = "mistral,openai,anthropic";  // cheapest first by default
    std::string devName  = "Nimbus-Lab";
    // W14: the ROLE the lab's chats run as, so the principal-scoped tool
    // advertisement can be exercised against a real provider ("admin" is the
    // lab default - a lab is single-owner by construction).
    std::string role     = "admin";   // admin | user | guest | unknown
    bool        toolLoop = true;
    bool        verboseHttp = false;
    bool        embeddings = true;   // real embeddings => real associative recall
    // Simulated INTERNAL-heap reading, in bytes. The device's turn/loop gates all
    // read ESP.getFreeHeap(); reporting a large value here means they never fire,
    // which is right for logic testing but makes the lab blind to the failure the
    // owner actually hit (heap 30980 -> 10032, the loop bailing with cap=heap and
    // leaving an unanswered tool call in the provider's stored chain). Setting this
    // to a device-like number reproduces those gates on the host.
    // 0 = report plenty (default).
    uint32_t    heapBytes = 0;
    // Bytes subtracted from the reported heap on every read, so the value FALLS
    // the way it does on the board. A static low number cannot reproduce the real
    // failure: the turn is admitted above ORCH_TURN_HARD_FLOOR and only later
    // crosses ORCH_LOOP_MIN_HEAP mid-loop. Decay reproduces exactly that.
    uint32_t    heapDecay = 0;
    uint32_t    heapFloor = 6000;   // never report below this
    std::string embedModel = "mistral-embed";
    std::string embedHost  = "mistral";
    int         embedDims  = 1024;
    std::map<std::string, std::string> models;  // host -> model override
  };

  LabRig(Env env, Options opt) : env_(std::move(env)), opt_(std::move(opt)) {
    http_.verbose = opt_.verboseHttp;
    // The harness's own log lines (headloop rounds, cap reasons, provider
    // failures) are the single most useful diagnostic there is - on the device
    // they go to a 1280-byte ring that wraps in seconds. Here they just print.
    installLog(opt_.verboseHttp);
    buildMemory();
    buildEngine();
  }

  // ---- the surface a scenario drives ---------------------------------------

  // One owner turn, start to finish, exactly as a Telegram message would run.
  TurnRecord say(const std::string& chatId, const std::string& text) {
    resetSimHeap();   // each turn starts from the device's resting heap
    cur_ = TurnRecord{};
    cur_.chatId = chatId;
    cur_.userText = text;
    const auto t0 = std::chrono::steady_clock::now();
    eng_->handleMessage(text, "Owner", chatId);
    cur_.seconds = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - t0).count();
    cur_.systemPrompt = eng_->lastInstructions();
    cur_.tokensIn = eng_->lastTurnUsage().promptTokens;
    cur_.tokensOut = eng_->lastTurnUsage().completionTokens;
    cur_.ok = !cur_.deliveries.empty();
    if (!cur_.deliveries.empty()) cur_.reply = cur_.deliveries.back();
    turns_.push_back(cur_);
    return cur_;
  }

  // An unattended turn (a routine firing, or the nightly dream stage).
  orch::FireOutcome scheduled(const std::string& chatId, const std::string& prompt,
                              const std::string& name, bool quietOk = false) {
    cur_ = TurnRecord{};
    cur_.chatId = chatId;
    cur_.userText = prompt;
    auto r = eng_->injectScheduledTurn(chatId, prompt, name, "", quietOk);
    if (!cur_.deliveries.empty()) cur_.reply = cur_.deliveries.back();
    cur_.ok = r.ok;
    turns_.push_back(cur_);
    return r;
  }

  // Call a tool directly, without a model in the loop - for asserting what the
  // tool surface itself does.
  std::string callTool(const std::string& name, const std::string& argsJson) {
    std::string rpc = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" +
                      name + R"(","arguments":)" + argsJson + "}}";
    return reg_.handleRpc(rpc, principal());
  }

  agent::TurnEngine& engine() { return *eng_; }
  orch::ToolRegistry& registry() { return reg_; }
  orch::VectorMemory& vectors() { return vec_; }
  orch::EpisodicStore& episodic() { return *epi_; }
  CurlHttpTransport& http() { return http_; }
  LabFiles& files() { return files_; }
  const std::vector<TurnRecord>& turns() const { return turns_; }
  const std::vector<std::string>& toolNames() const { return toolNames_; }

  uint32_t totalIn() const {
    uint32_t n = 0; for (auto& t : turns_) n += t.tokensIn; return n;
  }
  uint32_t totalOut() const {
    uint32_t n = 0; for (auto& t : turns_) n += t.tokensOut; return n;
  }

  bool hostAvailable(const std::string& h) const { return !env_.providerKey(h).empty(); }
  const Options& options() const { return opt_; }

  // ⚠ prism: roleFromName leaves the out-param UNTOUCHED on an unrecognized
  // string, so discarding its result made "--role=Guest" (or any typo) run as
  // ADMIN - a scoping check would then "pass" from a session where nothing was
  // hidden. validateRole() hard-fails at startup instead; this stays total.
  orch::Role role() const {
    orch::Role r = orch::Role::Admin;
    orch::roleFromName(opt_.role, r);
    return r;
  }
  // true when opt_.role names a real role; main() refuses to start otherwise.
  static bool validRole(const std::string& s) {
    orch::Role r = orch::Role::Admin;
    return orch::roleFromName(s, r);
  }

  orch::Principal principal() const {
    return orch::principalForRole("lab", role());
  }

 private:
  // hlog is a plain function-pointer sink, so the capture buffer is a static.
  static std::vector<std::string>& logLines() {
    static std::vector<std::string> v;
    return v;
  }
  static void installLog(bool echo) {
    logLines().clear();
    static bool s_echo = false;
    s_echo = echo;
    agent::hlog::setSink(+[](const char* l) {
      logLines().push_back(l);
      if (s_echo) std::fprintf(stderr, "[harness] %s\n", l);
    });
  }

 public:
  // Every harness log line from this run - scenarios assert on these.
  static const std::vector<std::string>& harnessLog() { return logLines(); }
  static bool loggedContaining(const char* needle) {
    for (const auto& l : logLines())
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }

 private:
  // ---- memory + tools -------------------------------------------------------
  void buildMemory() {
    vec_.configure(opt_.embedDims);
    vec_.setMaxEntries(2000);
    epi_.reset(new orch::InMemoryEpisodicStore());

    orch::MemoryContext mc;
    mc.vec = &vec_;
    mc.scratch = &scratch_;
    mc.cfg = &memCfg_;
    mc.episodic = epi_.get();
    mc.nowHours = [] {
      return (uint32_t)(std::chrono::duration_cast<std::chrono::hours>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
    };
    mc.embed = [this](const std::string& text) { return embed(text); };
    orch::registerMemoryTools(reg_, mc);

    // Split by tool group so each registration (and its lambda) stays inside the
    // complexity gate. Registration order is preserved (it drives toolNames_).
    registerWebSearchTool();
    registerDocsTools();
    registerDeviceStatusTool();
    files_.registerTools(reg_);

    for (const auto& s : reg_.toolSpecs()) toolNames_.push_back(s.name);
  }

  // web.search - the real thing, through the real portable path. A key is optional:
  // without one the tool is simply not advertised, exactly as on a device with no
  // Tavily key.
  void registerWebSearchTool() {
    tavilyKey_ = env_.get("TAVILY_API_KEY");
    if (tavilyKey_.empty()) return;
    reg_.add("web.search",
             "Search the live web for up-to-date information. Returns an answer plus "
             "top results (title, url, snippet).",
             [this](ArduinoJson::JsonObjectConst a,
                    const orch::Principal&) -> orch::ToolResult {
               std::string q = a["query"].is<const char*>()
                                   ? std::string(a["query"].as<const char*>()) : std::string();
               if (q.empty()) return orch::ToolResult::fail("missing 'query'");
               int k = a["max_results"].is<int>() ? a["max_results"].as<int>() : 5;
               auto r = agent::websearch::search(http_, tavilyKey_, q, k);
               if (!r.ok) return orch::ToolResult::fail("web search failed: " + r.err);
               return orch::ToolResult::ok(r.digest);
             },
             R"({"type":"object","properties":{"query":{"type":"string"},)"
             R"("max_results":{"type":"integer"}},"required":["query"]})");
  }

  // The REAL on-device docs pack (docs.search / docs.read) through the real portable
  // retrieval - so the github-setup scenario exercises the actual failure mechanism (a
  // maker section outranking the user answer), not just the prompt rail. Mirrors the
  // device registrations (memory_subsystem.cpp), minus docs.list (the scenarios need
  // retrieval, not browsing).
  void registerDocsTools() {
    reg_.add("docs.search",
             "Search your own device documentation (ranked keyword match) - use it "
             "BEFORE saying what you can or cannot do. Results marked audience:maker "
             "are firmware-development docs - answer an owner's how-do-I question "
             "from the unmarked (user) sections.",
             [](ArduinoJson::JsonObjectConst a, const orch::Principal&) -> orch::ToolResult {
               std::string q = a["query"].is<const char*>()
                                   ? std::string(a["query"].as<const char*>()) : std::string();
               if (q.empty()) return orch::ToolResult::fail("missing 'query'");
               const nimbus::docs::DocSection* hits[8];
               size_t n = nimbus::docs::search(q, hits, 8);
               if (n == 0)
                 return orch::ToolResult::ok("no sections match - try fewer keywords");
               JsonDocument d;
               auto arr = d.to<JsonArray>();
               for (size_t i = 0; i < n; i++) {
                 auto o = arr.add<JsonObject>();
                 o["id"] = hits[i]->id;
                 o["title"] = hits[i]->title;
                 o["snippet"] = nimbus::docs::snippet(*hits[i], q);
               }
               std::string s; serializeJson(d, s);
               return orch::ToolResult::ok(s);
             },
             R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})");
    reg_.add("docs.read",
             "Read ONE section of your own device documentation by id (from docs.search).",
             [](ArduinoJson::JsonObjectConst a, const orch::Principal&) -> orch::ToolResult {
               std::string id = a["id"].is<const char*>()
                                    ? std::string(a["id"].as<const char*>()) : std::string();
               if (id.empty()) return orch::ToolResult::fail("missing 'id'");
               const nimbus::docs::DocSection* s = nimbus::docs::find(id);
               if (!s) return orch::ToolResult::fail("unknown doc id '" + id + "'");
               std::string out;
               out += "## ";
               out += s->title;
               out += "\n\n";
               out += s->body;
               return orch::ToolResult::ok(out);
             },
             R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
  }

  // A stand-in for the device-status tool, so prompts that ask "what are you running
  // on" have something honest to call.
  void registerDeviceStatusTool() {
    reg_.add("device.status", "Report the device's current state.",
             [this](ArduinoJson::JsonObjectConst, const orch::Principal&) {
               return orch::ToolResult::ok(
                   std::string("{\"name\":\"") + opt_.devName +
                   "\",\"mode\":\"orchestrator\",\"host\":\"harness-lab\",\"vectors\":" +
                   std::to_string(vec_.size()) + "}");
             },
             R"({"type":"object","properties":{}})");
  }

  // Real embeddings through the same portable request/response codec the device
  // uses - so recall in the lab is genuine associative recall, not a fake.
  std::vector<int8_t> embed(const std::string& text) {
    if (!opt_.embeddings) return {};
    const std::string key = env_.providerKey(opt_.embedHost);
    if (key.empty()) return {};

    agent::HttpRequest req;
    req.method = "POST";
    req.host = opt_.embedHost == "mistral" ? "api.mistral.ai" : "api.openai.com";
    req.path = "/v1/embeddings";
    req.timeoutMs = 30000;
    req.headers.push_back({"Content-Type", "application/json"});
    req.headers.push_back({"Authorization", "Bearer " + key});
    req.body = orch::buildEmbeddingRequest(opt_.embedModel, text,
                                           opt_.embedHost == "mistral" ? 0 : opt_.embedDims);

    agent::HttpResponse resp;
    std::string err;
    if (!http_.exec(req, resp, err) || resp.status < 200 || resp.status >= 300) {
      std::fprintf(stderr, "[embed] failed: %s (status %d)\n", err.c_str(), resp.status);
      return {};
    }
    std::vector<float> f;
    if (!orch::parseEmbeddingResponse(resp.body.c_str(), 0, f, err)) {
      std::fprintf(stderr, "[embed] parse failed: %s\n", err.c_str());
      return {};
    }
    return orch::VectorMemory::quantize(f);
  }

  // The simulated internal-heap reading shared by Platform and ProviderDeps.
  uint32_t simHeap() const {
    if (!opt_.heapBytes) return 4u * 1024u * 1024u;
    const uint32_t used = heapReads_++ * opt_.heapDecay;
    return used >= opt_.heapBytes - opt_.heapFloor ? opt_.heapFloor
                                                   : opt_.heapBytes - used;
  }
  void resetSimHeap() { heapReads_ = 0; }

  // ---- the engine -----------------------------------------------------------
  agent::Platform platform() {
    agent::Platform p;
    p.nowMs = [] {
      return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    // Default: far above every device floor, so the lab exercises logic rather
    // than memory pressure. --heap=<n> reports a device-like value instead, which
    // drives the REAL gates (ORCH_TURN_HARD_FLOOR, ORCH_LOOP_MIN_HEAP) and lets a
    // scenario reproduce a mid-loop heap bail without a board.
    p.freeHeap = [this] { return simHeap(); };
    p.delayMs = [](uint32_t ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };
    p.allocLarge = [](size_t n) { return std::malloc(n); };
    p.freeLarge = [](void* q) { std::free(q); };
    p.nowEpoch = [] {
      return (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count();
    };
    return p;
  }

  agent::HarnessConfig config() {
    agent::HarnessConfig c;
    auto& p = c.provider;
    p.hasKey = [this](const std::string& h) { return !env_.providerKey(h).empty(); };
    p.key = [this](const std::string& h) { return env_.providerKey(h); };
    p.orchHost = [] { return std::string(); };            // "" => top of priority
    p.providerPriority = [this] { return opt_.priority; };
    p.subPriority = [this] { return opt_.priority; };
    p.orchModel = [this](const std::string& h) { return modelFor(h); };
    p.subModel = [this](const std::string& h) { return modelFor(h); };
    p.modelChoices = [](const std::string& h) {
      if (h == "openai")    return std::string("gpt-5.5,gpt-5.4-mini");
      if (h == "anthropic") return std::string("claude-opus-4-8,claude-sonnet-4-6,claude-haiku-4-5");
      if (h == "mistral")   return std::string("mistral-large-latest,mistral-medium-latest,mistral-small-latest");
      return std::string();
    };
    p.convId = [this] { return convId_; };
    p.setConvId = [this](const std::string& v) { convId_ = v; };
    p.customBase = [] { return std::string(); };
    p.customKey = [] { return std::string(); };
    p.customConv = [] { return std::string("openai"); };
    p.customModel = [] { return std::string(); };

    c.loop.toolLoopOn = [this] { return opt_.toolLoop; };
    c.loop.rounds = [] { return 12; };
    c.loop.deadlineS = [] { return 600; };
    c.loop.resultCap = [] { return 4096; };
    c.loop.totalCap = [] { return 24576; };

    c.budget.overBudget = [](const std::string&) { return false; };
    c.budget.recordTokens = [this](const std::string& h, uint32_t in, uint32_t out,
                                   uint32_t, uint32_t, const std::string& tag) {
      spend_.push_back({h, in, out, tag});
    };
    c.ttsEnabled = [] { return false; };
    c.deviceName = [this] { return opt_.devName; };
    return c;
  }

  std::string modelFor(const std::string& h) const {
    auto it = opt_.models.find(h);
    if (it != opt_.models.end()) return it->second;
    if (h == "openai")    return "gpt-5.5";
    if (h == "anthropic") return "claude-sonnet-4-6";
    if (h == "mistral")   return "mistral-large-latest";
    return std::string();
  }

  agent::providers::ProviderDeps providerDeps() {
    agent::providers::ProviderDeps pd;
    pd.http = &http_;
    pd.key = [this](const char* h) { return env_.providerKey(h); };
    pd.orchModel = [this](const char* h) { return modelFor(h); };
    pd.toolLoopOn = [this] { return opt_.toolLoop; };
    pd.antEnvId = [this] { return antEnv_; };
    pd.setAntEnvId = [this](const std::string& v) { antEnv_ = v; };
    pd.antAgentMap = [this] { return antAgents_; };
    pd.setAntAgentMap = [this](const std::string& v) { antAgents_ = v; };
    pd.customBase = [] { return std::string(); };
    pd.customKey = [] { return std::string(); };
    pd.customConv = [] { return std::string("openai"); };
    pd.customModel = [] { return std::string(); };
    pd.nowMs = [] {
      return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    pd.freeHeap = [this] { return simHeap(); };
    return pd;
  }

  void buildEngine() {
    agent::JobEngine::Deps jd;
    jd.platform = platform();
    jd.deliver = [this](const std::string& c, const std::string& t) { record(c, t); };
    jobs_.reset(new agent::JobEngine(std::move(jd)));

    agent::TurnEngine::Deps d;
    d.cfg = config();
    d.platform = platform();
    d.jobs = jobs_.get();
    d.deliver = [this](const std::string& c, const std::string& t) { record(c, t); };
    d.recall = [this](const std::string& q, const orch::Principal&) {
      std::vector<std::string> out;
      if (!opt_.embeddings) return out;
      auto v = embed(q);
      if (v.empty()) return out;
      for (const auto& hit : vec_.search(v, 5)) out.push_back(hit.content);
      return out;
    };
    d.composeInputs = [this](const std::string& chat) {
      agent::ComposeInputs in;
      in.devName = opt_.devName;
      in.hostLabel = lastHost_.empty() ? std::string("(picking)") : lastHost_;
      in.recentConversation = recentWindow(chat);
      in.runningMemory = memory_;
      return in;
    };
    d.toolSpecs = [this](const orch::Principal& who) { return reg_.toolSpecsFor(who); };
    d.mcpDispatch = [this](const std::string& req, const orch::Principal& who) {
      return reg_.handleRpc(req, who);
    };
    d.connectorsCatalog = [] { return std::string(); };
    d.modelChoices = [this](const std::string& p) { return config().provider.modelChoices(p); };
    d.episodicCaptureUser = [this](const std::string& c, const std::string& t,
                                   const std::string& tag) {
      capture(c, "user", t, tag);
    };
    d.firstAllowedChat = [] { return std::string("owner"); };
    d.journalGc = [] {};

    // The glass box, for free: on the host these hooks are just callbacks.
    d.hooks.onToolCall = [this](const orch::HeadToolCall& c) {
      cur_.toolCalls.push_back(c.name + "(" + trunc(c.argsJson, 200) + ")");
      if (opt_.verboseHttp)
        std::fprintf(stderr, "[tool] %s %s\n", c.name.c_str(), trunc(c.argsJson, 160).c_str());
    };
    d.hooks.onToolResult = [this](const orch::HeadToolResult& r) {
      cur_.toolResults.push_back(trunc(r.output, 300));
      if (opt_.verboseHttp)
        std::fprintf(stderr, "[tool] -> %s%s\n", r.isError ? "ERROR " : "",
                     trunc(r.output, 200).c_str());
    };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) { lastHost_ = ev.host; };

    d.apply.deliver = d.deliver;
    d.apply.stageDevice = [this](const orch::ValidatedAction& va) {
      deviceActions_.push_back((int)va.kind);
    };
    // WHO the turn runs as. Without this hook ApplyDeps::whoFor falls back to a
    // deny-all principal and every mid-turn memory call comes back "this
    // conversation isn't approved to store memories yet" - which is the correct
    // fail-safe, and is exactly what the lab reported before this was wired.
    // A lab is single-owner by construction, so every chat is the admin.
    d.apply.principalFor = [this](const std::string& chat) {
      return orch::principalForRole(chat, role());
    };
    d.apply.setModelMemory = [this](const std::string&, const std::string& m) { memory_ = m; return true; };

    for (const char* h : {"openai", "anthropic", "mistral"}) {
      if (env_.providerKey(h).empty()) continue;
      const std::string host = h;
      d.hosts.add(host, [this, host](std::string& conv, const std::string& ins,
                                     const std::string& inp, std::string& out,
                                     std::string& err, const agent::HeadTools* tools,
                                     orch::TokenUsage* usage) -> bool {
        auto pd = providerDeps();
        if (host == "anthropic")
          return agent::providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, usage);
        if (host == "openai")
          return agent::providers::orchTurnOpenAI(pd, conv, ins, inp, out, err, tools, usage);
        return agent::providers::orchTurnMistral(pd, conv, ins, inp, out, err, tools, usage);
      });
    }
    eng_.reset(new agent::TurnEngine(std::move(d)));
  }

  // ---- episodic helpers -----------------------------------------------------
  void capture(const std::string& chat, const char* role, const std::string& text,
               const std::string& tags) {
    orch::EpisodicMessage m;
    m.sessionId = chat;
    m.kind = orch::MsgKind::Message;
    m.role = role;
    m.text = text;
    m.tags = tags;
    m.tsHours = (uint32_t)std::chrono::duration_cast<std::chrono::hours>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
    epi_->addMessage(m);
  }

  // The per-chat RECENT CONVERSATION window the device builds from its ring.
  std::string recentWindow(const std::string& chat) {
    orch::MsgQuery q;
    q.sessionId = chat;
    q.limit = 12;
    std::string out;
    for (const auto& m : epi_->query(q)) {
      const char* who = (m.role == "user") ? "owner" : "nimbus";
      out += std::string(who) + ": " + trunc(m.text, 400) + "\n";
    }
    return out;
  }

  void record(const std::string& chat, const std::string& text) {
    cur_.deliveries.push_back(text);
    capture(chat, "assistant", text, "");
  }

  static std::string trunc(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
  }

  Env  env_;
  Options opt_;
  CurlHttpTransport http_;

  orch::VectorMemory vec_;
  orch::Scratchpad   scratch_;
  orch::MemConfig    memCfg_;
  std::unique_ptr<orch::EpisodicStore> epi_;
  orch::ToolRegistry reg_;
  LabFiles           files_;
  std::vector<std::string> toolNames_;
  std::string tavilyKey_;

  std::unique_ptr<agent::JobEngine>  jobs_;
  std::unique_ptr<agent::TurnEngine> eng_;

  std::string convId_, antEnv_, antAgents_, memory_, lastHost_;
  std::vector<int> deviceActions_;
  struct Spend { std::string host; uint32_t in, out; std::string tag; };
  std::vector<Spend> spend_;

  mutable uint32_t heapReads_ = 0;

  TurnRecord cur_;
  std::vector<TurnRecord> turns_;
};

}  // namespace lab
