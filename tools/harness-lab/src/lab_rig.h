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
#include "lab_prefs.h"   // lane J: the standing rules
#include "lab_wire.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/fault_transport.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/websearch.h"
#include "nimbus/docs_pack.h"
#include "nimbus/orch/embedding.h"
#include "nimbus/orch/food_portions.h"   // lane CTX: food.portions
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_defaults.h"   // --defaults: the device role table
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/session_tools.h"
#include "nimbus/orch/storage_state.h"
#include "nimbus/orch/tool_gates.h"
#include "lab_skills.h"
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
//
// Every request goes through the lab's transport chain (lab_wire.h): the run's
// pins (effort, output cap, per-request ceiling), then any injected faults
// (--fault), then the recorder (--record), then libcurl.
namespace lab {
// True while a scheduled (unattended) turn runs - the prefs rails read it.
inline bool g_labScheduledTurn = false;

// The model a host runs when nothing overrides it. Cheapest current model per
// provider (live tests run Anthropic first, on claude-haiku-5-5).
inline std::string labDefaultModel(const std::string& h) {
  if (h == "anthropic") return "claude-haiku-5-5";
  if (h == "openai")    return "gpt-5.6-luna";
  if (h == "mistral")   return "mistral-small-latest";
  if (h == "zai")       return "glm-5.3-flash";
  return std::string();
}

// Z.ai's OpenAI-compatible head endpoint (the device's ZAI_HOST_PRIMARY /
// ZAI_BASE_PATH, mirrored as nimbusd does).
inline constexpr const char* kLabZaiHost = "api.z.ai";
inline constexpr const char* kLabZaiPath = "/api/paas/v4";

namespace orch = nimbus::orch;

struct TurnRecord {
  std::string chatId, userText, reply;
  std::string turnInputs;   // the per-turn INPUT (where [ACTIVE SKILLS] and [STANDING RULES] ride)
  std::vector<std::string> toolCalls;    // "name(args)"
  std::vector<std::string> toolResults;  // truncated result text
  std::vector<std::string> deliveries;
  std::string host;
  uint32_t    tokensIn = 0, tokensOut = 0;
  uint32_t    cacheRead = 0, cacheWrite = 0;   // the turn's prompt-cache tokens
  double      seconds = 0;
  bool        ok = false;
  std::string systemPrompt;
};

class LabRig {
 public:
  struct Options {
    std::string priority = "anthropic,mistral,zai,openai";  // Anthropic first (owner rule)
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
    // NIMBUS_LAB_EFFORT (default "low"), NIMBUS_LAB_MAXTOK, NIMBUS_LAB_CEILING_MS.
    WirePins    pins = WirePins{"low", 0, 0};
    int         rounds = 12;            // --rounds=<n>: head tool-loop rounds per turn
    bool        midTurnFailover = true; // the fabric loop, as on the device
    std::vector<agent::FaultRule> faults;  // --fault=<spec>, already parsed
    std::string recordDir;              // --record=<dir>: usage + error bodies
    // The scratchpad save fails, as on a full NVS partition: changes live in RAM
    // only and every surface must say so (the claim check holds a "survives a
    // restart" reply against it).
    bool        scratchSaveFails = false;
    // The turn-structured history (histV2), as on the device: on by default;
    // --hist-v1 renders the previous 12-row window instead.
    bool        histV2 = true;
    // --defaults: unpinned hosts run the device's role defaults (lane DEF,
    // nimbus/orch/model_defaults.h): the head row, the sub row for sub-agents, and the
    // background row while a scheduled turn runs - the defaults wired end to end.
    bool        roleDefaults = false;
  };

  LabRig(Env env, Options opt)
      : env_(std::move(env)), opt_(std::move(opt)), rec_(&http_, opt_.recordDir),
        fault_(&rec_, &faultPlan_), wire_(&fault_, &opt_.pins) {
    for (const auto& f : opt_.faults) faultPlan_.add(f);
    http_.verbose = opt_.verboseHttp;
    // The harness's own log lines (headloop rounds, cap reasons, provider
    // failures) are the single most useful diagnostic there is - on the device
    // they go to a 1280-byte ring that wraps in seconds. Here they just print.
    installLog(opt_.verboseHttp);
    buildMemory();
    buildEngine();
  }

  // ---- the surface a scenario drives ---------------------------------------

  // L19-27 live leg: the REAL engine fold - the same runFold the device's
  // compactTick calls - over a recorded digest, minus the device-only storage
  // (the fold-blob write-back and the chain clear live on the device). The
  // provider call, failover ladder, metering and capSummary are identical.
  agent::TurnEngine::FoldResult runFold(const std::string& chat,
                                        const std::string& prevSummary,
                                        const std::string& digest, std::string& summary,
                                        size_t* producedBytes) {
    return eng_->runFold(chat, prevSummary, digest, summary, producedBytes);
  }

  // One owner turn, start to finish, exactly as a Telegram message would run.
  TurnRecord say(const std::string& chatId, const std::string& text) {
    return recordTurn(chatId, text, [&] { eng_->handleMessage(text, "Owner", chatId); });
  }

  // The synthesis turn the job engine runs when its sub-agents' results are parked
  // (TurnEngine::maybeConsolidate), recorded like an owner turn.
  TurnRecord synthesize(const std::string& chatId) {
    return recordTurn(chatId, std::string(), [&] { eng_->maybeConsolidate(chatId); });
  }

  // Run one turn and record it: what was delivered, the prompt, the usage, the time.
  template <typename Run>
  TurnRecord recordTurn(const std::string& chatId, const std::string& userText, Run run) {
    resetSimHeap();   // each turn starts from the device's resting heap
    cur_ = TurnRecord{};
    cur_.chatId = chatId;
    cur_.userText = userText;
    const auto t0 = std::chrono::steady_clock::now();
    run();
    cur_.seconds = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - t0).count();
    cur_.systemPrompt = eng_->lastInstructions();
    cur_.host = lastHost_;
    cur_.tokensIn = eng_->lastTurnUsage().promptTokens;
    cur_.tokensOut = eng_->lastTurnUsage().completionTokens;
    cur_.cacheRead = eng_->lastTurnUsage().cacheReadTokens;
    cur_.cacheWrite = eng_->lastTurnUsage().cacheWriteTokens;
    cur_.ok = !cur_.deliveries.empty();
    if (!cur_.deliveries.empty()) cur_.reply = cur_.deliveries.back();
    turns_.push_back(cur_);
    return cur_;
  }

  // An unattended turn (a routine firing, or the nightly dream stage).
  // loopId "dream" runs it as the reserved dream (memory-only surface).
  orch::FireOutcome scheduled(const std::string& chatId, const std::string& prompt,
                              const std::string& name, bool quietOk = false,
                              const std::string& loopId = "") {
    cur_ = TurnRecord{};
    cur_.chatId = chatId;
    cur_.userText = prompt;
    g_labScheduledTurn = true;
    auto r = eng_->injectScheduledTurn(chatId, prompt, name, loopId, quietOk);
    g_labScheduledTurn = false;
    cur_.host = lastHost_;
    cur_.systemPrompt = eng_->lastInstructions();   // lane J: the block rides the static band
    cur_.tokensIn = r.tokens.promptTokens;
    cur_.tokensOut = r.tokens.completionTokens;
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
  // A row as the device captures it (the device's id shape, the next id): a
  // photo's media row, a replayed message, a /clear boundary.
  struct Row {
    std::string chat;
    const char* role = "user";
    orch::MsgKind kind = orch::MsgKind::Message;
    std::string text, tags, blob;
    uint32_t tsHours = 0;   // 0 = now
  };
  std::string addRow(const Row& r) { return captureRow(r); }
  orch::ToolRegistry& registry() { return reg_; }
  orch::VectorMemory& vectors() { return vec_; }
  orch::Scratchpad& scratchpad() { return scratch_; }
  agent::JobEngine& jobs() { return *jobs_; }
  // Lane C2 (L19-35): give the job engine a journal (no fabric: spawns queue, never
  // dispatch) and rebuild the engines over it, as the device does after a restart.
  void attachJournal(orch::Journal* j) {
    journal_ = j;
    buildEngine();
  }
  // The eval drivers (tests/evals): a sub-agent fabric and a journal for the job
  // engine, with the device's routing reads, and the engines rebuilt over them. A
  // scripted adapter stands in for a provider's sub-agent run; everything after its
  // envelope (the spawn ledger, [FRESH RESULTS], the synthesis turn) is the shipping code.
  void attachSubAgents(agent::HeavyFabric* fabric, orch::Journal* j) {
    fabric_ = fabric;
    journal_ = j;
    buildEngine();
  }
  const orch::MemOpCounters& memOps() const { return memOps_; }
  orch::EpisodicStore& episodic() { return *epi_; }
  // The settings-storage state the device.status stand-in reports (L19-9),
  // through the same portable builder the device uses. Fresh by default; a
  // scenario sets the shape it needs (for example the NVSFILL-0 shape).
  nimbus::orch::StorageState& storageState() { return deviceStorage_; }
  CurlHttpTransport& http() { return http_; }
  // The top of the transport chain: what every lab request goes through.
  agent::HttpTransport& net() { return wire_; }
  const agent::FaultPlan& faults() const { return faultPlan_; }
  const RecordingTransport& recorder() const { return rec_; }
  LabFiles& files() { return files_; }
  LabSkills& skills() { return skills_; }   // seed + inspect the store (lane I)
  LabPrefs& prefs() { return prefs_; }   // lane J: the standing-rules store
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
  const Env& env() const { return env_; }

  // The model a host's head and subs request: an override, else the lab default.
  std::string modelFor(const std::string& h) const {
    auto it = opt_.models.find(h);
    if (it != opt_.models.end()) return it->second;
    if (opt_.roleDefaults) {
      const bool bg = nimbus::orch::currentTurnRole() == nimbus::orch::TurnRole::Background;
      const std::string m = roleDefault(h, bg ? nimbus::orch::DefaultRole::Background
                                              : nimbus::orch::DefaultRole::Head);
      if (!m.empty()) return m;
    }
    return labDefaultModel(h);
  }
  // The sub-agent model: a pin, else (--defaults) the device's sub row, else modelFor.
  std::string subModelFor(const std::string& h) const {
    if (opt_.roleDefaults && !opt_.models.count(h)) {
      const std::string m = roleDefault(h, nimbus::orch::DefaultRole::Sub);
      if (!m.empty()) return m;
    }
    return modelFor(h);
  }
  static std::string roleDefault(const std::string& h, nimbus::orch::DefaultRole r) {
    return nimbus::orch::roleDefaultModel(h.c_str(), r, nimbus::orch::staticModelChoices(h.c_str()),
                                          false);
  }

  // One line that makes a run self-documenting: what it actually ran with.
  std::string banner() const;

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
  // One metered ledger row (BudgetConfig::recordTokens), in ledger units.
  struct Spend { std::string host; uint32_t in, out, cr, cw; std::string tag, model; };
  const std::vector<Spend>& spendRows() const { return spend_; }
  // Every harness log line from this run - scenarios assert on these.
  static const std::vector<std::string>& harnessLog() { return logLines(); }
  static bool loggedContaining(const char* needle) {
    for (const auto& l : logLines())
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }

 private:
  // ---- memory + tools -------------------------------------------------------
  static uint32_t nowHoursLab() {
    return (uint32_t)(std::chrono::duration_cast<std::chrono::hours>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
  }

  void buildMemory() {
    vec_.configure(opt_.embedDims);
    vec_.setMaxEntries(2000);
    epi_.reset(new orch::InMemoryEpisodicStore());

    orch::MemoryContext mc;
    mc.vec = &vec_;
    mc.scratch = &scratch_;
    mc.cfg = &memCfg_;
    mc.episodic = epi_.get();
    mc.ops = &memOps_;   // what memory writes did (the dream's honest outcome)
    mc.nowHours = [] { return nowHoursLab(); };
    // CUM-435: the Embedder seam now threads the failure cause; the rig logs its
    // own embed errors to stderr, so clear err here (empty vector still signals
    // "unavailable" to the memory tools, which then degrade / refuse honestly).
    mc.embed = [this](const std::string& text, std::string& err) { err.clear(); return embed(text); };
    mc.persistScratch = [this] { return saveScratch(); };
    prefs_.bindStanding(mc);   // lane J: mem_write standing:true -> the rules
    orch::registerMemoryTools(reg_, mc);
    prefs_.registerTools(reg_);   // lane J: prefs.edit, registered as the device does
    orch::registerFoodTools(reg_);   // lane CTX: food.portions, as the device registers it

    // Split by tool group so each registration (and its lambda) stays inside the
    // complexity gate. Registration order is preserved (it drives toolNames_).
    registerWebSearchTool();
    registerDocsTools();
    registerDeviceStatusTool();
    files_.registerTools(reg_);
    skills_.registerTools(reg_);   // the device's four skill tools over a store (lane I)

    for (const auto& s : reg_.toolSpecs()) toolNames_.push_back(s.name);
  }

  // web.search - the real thing, through the real portable registration the device
  // uses (agent::websearch::registerWebSearchTool + the live key gate, CUM-508).
  // Without a key it is registered but not advertised, exactly as on a device
  // with no Tavily key.
  void registerWebSearchTool() {
    tavilyKey_ = env_.get("TAVILY_API_KEY");
    agent::websearch::WebSearchToolDeps wd;
    wd.search = [this](const std::string& q, int k) {
      return agent::websearch::search(net(), tavilyKey_, q, k);
    };
    agent::websearch::registerWebSearchTool(reg_, wd);
    orch::applyToolGates(reg_, [this](orch::ToolGate g) {
      if (g == orch::ToolGate::WebSearchKey) return !tavilyKey_.empty();
      return !env_.providerKey("openai").empty();
    });
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
  // on" have something honest to call. deviceStorage_ carries the settings-storage
  // state the same block the device reports (L19-9), built by the same portable
  // builder (storageState() is the public setter surface).
  nimbus::orch::StorageState deviceStorage_;
  void registerDeviceStatusTool() {
    // The stand-in carries the same contract the device's device.status carries:
    // the model reads these sentences to know what the block means, so the lab
    // description mirrors the device's (main.cpp) wording for the state it
    // reports, not a shorter one.
    reg_.add(
        "device.status",
        "Read the COMPLETE live device state - call this before answering any question "
        "about the device's behavior, current settings, capacity, versions or the "
        "time. Includes nvs = settings-storage health (free bytes, saves that failed "
        "since boot, the last key that failed - never its value) and tgOffsetSaved "
        "(false means the device owes a Telegram offset write, so messages can "
        "arrive again - a device-side repeat, not the owner's app).",
        [this](ArduinoJson::JsonObjectConst, const orch::Principal&) {
          return orch::ToolResult::ok(
              std::string("{\"name\":\"") + opt_.devName +
              "\",\"mode\":\"orchestrator\",\"host\":\"harness-lab\",\"vectors\":" +
              std::to_string(vec_.size()) + "," +
              nimbus::orch::storageStateJson(deviceStorage_) + "}");
        },
        R"({"type":"object","properties":{}})");
  }

  // Real embeddings through the same portable request/response codec the device
  // uses - so recall in the lab is genuine associative recall, not a fake.
  std::vector<int8_t> embed(const std::string& text) {
    if (!opt_.embeddings) return {};
    const std::string key = env_.providerKey(opt_.embedHost);
    if (key.empty()) return {};

    // One provider decision drives both the host and the body dialect (CUM-469:
    // the portable builder is provider-aware, so mistral never gets OpenAI fields).
    const std::string prov = opt_.embedHost == "mistral" ? "mistral" : "openai";
    agent::HttpRequest req;
    req.method = "POST";
    req.host = prov == "mistral" ? "api.mistral.ai" : "api.openai.com";
    req.path = "/v1/embeddings";
    req.timeoutMs = 30000;
    req.headers.push_back({"Content-Type", "application/json"});
    req.headers.push_back({"Authorization", "Bearer " + key});
    req.body = orch::buildEmbeddingRequest(prov, opt_.embedModel, text, opt_.embedDims);

    agent::HttpResponse resp;
    std::string err;
    if (!net().exec(req, resp, err) || resp.status < 200 || resp.status >= 300) {
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
    p.subModel = [this](const std::string& h) { return subModelFor(h); };
    // The device's own cold-start lists (one source: model_choices.h).
    p.modelChoices = [](const std::string& h) {
      return std::string(nimbus::orch::staticModelChoices(h.c_str()));
    };
    p.convId = [this] { return convId_; };
    p.setConvId = [this](const std::string& v) { convId_ = v; };
    p.customBase = [] { return std::string(); };
    p.customKey = [] { return std::string(); };
    p.customConv = [] { return std::string("openai"); };
    p.customModel = [] { return std::string(); };

    c.loop.toolLoopOn = [this] { return opt_.toolLoop; };
    c.loop.rounds = [this] { return opt_.rounds; };
    c.loop.midTurnFailover = [this] { return opt_.midTurnFailover; };
    c.loop.deadlineS = [] { return 600; };
    c.loop.resultCap = [] { return 4096; };
    c.loop.totalCap = [] { return 24576; };

    c.budget.overBudget = [](const std::string&) { return false; };
    // Every row the engine meters (once per host and model that billed, per turn),
    // in ledger units: the metering scenarios compare them with provider usage.
    c.budget.recordTokens = [this](const std::string& h, uint32_t in, uint32_t out,
                                   uint32_t cr, uint32_t cw, const std::string& tag,
                                   const std::string& model) {
      spend_.push_back({h, in, out, cr, cw, tag, model});
    };
    c.ttsEnabled = [] { return false; };
    c.deviceName = [this] { return opt_.devName; };
    return c;
  }

  agent::providers::ProviderDeps providerDeps() {
    agent::providers::ProviderDeps pd;
    pd.http = &wire_;
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

  // The Z.ai head: the custom/OpenAI-compatible single-shot turn pointed at
  // Z.ai, with the same head policy the device and nimbusd apply.
  agent::providers::ProviderDeps zaiProviderDeps() {
    auto pd = providerDeps();
    pd.customBase = [] { return std::string(kLabZaiHost); };
    pd.customPathPrefix = [] { return std::string(kLabZaiPath); };
    pd.customKey = [this] { return env_.providerKey("zai"); };
    pd.customConv = [] { return std::string("openai"); };
    pd.customModel = [this] { return modelFor("zai"); };
    agent::providers::applyZaiHeadPolicy(pd);
    return pd;
  }

  // Lane I: the [SKILLS] index + the per-chat attach surface, wired as the
  // device does (orchestrator.cpp). skillsIndex feeds the STABLE catalogs; the
  // attach block rides the per-turn INPUT.
  void wireSkillsDeps(agent::TurnEngine::Deps& d) {
    d.skillsIndex = [this] { return skills_.indexText(); };
    d.skillsPicks = [this](const std::string& chat) { return skills_.picksFor(chat); };
    d.skillBody = [this](const std::string& id) { return skills_.attachBody(id); };
    d.skillUsed = [this](const std::string& chat, const std::string& id) {
      skills_.noteUsed(chat, id);
    };
  }

  // The device's host adapters (fabric failover, Z.ai single-shot, the three
  // loop hosts), split out of buildEngine to stay under the complexity gate.
  void wireHosts(agent::TurnEngine::Deps& d) {
    // The device's mid-turn failover: LOOP turns run the engine-owned fabric over
    // the same deps (a failed round re-runs on the next host against the shared
    // transcript). Z.ai is single-shot only, as on the device.
    d.hosts.fabric = [this](const std::vector<std::string>& hostList, const std::string& ins,
                            const std::string& inp, std::string& out, std::string& err,
                            const agent::HeadTools& ht, orch::TurnSpend* spend,
                            const std::function<void(const std::string&, const std::string&)>& notify) {
      return agent::providers::runFabricLoop(providerDeps(), hostList, ins, inp, out, err, ht,
                                             spend, notify);
    };
    d.hosts.fabricSupports = [](const std::string& h) { return agent::providers::fabricSupports(h); };
    if (!env_.providerKey("zai").empty()) {
      d.hosts.add("zai", [this](std::string& conv, const std::string& ins, const std::string& inp,
                                std::string& out, std::string& err, const agent::HeadTools* tools,
                                orch::TokenUsage* usage) -> bool {
        return agent::providers::orchTurnCustom(zaiProviderDeps(), conv, ins, inp, out, err, tools,
                                                usage);
      });
    }
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
  }

  // The journal (attachJournal) and, with attachSubAgents, the fabric and the routing reads a
  // spawn needs, as the device wires them.
  void wireJobs(agent::JobEngine::Deps& jd) {
    jd.journal = journal_;
    if (!fabric_) return;
    jd.fabric = fabric_;
    jd.subPriority = [this] { return opt_.priority; };
    jd.providerHasKey = [this](const std::string& p) { return !env_.providerKey(p).empty(); };
    jd.subModel = [this](const std::string& p) { return subModelFor(p); };
    jd.modelChoices = [](const std::string& p) {
      return std::string(nimbus::orch::staticModelChoices(p.c_str()));
    };
    jd.modelIsValid = [](const std::string& p, const std::string& m) {
      const std::string list = std::string(",") + nimbus::orch::staticModelChoices(p.c_str()) + ",";
      return !m.empty() && list.find("," + m + ",") != std::string::npos;
    };
  }

  void buildEngine() {
    agent::JobEngine::Deps jd;
    jd.platform = platform();
    jd.deliver = [this](const std::string& c, const std::string& t) { record(c, t); };
    wireJobs(jd);
    jobs_.reset(new agent::JobEngine(std::move(jd)));

    agent::TurnEngine::Deps d;
    d.cfg = config();
    d.platform = platform();
    d.jobs = jobs_.get();
    d.deliver = [this](const std::string& c, const std::string& t) { record(c, t); };
    d.recall = [this](const std::string& q, const orch::Principal&) {
      orch::RecallOut out;
      if (!opt_.embeddings || vec_.size() == 0) return out;
      auto v = embed(q);
      // An embed failure never reads as "nothing relevant" (device parity): the
      // keyword scan answers, and the prompt says recall is keyword-only.
      if (v.empty()) return orch::lexicalRecall(vec_, nullptr, q, orch::RecallScope{{}, nowHoursLab(), 5}, "unavailable");
      for (const auto& hit : vec_.search(v, 5)) out.hits.push_back(hit.content);
      return out;
    };
    d.composeInputs = [this](const std::string& chat) {
      agent::ComposeInputs in;
      in.devName = opt_.devName;
      // The device names its configured head (compose.h hostForPrompt), so the
      // identity line is the same on every turn and the static block caches.
      in.hostLabel = agent::hostForPrompt(config().provider);
      in.recentConversation = recentWindow(chat);
      in.runningMemory = memory_;
      in.scratchpad = &scratch_;
      in.standingPrefs = prefs_.blockFor(chat);   // lane J: [STANDING RULES]
      return in;
    };
    d.toolSpecs = [this](const orch::Principal& who) { return reg_.toolSpecsFor(who); };
    d.mcpDispatch = [this](const std::string& req, const orch::Principal& who) {
      return reg_.handleRpc(req, who);
    };
    d.connectorsCatalog = [](bool) { return std::string(); };
    wireSkillsDeps(d);
    d.modelChoices = [this](const std::string& p) { return config().provider.modelChoices(p); };
    d.episodicCaptureUser = [this](const std::string& c, const std::string& t,
                                   const std::string& tag) {
      capture(c, "user", t, tag);
    };
    d.firstAllowedChat = [] { return std::string("owner"); };
    d.journalGc = [] {};
    wireHistory(d);
    // Lane CTX: today's food ledger over the RAM files, as the device reads its card
    // (the lab is single-owner, so every chat reads the owner's file).
    d.foodLedger = [this](const std::string&) {
      const std::string where = std::string(orch::kFoodLedgerProject) + "/" + orch::kFoodLedgerOwnerName;
      return orch::scanFoodLedger(files_.get(where), files_.has(where), orch::logDayOf(std::time(nullptr), true),
                                  orch::kFoodLedgerProject, orch::kFoodLedgerOwnerName);
    };

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
    d.hooks.onTurnDebug = [this](const agent::TurnDebugEv& ev) {
      if (ev.inputs) cur_.turnInputs = *ev.inputs;   // where [ACTIVE SKILLS] and [STANDING RULES] ride
    };

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
    // The orch_turn mem_write[] / mem_query[] fields dispatch through the same
    // registry as the mid-turn tools, as on the device (a single-shot head, Z.ai,
    // can store memories only this way) - wired inside wireTurnFields.
    wireTurnFields(d.apply);

    wireHosts(d);
    eng_.reset(new agent::TurnEngine(std::move(d)));
  }

  // The turn's own fields run as on the device: mem_write items through the
  // memory registry, the inline scratchpad (saved, or RAM only), spawn items
  // into the job queue (accepted or refused).
  void wireTurnFields(agent::ApplyDeps& a) {
    a.withMemoryLock = [](const std::function<void()>& fn) { fn(); };
    a.memDispatch = [this](const char* name, ArduinoJson::JsonObjectConst args,
                           const orch::Principal& who) { return reg_.dispatch(name, args, who); };
    a.persistMemory = [] {};
    a.applyScratch = [this](const orch::ScratchUpdate& u) {
      const std::string before = scratch_.serialize();
      if (u.hasActive) scratch_.setActiveTask(u.active);
      if (u.hasShort)  scratch_.replace(orch::Tier::Short, u.shortItems);
      if (u.hasMid)    scratch_.replace(orch::Tier::Mid, u.midItems);
      if (u.hasLong)   scratch_.replace(orch::Tier::Long, u.longItems);
      if (scratch_.serialize() == before) return agent::ScratchOutcome::Skipped;   // as the device
      return saveScratch() ? agent::ScratchOutcome::Applied : agent::ScratchOutcome::AppliedRamOnly;
    };
    a.enqueueSpawn = [this](const orch::Spawn& s, const std::string& chat, bool quiet) {
      return jobs_->enqueueSpawn(s, chat, quiet);
    };
  }

  // The lab's scratchpad "save": nothing to write, but it reports what the
  // device would (and keeps the live header in step).
  bool saveScratch() {
    scratch_.setSaveFailing(opt_.scratchSaveFails);
    return !opt_.scratchSaveFails;
  }

  // The device's history seams over the lab's store (engine.h, histV2).
  void wireHistory(agent::TurnEngine::Deps& d) {
    d.histV2 = [this] { return opt_.histV2; };
    d.historyRows = [this](const std::string& chat, size_t limit, const std::string& before) {
      orch::MsgQuery q;
      q.sessionId = chat;
      q.haveKind = true;
      q.kind = orch::MsgKind::Message;
      q.alsoKinds = {orch::MsgKind::Image, orch::MsgKind::File, orch::MsgKind::Audio};
      q.limit = (int)limit;
      q.before = before;
      return epi_->query(q);
    };
    d.captureUserRow = [this](const std::string& c, const std::string& t, const std::string& tags) {
      return turnTag_ = capture(c, "user", t, tags);
    };
    d.historyMarks = [this](const std::string& c) { return marks_[c]; };
    d.noteHistoryShift = [this](const std::string& c, const std::string& s) { marks_[c].softStart = s; };
    if (!opt_.histV2) return;
    // As on the device: the assistant rows come from apply (with their fx: and
    // out: tags and the turn id), not from every delivery, and a seedless turn
    // gets a trigger row from a minted turn id.
    d.hooks.onTurnStart = [this](const agent::TurnStartEv& ev) {
      if (ev.source == agent::TurnSource::Owner || ev.source == agent::TurnSource::Serial) return;
      turnTag_ = mintRowId();
    };
    d.apply.captureAssistant = [this](const std::string& c, const std::string& t) {
      capture(c, "assistant", t, eng_->assistantRowTags() + ",turn:" + turnTag_);
    };
    d.captureTrigger = [this](const std::string& c, const std::string& t, const std::string& tags) {
      capture(c, "system", t, tags + ",turn:" + turnTag_);
    };
  }

  // ---- episodic helpers -----------------------------------------------------
  std::string capture(const std::string& chat, const char* role, const std::string& text,
                      const std::string& tags) {
    Row r;
    r.chat = chat;
    r.role = role;
    r.text = text;
    r.tags = tags;
    return captureRow(r);
  }
  // The next row id, in the device's shape.
  std::string mintRowId() {
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++rowSeq_));
    return id;
  }
  std::string captureRow(const Row& r) {
    orch::EpisodicMessage m;
    m.id = mintRowId();
    m.sessionId = r.chat;
    m.kind = r.kind;
    m.blobPath = r.blob;
    m.role = r.role;
    m.text = r.text;
    m.tags = r.tags;
    m.tsHours = r.tsHours ? r.tsHours
                          : (uint32_t)std::chrono::duration_cast<std::chrono::hours>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
    epi_->addMessage(m);
    return m.id;
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
    if (!opt_.histV2) capture(chat, "assistant", text, "");   // histV2: apply captures the reply
  }

  static std::string trunc(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
  }

  Env  env_;
  Options opt_;
  // The transport chain, declared in construction order (each wraps the one above).
  CurlHttpTransport http_;
  agent::FaultPlan faultPlan_;
  RecordingTransport rec_;
  agent::FaultInjectingTransport fault_;
  WireOverrideTransport wire_;

  orch::VectorMemory vec_;
  orch::Scratchpad   scratch_;
  orch::MemOpCounters memOps_;
  orch::MemConfig    memCfg_;
  std::unique_ptr<orch::EpisodicStore> epi_;
  orch::ToolRegistry reg_;
  LabFiles           files_;
  LabSkills          skills_;   // the lab's SkillStore + attach table (lane I)
  LabPrefs           prefs_;   // lane J: the standing-rules store + tools
  std::vector<std::string> toolNames_;
  std::string tavilyKey_;

  std::unique_ptr<agent::JobEngine>  jobs_;
  orch::Journal*                     journal_ = nullptr;   // attachJournal (C2)
  agent::HeavyFabric*                fabric_ = nullptr;    // attachSubAgents (eval drivers)
  std::unique_ptr<agent::TurnEngine> eng_;

  std::string convId_, antEnv_, antAgents_, memory_, lastHost_;
  uint32_t rowSeq_ = 0;
  std::string turnTag_;   // the in-flight turn's id (assistant rows' turn: tag)
  std::map<std::string, agent::HistoryMarks> marks_;
  std::vector<int> deviceActions_;
  std::vector<Spend> spend_;

  mutable uint32_t heapReads_ = 0;

  TurnRecord cur_;
  std::vector<TurnRecord> turns_;
};

// The run banner: what a live run actually used (models, pins, faults).
inline std::string labBanner(const Env& env, const LabRig::Options& opt) {
  std::string m;
  for (const char* h : {"anthropic", "mistral", "zai", "openai"}) {
    if (env.providerKey(h).empty()) continue;
    auto it = opt.models.find(h);
    const std::string dflt = opt.roleDefaults ? LabRig::roleDefault(h, nimbus::orch::DefaultRole::Head)
                                              : labDefaultModel(h);
    m += std::string(m.empty() ? "" : " ") + h + "=" + (it != opt.models.end() ? it->second : dflt);
  }
  const auto num = [](uint32_t v) { return v ? std::to_string(v) : std::string("adapter"); };
  return "lab: priority=" + opt.priority + " models[" + m + "] effort=" +
         (opt.pins.effort.empty() ? std::string("adapter") : opt.pins.effort) +
         " maxtok=" + num(opt.pins.maxTokens) + " ceiling_ms=" + num(opt.pins.ceilingMs) +
         " rounds=" + std::to_string(opt.rounds) + " failover=" + (opt.midTurnFailover ? "on" : "off") +
         " faults=" + std::to_string(opt.faults.size()) +
         " record=" + (opt.recordDir.empty() ? std::string("off") : opt.recordDir) +
         (opt.roleDefaults ? " defaults=device-roles" : "");
}

inline std::string LabRig::banner() const { return labBanner(env_, opt_); }

}  // namespace lab
