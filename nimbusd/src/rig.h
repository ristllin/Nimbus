#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "connectors_store.h"
#include "daemon_config.h"
#include "daemon_http.h"
#include "nimbus/docs_pack.h"
#include "nimbus/harness/dream.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/websearch.h"
#include "nimbus/orch/embedding.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/router_route.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_memory.h"
#include "posix_files.h"
#include "posix_fs.h"
#include "posix_platform.h"
#include "sub_fabric.h"
#include "tg_access.h"

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
constexpr const char* kCumuloModel      = "gpt-5.6";               // CUMULO_MODEL default
constexpr const char* kCumuloSlug       = "cumulo";               // head + routing slug

// The Z.ai (GLM) direct-BYOK head the DEVICE consumes (src/agent/orchestrator.cpp
// zai head + agent_config.h ZAI_HOST_PRIMARY / ZAI_BASE_PATH / ZAI_MODEL): an
// OpenAI-compatible provider with its own key. Mirrored here (the daemon cannot
// include the firmware header) so a keyed Virtual Nimbus routes to Z.ai identically
// to a keyed device (CUM-445). The canonical env name is Z_AI_TOKEN (daemon_config.h
// providerEnvName is the single slug->env source). The device probes api.z.ai then
// open.bigmodel.cn; a hosted instance has no probe seam, so it uses the primary host
// and lets NIMBUSD_ZAI_BASE override it.
constexpr const char* kZaiEnvKey     = "Z_AI_TOKEN";     // canonical env name
constexpr const char* kZaiHost       = "api.z.ai";       // ZAI_HOST_PRIMARY
constexpr const char* kZaiPathPrefix = "/api/paas/v4";   // ZAI_BASE_PATH (NOT /v1)
constexpr const char* kZaiConv       = "openai";         // OpenAI-compatible wire
constexpr const char* kZaiModel      = "glm-5.3";        // ZAI_MODEL default
constexpr const char* kZaiSlug       = "zai";            // head + routing slug

// The chat id the web app's chat surface runs its turns under (web_api.h posts
// every /api/chat turn with it). It is the hosted instance's SYNCHRONOUS channel:
// the page resolves its poll only on a delivered message.
constexpr const char* kWebChatId = "owner";

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
    // Device parity (store::subPriority / store::orchHost): the sub-agent provider
    // ladder ("" -> same as priority) and an explicit head pin ("" -> the first
    // keyed provider in priority, the device's resolvedOrchHost rule).
    std::string subPriority;
    std::string orchHost;
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
    // Telegram trust (CUM-459): the device model, durable on the instance volume.
    access_.reset(new TelegramAccess(memDir()));
    seedTelegramAllowlist();
    buildMemory();
    loadSecrets();   // in-app provider keys persisted from a prior session (CUM-279)
    loadModels();    // in-app model picks persisted from a prior session (CUM-425)
    conns_.setPath(memDir() + "/connectors.json");
    conns_.load();   // connector registry (CUM-424); tolerant of absent/torn
    buildFabric();   // sub-agent adapters + the durable job journal (device parity)
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
  // In-app model picks (CUM-425): durable like the keys, but not secret material,
  // so they live in their own plain file (slug=model lines).
  std::string modelsPath() const { return memDir() + "/models.txt"; }

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
    // The engine seam fails closed too (CUM-459): whatever posted it (the Telegram
    // poll, POST /api/message), a Telegram chat the owner has not approved - or has
    // revoked since the message was queued - never gets a turn.
    if (!access_->mayConverse(chatId)) {
      std::fprintf(stderr, "[nimbusd] telegram: chat %s is not approved - no turn\n", chatId.c_str());
      return cur_;
    }
    // Safety net for the one-namespace rule below: the engine thread HOLDS such a
    // turn until it may run (EngineThread), so this only refuses a direct caller.
    if (turnMustWait(chatId)) {
      std::fprintf(stderr, "[nimbusd] chat %s must wait for another person's sub-agents\n", chatId.c_str());
      return cur_;
    }
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
    if (!access_->mayConverse(chatId)) {   // same fail-closed seam as say()
      orch::FireOutcome blocked;
      blocked.detail = "blocked: chat not approved";
      return blocked;
    }
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

  // ---- connectors (CUM-424) -------------------------------------------------
  ConnectorsStore& connectors() { return conns_; }
  const ConnectorsStore& connectors() const { return conns_; }
  // Every successful connectors write drops the stored conversation id: Mistral
  // pins connectors at conversation creation, so the next turn must start a
  // fresh conversation carrying the new set (device parity: setOrchConvId("")).
  void noteConnectorsWrite() { convId_.clear(); }
  bool providerKeyed(const char* h) const { return !cfg_.providerKey(h).empty(); }
  // The host the turn ACTUALLY runs on, which the connector catalog, the web badge
  // and the Capabilities table all key "callable on your own turns" off: an explicit
  // orchHost, else the first KEYED provider in priority, else the router fallback
  // head, else the bare first token - the engine's resolveTurnHost rule. The old
  // raw-first-token badge told a mistral-only instance whose priority led with
  // openai that it was "running on openai", so it misjudged every connector.
  std::string hostBadge() const {
    return nimbus::orch::resolveHeadHost(
        opt_.orchHost, opt_.priority,
        [this](const std::string& h) { return !cfg_.providerKey(h).empty(); },
        hasCumulo() ? std::string(kCumuloSlug)
                    : (hostAvailable(kZaiSlug) ? std::string(kZaiSlug) : std::string()));
  }

  // ---- Telegram trust (CUM-459, the device model) -------------------------------
  // The fail-closed allowlist + RBAC table the owner manages in the web app. It
  // replaced the VNC stopgap (an "untrusted chat" set that only blocked sub-agent
  // spawns while EVERY chat still ran turns as the owner): an unapproved chat now
  // never reaches a turn at all, and an approved one runs as its real role.
  TelegramAccess& telegramAccess() { return *access_; }
  const TelegramAccess& telegramAccess() const { return *access_; }

  // ---- one person's background work at a time -----------------------------------
  // The shared JobEngine (lib/harness) keeps ONE pool of sub-agent results: every
  // result is folded into the NEXT turn whoever sends it, and the report goes to the
  // chat whose job finished last. That is fine for one owner and wrong for several
  // approved people (one would read another's calendar), so a hosted instance keeps
  // all queued, running and unreported sub-agent work inside ONE data namespace at
  // a time: a spawn from another namespace is refused with the reason, and a
  // member's turn from another namespace WAITS until that work is reported (the
  // engine thread holds it; nothing is dropped). The owner's own turns never wait.
  // Engine thread only.
  bool backgroundBusy() const {
    return jobs_ && (jobs_->activeCount() > 0 || jobs_->hasFreshResults());
  }
  bool turnMustWait(const std::string& chat) const {
    if (!backgroundBusy()) return false;
    const orch::Principal who = access_->principalFor(chat);
    return !who.owner && who.ns != bgNs_;
  }

  // ---- sub-agent jobs (device parity: orchestrator pollJobs -> JobEngine::pump) ----
  // One pump step: reap, the synthesis clock, at most one dispatch, one poll round.
  // Engine thread only (a Mistral dispatch is a synchronous provider call, and a
  // synthesis runs a whole turn) - the EngineThread drives it on every tick.
  int pumpJobs() { return jobs_ ? jobs_->pump() : 0; }
  // Whether a pump can do real work right now (a job queued, running, or results
  // waiting for synthesis) - lets the engine thread mark itself busy around it.
  bool jobsBusy() const { return jobs_ && (jobs_->activeCount() > 0 || jobs_->hasFreshResults()); }
  agent::JobEngine& jobs() { return *jobs_; }

  // ---- Mistral workspace probe (device parity: provider_verify syncMistralConnectors)
  // A Mistral Studio connector is usable once the key's workspace LISTS it as active
  // in GET /v1/connectors (the portable rule in connectors_wire.h). The device runs
  // this after each Mistral verify; a hosted instance has no verify button, so the
  // daemon runs it at startup, after a Mistral key is set, after a connectors write
  // that finds it unchecked, and on POST /api/verify provider=mistral. Skipped (no
  // request) unless an enabled Studio connector exists and a Mistral key is set.
  // Returns whether a definitive answer landed. Engine thread only (it uses the
  // engine's transport); readers take the snapshot under wsMu_.
  struct WorkspaceProbe {
    int status = -1;        // last HTTP status (0 transport failure, -1 never ran)
    size_t bytes = 0;       // size of the last response body
    bool noted = false;     // the last body parsed into an answer
    uint64_t epoch = 0;     // when it ran
    uint32_t runs = 0;      // probes attempted since start (lets a poller see a new one)
  };
  // Whether a probe would do anything: a Mistral key (or `assumeKey`, for a caller
  // about to apply one) and an enabled Studio connector. Lets the web layer skip
  // queueing a no-op behind the engine.
  bool wantsWorkspaceProbe(bool assumeKey = false) const {
    return (assumeKey || !cfg_.providerKey("mistral").empty()) &&
           nimbus::orch::wantsMistralWorkspaceProbe(conns_.parsed());
  }
  // Coalescing: at most one probe queued at a time. claimProbe() is true for the
  // caller that should post one; the probe releases the claim as it starts, so a
  // request that lands mid-probe queues exactly one follow-up.
  bool claimProbe() { return !probeQueued_.exchange(true); }
  // Whether the answer is worth refreshing on a connectors write: none yet, or an
  // enabled Studio connector the last answer did not list (the owner may have just
  // added it in Mistral). A hosted page has no verify button to force this.
  bool workspaceStale() const {
    for (const auto& c : connectorsView())
      if (c.enabled && c.workspace != nimbus::orch::ConnectorInfo::Workspace::NotApplicable &&
          c.workspace != nimbus::orch::ConnectorInfo::Workspace::ListedSignedIn &&
          c.workspace != nimbus::orch::ConnectorInfo::Workspace::ListedNotSignedIn)
        return true;
    return false;
  }
  bool refreshMistralWorkspace() {
    probeQueued_.store(false);   // a request arriving from now on queues a fresh probe
    const std::string key = cfg_.providerKey("mistral");
    if (!wantsWorkspaceProbe()) return false;
    agent::HttpRequest req;
    req.method = "GET";
    req.host = "api.mistral.ai";
    req.path = "/v1/connectors?page_size=100";
    req.timeoutMs = 25000;
    req.headers.push_back({"Authorization", "Bearer " + key});
    req.headers.push_back({"Accept", "application/json"});
    agent::HttpResponse resp;
    std::string err;
    const bool sent = http_->exec(req, resp, err);
    nimbus::orch::MistralWorkspace fresh;
    // Only a 200 carries a listing; anything else is "no signal" (keep the last).
    const bool noted = sent && resp.status == 200 &&
                       nimbus::orch::noteMistralWorkspaceProbe(fresh, resp.body.c_str());
    std::lock_guard<std::mutex> lk(wsMu_);
    if (noted) ws_ = std::move(fresh);
    wsProbe_ = WorkspaceProbe{sent ? resp.status : 0, resp.body.size(), noted,
                              (uint64_t)time(nullptr), wsProbe_.runs + 1};
    std::fprintf(stderr, "[nimbusd] connectors: mistral workspace probe HTTP %d (%zu bytes)%s\n",
                 wsProbe_.status, wsProbe_.bytes,
                 noted ? "" : " - no answer, kept the last one");
    return noted;
  }
  WorkspaceProbe workspaceProbe() const {
    std::lock_guard<std::mutex> lk(wsMu_);
    return wsProbe_;
  }
  bool workspaceProbed() const {
    std::lock_guard<std::mutex> lk(wsMu_);
    return ws_.probed;
  }
  // The credential state for one connector by the shared rule, against the current
  // workspace answer. T3 OAuth has no hosted mint, so its live signal is -1.
  int8_t connectorAuth(const nimbus::orch::ConnectorInfo& c) const {
    std::lock_guard<std::mutex> lk(wsMu_);
    return nimbus::orch::connectorAuthFor(c, ws_, -1);
  }
  // The parsed registry with auth + the workspace hint stamped: the ONE view the
  // model catalog, GET /api/connectors and /api/tools all read (device parity:
  // connectors.cpp portableList).
  std::vector<nimbus::orch::ConnectorInfo> connectorsView() const {
    auto cs = conns_.parsed();
    std::lock_guard<std::mutex> lk(wsMu_);
    nimbus::orch::applyConnectorAuth(cs, ws_);
    return cs;
  }
  // The provider state the catalog and the capability scope read.
  nimbus::orch::ProviderState providerState() const {
    nimbus::orch::ProviderState ps;
    ps.openaiKeyed = providerKeyed("openai");
    ps.anthropicKeyed = providerKeyed("anthropic");
    ps.mistralKeyed = providerKeyed("mistral");
    ps.capProbe = 0;   // no verify cache: report key presence, never claim "verified"
    ps.currentHost = hostBadge();
    ps.mistralHeadCarriesStudio = false;   // Studio connectors ride subs only (headDeps)
    return ps;
  }
  // The "[PROVIDERS & CONNECTORS]" block exactly as the next turn's context gets it
  // ("" when no connector is configured, matching the turn path).
  std::string connectorsCatalog() const {
    auto cs = connectorsView();
    if (cs.empty()) return std::string();
    return nimbus::orch::catalogText(cs, providerState());
  }

  // True iff at least one chat provider is configured - a direct BYOK key OR the
  // Cumulo router key. Drives the web chat page's honest "no provider key
  // configured" state (CUM-211/CUM-286): a keyless instance produces no reply,
  // so the surface must say so, but a Cumulo-only instance DOES reply (via the
  // router head) and must read healthy, never degraded.
  // Enumerated over the CANONICAL registry (anySlotWhere), not a hand-listed provider
  // array: the b2f4930 / CUM-246 drift ("anyKeyed missed cumulo/zai") that told a
  // Z.ai-only instance it had no provider - and so never dispatched its turn - cannot
  // recur, because a slot added to provider_slots.h is counted here with no edit.
  // cumulo is the router key (not a BYOK env slot), so it is read via hasCumulo().
  bool anyProviderConfigured() const {
    return nimbus::orch::anySlotWhere([this](const char* slug) {
      if (std::string(slug) == kCumuloSlug) return hasCumulo();
      return !cfg_.providerKey(slug).empty();
    });
  }

  // ---- in-app provider keys (CUM-279, device parity) ------------------------
  // Canonical env name for a provider slug - delegates to the single source in
  // Config (daemon_config.h). Empty for an unknown slug (the caller rejects it).
  static std::string keyEnvFor(const std::string& host) {
    return Config::providerEnvName(host);
  }
  // Map a web /api/orch key FIELD back to its provider slug through the CANONICAL
  // registry (lib/core provider_slots.h): the device UI posts `oaiKey`/`antKey`/
  // `mistKey`/`zaiKey`/`cumuloKey` (NOT `<slug>Key` - `oaiKey` != `openaiKey`), so a
  // suffix strip silently dropped every direct-provider write (CUM-445). The registry
  // is the ONE table the device, this GET, and this POST all read, so the field name
  // can never drift again. Empty for a field no slot owns.
  static std::string hostForKeyField(const std::string& field) {
    for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; i++)
      if (field == nimbus::orch::kProviderSlots[i].keyField)
        return std::string(nimbus::orch::kProviderSlots[i].slug);
    return std::string();
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
    // Device parity (resetMistralConnectorsProbe on a mistKey write): a connector
    // listed under the OLD key's workspace must not read usable under the new key
    // until a fresh probe lands (the web layer kicks one right after).
    if (host == "mistral") {
      std::lock_guard<std::mutex> lk(wsMu_);
      ws_ = nimbus::orch::MistralWorkspace();
    }
    buildEngine();                // re-register heads for the new key set
    ++keyGen_;
    return true;
  }
  // Set (or clear, when `model` is empty) a per-head model pick from the running UI
  // (device parity: store::setOrchModel, "" -> provider default). The hosted head
  // has no live-harvested choice list to validate against, so the gate is shape,
  // not membership: a bounded selector token ("glm-4.5-flash", "zai/glm-4.5-flash").
  // The cumulo head resolves its router route from this at engine build (CUM-425),
  // so the same rebuild-on-the-engine-thread discipline as applyProviderKey applies.
  bool applyOrchModel(const std::string& host, const std::string& model) {
    if (keyEnvFor(host).empty()) return false;   // unknown provider slug
    if (model.size() > 64) return false;
    for (char ch : model)
      if (!isalnum((unsigned char)ch) && ch != '.' && ch != '_' && ch != '-' &&
          ch != '/' && ch != ':')
        return false;
    if (model.empty()) opt_.models.erase(host);
    else opt_.models[host] = model;
    saveModels();
    buildEngine();                // the head captures its model/route at build time
    ++keyGen_;                    // the web save-flow polls this counter
    return true;
  }

  // Monotonic key-change counter, surfaced as the providers' verify timestamp so the
  // web save-flow's poll observes the change (a hosted instance has no cheap verify).
  uint32_t keyGen() const { return keyGen_; }

  // Served-by of the most recent turn (CUM-236), for the web chat's honest footer.
  bool lastFallback() const { return lastFallback_; }
  std::string lastServedBy() const { return lastServedBy_; }

  const Options& options() const { return opt_; }
  // The sub-agent provider ladder (device store::subPriority): the configured one,
  // else the head priority.
  std::string subPriority() const {
    return opt_.subPriority.empty() ? opt_.priority : opt_.subPriority;
  }

  // The model a head requests: an operator override (opt.models[h]) else the
  // provider default. Wired into the provider deps as orchModel/subModel, so the
  // engine's budget derivation reads through here for the resolved head. Public so
  // the one-key path test can assert the router head resolves a model (CUM-288).
  std::string modelFor(const std::string& h) const {
    auto it = opt_.models.find(h);
    if (it != opt_.models.end()) return it->second;
    if (h == "openai")    return "gpt-5.6";
    if (h == "anthropic") return "claude-sonnet-4-6";
    if (h == "mistral")   return "mistral-large-latest";
    // CUM-288: the Cumulo router head resolves to the router default model, not ""
    // - an empty model made budgeting fall to the conservative default window and
    // (with the fold path fixed) the fold request would carry no model at all.
    if (h == kCumuloSlug) return std::string(kCumuloModel);
    // Z.ai direct head resolves to its default GLM model so budgeting reads a real
    // window and the model picker shows a value (CUM-445), same shape as cumulo.
    if (h == kZaiSlug) return std::string(kZaiModel);
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

  // Legacy NIMBUSD_TG_CHAT_ID: an optional one-time SEED of the allowlist, no longer
  // a gate (TelegramAccess::seedFromEnv has the rules). Logged so an operator can see
  // what it did.
  void seedTelegramAllowlist() {
    const std::string note = access_->seedFromEnv(cfg_.get("NIMBUSD_TG_CHAT_ID"));
    if (!note.empty())
      std::fprintf(stderr, "[nimbusd] telegram: NIMBUSD_TG_CHAT_ID (legacy seed): %s\n", note.c_str());
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

  // ---- in-app model picks: durable overrides (CUM-425) ----------------------
  // slug=model lines, tolerant of an absent/torn file. Only slugs the daemon
  // knows are loaded (a stale line for a removed provider is dropped on read).
  void loadModels() {
    std::string blob;
    if (!fsutil::readFile(modelsPath(), blob)) return;
    std::istringstream is(blob);
    std::string line;
    while (std::getline(is, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
      if (!k.empty() && !v.empty() && !keyEnvFor(k).empty()) opt_.models[k] = v;
    }
  }
  void saveModels() {
    std::string blob;
    for (const auto& kv : opt_.models) blob += kv.first + "=" + kv.second + "\n";
    fsutil::writeFileAtomic(modelsPath(), blob);
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
    mc.embed = [this](const std::string& text, std::string& err) { return embed(text, err); };
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

  // Text -> quantized embedding, on the honest 2-arg Embedder contract (CUM-435):
  // an empty return writes the REAL cause into `err` using the SAME token vocabulary
  // the device seam emits (src/agent/adapters/embeddings.cpp), so orch::classifyEmbedFail
  // names the true failure (no key / key rejected / HTTP nnn / parse: ...) instead of a
  // fixed guess. `err` is left empty on success.
  std::vector<int8_t> embed(const std::string& text, std::string& err) {
    err.clear();
    if (!opt_.embeddings)     { err = "no model"; return {}; }
    const std::string key = cfg_.providerKey(opt_.embedHost);
    if (key.empty())          { err = "no embeddings key for " + opt_.embedHost; return {}; }
    if (text.empty())         { err = "empty text"; return {}; }
    if (opt_.embedModel.empty()) { err = "no model"; return {}; }
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
    std::string httpErr;
    if (!http_->exec(req, resp, httpErr))            { err = "connect failed"; return {}; }
    if (resp.status == 401 || resp.status == 403)    { err = "key rejected"; return {}; }
    if (resp.status < 200 || resp.status >= 300)     { err = "HTTP " + std::to_string(resp.status); return {}; }
    std::vector<float> f;
    std::string perr;
    if (!orch::parseEmbeddingResponse(resp.body.c_str(), 0, f, perr)) {
      err = "parse: " + perr;
      return {};
    }
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
    // Device parity (src/agent/store_config.cpp routerFallbackHost): Cumulo first
    // (the flagship one-key path), then Z.ai. Consulted only after every BYOK head in
    // the priority list misses, so a VN with ONLY a Z.ai key still runs its turns on
    // Z.ai instead of falling through to the first (keyless) priority head (CUM-445).
    p.routerFallbackHost = [this] {
      if (hasCumulo()) return std::string(kCumuloSlug);
      if (hostAvailable(kZaiSlug)) return std::string(kZaiSlug);
      return std::string();
    };
    // Device-truth "is any provider configured", INCLUDING the router key that
    // hasKey() above does not report (cumulo is not a canonical-env BYOK slot).
    // Keeps the engine's honest "no provider set up" reply from firing on a
    // Cumulo-only instance (CUM-211).
    p.anyKeyed = [this] { return anyProviderConfigured(); };
    p.orchHost = [this] { return opt_.orchHost; };
    p.providerPriority = [this] { return opt_.priority; };
    p.subPriority = [this] { return subPriority(); };
    p.orchModel = [this](const std::string& h) { return modelFor(h); };
    p.subModel = [this](const std::string& h) { return modelFor(h); };
    p.modelChoices = [](const std::string& h) {
      if (h == "openai")    return std::string("gpt-6-astra,gpt-5.6,gpt-5.6-luna");
      if (h == "anthropic") return std::string("claude-opus-5,claude-sonnet-5,claude-haiku-4-5");
      if (h == "mistral")   return std::string("mistral-large-latest,mistral-medium-latest,mistral-small-latest");
      if (h == "zai")       return std::string("glm-5.3,glm-5.2,glm-5.3-flash");
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
    // Connector attach seams (CUM-424): BYOK heads advertise the instance's
    // configured connectors exactly as a device with direct keys does
    // (builtinsOnly=false; the portable guard already keeps disabled, wrong-prov
    // and private-URL entries off the wire). The cumulo router head runs through
    // orchTurnCustom, which has no attach seam - router lanes carry no private
    // connectors, the same boundary the device draws on shared keys.
    pd.attachOpenAI = [this](JsonDocument& doc) {
      nimbus::orch::attachOpenAIWire(doc, conns_.parsed(), bearerFn(), false);
    };
    pd.attachMistral = [this](JsonDocument& doc) {
      nimbus::orch::attachMistralWire(doc, conns_.parsed(), false);
    };
    pd.attachAnthropic = [this](JsonDocument& doc) {
      nimbus::orch::attachAnthropicWire(doc, conns_.parsed(), bearerFn(), false);
    };
    return pd;
  }

  // The deps a HEAD turn runs with: the base deps, except that a Mistral head turn
  // attaches only the hosted built-ins, never the Studio connectors. The head's
  // single-shot turn always carries the strict orch_turn json_schema, and a
  // Conversations call with a Studio connector plus that schema never answers
  // (Mistral's edge closes it at 60 s, measured 3 of 3; without the schema the same
  // call returns in ~5 s). So Studio connectors run where the device's default loop
  // runs them anyway: on a spawned mistral sub-agent (free text, no schema). The
  // catalog says so (ProviderState.mistralHeadCarriesStudio = false).
  agent::providers::ProviderDeps headProviderDeps() {
    auto pd = providerDeps();
    pd.attachMistral = [this](JsonDocument& doc) {
      nimbus::orch::attachMistralWire(doc, conns_.parsed(), /*builtinsOnly=*/true);
    };
    return pd;
  }

  // The provider deps for the Cumulo router head: the base ProviderDeps with the
  // custom/proxy fields pointed at the fixed router base + path prefix and the
  // router key. Byte-for-byte the seam the device's cumulo head fills in
  // (src/agent/orchestrator.cpp:1138-1150) - orchTurnCustom over
  // app.cumulo-nimbus.ai + /router/openai/v1, bearer = the Cumulo key.
  agent::providers::ProviderDeps cumuloProviderDeps() {
    auto pd = providerDeps();
    // CUM-425: resolve the router coordinates from the model selector through the
    // SAME portable rule the device head and the sub-session adapter share, so a
    // "zai/glm-4.5-flash" selector reaches /router/zai/v1 with the bare model id
    // the router prices, instead of 403ing model_not_priced on the openai
    // upstream. A bare id (no '/') keeps the openai upstream - byte-identical to
    // the old constant behavior for every previously-working selector.
    const nimbus::orch::RouterRoute rr = nimbus::orch::resolveRouterRoute(cumuloModel());
    pd.customBase       = [] { return std::string(kCumuloHost); };
    pd.customPathPrefix = [rr] { return rr.basePath; };
    pd.customKey        = [this] { return cumuloKey(); };
    pd.customConv       = [] { return std::string(kCumuloConv); };
    pd.customModel      = [rr] { return rr.model; };
    return pd;
  }

  // The provider deps for the Z.ai direct-BYOK head (CUM-445): the base ProviderDeps
  // with the custom/proxy fields pointed at Z.ai's OpenAI-compatible endpoint
  // (api.z.ai + /api/paas/v4), bearer = the Z_AI_TOKEN the owner set in the UI. This
  // mirrors the device's zai head (src/agent/orchestrator.cpp) - orchTurnCustom over
  // the same base/path/conv - so a keyed VN talks to Z.ai byte-for-byte like a device.
  // NIMBUSD_ZAI_BASE overrides the host (the device's api.z.ai/open.bigmodel.cn probe
  // has no hosted equivalent). The model is the in-app pick else the GLM default.
  agent::providers::ProviderDeps zaiProviderDeps() {
    auto pd = providerDeps();
    const std::string base = cfg_.get("NIMBUSD_ZAI_BASE");
    pd.customBase       = [base] { return base.empty() ? std::string(kZaiHost) : base; };
    pd.customPathPrefix = [] { return std::string(kZaiPathPrefix); };
    pd.customKey        = [this] { return cfg_.providerKey(kZaiSlug); };
    pd.customConv       = [] { return std::string(kZaiConv); };
    pd.customModel      = [this] { return modelFor(kZaiSlug); };
    return pd;
  }

  void buildEngine() {
    agent::JobEngine::Deps jd;
    // Built ONCE: an engine rebuild (a key/model save) must not drop spawns still
    // queued or finished results waiting for synthesis - both live only in the
    // JobEngine, and every dep below reads the rig's current state through `this`.
    if (!jobs_) {
      jd.platform = makePosixPlatform(&mem_);
      // A chat the owner removed or revoked after it queued work hears nothing more.
      jd.deliver = [this](const std::string& c, const std::string& t) {
        if (access_->mayConverse(c)) record(c, t);
      };
      wireJobDeps(jd);
      jobs_.reset(new agent::JobEngine(std::move(jd)));
    }

    agent::TurnEngine::Deps d;
    d.cfg = config();
    d.platform = makePosixPlatform(&mem_);
    d.jobs = jobs_.get();
    d.deliver = [this](const std::string& c, const std::string& t) { record(c, t); };
    d.recall = [this](const std::string& q, const orch::Principal& who) {
      std::vector<std::string> out;
      if (!opt_.embeddings) return out;
      std::string eerr;
      auto v = embed(q, eerr);
      if (v.empty()) return out;
      // Device read boundary (memory_subsystem recall): STRICTLY the caller's own
      // namespace, so an approved member never recalls the owner's memories; an
      // unattributed caller recalls nothing rather than everything.
      const std::vector<std::string> ns{who.valid() ? who.ns : std::string("\x01none")};
      for (const auto& hit : vec_.search(v, 5, nowHours(), ns)) out.push_back(hit.content);
      return out;
    };
    d.composeInputs = [this](const std::string& chat) {
      agent::ComposeInputs in;
      in.devName = opt_.devName;
      in.hostLabel = lastHost_.empty() ? std::string("(picking)") : lastHost_;
      in.recentConversation = recentWindow(chat);
      in.runningMemory = runningMemory(chat);
      // Who this message is from (device W10): the prompt names the speaker's real
      // role, so an approved member is never addressed as the owner.
      in.speakerRole = orch::roleName(access_->roleOf(chat));
      in.speakerLabel = access_->labelOf(chat);
      return in;
    };
    d.toolSpecs = [this](const orch::Principal& who) { return reg_.toolSpecsFor(who); };
    d.mcpDispatch = [this](const std::string& req, const orch::Principal& who) {
      return reg_.handleRpc(req, who);
    };
    // Connector catalog (CUM-424): the "[PROVIDERS & CONNECTORS]" context block,
    // rendered by the same portable composer the device uses, over the SAME
    // auth-stamped view GET /api/connectors badges (connectorsView). capProbe=0:
    // nimbusd has no verify cache yet, so the catalog reports key PRESENCE and never
    // claims "verified" (the honest mode the composer defines for exactly this).
    d.connectorsCatalog = [this] { return connectorsCatalog(); };
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
    // The WHOLE principal from the tenant table (device principalFor): the owner's
    // own surfaces are Admin, an approved Telegram chat runs as its RBAC role.
    d.apply.principalFor = [this](const std::string& chat) {
      return access_->principalFor(chat);
    };
    // Per-chat running memory (device g_mem.model(chatId)): one person's rolling
    // summary never rides another person's prompt. Engine thread only.
    d.apply.setModelMemory = [this](const std::string& chat, const std::string& m) {
      memory_[chat] = m;
      return true;
    };
    // Sub-agent session ops (device buildApplyDeps): spawn/terminate/await reach the
    // JobEngine, which dispatches through the fabric on the engine thread's pump.
    // Unwired, a model's spawn vanished at the apply layer with no trace.
    // An approved chat spawns normally (sub-agents reach the owner's connectors, which
    // is exactly what the owner granted by approving it). The live re-check only
    // covers a chat the owner removed while its turn was running.
    d.apply.enqueueSpawn = [this](const orch::Spawn& s, const std::string& chat, bool quiet) {
      if (!access_->mayConverse(chat)) {
        record(chat, "Sub-agents are off for this chat: the owner has not approved it.");
        return;
      }
      const std::string ns = access_->principalFor(chat).ns;
      if (backgroundBusy() && ns != bgNs_) {
        record(chat, "Sub-agents are busy with another person's request. Try again in a few minutes.");
        return;
      }
      bgNs_ = ns;
      if (jobs_) jobs_->enqueueSpawn(s, chat, quiet);
    };
    // Stopping a sub-agent is the owner's, or its own namespace's, to do (a member
    // turn only runs while the in-flight work is its own - see turnMustWait).
    d.apply.cancelSession = [this](const std::string& id) {
      const orch::Principal who = access_->principalFor(cur_.chatId);
      if (!who.owner && who.ns != bgNs_) return false;
      return jobs_ && jobs_->cancel(id.c_str());
    };
    d.apply.awaitTag = [this](const std::string& tag) {
      if (jobs_) jobs_->awaitTag(tag);
    };
    d.apply.noteSpawned = [this] {
      if (jobs_) jobs_->noteSpawned();
    };
    // Device parity (orchestrator turnComplete): a turn that ends with no reply, no
    // spawn and no tool reply still confirms it finished on the SYNCHRONOUS channel
    // (the web chat), instead of leaving the page waiting out its poll with nothing.
    // Async channels (Telegram, /api/message) stay silent, as on the device.
    d.apply.turnComplete = [this](const std::string& chat) {
      if (chat == kWebChatId) record(chat, "Done.");
    };

    registerHeads(d);
    eng_.reset(new agent::TurnEngine(std::move(d)));
  }

  // Register the provider heads for the current key set. Kept out of buildEngine so
  // each is one small, named unit: the direct BYOK heads (openai/anthropic/mistral),
  // then the router-style OpenAI-compatible heads (cumulo, Z.ai) that a keyed VN with
  // no direct key still runs on.
  void registerHeads(agent::TurnEngine::Deps& d) {
    for (const char* h : {"openai", "anthropic", "mistral"}) {
      if (cfg_.providerKey(h).empty()) continue;
      const std::string host = h;
      d.hosts.add(host, [this, host](std::string& conv, const std::string& ins,
                                     const std::string& inp, std::string& out,
                                     std::string& err, const agent::HeadTools* tools,
                                     orch::TokenUsage* usage) -> bool {
        auto pd = headProviderDeps();
        if (host == "anthropic")
          return agent::providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, usage);
        if (host == "openai")
          return agent::providers::orchTurnOpenAI(pd, conv, ins, inp, out, err, tools, usage);
        return agent::providers::orchTurnMistral(pd, conv, ins, inp, out, err, tools, usage);
      });
    }
    // CUM-286: the Cumulo router as a first-class head, so a keyed VN with NO direct
    // provider key still runs the whole assistant - the "one key, one balance" path.
    // Head resolution reaches it via routerFallbackHost() once every BYOK head misses.
    if (hasCumulo()) addCustomHead(d, kCumuloSlug, [this] { return cumuloProviderDeps(); });
    // CUM-445: the Z.ai direct-BYOK head, when Z_AI_TOKEN is present. Like cumulo it
    // runs through orchTurnCustom (OpenAI-compatible), NOT the loop above whose else-
    // branch is Mistral - routing a Z.ai key there would post it to Mistral, exactly
    // the silent-misroute class the UI comment ("wrote a cumulo key into Mistral") warns
    // against.
    if (hostAvailable(kZaiSlug)) addCustomHead(d, kZaiSlug, [this] { return zaiProviderDeps(); });
  }

  // Register one OpenAI-compatible head (cumulo / Z.ai) that resolves its wire deps
  // from `depsFn` at turn time. Shared by the router-style heads so each call site is
  // one line.
  void addCustomHead(agent::TurnEngine::Deps& d, const std::string& slug,
                     std::function<agent::providers::ProviderDeps()> depsFn) {
    d.hosts.add(slug, [depsFn](std::string& conv, const std::string& ins, const std::string& inp,
                               std::string& out, std::string& err, const agent::HeadTools* tools,
                               orch::TokenUsage* usage) -> bool {
      auto pd = depsFn();
      return agent::providers::orchTurnCustom(pd, conv, ins, inp, out, err, tools, usage);
    });
  }

  // ---- sub-agent fabric (device parity: adapters/adapter_factory.cpp) -------
  // The five backends the device registers (anthropic/openai/mistral always; zai +
  // cumulo when keyed there - here always, since a key can arrive in-app later and
  // dispatch answers Auth without one). Each call resolves the CURRENT deps, key
  // and model, mirroring the device adapters line for line.
  void buildFabric() {
    journalStore_.reset(new FileJournalStore(memDir() + "/journal"));
    journal_.begin(journalStore_.get());   // re-attach unfinished jobs after a restart
    auto add = [this](const char* backend, SubAdapter::Ops ops) {
      subAdapters_.emplace_back(new SubAdapter(backend, std::move(ops)));
      fabric_.registerAdapter(subAdapters_.back().get());
    };
    add("anthropic", anthropicOps());
    add("openai", openaiOps());
    add("mistral", mistralOps());
    add(kZaiSlug, compatOps(kZaiSlug));
    add(kCumuloSlug, compatOps(kCumuloSlug));
  }
  SubAdapter::Ops mistralOps() {
    SubAdapter::Ops o;
    o.dispatch = [this](const agent::Directive& d, char out[72]) {
      return agent::providers::mistralDispatch(providerDeps(), modelFor("mistral"), d, out);
    };
    o.poll = [this](const char* id, agent::ResultEnvelope& env) {
      return agent::providers::mistralPoll(providerDeps(), id, env);
    };
    o.cancel = [this](const char* id) { return agent::providers::mistralCancel(providerDeps(), id); };
    return o;
  }
  SubAdapter::Ops openaiOps() {
    static const char* kHost = "api.openai.com";   // device OPENAI_HOST
    SubAdapter::Ops o;
    o.dispatch = [this](const agent::Directive& d, char out[72]) {
      const std::string key = cfg_.providerKey("openai");
      if (key.empty()) return agent::FabricErr::Auth;
      const std::string model = (d.model && d.model[0]) ? std::string(d.model) : modelFor("openai");
      return agent::providers::oaiDispatch(providerDeps(), kHost, key, model.c_str(), "openai", d, out);
    };
    o.poll = [this](const char* id, agent::ResultEnvelope& env) {
      return agent::providers::oaiPoll(providerDeps(), kHost, cfg_.providerKey("openai"), "openai", id, env);
    };
    o.cancel = [this](const char* id) {
      return agent::providers::oaiCancel(providerDeps(), kHost, cfg_.providerKey("openai"), id);
    };
    return o;
  }
  SubAdapter::Ops anthropicOps() {
    SubAdapter::Ops o;
    o.dispatch = [this](const agent::Directive& d, char out[72]) {
      if (cfg_.providerKey("anthropic").empty()) return agent::FabricErr::Auth;
      return agent::providers::antDispatch(providerDeps(), modelFor("anthropic").c_str(), d, out);
    };
    o.poll = [this](const char* id, agent::ResultEnvelope& env) {
      return agent::providers::antPoll(providerDeps(), id, env);
    };
    o.cancel = [this](const char* id) { return agent::providers::antCancel(providerDeps(), id); };
    return o;
  }
  // Z.ai and the Cumulo router: the OpenAI-compatible sub-session over an explicit
  // endpoint (device ZaiAdapter / CumuloAdapter), same host/path/model/route rules
  // as this instance's own zai/cumulo heads.
  SubAdapter::Ops compatOps(const char* tag) {
    const std::string backend = tag;
    SubAdapter::Ops o;
    o.dispatch = [this, backend](const agent::Directive& d, char out[72]) {
      agent::providers::CompatEndpoint ep;
      std::string host;
      if (backend == kZaiSlug) {
        host = cfg_.get("NIMBUSD_ZAI_BASE");
        if (host.empty()) host = kZaiHost;
        ep.basePath = kZaiPathPrefix;
        ep.key = cfg_.providerKey(kZaiSlug);
        ep.model = modelFor(kZaiSlug);
      } else {
        const nimbus::orch::RouterRoute route = nimbus::orch::resolveRouterRoute(cumuloModel());
        if (route.model.empty()) return agent::FabricErr::BadRequest;
        host = kCumuloHost;
        ep.basePath = route.basePath;
        ep.key = cumuloKey();
        ep.model = route.model;
        ep.wire = route.upstream == "anthropic" ? agent::providers::CompatWire::AnthropicMessages
                                                : agent::providers::CompatWire::OpenAIChat;
      }
      if (ep.key.empty()) return agent::FabricErr::Auth;
      ep.host = host.c_str();
      ep.backendTag = backend == kZaiSlug ? kZaiSlug : kCumuloSlug;
      return agent::providers::openaiCompatDispatch(providerDeps(), ep, d, out);
    };
    o.poll = [this, backend](const char* id, agent::ResultEnvelope& env) {
      return agent::providers::openaiCompatPoll(providerDeps(), backend.c_str(), id, env);
    };
    o.cancel = [this](const char* id) { return agent::providers::openaiCompatCancel(providerDeps(), id); };
    return o;
  }

  // The JobEngine's config reads + execution closures (device buildJobDeps). Left
  // unwired on purpose: skill capsules, doc attachments, result persistence and
  // provider-file capture (no hosted equivalents yet - the engine then passes the
  // task through and notes attachments instead of splicing them).
  void wireJobDeps(agent::JobEngine::Deps& jd) {
    jd.fabric = &fabric_;
    jd.journal = &journal_;
    jd.subPriority = [this] { return subPriority(); };
    jd.providerHasKey = [this](const std::string& p) {
      return p == kCumuloSlug ? hasCumulo() : !cfg_.providerKey(p).empty();
    };
    jd.subModel = [this](const std::string& p) {
      return p == kCumuloSlug ? cumuloModel() : modelFor(p);
    };
    // No live-harvested model catalog on a hosted instance to validate a spawn's
    // model pick against, so every spawn runs the configured sub model (the engine
    // coerces when this is unset) - never a model the key may not be entitled to.
    jd.modelIsValid = nullptr;
    jd.connectorProvider = [this](const std::string& skill) { return connectorProviderFor(skill); };
    jd.nowString = [] { return localNowString(); };
    jd.chatContext = [this](const std::string& chat) { return spawnContext(chat); };
    // The report turn for a chat the owner removed or revoked meanwhile never runs:
    // its results are dropped instead of delivered.
    jd.synthesize = [this](const std::string& chat) {
      if (!access_->mayConverse(chat)) {
        if (jobs_) jobs_->takeFreshResults();
        return;
      }
      if (eng_) eng_->maybeConsolidate(chat);
    };
    jd.turnInFlight = [this] { return eng_ && eng_->turnInFlight(); };
  }

  // Connector-aware routing (device d.connectorProvider): a spawn whose skill names
  // an enabled connector runs on the provider that hosts it.
  std::string connectorProviderFor(const std::string& skill) const {
    for (const auto& c : conns_.parsed()) {
      if (!c.enabled) continue;
      const bool match = skill == c.name || (!c.type.empty() && skill == c.type);
      if (match && c.prov != "any") return c.prov;
    }
    return std::string();
  }

  static std::string localNowString() {
    const time_t now = time(nullptr);
    struct tm lt{};
    localtime_r(&now, &lt);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M %Z (%A)", &lt);
    return buf;
  }

  // The spawning chat's recent window for the sub-agent brief (device chatContext):
  // the same digest builder, so each row is flattened to one line (a newline in a
  // relayed calendar/Slack text cannot forge an "owner:" line) and clipped
  // UTF-8-safe (a torn character is invalid JSON the provider rejects), within the
  // device's brief budget.
  std::string spawnContext(const std::string& chat) {
    if (chat.empty() || chat == "system") return std::string();
    orch::MsgQuery q;
    q.sessionId = chat;
    q.haveKind = true;
    q.kind = orch::MsgKind::Message;
    q.limit = 6;
    const auto rows = epi_->query(q);
    if (rows.empty()) return std::string();
    return "[CONTEXT] (the conversation this task came from, oldest first)\n" +
           agent::dream::buildEpisodicDigest(rows, kSpawnBriefBytes);
  }
  static constexpr size_t kSpawnBriefBytes = 1200;   // device brief budget at the 200K anchor

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

  // The chat's recent window. A member's lines are labeled "user", never "owner":
  // the model must not read an approved guest's words as the owner's.
  std::string recentWindow(const std::string& chat) {
    orch::MsgQuery q;
    q.sessionId = chat;
    q.limit = 12;
    const char* speaker = access_->roleOf(chat) == orch::Role::Admin ? "owner" : "user";
    std::string out;
    for (const auto& m : epi_->query(q)) {
      const char* who = (m.role == "user") ? speaker : "nimbus";
      out += std::string(who) + ": " + trunc(m.text, 400) + "\n";
    }
    return out;
  }

  std::string runningMemory(const std::string& chat) const {
    auto it = memory_.find(chat);
    return it == memory_.end() ? std::string() : it->second;
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
  // Telegram allowlist + RBAC (CUM-459). Declared early so it outlives the engine and
  // job engine whose closures consult it.
  std::unique_ptr<TelegramAccess> access_;
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

  // Sub-agent fabric (device parity): adapters + the durable journal. Rig-owned so
  // an engine rebuild (key/model change) keeps in-flight jobs re-attachable.
  agent::HeavyFabric fabric_;
  std::vector<std::unique_ptr<SubAdapter>> subAdapters_;
  std::unique_ptr<FileJournalStore> journalStore_;
  nimbus::orch::Journal journal_;
  std::unique_ptr<agent::JobEngine>  jobs_;
  std::unique_ptr<agent::TurnEngine> eng_;

  // Bearer resolution for the connector attach seams (CUM-424): the stored
  // static token by name; "" for T1 (none needed) and unminted T3 (skipped).
  nimbus::orch::BearerFn bearerFn() {
    return [this](const nimbus::orch::ConnectorInfo& c) {
      return conns_.bearerForName(c.name);
    };
  }

  ConnectorsStore conns_;   // connector registry (CUM-424), file-backed
  // The Mistral workspace answer (Studio-connector usability) + the last probe's
  // outcome; written on the engine thread, read by the HTTP thread, so under wsMu_.
  mutable std::mutex wsMu_;
  nimbus::orch::MistralWorkspace ws_;
  WorkspaceProbe wsProbe_;
  std::atomic<bool> probeQueued_{false};   // one workspace probe queued at a time
  std::map<std::string, std::string> memory_;   // running memory per chat (engine thread)
  std::string bgNs_;   // the data namespace that owns the in-flight sub-agent work
  std::string convId_, antEnv_, antAgents_, lastHost_, lastServedBy_;
  bool lastFallback_ = false;   // CUM-236 served-by of the most recent turn
  std::function<void(const std::string&, const std::string&)> onDeliver_;

  TurnRecord cur_;
  std::vector<TurnRecord> turns_;
  uint32_t keyGen_ = 0;   // bumps on each in-app key change (CUM-279 verify-ts seam)
};

}  // namespace nimbusd
