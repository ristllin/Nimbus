#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "daemon_config.h"
#include "daemon_http.h"
#include "nimbus/docs_pack.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/websearch.h"
#include "nimbus/orch/embedding.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_memory.h"
#include "posix_files.h"
#include "posix_fs.h"
#include "posix_platform.h"

// NimbusdRig - a whole Nimbus orchestrator as a hosted daemon.
//
// The SAME composition harness-lab's LabRig builds (the device's TurnEngine,
// tool registry, memory engines and provider adapters, above one HttpTransport
// seam), but wired to DURABLE POSIX stores instead of RAM: the episodic
// append-log, vector memory, scratchpad, memory config and file artifacts all
// live under a data directory and are rehydrated on construction, so the assistant's
// memory survives a process restart. Everything above the transport seam is
// byte-for-byte the code the device runs.
//
// It exposes the same surface the scenario suite drives (say / scheduled /
// callTool / vectors / episodic / files / turns), so the harness-lab scenarios
// run against the daemon composition unchanged, plus flush() + a restart-safe
// data layout the lab (in-memory) cannot have.
namespace nimbusd {

namespace orch = nimbus::orch;

// The Cumulo router seam the DEVICE consumes (src/agent/orchestrator.cpp cumulo
// head, over agent_config.h's CUMULO_HOST_DEFAULT / CUMULO_MODEL): a keyed
// instance runs the WHOLE assistant over the fixed router base + path prefix and
// the router key - the flagship "one key, one balance" path. Mirrored here (the
// daemon cannot include the firmware header) so a keyed Virtual Nimbus routes
// identically to a keyed device (CUM-286). Values are the router's external
// contract; the device's agent_config.h stays the source of truth.
constexpr const char* kCumuloEnvKey     = "CUMULO_API_KEY";      // canonical env name
constexpr const char* kCumuloHost       = "app.cumulo-nimbus.ai";  // CUMULO_HOST_DEFAULT
constexpr const char* kCumuloPathPrefix = "/router/openai/v1";     // replaces the default /v1
constexpr const char* kCumuloConv       = "openai";                // wire convention
constexpr const char* kCumuloModel      = "gpt-4o";                // CUMULO_MODEL default
constexpr const char* kCumuloSlug       = "cumulo";               // head + routing slug

struct TurnRecord {
  std::string chatId, userText, reply;
  std::vector<std::string> toolCalls;
  std::vector<std::string> toolResults;
  std::vector<std::string> deliveries;
  std::string host;
  uint32_t    tokensIn = 0, tokensOut = 0;
  double      seconds = 0;
  bool        ok = false;
};

class NimbusdRig {
 public:
  struct Options {
    std::string dataDir = "/data";               // durable store root
    std::string priority = "mistral,openai,anthropic";
    std::string devName = "Nimbus";
    std::string role = "admin";                   // hosted instance is single-owner
    bool        toolLoop = true;
    bool        embeddings = true;
    bool        verboseHttp = false;
    std::string embedModel = "mistral-embed";
    std::string embedHost = "mistral";
    int         embedDims = 1024;
    int         maxVectors = 5000;
    std::map<std::string, std::string> models;
  };

  // `httpOverride` lets a host test inject a FakeHttpTransport in place of the
  // libcurl transport, so the router wire (base/path/bearer) can be asserted
  // offline. Production passes nullptr and gets the owned DaemonHttpTransport.
  NimbusdRig(Config cfg, Options opt, agent::HttpTransport* httpOverride = nullptr)
      : cfg_(std::move(cfg)), opt_(std::move(opt)) {
    if (httpOverride) {
      http_ = httpOverride;
    } else {
      ownedHttp_.reset(new DaemonHttpTransport());
      http_ = ownedHttp_.get();
    }
    installLog(opt_.verboseHttp);
    fsutil::mkdirs(memDir());
    buildMemory();
    loadSecrets();   // in-app provider keys persisted from a prior session (CUM-279)
    buildEngine();
  }

  ~NimbusdRig() { flush(); }

  // ---- durable data layout --------------------------------------------------
  std::string memDir() const { return opt_.dataDir + "/mem"; }
  std::string vectorsPath() const { return memDir() + "/vectors.bin"; }
  std::string scratchPath() const { return memDir() + "/scratchpad.txt"; }
  std::string memConfigPath() const { return memDir() + "/memconfig.txt"; }
  std::string episodicDir() const { return memDir() + "/episodic"; }
  std::string filesDir() const { return memDir() + "/files"; }
  // In-app provider keys (CUM-279): durable, owner-only. The instance disk plays the
  // device's NVS role, so a key set in the UI survives a process/pod restart.
  std::string secretsPath() const { return memDir() + "/secrets.env"; }

  // Persist every whole-file store with the atomic tmp->rename writer. The
  // episodic append-log is already durable per-message; this flushes the RAM
  // engines (vectors, scratchpad, memconfig). Called after each turn and on exit.
  void flush() {
    fsutil::writeFileAtomic(vectorsPath(), vec_.serialize());
    fsutil::writeFileAtomic(scratchPath(), scratch_.serialize());
    fsutil::writeFileAtomic(memConfigPath(), memCfg_.serialize());
  }

  // ---- the surface a scenario drives ---------------------------------------
  TurnRecord say(const std::string& chatId, const std::string& text) {
    cur_ = TurnRecord{};
    cur_.chatId = chatId;
    cur_.userText = text;
    const auto t0 = std::chrono::steady_clock::now();
    eng_->handleMessage(text, "Owner", chatId);
    cur_.seconds = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - t0).count();
    cur_.tokensIn = eng_->lastTurnUsage().promptTokens;
    cur_.tokensOut = eng_->lastTurnUsage().completionTokens;
    cur_.ok = !cur_.deliveries.empty();
    if (!cur_.deliveries.empty()) cur_.reply = cur_.deliveries.back();
    turns_.push_back(cur_);
    flush();
    return cur_;
  }

  orch::FireOutcome scheduled(const std::string& chatId, const std::string& prompt,
                              const std::string& name, bool quietOk = false) {
    cur_ = TurnRecord{};
    cur_.chatId = chatId;
    cur_.userText = prompt;
    auto r = eng_->injectScheduledTurn(chatId, prompt, name, "", quietOk);
    if (!cur_.deliveries.empty()) cur_.reply = cur_.deliveries.back();
    cur_.ok = r.ok;
    turns_.push_back(cur_);
    flush();
    return r;
  }

  std::string callTool(const std::string& name, const std::string& argsJson) {
    std::string rpc = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" +
                      name + R"(","arguments":)" + argsJson + "}}";
    std::string out = reg_.handleRpc(rpc, principal());
    flush();
    return out;
  }

  // Outbound delivery hook: called (on the engine thread, inside a turn) with
  // every reply the engine produces, so the daemon can forward it to Telegram.
  // Set once at wiring time; the rig still records deliveries for scenarios.
  void setDeliver(std::function<void(const std::string&, const std::string&)> fn) {
    onDeliver_ = std::move(fn);
  }

  agent::TurnEngine& engine() { return *eng_; }
  orch::ToolRegistry& registry() { return reg_; }
  orch::VectorMemory& vectors() { return vec_; }
  orch::Scratchpad& scratchpad() { return scratch_; }
  orch::MemConfig& memConfig() { return memCfg_; }
  orch::EpisodicStore& episodic() { return *epi_; }
  const Config& cfg() const { return cfg_; }
  // The instance's honest memory picture (the container's free/cap heap), for the
  // web app's "Free RAM" tile - a real hosted-process figure, never faked HW SRAM.
  uint32_t freeHeapBytes() const { return mem_.freeBytes(); }
  uint32_t heapCapBytes() const { return mem_.capBytes(); }
  bool embeddingsOn() const { return opt_.embeddings; }
  // Persist the whole-file stores after a web mutation (config/vector edit). Same
  // atomic writer flush() uses; safe to call on the engine thread.
  void persist() { flush(); }
  agent::HttpTransport& http() { return *http_; }
  PosixFiles& files() { return *files_; }
  const std::vector<TurnRecord>& turns() const { return turns_; }

  uint32_t totalIn() const { uint32_t n = 0; for (auto& t : turns_) n += t.tokensIn; return n; }
  uint32_t totalOut() const { uint32_t n = 0; for (auto& t : turns_) n += t.tokensOut; return n; }

  bool hostAvailable(const std::string& h) const { return !cfg_.providerKey(h).empty(); }

  // The Cumulo router key from the canonical env (CUM-286). This is the daemon's
  // equivalent of the device's store::cumuloKey() - the material the "cumulo"
  // head places in its bearer, and the source that makes the router the fallback
  // head when no direct provider key is set.
  std::string cumuloKey() const { return cfg_.get(kCumuloEnvKey); }
  bool hasCumulo() const { return !cumuloKey().empty(); }

  // The model the cumulo head requests: an operator override (opt.models
  // ["cumulo"]) or the router default. Mirrors the device's orchModel("cumulo").
  std::string cumuloModel() const {
    auto it = opt_.models.find(kCumuloSlug);
    return (it != opt_.models.end() && !it->second.empty()) ? it->second
                                                            : std::string(kCumuloModel);
  }

  // True iff at least one chat provider is configured - a direct BYOK key OR the
  // Cumulo router key. Drives the web chat page's honest "no provider key
  // configured" state (CUM-211/CUM-286): a keyless instance produces no reply,
  // so the surface must say so, but a Cumulo-only instance DOES reply (via the
  // router head) and must read healthy, never degraded.
  bool anyProviderConfigured() const {
    if (hasCumulo()) return true;
    for (const char* h : {"openai", "anthropic", "mistral"})
      if (!cfg_.providerKey(h).empty()) return true;
    return false;
  }

  // ---- in-app provider keys (CUM-279, device parity) ------------------------
  // Canonical env name for a provider slug - delegates to the single source in
  // Config (daemon_config.h). Empty for an unknown slug (the caller rejects it).
  static std::string keyEnvFor(const std::string& host) {
    return Config::providerEnvName(host);
  }
  // Map a web /api/orch key FIELD (the daemon emits `<slug>Key`) back to its slug: a
  // plain suffix strip, valid iff the slug is one keyEnvFor knows - so a new provider
  // added to the single env map works here with no edit.
  static std::string hostForKeyField(const std::string& field) {
    const size_t n = field.size();
    if (n <= 3 || field.compare(n - 3, 3, "Key") != 0) return std::string();
    const std::string slug = field.substr(0, n - 3);
    return keyEnvFor(slug).empty() ? std::string() : slug;
  }

  // Set (or clear, when `key` is empty) a provider key from the running UI and make
  // it take effect. Device parity (owner ruling, CUM-279): the key you set in the UI
  // is the one the instance uses. Persisted to a durable, owner-only secrets file
  // (survives a pod restart) and applied WITHOUT a restart by rebuilding the engine
  // in place - a head for a newly keyed provider is registered at build time, so a
  // rebuild is how it takes effect. MUST run on the engine thread (serialized, never
  // mid-turn - the web layer dispatches it there and refuses mid-turn with a 503).
  bool applyProviderKey(const std::string& host, const std::string& key) {
    const std::string env = keyEnvFor(host);
    if (env.empty()) return false;
    cfg_.setOverride(env, key);   // authoritative; an empty key erases the override
    saveSecrets();
    buildEngine();                // re-register heads for the new key set
    ++keyGen_;
    return true;
  }
  // Monotonic key-change counter, surfaced as the providers' verify timestamp so the
  // web save-flow's poll observes the change (a hosted instance has no cheap verify).
  uint32_t keyGen() const { return keyGen_; }

  // Served-by of the most recent turn (CUM-236), for the web chat's honest footer.
  bool lastFallback() const { return lastFallback_; }
  std::string lastServedBy() const { return lastServedBy_; }

  const Options& options() const { return opt_; }

  // The model a head requests: an operator override (opt.models[h]) else the
  // provider default. Wired into the provider deps as orchModel/subModel, so the
  // engine's budget derivation reads through here for the resolved head. Public so
  // the one-key path test can assert the router head resolves a model (CUM-288).
  std::string modelFor(const std::string& h) const {
    auto it = opt_.models.find(h);
    if (it != opt_.models.end()) return it->second;
    if (h == "openai")    return "gpt-5.5";
    if (h == "anthropic") return "claude-sonnet-4-6";
    if (h == "mistral")   return "mistral-large-latest";
    // CUM-288: the Cumulo router head resolves to the router default model, not ""
    // - an empty model made budgeting fall to the conservative default window and
    // (with the fold path fixed) the fold request would carry no model at all.
    if (h == kCumuloSlug) return std::string(kCumuloModel);
    return std::string();
  }

  orch::Role role() const {
    orch::Role r = orch::Role::Admin;
    orch::roleFromName(opt_.role, r);
    return r;
  }
  static bool validRole(const std::string& s) {
    orch::Role r = orch::Role::Admin;
    return orch::roleFromName(s, r);
  }
  orch::Principal principal() const { return orch::principalForRole("owner", role()); }

  // ---- harness log capture (the degraded scenario asserts on these) --------
  static const std::vector<std::string>& harnessLog() { return logLines(); }
  static bool loggedContaining(const char* needle) {
    for (const auto& l : logLines())
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }

 private:
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

  static uint32_t nowHours() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::hours>(
               std::chrono::system_clock::now().time_since_epoch()).count();
  }

  // ---- in-app provider keys: durable secrets (CUM-279) ----------------------
  // Load previously-set in-app keys as config OVERRIDES (authoritative), so a key
  // the owner set in the UI persists across restarts. Tolerant of an absent/torn
  // file (KEY=VALUE lines, trailing CR/LF trimmed). Never logs a value.
  void loadSecrets() {
    std::string blob;
    if (!fsutil::readFile(secretsPath(), blob)) return;
    std::istringstream is(blob);
    std::string line;
    while (std::getline(is, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
      if (!k.empty() && !v.empty()) cfg_.setOverride(k, v);
    }
  }
  // Persist the current in-app overrides to the owner-only secrets file. Written
  // 0600 from creation (never a world-readable window - the general atomic writer
  // creates 0644 then chmods, which leaves a race and a 0644 file if the process
  // dies between rename and chmod), then atomically renamed into place. Never logs a
  // value.
  void saveSecrets() {
    std::string blob;
    for (const auto& kv : cfg_.overrides()) blob += kv.first + "=" + kv.second + "\n";
    const std::string tmp = secretsPath() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return;
    const ssize_t n = ::write(fd, blob.data(), blob.size());
    ::close(fd);
    if (n != (ssize_t)blob.size()) { ::unlink(tmp.c_str()); return; }
    if (::rename(tmp.c_str(), secretsPath().c_str()) != 0) ::unlink(tmp.c_str());
  }

  // ---- memory + tools -------------------------------------------------------
  void buildMemory() {
    vec_.configure(opt_.embedDims);
    vec_.setMaxEntries(opt_.maxVectors);
    // Rehydrate the durable stores (each is tolerant of an absent/torn file).
    std::string blob;
    if (fsutil::readFile(vectorsPath(), blob)) vec_.deserialize(blob);
    if (fsutil::readFile(scratchPath(), blob)) scratch_.deserialize(blob);
    if (fsutil::readFile(memConfigPath(), blob)) memCfg_.deserialize(blob);

    epi_.reset(new orch::AppendLogEpisodicStore(epiFs_, episodicDir(), /*recentCap=*/256));
    epi_->hydrate();  // rebuild index/cache from the day-streams

    files_.reset(new PosixFiles(filesDir()));

    orch::MemoryContext mc;
    mc.vec = &vec_;
    mc.scratch = &scratch_;
    mc.cfg = &memCfg_;
    mc.episodic = epi_.get();
    mc.nowHours = [] { return nowHours(); };
    mc.embed = [this](const std::string& text) { return embed(text); };
    orch::registerMemoryTools(reg_, mc);

    registerWebSearchTool();
    registerDocsTools();
    registerDeviceStatusTool();
    files_->registerTools(reg_);

    for (const auto& s : reg_.toolSpecs()) toolNames_.push_back(s.name);
  }

  void registerWebSearchTool() {
    tavilyKey_ = cfg_.get("TAVILY_API_KEY");
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
               auto r = agent::websearch::search(*http_, tavilyKey_, q, k);
               if (!r.ok) return orch::ToolResult::fail("web search failed: " + r.err);
               return orch::ToolResult::ok(r.digest);
             },
             R"({"type":"object","properties":{"query":{"type":"string"},)"
             R"("max_results":{"type":"integer"}},"required":["query"]})");
  }

  // The on-device docs pack (docs.search / docs.read) through the real portable
  // retrieval. Unlike the device build there is no maker/user audience split
  // here (DocSection carries no audience flag in the current core), so hosted
  // docs are served straight.
  void registerDocsTools() {
    reg_.add("docs.search",
             "Search your own documentation (ranked keyword match) - use it BEFORE "
             "saying what you can or cannot do.",
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
             "Read ONE section of your documentation by id (from docs.search).",
             [](ArduinoJson::JsonObjectConst a, const orch::Principal&) -> orch::ToolResult {
               std::string id = a["id"].is<const char*>()
                                    ? std::string(a["id"].as<const char*>()) : std::string();
               if (id.empty()) return orch::ToolResult::fail("missing 'id'");
               const nimbus::docs::DocSection* s = nimbus::docs::find(id);
               if (!s) return orch::ToolResult::fail("unknown doc id '" + id + "'");
               std::string out = std::string("## ") + s->title + "\n\n" + s->body;
               return orch::ToolResult::ok(out);
             },
             R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
  }

  void registerDeviceStatusTool() {
    reg_.add("device.status", "Report the instance's current state.",
             [this](ArduinoJson::JsonObjectConst, const orch::Principal&) {
               return orch::ToolResult::ok(
                   std::string("{\"name\":\"") + opt_.devName +
                   "\",\"mode\":\"orchestrator\",\"host\":\"nimbusd\",\"vectors\":" +
                   std::to_string(vec_.size()) + "}");
             },
             R"({"type":"object","properties":{}})");
  }

  std::vector<int8_t> embed(const std::string& text) {
    if (!opt_.embeddings) return {};
    const std::string key = cfg_.providerKey(opt_.embedHost);
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
    if (!http_->exec(req, resp, err) || resp.status < 200 || resp.status >= 300) return {};
    std::vector<float> f;
    if (!orch::parseEmbeddingResponse(resp.body.c_str(), 0, f, err)) return {};
    return orch::VectorMemory::quantize(f);
  }

  // ---- config / provider deps ----------------------------------------------
  agent::HarnessConfig config() {
    agent::HarnessConfig c;
    auto& p = c.provider;
    p.hasKey = [this](const std::string& h) { return !cfg_.providerKey(h).empty(); };
    p.key = [this](const std::string& h) { return cfg_.providerKey(h); };
    // CUM-286 / CUM-242 mirror: with NO direct BYOK head keyed, the verified
    // Cumulo router key is the fallback head that runs the whole assistant - the
    // engine consults this only after every BYOK head in the priority list
    // misses (a keyed BYOK head still wins), exactly as the device does
    // (src/agent/store_config.cpp routerFallbackHost).
    p.routerFallbackHost = [this] {
      return hasCumulo() ? std::string(kCumuloSlug) : std::string();
    };
    // Device-truth "is any provider configured", INCLUDING the router key that
    // hasKey() above does not report (cumulo is not a canonical-env BYOK slot).
    // Keeps the engine's honest "no provider set up" reply from firing on a
    // Cumulo-only instance (CUM-211).
    p.anyKeyed = [this] { return anyProviderConfigured(); };
    p.orchHost = [] { return std::string(); };
    p.providerPriority = [this] { return opt_.priority; };
    p.subPriority = [this] { return opt_.priority; };
    p.orchModel = [this](const std::string& h) { return modelFor(h); };
    p.subModel = [this](const std::string& h) { return modelFor(h); };
    p.modelChoices = [](const std::string& h) {
      if (h == "openai")    return std::string("gpt-6-astra,gpt-5.5,gpt-5.4-mini");
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
    c.budget.recordTokens = [](const std::string&, uint32_t, uint32_t,
                               uint32_t, uint32_t, const std::string&) {};
    c.ttsEnabled = [] { return false; };
    c.deviceName = [this] { return opt_.devName; };
    return c;
  }

  agent::providers::ProviderDeps providerDeps() {
    agent::providers::ProviderDeps pd;
    pd.http = http_;
    pd.key = [this](const char* h) { return cfg_.providerKey(h); };
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
    pd.freeHeap = [this] { return mem_.freeBytes(); };
    return pd;
  }

  // The provider deps for the Cumulo router head: the base ProviderDeps with the
  // custom/proxy fields pointed at the fixed router base + path prefix and the
  // router key. Byte-for-byte the seam the device's cumulo head fills in
  // (src/agent/orchestrator.cpp:1138-1150) - orchTurnCustom over
  // app.cumulo-nimbus.ai + /router/openai/v1, bearer = the Cumulo key.
  agent::providers::ProviderDeps cumuloProviderDeps() {
    auto pd = providerDeps();
    pd.customBase       = [] { return std::string(kCumuloHost); };
    pd.customPathPrefix = [] { return std::string(kCumuloPathPrefix); };
    pd.customKey        = [this] { return cumuloKey(); };
    pd.customConv       = [] { return std::string(kCumuloConv); };
    pd.customModel      = [this] { return cumuloModel(); };
    return pd;
  }

  void buildEngine() {
    agent::JobEngine::Deps jd;
    jd.platform = makePosixPlatform(&mem_);
    jd.deliver = [this](const std::string& c, const std::string& t) { record(c, t); };
    jobs_.reset(new agent::JobEngine(std::move(jd)));

    agent::TurnEngine::Deps d;
    d.cfg = config();
    d.platform = makePosixPlatform(&mem_);
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
                                   const std::string& tag) { capture(c, "user", t, tag); };
    d.firstAllowedChat = [] { return std::string("owner"); };
    d.journalGc = [] {};

    d.hooks.onToolCall = [this](const orch::HeadToolCall& c) {
      cur_.toolCalls.push_back(c.name + "(" + trunc(c.argsJson, 200) + ")");
    };
    d.hooks.onToolResult = [this](const orch::HeadToolResult& r) {
      cur_.toolResults.push_back(trunc(r.output, 300));
    };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) {
      lastHost_ = ev.host;
      // Served-by disclosure (CUM-236): remember whether this turn was served by a
      // fallback provider/model and how to say it, so the web chat can surface it.
      lastFallback_ = ev.fallback;
      lastServedBy_ = ev.host + (ev.servedModel.empty() ? std::string() : (" " + ev.servedModel));
    };

    d.apply.deliver = d.deliver;
    d.apply.stageDevice = [](const orch::ValidatedAction&) {};  // hosted: no device actions
    d.apply.principalFor = [this](const std::string& chat) {
      return orch::principalForRole(chat, role());
    };
    d.apply.setModelMemory = [this](const std::string&, const std::string& m) {
      memory_ = m;
      return true;
    };

    for (const char* h : {"openai", "anthropic", "mistral"}) {
      if (cfg_.providerKey(h).empty()) continue;
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
    // CUM-286: the Cumulo router as a first-class head, so a keyed VN with NO
    // direct provider key still runs the whole assistant - the "one key, one
    // balance" path. Registered when the key is present (the pod restarts to pick
    // up the env, so no after-boot add is needed here); head resolution reaches
    // it via routerFallbackHost() above once every BYOK head misses.
    if (hasCumulo()) {
      d.hosts.add(kCumuloSlug,
                  [this](std::string& conv, const std::string& ins, const std::string& inp,
                         std::string& out, std::string& err, const agent::HeadTools* tools,
                         orch::TokenUsage* usage) -> bool {
                    auto pd = cumuloProviderDeps();
                    return agent::providers::orchTurnCustom(pd, conv, ins, inp, out, err,
                                                            tools, usage);
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
    m.tsHours = nowHours();
    epi_->addMessage(m);
  }

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
    if (onDeliver_) onDeliver_(chat, text);
  }

  static std::string trunc(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
  }

  Config cfg_;
  Options opt_;
  CgroupMemory mem_;
  std::unique_ptr<DaemonHttpTransport> ownedHttp_;  // owned unless a test injects one
  agent::HttpTransport* http_ = nullptr;            // the live transport (owned or injected)

  PosixEpiFs         epiFs_;
  orch::VectorMemory vec_;
  orch::Scratchpad   scratch_;
  orch::MemConfig    memCfg_;
  std::unique_ptr<orch::AppendLogEpisodicStore> epi_;
  std::unique_ptr<PosixFiles> files_;
  orch::ToolRegistry reg_;
  std::vector<std::string> toolNames_;
  std::string tavilyKey_;

  std::unique_ptr<agent::JobEngine>  jobs_;
  std::unique_ptr<agent::TurnEngine> eng_;

  std::string convId_, antEnv_, antAgents_, memory_, lastHost_, lastServedBy_;
  bool lastFallback_ = false;   // CUM-236 served-by of the most recent turn
  std::function<void(const std::string&, const std::string&)> onDeliver_;

  TurnRecord cur_;
  std::vector<TurnRecord> turns_;
  uint32_t keyGen_ = 0;   // bumps on each in-app key change (CUM-279 verify-ts seam)
};

}  // namespace nimbusd
