#pragma once
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "spend_ceiling_vn.h"   // lane DEF: the monthly spend ceiling (R7: hosted too)
#include "nimbus/orch/spend_guard.h"   // lane QW4: sentWithoutAnswer - a sent embedding counts
#include "connectors_store.h"
#include "daemon_config.h"
#include "daemon_http.h"
#include "hosted_settings.h"
#include "nimbus/docs_pack.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/dream.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/turn_anatomy.h"   // the /api/lastturn page head
#include "nimbus/harness/websearch.h"
#include "nimbus/orch/caps.h"
#include "nimbus/orch/command.h"
#include "nimbus/orch/embedding.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/key_shape.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/pending_embed.h"   // memory writes that survive an embedding outage
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/router_route.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_memory.h"
#include "posix_files.h"
#include "posix_fs.h"
#include "posix_platform.h"
#include "provider_verify.h"
#include "router_head.h"
#include "nimbus/harness/router_catalog.h"   // CUM-530: the device's bound + order for the router list
#include "sub_fabric.h"
#include "tg_access.h"
#include "version.h"

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
// the page resolves its poll only on a delivered message. The shared engine names
// it the web UI in the turn's [CHANNEL] line, never Telegram (CUM-542).
constexpr const char* kWebChatId = nimbus::orch::kHostedWebChatId;

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

// One head call's inputs and outputs as the engine hands them to a head (the
// provider conversation, the instructions, the per-turn input, the loop tools; the
// orch_turn JSON, the error, the usage), bundled for the router head's helpers.
struct HeadCall {
  std::string& conv;
  const std::string& ins;
  const std::string& inp;
  const agent::HeadTools* tools;
};
struct HeadOut {
  std::string& out;
  std::string& err;
  orch::TokenUsage* usage;
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
    // Master switch for long-term memory embeddings (NIMBUSD_EMBEDDINGS). Whether a
    // call can actually run is decided per call from the CURRENT keys (embedRoute),
    // so a key set in the app later, or a Cumulo-only instance, embeds without a
    // restart.
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
    // The hosted page chats as "owner" (kWebChatId): make the shared chat rule read it
    // as the web chat for this process (a device never does this).
    nimbus::orch::setHostedWebChatAlias(true);
    if (httpOverride) {
      http_ = httpOverride;
    } else {
      ownedHttp_.reset(new DaemonHttpTransport());
      http_ = ownedHttp_.get();
    }
    tap_.setInner(http_);   // the cumulo head's view of the transport (router refusals)
    installLog(opt_.verboseHttp);
    // Lane DEF: an instance that already ran an earlier release has its memory dir.
    struct stat st;
    const bool hadState = ::stat(memDir().c_str(), &st) == 0;
    fsutil::mkdirs(memDir());
    ceiling_.reset(new SpendCeilingVn(memDir() + "/spend_ceiling.txt", hadState,
                                      cfg_.has("TELEGRAM_BOT_TOKEN")));
    ceiling_->install();
    loadSettings();   // the owner's settings from the app (CUM-543), else the env defaults
    // Telegram trust (CUM-459): the device model, durable on the instance volume.
    access_.reset(new TelegramAccess(memDir()));
    seedTelegramAllowlist();
    buildMemory();
    loadSecrets();   // in-app provider keys persisted from a prior session (CUM-279)
    loadModels();    // in-app model picks persisted from a prior session (CUM-425)
    loadSubModels(); // in-app sub-agent model picks (device subM_<slug>)
    conns_.setPath(memDir() + "/connectors.json");
    conns_.load();   // connector registry (CUM-424); tolerant of absent/torn
    installToolHints();
    buildFabric();   // sub-agent adapters + the durable job journal (device parity)
    buildEngine();
  }

  ~NimbusdRig() { flush(); }

  // ---- durable data layout --------------------------------------------------
  std::string memDir() const { return opt_.dataDir + "/mem"; }
  std::string vectorsPath() const { return memDir() + "/vectors.bin"; }
  // Memory writes waiting for an embedding (an outage), as text (lane P).
  std::string pendingPath() const { return memDir() + "/pending_embed.bin"; }
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
  std::string subModelsPath() const { return memDir() + "/submodels.txt"; }

  // Persist every whole-file store with the atomic tmp->rename writer. The
  // episodic append-log is already durable per-message; this flushes the RAM
  // engines (vectors, scratchpad, memconfig). Called after each turn and on exit.
  void flush() {
    fsutil::writeFileAtomic(vectorsPath(), vec_.serialize());
    fsutil::writeFileAtomic(scratchPath(), scratch_.serialize());
    fsutil::writeFileAtomic(memConfigPath(), memCfg_.serialize());
    if (pending_.dirty()) persistPending();
  }

  // ---- memory writes that survive an embedding outage (lane P) ------------
  static uint32_t steadyMs() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  bool persistPending() {
    const bool ok = fsutil::writeFileAtomic(pendingPath(), pending_.serialize());
    if (ok) pending_.markClean();
    return ok;
  }
  size_t pendingCount() const { return pending_.size(); }
  const orch::EmbedPark& embedPark() const { return park_; }
  // One drain step (engine thread): the oldest queued fact through the same write
  // rails, under its namespace's current principal; the vectors are persisted
  // before the entry leaves the queue.
  orch::EmbedDrainResult drainPendingOnce() {
    orch::EmbedDrainDeps d;
    d.principalForNs = [this](const std::string& ns, orch::Principal& who) {
      if (ns == orch::kOwnerNs) { who = principal(); return true; }
      if (ns.rfind("chat:", 0) != 0) return false;
      who = access_->principalFor(ns.substr(5));
      return true;
    };
    d.persistVectors = [this] { return fsutil::writeFileAtomic(vectorsPath(), vec_.serialize()); };
    const orch::EmbedDrainResult r = orch::drainOne(memCtx_, d);
    if (r.step != orch::EmbedDrainStep::Idle)
      std::fprintf(stderr, "[nimbusd] pending drain %s %s (left=%zu)\n", orch::embedDrainStepName(r.step),
                   r.id.c_str(), pending_.size());
    return r;
  }
  // The recall embed, through the park memo (device parity): no doomed call on
  // every turn while parked; a failure parks, a success ends the park. `why` gets
  // the cause words when no vector came back.
  std::vector<int8_t> recallEmbed(const std::string& q, std::string& why) {
    if (park_.blocked(steadyMs())) {
      why = orch::embedFailWords(park_.reason(), park_.detail());
      return {};
    }
    std::string eerr, detail;
    auto v = embed(q, eerr);
    orch::EmbedFail kind = orch::EmbedFail::None;
    if (v.empty()) {
      kind = orch::classifyEmbedFail(eerr, detail);
      if (kind == orch::EmbedFail::None) kind = orch::EmbedFail::Unknown;
      why = orch::embedFailWords(kind, detail);
    }
    park_.onResult(kind, steadyMs(), detail);
    return v;
  }
  // Automatic recall (device parity, memory_subsystem recallFor): semantic when
  // embeddings serve it, else the labelled keyword scan over the store and the
  // pending queue. Device read boundary: STRICTLY the caller's own namespace, so
  // an approved member never recalls the owner's memories; an unattributed caller
  // recalls nothing rather than everything.
  orch::RecallOut recallFor(const std::string& q, const orch::Principal& who) {
    orch::RecallOut out;
    if (vec_.size() == 0 && pending_.empty()) return out;
    const std::vector<std::string> ns{who.valid() ? who.ns : std::string("\x01none")};
    std::string why;
    std::vector<int8_t> v;
    if (embedAvailable()) v = recallEmbed(q, why);
    else why = orch::embedFailWords(orch::EmbedFail::NoKey, "");
    if (v.empty()) return orch::lexicalRecall(vec_, &pending_, q, orch::RecallScope{ns, nowHours(), 5}, why);
    for (const auto& hit : vec_.search(v, 5, nowHours(), ns)) out.hits.push_back(hit.content);
    return out;
  }
  // Lane DEF: the monthly spend ceiling (the web API reads and edits it).
  SpendCeilingVn& spendCeiling() { return *ceiling_; }
  // The engine thread's idle tick for the ceiling: the month rollover, and the notices
  // it owes the owner, once per channel (the web chat; Telegram's owner chat - the first
  // allowlisted chat - when a bot runs here), each recorded and forwarded like any reply.
  void spendTick() {
    ceiling_->tick();
    if (eng_) ceiling_->setHeadModel(modelFor(eng_->headHost()));
    spendNotice(nimbus::orch::NoticeChannel::Web, kWebChatId);
    if (cfg_.has("TELEGRAM_BOT_TOKEN") && access_) {
      const std::string allow = access_->allowCsv();
      const std::string owner = allow.substr(0, allow.find(','));
      if (!owner.empty()) spendNotice(nimbus::orch::NoticeChannel::Telegram, owner);
    }
  }
  void spendNotice(nimbus::orch::NoticeChannel ch, const std::string& chat) {
    std::string text;
    const auto k = ceiling_->nextNotice(ch, text);
    if (k == SpendCeilingVn::Kind::None) return;
    record(chat, text);
    ceiling_->delivered(ch, k);
  }
  // The engine thread's idle tick: at most one step every 5 s, never while parked.
  void drainTick() {
    if (pending_.empty() || park_.blocked(steadyMs())) return;
    const uint32_t now = steadyMs();
    if (lastDrainMs_ && now - lastDrainMs_ < 5000) return;
    lastDrainMs_ = now;
    drainPendingOnce();
  }
  // Test seam (offline suite, live embed-outage scenario): make only the
  // embedding call fail with a recorded provider token ("" = off). Off also ends
  // the embedding pause, like the device's FAULT embed_fail off.
  void setEmbedShim(const std::string& token) {
    embedShim_ = token;
    if (token.empty()) park_.clear();
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
    // Deterministic commands (/clear, /help) are answered here, before any model
    // call: never a paid turn (device parity, orchestrator.cpp handleMessage).
    if (handleCommand(chatId, text)) {
      if (!cur_.deliveries.empty()) cur_.reply = cur_.deliveries.back();
      cur_.ok = true;
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

  // The slash commands a hosted instance answers itself. /clear is the device's
  // light two-step (it explains and asks; "/clear yes" clears); /help lists them;
  // the device commands that need hardware or a routines engine say so; anything
  // else gets a deterministic reply instead of a paid model turn.
  bool handleCommand(const std::string& chat, const std::string& text) {
    const orch::Command cmd = orch::parseCommand(text);
    if (!cmd.isCommand) return false;
    const std::string& v = cmd.verb;
    if (v == "help" || v == "start") {
      record(chat, "Commands on this instance:\n/clear - forget this conversation (keeps memory and files)\n"
                   "/help - this list\nTo chat, write without the slash.");
      return true;
    }
    if (v == "clear") {
      commandClear(chat, cmd.args);
      return true;
    }
    for (const char* d : {"update", "loops", "loop", "remind", "compact", "skill", "fetch", "mcp", "play"}) {
      if (v != d) continue;
      record(chat, "/" + v + " is not available on a hosted Nimbus yet.");
      return true;
    }
    // The reply is stored as an assistant row, so it never echoes free text a
    // sender chose (a "verb" can be a whole sentence spaced with non-ASCII blanks).
    const bool plain = !v.empty() && v.size() <= 24 &&
                       std::all_of(v.begin(), v.end(), [](char ch) { return isalnum((unsigned char)ch) || ch == '_'; });
    record(chat, (plain ? "There is no /" + v + " command." : std::string("There is no such command.")) +
                     " Send /help for the list, or write without the slash to chat.");
    return true;
  }
  void commandClear(const std::string& chat, std::string arg) {
    if (!access_->principalFor(chat).owner) {
      record(chat, "Only the owner can clear this conversation.");
      return;
    }
    for (char& ch : arg) ch = (char)tolower((unsigned char)ch);
    if (arg == "yes" || arg == "confirm") {
      clearConversation(chat);
      record(chat, "Cleared this conversation and its active task. Long-term memory and files are kept.");
      return;
    }
    record(chat, "Clear this conversation and the current active task? Long-term memory and files are kept. "
                 "Send /clear yes to confirm.");
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
  // Session usage since the process started, across engine rebuilds (CUM-546).
  // Engine thread (the snapshot publishes them for the HTTP thread).
  orch::TokenUsage sessionUsage() const {
    orch::TokenUsage u = carriedUsage_;
    u += eng_->sessionUsage();
    return u;
  }
  uint32_t turnCount() const { return carriedTurns_ + eng_->turnCount(); }
  orch::TokenUsage lastTurnUsage() const { return lastUsage_; }
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
  // Whether an embedding call can run right now (the master switch plus a route
  // with a key: direct, or through the Cumulo router). Engine thread only.
  bool embedAvailable() const {
    EmbedRoute r;
    return opt_.embeddings && embedRoute(opt_.embedHost, r);
  }
  // The live embedding config (engine thread only, like every embed call).
  std::string embedProvider() const { return opt_.embedHost; }
  std::string embedModelName() const { return opt_.embedModel; }
  int embedWidth() const { return opt_.embedDims; }
  // The route an embedding takes right now: "direct", "router" (the Cumulo key)
  // or "none" (off, or no key can carry it). For /api/mem/stats.
  std::string embedRouteLabel() const {
    EmbedRoute r;
    if (!opt_.embeddings || !embedRoute(opt_.embedHost, r)) return "none";
    return r.viaRouter ? "router" : "direct";
  }
  // Persist the whole-file stores after a web mutation (config/vector edit). Same
  // atomic writer flush() uses; safe to call on the engine thread.
  void persist() { flush(); }
  agent::HttpTransport& http() { return *http_; }
  PosixFiles& files() { return *files_; }
  const std::vector<TurnRecord>& turns() const { return turns_; }

  uint32_t totalIn() const { uint32_t n = 0; for (auto& t : turns_) n += t.tokensIn; return n; }
  uint32_t totalOut() const { uint32_t n = 0; for (auto& t : turns_) n += t.tokensOut; return n; }

  bool hostAvailable(const std::string& h) const { return !cfg_.providerKey(h).empty(); }

  // Whether the ENGINE may treat `h` as a keyed provider (CUM-541): every BYOK key,
  // and the instance's own Cumulo router key, which Config::providerKey() keeps out
  // of the BYOK-only paths (connector auth, embeddings-by-key). Device parity
  // (src/agent/store_config.cpp hasKey): the router head is a first-class keyed
  // provider, so [AVAILABLE MODELS] lists it and a spawn may run on it.
  bool keyedForEngine(const std::string& h) const { return !engineKey(h).empty(); }
  // The key the engine uses for `h` (the Cumulo router key for "cumulo").
  std::string engineKey(const std::string& h) const {
    return h == kCumuloSlug ? cumuloKey() : cfg_.providerKey(h);
  }

  // The sub-agent ladder the JobEngine walks: the configured order, then the
  // router keys this instance holds (Cumulo, then Z.ai) when the order does not
  // name them, so a one-key instance runs a spawn on its own key instead of
  // answering "No provider is configured to run that agent". A keyed BYOK
  // provider in the configured order still wins (it comes first).
  std::string subLadder() const {
    std::string ladder = subPriority();
    for (const char* r : {kCumuloSlug, kZaiSlug}) {
      if (!keyedForEngine(r) || nimbus::orch::modelInChoices(ladder, r)) continue;
      ladder += (ladder.empty() ? "" : ",") + std::string(r);
    }
    return ladder;
  }

  // The Cumulo router key from the canonical env (CUM-286). This is the daemon's
  // equivalent of the device's store::cumuloKey() - the material the "cumulo"
  // head places in its bearer, and the source that makes the router the fallback
  // head when no direct provider key is set.
  std::string cumuloKey() const { return cfg_.get(kCumuloEnvKey); }
  bool hasCumulo() const { return !cumuloKey().empty(); }

  // The model the cumulo head requests: an operator override (opt.models
  // ["cumulo"]) or the router default. Mirrors the device's orchModel("cumulo").
  std::string cumuloModel() const {
    std::lock_guard<std::mutex> lk(modelsMu_);
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
    const HostedSettings st = settings_.get();
    return nimbus::orch::resolveHeadHost(
        st.orchHost, st.provPrio,
        [this](const std::string& h) { return keyedForEngine(h); },
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
  // The shared JobEngine (lib/harness, tracked as CUM-463) keeps ONE pool of
  // sub-agent results: every result is folded into the NEXT turn whoever sends it,
  // and the report goes to the chat whose job finished last. That is fine for one owner and wrong for several
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
  // Lane QW4 round 3: the stop list (helpers whose stop is not yet confirmed, read from
  // disk before the first pump) is work too, or nothing would ever ask them again.
  bool jobsBusy() const { return jobs_ && jobs_->hasBackgroundWork(); }
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
  // ---- provider verify + model catalogs (CUM-545, device provider_verify) -------
  // One unmetered read of the provider's model list: its status is the badge
  // (verified / rejected / could not verify + reason) and its body is the catalog
  // the model picker and the prompt's model list read. Engine thread only (it uses
  // the engine's transport); readers take a copy from verify_.
  bool verifyProvider(const std::string& slug) {
    releaseVerify(slug);
    const std::string key = engineKey(slug);
    if (key.empty()) return false;
    ProbeTarget t;
    if (!probeTarget(slug, key, ProbeHosts{zaiHost(), kCumuloHost}, t)) return false;
    agent::HttpRequest req;
    req.method = "GET";
    req.host = t.host;
    req.path = t.path;
    req.timeoutMs = 15000;
    req.headers = t.headers;
    agent::HttpResponse resp;
    std::string err;
    const int status = http_->exec(req, resp, err) ? resp.status : 0;
    std::string reason;
    verify_.record(slug, probeVerdict(slug, status, reason), reason);
    if (status == 200) {
      std::vector<nimbus::orch::ModelInfo> cat;
      nimbus::orch::parseModelsList(slug, resp.body, cat);
      // The router body is untrusted and only byte-bounded: the device's rule keeps
      // at most kCatalogMaxRows rows, the router default, each upstream's newest
      // model and the flagships first, and drops dated twins and ids no upstream
      // runs. Then the parser's (upstream, bare id) becomes the routable
      // "<upstream>/<model>" the picker posts back as the head selector.
      if (slug == kCumuloSlug) {
        agent::router_catalog::protectHeads(cat, kCumuloModel);
        for (auto& m : cat) m.id = agent::router_catalog::selectorOf(m);
      }
      if (!cat.empty()) verify_.setCatalog(slug, std::move(cat));
    }
    std::fprintf(stderr, "[nimbusd] verify %s: HTTP %d%s%s\n", slug.c_str(), status,
                 reason.empty() ? "" : " ", reason.c_str());
    return status == 200;
  }
  ProviderVerify verifyState(const std::string& slug) const { return verify_.get(slug); }
  // At most one verify queued per provider (claim before posting; the verify
  // releases the claim as it starts, so a request mid-verify queues one more).
  bool claimVerify(const std::string& slug) {
    std::lock_guard<std::mutex> lk(verifyMu_);
    return verifyQueued_.insert(slug).second;
  }
  void releaseVerify(const std::string& slug) {
    std::lock_guard<std::mutex> lk(verifyMu_);
    verifyQueued_.erase(slug);
  }
  // The Recommended list the picker and the prompt read for a provider: the
  // harvested catalog when there is one, else the shared cold-start list
  // (orch::staticModelChoices, the device's source). Cumulo advertises only what
  // a spawn accepts (jd.modelIsValid): the harvested router catalog, else nothing
  // (the prompt then names the configured sub model).
  std::string modelChoicesFor(const std::string& h) const {
    const std::string harvested =
        catalogCsv(verify_.get(h).models, nimbus::orch::RoleOrchestrator, 8);
    if (!harvested.empty() || h == kCumuloSlug) return harvested;
    return nimbus::orch::staticModelChoices(h.c_str());
  }
  // The Z.ai host: NIMBUSD_ZAI_BASE reduced to a bare host (a scheme, port or path
  // in the operator's value never becomes part of a hostname), else api.z.ai.
  std::string zaiHost() const {
    std::string b = cfg_.get("NIMBUSD_ZAI_BASE");
    const size_t sch = b.find("://");
    if (sch != std::string::npos) b = b.substr(sch + 3);
    b = b.substr(0, b.find_first_of("/:"));
    return b.empty() ? std::string(kZaiHost) : b;
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
  // A Virtual Nimbus has no provider-file capture, so the hosted code_interpreter
  // stays off its wires (a file it built could never come back) and the web page
  // hides the Code sandbox switch; any stored Code Interpreter card is dropped the
  // same way the device drops it (applyCodeSandbox, wf_plan 3).
  std::vector<nimbus::orch::ConnectorInfo> connectorsView() const {
    auto cs = conns_.parsed();
    nimbus::orch::applyCodeSandbox(cs, false);
    std::lock_guard<std::mutex> lk(wsMu_);
    nimbus::orch::applyConnectorAuth(cs, ws_);
    return cs;
  }
  // What holds a provider off on `seam` (the shared park: a 402 or 401 parks a VN
  // provider as it does a device's). A Virtual Nimbus has no monthly budget check.
  // `dueIsOpen`: the prompt's reads (a park whose re-check is due is open); the head guard
  // reads Chat without it, as the device does.
  static nimbus::orch::HeadPause pauseOf(const std::string& p, nimbus::orch::PaidSeam seam,
                                         bool dueIsOpen = true) {
    return nimbus::orch::parkPause(p, seam, steadyMs(), dueIsOpen);
  }
  // Does a head on `h` run the tool loop (web.search on its own turn)? The device's rule:
  // native hosts, the Cumulo head on a native upstream, never Z.ai or custom.
  bool headRunsTools(const std::string& h) const { return nimbus::orch::headRunsToolLoop(h, cumuloModel()); }
  // The provider state the catalog and the capability scope read. `headCallsTools`:
  // the head runs a tool loop, so web.search (a Tavily key) is a web route (CUM-822).
  nimbus::orch::ProviderState providerState(bool headCallsTools = true) const {
    nimbus::orch::ProviderState ps;
    ps.openaiKeyed = providerKeyed("openai");
    ps.anthropicKeyed = providerKeyed("anthropic");
    ps.mistralKeyed = providerKeyed("mistral");
    // CUM-821: a parked provider is PAUSED in the catalog and no spawn or file route.
    ps.openaiPause = pauseOf("openai", nimbus::orch::PaidSeam::Spawn);
    ps.anthropicPause = pauseOf("anthropic", nimbus::orch::PaidSeam::Spawn);
    ps.mistralPause = pauseOf("mistral", nimbus::orch::PaidSeam::Spawn);
    ps.hostPause = pauseOf(hostBadge(), nimbus::orch::PaidSeam::Chat);
    ps.webSearchTool = headCallsTools && !tavilyKey_.empty();
    ps.capProbe = 0;   // no verify cache: report key presence, never claim "verified"
    ps.currentHost = hostBadge();
    ps.mistralHeadCarriesStudio = false;   // Studio connectors ride subs only (headDeps)
    // The durable volume keeps files (the SD role, web_api.h mem/stats), but no
    // capture path brings a sub-agent's file back and files.pdf is not registered:
    // the PDF line says so instead of promising a file (pdfRoute).
    ps.fileStoreReady = true;
    ps.fileCapture = false;
    ps.pdfWriter = false;
    return ps;
  }
  // The "[PROVIDERS & CONNECTORS]" block exactly as the next turn's context gets it
  // ("" when no connector is configured, matching the turn path).
  // The catalog as the engine renders it for the current head (its rule: the tool loop on and
  // this head runs it), for GET /api/connectors/catalog.
  std::string connectorsCatalogForHead() const {
    return connectorsCatalog(settings_.get().toolLoop && headRunsTools(hostBadge()));
  }
  std::string connectorsCatalog(bool headCallsTools = true) const {
    auto cs = connectorsView();
    if (cs.empty()) return std::string();
    return nimbus::orch::catalogText(cs, providerState(headCallsTools));
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
  bool applyProviderKey(const std::string& host, const std::string& posted) {
    const std::string env = keyEnvFor(host);
    const std::string key = storableKey(posted);   // stored without spaces around it (CUM-545)
    if (env.empty() || (!posted.empty() && key.empty())) return false;
    cfg_.setOverride(env, key);   // authoritative; an empty key erases the override
    saveSecrets();
    verify_.forget(host);         // a new key starts unverified; the web layer re-verifies it
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
    if (!validSelector(model)) return false;
    setModelPick(host, model);
    buildEngine();                // the head captures its model/route at build time
    ++keyGen_;                    // the web save-flow polls this counter
    return true;
  }

  // Monotonic key-change counter, surfaced as the providers' verify timestamp so the
  // web save-flow's poll observes the change (a hosted instance has no cheap verify).
  uint32_t keyGen() const { return keyGen_.load(); }

  // Served-by of the most recent turn (CUM-236), for the web chat's honest footer.
  bool lastFallback() const { return lastFallback_; }
  std::string lastServedBy() const { return lastServedBy_; }

  const Options& options() const { return opt_; }
  // The sub-agent provider ladder (device store::subPriority): the configured one,
  // else the head priority.
  std::string subPriority() const {
    const HostedSettings st = settings_.get();
    return st.subPrio.empty() ? st.provPrio : st.subPrio;
  }

  // ---- the owner's settings (CUM-543) ---------------------------------------
  HostedSettings settings() const { return settings_.get(); }
  // Apply a settings change, only if it persists on the instance volume. A new
  // time zone takes effect at once (the next turn's clock and /api/state read it);
  // clearing it returns to the operator's TZ, else UTC - the same zone a restart
  // would run. Callers validate first (hosted_settings.h). Thread-safe (the store
  // and the tz lock); called from the engine thread (/api/orch) and the HTTP
  // thread (/api/config).
  bool updateSettings(const std::function<void(HostedSettings&)>& fn) {
    const std::string tzBefore = settings_.get().devTz;
    if (!settings_.update(fn)) return false;
    const std::string tzAfter = settings_.get().devTz;
    if (tzAfter != tzBefore) hosted::applyTz(tzAfter.empty() ? envTz_ : tzAfter);
    return true;
  }
  // The time zone /api/state shows: the owner's, else the operator's TZ, else UTC.
  std::string clockTz() const {
    const std::string tz = settings_.get().devTz;
    if (!tz.empty()) return tz;
    return envTz_.empty() ? std::string("UTC") : envTz_;
  }

  // The web app's "Clear conversation" and the /clear command (device parity,
  // orchestrator.cpp clearConversation, CUM-511): a boundary row in the chat's own
  // history that every conversation read stops at, the chat's provider-side thread
  // and running notes dropped, and the scratchpad's active task cleared. Long-term
  // memory, files and the history itself are kept. Engine thread.
  void clearConversation(const std::string& chat) {
    capture(chat, "system", agent::kClearBoundaryText, agent::kClearBoundaryTag);
    if (eng_) eng_->clearChatConv(chat);
    memory_.erase(chat);
    scratch_.setActiveTask("");
    flush();
  }
  // "Erase the assistant's memory" (device requestMemoryClear): every chat's
  // running notes; the directive and long-term memory are kept. Engine thread.
  void clearRunningMemory() { memory_.clear(); }
  // Drop every provider-side conversation so the next turn starts clean (device
  // convReset). Engine thread.
  void resetConversations() { convId_.clear(); }

  // Per-provider sub-agent model picks (device subM_<slug>, "" = the head's model).
  bool applySubModel(const std::string& host, const std::string& model) {
    if (keyEnvFor(host).empty() || !validSelector(model)) return false;
    storePick(subModels_, subModelsPath(), host, model);
    return true;
  }
  // The model a spawn on `h` runs: the sub pick, else the head's model there.
  std::string subModelFor(const std::string& h) const {
    {
      std::lock_guard<std::mutex> lk(modelsMu_);
      auto it = subModels_.find(h);
      if (it != subModels_.end()) return it->second;
    }
    return h == kCumuloSlug ? cumuloModel() : modelFor(h);
  }
  // A provider key as it is stored (CUM-545), by the shared credential rule the
  // device applies (lib/core key_shape.h): the spaces around it dropped (a header
  // never carried them), then 1 to kKeyMax printable ASCII characters with no space
  // or control character inside. Keys are stored one KEY=VALUE per line in the
  // secrets file, so a line break in a value would add lines of the poster's
  // choosing that the next start reads as config. "" when it cannot be stored.
  // Only a new key from the page meets the field's own shape too (keyShapeErrorFor):
  // a stored key is held to this rule alone, so a key that worked before still loads.
  static constexpr size_t kKeyMax = 512;
  static std::string storableKey(const std::string& v) {
    const std::string t = nimbus::orch::trimSpaces(v);
    if (t.empty() || t.size() > kKeyMax || !nimbus::orch::credentialCharsOk(t)) return std::string();
    return t;
  }
  // The device's own check and owner sentence for a key posted to provider `host`
  // (POST /api/orch): its field's shape in key_shape.h. "" when `posted` fits.
  static std::string keyShapeErrorFor(const std::string& host, const std::string& posted) {
    const nimbus::orch::ProviderSlot* slot = nimbus::orch::findProviderSlot(host.c_str());
    const nimbus::orch::KeyShape* ks = slot ? nimbus::orch::keyShapeFor(slot->keyField) : nullptr;
    if (ks) return nimbus::orch::keyShapeError(*ks, posted);
    return storableKey(posted).empty() ? "That key is not valid. Paste just the key and save again." : "";
  }
  // The env names the secrets file may hold: the in-app provider keys, nothing else.
  static bool secretsName(const std::string& name) {
    return nimbus::orch::anySlotWhere([&name](const char* slug) { return name == Config::providerEnvName(slug); });
  }
  // A secrets-file line's stored form: the provider key it holds (storableKey), or
  // "" when it is not one clean provider key, logging the NAME (never the value)
  // with `what` saying what happens to it.
  static std::string storedSecret(const std::string& name, const std::string& value, const char* what) {
    const bool known = secretsName(name);
    const std::string stored = known ? storableKey(value) : std::string();
    if (stored.empty())
      std::fprintf(stderr, "[nimbusd] secrets: %s %s (not a clean provider key)\n", what,
                   known ? name.c_str() : "a line with an unknown name");
    return stored;
  }

  // A bounded selector token ("glm-4.5-flash", "zai/glm-4.5-flash"); "" clears.
  static bool validSelector(const std::string& model) {
    if (model.size() > 64) return false;
    for (char ch : model)
      if (!isalnum((unsigned char)ch) && ch != '.' && ch != '_' && ch != '-' && ch != '/' && ch != ':')
        return false;
    return true;
  }

  // The model a head requests: an operator override (opt.models[h]) else the
  // provider default. Wired into the provider deps as orchModel (subModelFor falls
  // back to it), so the
  // engine's budget derivation reads through here for the resolved head. Public so
  // the one-key path test can assert the router head resolves a model (CUM-288).
  std::string modelFor(const std::string& h) const {
    {
      std::lock_guard<std::mutex> lk(modelsMu_);
      auto it = opt_.models.find(h);
      if (it != opt_.models.end()) return it->second;
    }
    if (h == "openai")    return "gpt-5.6";
    if (h == "anthropic") return "claude-haiku-5-5";
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
  // A BOUNDED ring (CUM-546): it used to be an unbounded vector that grew for the
  // life of the daemon. Thread-safe: the engine thread writes, GET /api/log reads.
  static std::vector<std::string> harnessLog() {
    std::lock_guard<std::mutex> lk(logMu());
    return std::vector<std::string>(logLines().begin(), logLines().end());
  }
  static bool loggedContaining(const char* needle) {
    std::lock_guard<std::mutex> lk(logMu());
    for (const auto& l : logLines())
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }
  // The log tail as text (newest last), at most `maxBytes`; `failuresOnly` keeps
  // the lines that record a failure (the hosted /api/errlog/recent).
  static std::string logTail(size_t maxBytes, bool failuresOnly) {
    std::vector<std::string> picked;   // newest first, chosen under the lock
    {
      std::lock_guard<std::mutex> lk(logMu());
      size_t total = 0;
      for (auto it = logLines().rbegin(); it != logLines().rend(); ++it) {
        if (failuresOnly && !isFailureLine(*it)) continue;
        if (total + it->size() + 1 > maxBytes) break;
        total += it->size() + 1;
        picked.push_back(*it);
      }
    }
    std::string out;
    for (auto it = picked.rbegin(); it != picked.rend(); ++it) out += *it + "\n";
    return out;
  }
  // Mask anything shaped like a key in a log line before it is kept (provider error
  // texts can quote a key's prefix and last characters, and the log is served by
  // /api/log): the prefixes of the keys the page takes (sk-, which covers sk-ant-,
  // sk_, cumulo_sk_, Tavily's tvly-), and, for a key with no prefix (a Mistral
  // key), a long token named a key or a token (namedTokenSpans).
  static constexpr const char* kKeyMask = "[key]";   // what a masked key reads as
  static constexpr size_t kKeyMaskLen = 5;
  static std::string redactKeys(std::string l) {
    for (const char* pre : {"cumulo_sk_", "sk-", "sk_", "tvly-"}) {
      for (size_t at = l.find(pre); at != std::string::npos; at = l.find(pre, at + 1)) {
        size_t end = at + std::strlen(pre);
        while (end < l.size() && (isalnum((unsigned char)l[end]) || l[end] == '_' || l[end] == '-' || l[end] == '*' ||
                                  l[end] == '.'))
          end++;
        if (end - at > std::strlen(pre) + 3) l.replace(at, end - at, kKeyMask);
      }
    }
    const std::vector<std::pair<size_t, size_t>> spans = namedTokenSpans(l);
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) l.replace(it->first, it->second, kKeyMask);
    return l;
  }
  // A token that could be a key: 20 to 512 characters of letters, digits and
  // _ - . : + / with at least one letter and one digit (a long ordinary word has
  // no digit, so "the key internationalization" is left alone).
  static bool keyLikeToken(const std::string& t) {
    if (t.size() < 20 || t.size() > 512) return false;
    bool alpha = false, digit = false;
    for (unsigned char ch : t) {
      if (!tokenChar(ch)) return false;
      alpha = alpha || isalpha(ch);
      digit = digit || isdigit(ch);
    }
    return alpha && digit;
  }
  // "key", "keys", "token", "tokens" or "bearer" (any case), or a name ending in
  // one after a separator or a camel-case hump ("API_KEY", "apiKey", "accessToken").
  static bool namesAKey(const std::string& w) {
    std::string lw = w;
    for (char& ch : lw) ch = (char)tolower((unsigned char)ch);
    for (const char* k : {"key", "keys", "token", "tokens", "bearer"}) {
      const size_t n = std::strlen(k);
      if (lw.size() < n || lw.compare(lw.size() - n, n, k) != 0) continue;
      const size_t at = lw.size() - n;   // where the naming word starts
      if (at == 0 || !isalpha((unsigned char)w[at - 1])) return true;
      if (islower((unsigned char)w[at - 1]) && isupper((unsigned char)w[at])) return true;
    }
    return false;
  }
  // Text around a token that is not part of it: ASCII quotes, brackets, punctuation,
  // markdown emphasis, a JSON escape, and any non-ASCII byte (curly quotes).
  static bool edgeChar(unsigned char c) {
    return c != 0 && (c >= 0x80 || std::strchr("\"'`()[]{}<>,;:.!?*\\", c));
  }
  // The characters a key or token is made of (keyLikeToken), and those of a name.
  static bool tokenChar(unsigned char c) { return isalnum(c) || (c != 0 && std::strchr("_-.:+/", c)); }
  static bool nameChar(unsigned char c) { return isalnum(c) || c == '_' || c == '-'; }
  // The next whitespace-separated word at or after `i` (moving `i` past it), with
  // the quotes, brackets and punctuation around it left out: [first, second), empty
  // at the end of the text.
  static std::pair<size_t, size_t> nextWord(const std::string& l, size_t& i) {
    while (i < l.size() && isspace((unsigned char)l[i])) i++;
    size_t b = i;
    while (i < l.size() && !isspace((unsigned char)l[i])) i++;
    size_t e = i;
    while (b < e && edgeChar((unsigned char)l[b])) b++;
    while (e > b && edgeChar((unsigned char)l[e - 1])) e--;
    return {b, e};
  }
  // Each NAME=value or "name":"value" in word `w` (at `base` in the text) whose name
  // ends in key or token and whose value is key-like, as (offset, length) spans. One
  // word can hold several: compact JSON ({"model":"m","api_key":"..."}) and a query
  // string (?api_key=...&alt=json). False when it holds none. Every scan around a
  // separator is bounded (a quote run, a naming word, a key's longest form), so a
  // word of thousands of ':' costs a constant per separator, not a rescan of the word.
  static constexpr size_t kEdgeMax = 8;    // quotes and escapes around a name or value
  static constexpr size_t kNameMax = 64;   // a key's name ("ANTHROPIC_API_KEY") is shorter
  static bool namedValues(const std::string& w, size_t base, std::vector<std::pair<size_t, size_t>>& out) {
    bool any = false;
    for (size_t sep = w.find_first_of("=:"); sep != std::string::npos; sep = w.find_first_of("=:", sep + 1)) {
      if (!namesAKey(nameBefore(w, sep))) continue;
      const std::pair<size_t, size_t> v = valueAfter(w, sep);
      if (!keyLikeToken(w.substr(v.first, v.second - v.first))) continue;   // over kKeyMax never is
      out.push_back({base + v.first, v.second - v.first});
      any = true;
      sep = v.second - 1;   // a ':' inside the value (a Telegram token) is not a separator
    }
    return any;
  }
  // The name before the separator at `sep` (its closing quote skipped), bounded.
  static std::string nameBefore(const std::string& w, size_t sep) {
    size_t ne = sep;
    while (ne > 0 && sep - ne < kEdgeMax && edgeChar((unsigned char)w[ne - 1])) ne--;
    size_t nb = ne;
    while (nb > 0 && ne - nb < kNameMax && nameChar((unsigned char)w[nb - 1])) nb--;
    return w.substr(nb, ne - nb);
  }
  // The value after the separator at `sep` (its opening quote skipped) as [first,
  // second), at most kKeyMax + 1 characters: anything longer is no key.
  static std::pair<size_t, size_t> valueAfter(const std::string& w, size_t sep) {
    size_t vb = sep + 1;
    while (vb < w.size() && vb - sep <= kEdgeMax && edgeChar((unsigned char)w[vb])) vb++;
    size_t ve = vb;
    while (ve < w.size() && ve - vb <= kKeyMax && tokenChar((unsigned char)w[ve])) ve++;
    return {vb, ve};
  }
  // Whether a word inside the three-word window after "key" or "token" may be a
  // key. A model id of a known family ("claude-sonnet-5-20260301",
  // "gpt-4o-2024-08-06", "anthropic/claude-opus-5") is not. A word with URL or
  // query punctuation (a URL, "host/path?sig=...", JSON around a URL) is judged by
  // its parts: any part that looks like a secret makes it one (a webhook's or a
  // bot's path, a query value, a UUID, "bot123:AAH-...", a Slack webhook's last
  // part), and a user:password part always does. A host label counts only when it
  // looks random (a long run of letters and digits with a digit, a function or
  // tunnel URL's id), so "generativelanguage.googleapis.com" is left alone. The host
  // ends at the first character a host cannot hold, so whatever follows a bare host
  // in the same word (JSON around it, a second URL in a query) is read part by part.
  // A word with no scheme that holds URL or query punctuation or a percent escape is
  // read part by part too; any other word must be one key-like token ("the key
  // insight is internationalization's reach" is words, not a key).
  static bool secretInWindow(const std::string& w) {
    const size_t scheme = w.find("://");
    if (scheme == std::string::npos) {
      if (w.find_first_of("?=&@#\\\"%") == std::string::npos) return keyLikeToken(w) && !modelId(w);
      return anySecretPart(w, 0, w.size());
    }
    const size_t authority = scheme + 3;
    const size_t authEnd = std::min(w.find_first_of("/?#", authority), w.size());
    size_t host = authority;
    const size_t at = authEnd > authority ? w.rfind('@', authEnd - 1) : std::string::npos;   // the last '@'
    if (at != std::string::npos && at >= authority) {
      if (hasPassword(w, authority, at) || anySecretPart(w, authority, at)) return true;
      host = at + 1;
    }
    size_t hostEnd = host;
    while (hostEnd < authEnd && hostChar((unsigned char)w[hostEnd])) hostEnd++;
    return anySecretPart(w, 0, scheme) || randomHost(w, host, hostEnd) || anySecretPart(w, hostEnd, w.size());
  }
  // A character a host (with its port) may hold: letters, digits, '.', '-', ':' and
  // the brackets of an IPv6 literal. A percent escape ends the host, so an encoded
  // path or query stuck to it ("host%2Fpath", "host%3Fsig%3D...") is read part by part.
  static bool hostChar(unsigned char c) { return isalnum(c) || (c != 0 && std::strchr(".-:[]", c)); }
  // A user:password part with a password (anything after its first ':').
  static bool hasPassword(const std::string& w, size_t from, size_t at) {
    const size_t colon = w.find(':', from);
    return colon < at && colon + 1 < at;
  }
  // A model id: lowercase, holding a '-', no 16-character run of letters and digits
  // (a key's random part has one: "xoxb-..."), a known family after any
  // "<upstream>/". A UUID or "sdk-<uuid>" names no family, so it is none.
  static bool modelId(const std::string& t) {
    if (longRun(t) || t.find('-') == std::string::npos) return false;
    for (unsigned char ch : t)
      if (isupper(ch)) return false;
    return modelFamily(t.substr(t.find('/') == std::string::npos ? 0 : t.find('/') + 1));
  }
  // A run of 16 or more letters and digits (a key's random part), anywhere in `t`.
  static bool longRun(const std::string& t) {
    size_t run = 0;
    for (unsigned char ch : t) {
      run = isalnum(ch) ? run + 1 : 0;
      if (run >= 16) return true;
    }
    return false;
  }
  // A character that stays inside one part of a URL or a query: letters, digits,
  // RFC 3986 unreserved punctuation, '%' (an escape) and '+'. Anything else (/ ? &
  // = : @ # ; and the quotes, commas and brackets of JSON around a URL) separates.
  static bool partChar(unsigned char c) { return isalnum(c) || (c != 0 && std::strchr("-._~%+", c)); }
  // A part that looks like a secret: a 16-character run, or 20+ characters with
  // letters and digits that is no model id.
  static bool secretPart(const std::string& p) {
    if (longRun(p)) return true;
    bool alpha = false, digit = false;
    for (unsigned char ch : p) {
      alpha = alpha || isalpha(ch);
      digit = digit || isdigit(ch);
    }
    return p.size() >= 20 && alpha && digit && !modelId(p);
  }
  // Whether any part of t[from, to) looks like a secret. Each part is read once.
  static bool anySecretPart(const std::string& t, size_t from, size_t to) {
    for (size_t b = from; b < to;) {
      size_t e = b;
      while (e < to && partChar((unsigned char)t[e])) e++;
      if (e > b && secretPart(t.substr(b, e - b))) return true;
      b = e + 1;
    }
    return false;
  }
  // Whether a host t[from, to) has a label that looks random: a run of 16 or more
  // letters and digits holding a digit.
  static bool randomHost(const std::string& t, size_t from, size_t to) {
    size_t run = 0;
    bool digit = false;
    for (size_t i = from; i < to; ++i) {
      const unsigned char ch = (unsigned char)t[i];
      if (!isalnum(ch)) { run = 0; digit = false; continue; }
      digit = digit || isdigit(ch);
      if (++run >= 16 && digit) return true;
    }
    return false;
  }
  // A model id's family name at the start of `m` ("o4-mini" counts, "o4x" does not).
  static bool modelFamily(const std::string& m) {
    for (const char* fam : {"claude", "gpt", "mistral", "ministral", "magistral", "codestral", "devstral",
                            "pixtral", "glm", "gemini", "llama", "qwen", "deepseek", "grok"})
      if (m.rfind(fam, 0) == 0) return true;
    return m.size() > 2 && m[0] == 'o' && isdigit((unsigned char)m[1]) && m[2] == '-';
  }
  // The (offset, length) of each key-like token named a key: within the three words
  // after "key" or "token" ("my mistral key is q7Kx...", "the key: q7Kx...", but not
  // a URL or a model id there: secretInWindow), or the
  // value of a NAME=value or "name":"value" whose name ends in one
  // ("MISTRAL_API_KEY=q7Kx...", {"api_key":"q7Kx..."}). One pass over the words,
  // so it stays linear in the text however long it is.
  static std::vector<std::pair<size_t, size_t>> namedTokenSpans(const std::string& l) {
    std::vector<std::pair<size_t, size_t>> out;
    int window = 0;   // words left in which a key-like token is masked
    for (size_t i = 0; i < l.size();) {
      const std::pair<size_t, size_t> at = nextWord(l, i);
      if (at.first == at.second) continue;
      const std::string w = l.substr(at.first, at.second - at.first);
      const size_t named = out.size();
      namedValues(w, at.first, out);
      // In the window the whole word may be the secret; it covers any named value
      // inside it (spans never overlap: they are replaced back to front).
      if (window > 0 && secretInWindow(w)) {
        out.resize(named);
        out.push_back({at.first, w.size()});
      }
      window = namesAKey(w) ? 3 : std::max(window - 1, 0);
    }
    return out;
  }
  static bool isFailureLine(const std::string& l) {
    for (const char* k : {"fail", "FAIL", "error", "refused", "HTTP 4", "HTTP 5", "rejected", "timed out"})
      if (l.find(k) != std::string::npos) return true;
    return false;
  }
  // Cut the anatomy at `cap` without keeping the head of a word the cut splits: a
  // key saved after the turn is masked on read only when enough of it is left to
  // recognize, so the trailing run of key characters (at most a key's length) goes
  // too. A longer run is no key's tail and stays.
  static void capAnatomy(std::string& t, size_t cap) {
    if (t.size() <= cap) return;
    t.resize(cap);
    size_t b = t.size();
    while (b > 0 && t.size() - b <= kKeyMax && tokenChar((unsigned char)t[b - 1])) b--;
    if (t.size() - b <= kKeyMax) t.resize(b);
  }
  // The most recent turn's anatomy (device /api/lastturn): what the model got and
  // what it answered. "" before the first turn. Masked again as it is served: a key
  // saved after the turn (typed in chat before the instance held it) is masked
  // too, not only the keys held when the turn was captured (256 KB at most, a few
  // linear passes).
  std::string lastTurnAnatomy() const {
    std::string t;
    {
      std::lock_guard<std::mutex> lk(diagMu_);
      t = lastTurn_;
    }
    return redactKeys(redactHeldKeys(std::move(t)));
  }

 private:
  static constexpr size_t kLogLinesMax = 1000;
  static std::deque<std::string>& logLines() {
    static std::deque<std::string> v;
    return v;
  }
  static std::mutex& logMu() {
    static std::mutex m;
    return m;
  }
  static void installLog(bool echo) {
    {
      std::lock_guard<std::mutex> lk(logMu());
      logLines().clear();
    }
    static bool s_echo = false;
    s_echo = echo;
    agent::hlog::setSink(+[](const char* l) {
      {
        std::lock_guard<std::mutex> lk(logMu());
        logLines().push_back(redactKeys(l));
        while (logLines().size() > kLogLinesMax) logLines().pop_front();
      }
      if (s_echo) std::fprintf(stderr, "[harness] %s\n", l);
    });
  }

  // Build the /api/lastturn page from the engine's introspection hook (device
  // captureTurnDebug shape), capped so a huge prompt cannot grow it without bound.
  void captureTurn(const agent::TurnDebugEv& ev) {
    static constexpr size_t kCap = 256 * 1024;
    // The head (provider chain, the history the device sent, the section legend)
    // is the device's own (turn_anatomy.h), so both pages tell the same truth.
    std::string t = agent::turnAnatomyHeader(ev) +
                    (ev.instructions ? hostedPrompt(*ev.instructions) : std::string()) +
                    "\n\n===== 2. INPUT - the per-turn input block =====\n" +
                    (ev.inputs ? *ev.inputs : std::string()) +
                    "\n\n===== 3. OUTPUT - the raw model output =====\n" + (ev.rawOut ? *ev.rawOut : std::string());
    if (ev.transcriptBrief && !ev.transcriptBrief->empty())
      t += "\n\n===== 4. TOOL LOOP - the mid-turn rounds =====\n" + *ev.transcriptBrief;
    // Masked like /api/log and /api/errlog (CUM-546): a key typed in chat, quoted in
    // the directive or carried by a tool result is never served back in clear. The
    // held values go first and the cap last, so neither the prefix mask nor the cut
    // can leave part of a key that the exact-value mask no longer recognizes.
    t = redactKeys(redactHeldKeys(std::move(t)));
    capAnatomy(t, kCap);
    std::lock_guard<std::mutex> lk(diagMu_);
    lastTurn_ = std::move(t);
  }
  // Mask every secret value this instance holds wherever it appears, whatever its
  // shape (a Mistral or Tavily key has no prefix redactKeys can recognize), and a
  // cut copy of one too: tool-loop and job lines are clipped (ending in "...")
  // before the anatomy is built, so a held key can arrive as its first part only.
  // A run of at least kHeldHead of a key's leading characters is masked as far as
  // it matches, and when it stops matching before the key ends, on to the end of
  // the token it sits in: keys of one provider share their first characters
  // (every Anthropic key starts "sk-ant-api03-"), so a DIFFERENT key of that
  // provider must not lose only its prefix and keep its body in clear. A clipped
  // copy's clip mark ("..." is made of token characters) is masked with it.
  static constexpr size_t kHeldHead = 12;
  std::string redactHeldKeys(std::string t) const {
    std::vector<std::string> names = {"TAVILY_API_KEY", "TELEGRAM_BOT_TOKEN", "NIMBUSD_WEB_TOKEN"};
    for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; i++)
      names.push_back(Config::providerEnvName(nimbus::orch::kProviderSlots[i].slug));
    for (const auto& n : names) {
      const std::string v = cfg_.get(n);
      if (v.size() < 8) continue;   // too short to be a key, and masking it would garble text
      const size_t head = std::min(v.size(), kHeldHead);
      for (size_t at = t.find(v.data(), 0, head); at != std::string::npos;
           at = t.find(v.data(), at + kKeyMaskLen, head)) {
        size_t len = head;
        while (len < v.size() && at + len < t.size() && t[at + len] == v[len]) len++;
        if (len < v.size())
          while (at + len < t.size() && tokenChar((unsigned char)t[at + len])) len++;
        t.replace(at, len, kKeyMask);
      }
    }
    return t;
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
  // file (KEY=VALUE lines, trailing CR/LF trimmed). Only provider key names with a
  // key-shaped value load: the file is never a general config channel. Never logs
  // a value. A key an earlier build stored with spaces around it (the page posts
  // the field as typed, and that build stored it verbatim; it worked, since HTTP
  // drops them) loads trimmed and the file is rewritten trimmed, instead of the
  // key vanishing on the first start after an update (CUM-545).
  void loadSecrets() {
    std::string blob;
    if (!fsutil::readFile(secretsPath(), blob)) return;
    std::istringstream is(blob);
    std::string line;
    bool trimmed = false;
    while (std::getline(is, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
      const std::string stored = storedSecret(k, v, "ignored");
      if (stored.empty()) continue;
      cfg_.setOverride(k, stored);
      if (stored == v) continue;
      trimmed = true;
      std::fprintf(stderr, "[nimbusd] secrets: %s had spaces around it; stored trimmed\n", k.c_str());
    }
    if (trimmed) saveSecrets();
  }
  // Persist the current in-app overrides to the owner-only secrets file. Written
  // 0600 from creation (never a world-readable window - the general atomic writer
  // creates 0644 then chmods, which leaves a race and a 0644 file if the process
  // dies between rename and chmod), then atomically renamed into place. Never logs a
  // value.
  void saveSecrets() {
    std::string blob;
    for (const auto& kv : cfg_.overrides()) {
      // The writer refuses what the loader would misread (the second guard behind
      // the web layer's shape check): one clean KEY=VALUE line per provider key.
      const std::string v = storedSecret(kv.first, kv.second, "not writing");
      if (!v.empty()) blob += kv.first + "=" + v + "\n";
    }
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
  void loadModels() { readPicks(modelsPath(), opt_.models); }
  void loadSubModels() { readPicks(subModelsPath(), subModels_); }
  static void readPicks(const std::string& path, std::map<std::string, std::string>& out) {
    std::string blob;
    if (!fsutil::readFile(path, blob)) return;
    std::istringstream is(blob);
    std::string line;
    while (std::getline(is, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
      if (!k.empty() && !v.empty() && !keyEnvFor(k).empty()) out[k] = v;
    }
  }
  // The owner's settings (CUM-543): the env/options are the defaults, a value the
  // owner saved in the app wins. The process time zone is put in the environment
  // here, before any thread starts, so a later change replaces it in place.
  void loadSettings() {
    HostedSettings def;
    def.orchHost = opt_.orchHost;
    def.provPrio = opt_.priority;
    def.subPrio = opt_.subPriority;
    def.devName = opt_.devName;
    def.toolLoop = opt_.toolLoop;
    settings_.init(memDir() + "/settings.json", def);
    const char* env = std::getenv("TZ");
    envTz_ = env ? env : "";
    const std::string tz = settings_.get().devTz;
    if (!tz.empty() || envTz_.empty()) hosted::applyTz(tz);
  }

  // Store (or clear, when `model` is empty) one head's pick and persist the set.
  // The picks are read by the HTTP thread (GET /api/orch) while the engine thread
  // writes them, so the map sits behind modelsMu_.
  void setModelPick(const std::string& host, const std::string& model) {
    storePick(opt_.models, modelsPath(), host, model);
  }
  void storePick(std::map<std::string, std::string>& picks, const std::string& path,
                 const std::string& host, const std::string& model) {
    std::string blob;
    {
      std::lock_guard<std::mutex> lk(modelsMu_);
      if (model.empty()) picks.erase(host);
      else picks[host] = model;
      for (const auto& kv : picks) blob += kv.first + "=" + kv.second + "\n";
    }
    fsutil::writeFileAtomic(path, blob);
  }

  // ---- memory + tools -------------------------------------------------------
  void buildMemory() {
    const bool savedWidth = loadEmbedCfg();   // the owner's embed choice from the app (CUM-540), else the env defaults
    vec_.configure(opt_.embedDims);
    vec_.setMaxEntries(opt_.maxVectors);
    // Rehydrate the durable stores (each is tolerant of an absent/torn file).
    std::string blob;
    if (fsutil::readFile(vectorsPath(), blob)) vec_.deserialize(blob);
    reconcileVectorWidth(savedWidth);
    if (fsutil::readFile(scratchPath(), blob)) scratch_.deserialize(blob);
    if (fsutil::readFile(memConfigPath(), blob)) memCfg_.deserialize(blob);

    epi_.reset(new orch::AppendLogEpisodicStore(epiFs_, episodicDir(), /*recentCap=*/256));
    epi_->hydrate();  // rebuild index/cache from the day-streams
    epiSeq_ = epi_->nextIdHint();   // row ids continue past the stored history

    files_.reset(new PosixFiles(filesDir()));

    orch::MemoryContext mc;
    mc.vec = &vec_;
    mc.scratch = &scratch_;
    mc.cfg = &memCfg_;
    mc.episodic = epi_.get();
    mc.nowHours = [] { return nowHours(); };
    mc.embed = [this](const std::string& text, std::string& err) { return embed(text, err); };
    // Memory writes survive an embedding outage (device parity, lane P): the
    // queue persists beside the vectors with the same atomic writer, loaded
    // after them so a crash replay is recognised by id on the first drain.
    if (fsutil::readFile(pendingPath(), blob)) pending_.deserialize(blob);
    pending_.markClean();
    mc.pending = &pending_;
    mc.persistPending = [this] { return persistPending(); };
    mc.park = &park_;
    mc.nowMs = [] { return steadyMs(); };
    mc.embedProvider = [this] { return opt_.embedHost; };
    memCtx_ = mc;
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

  // CUM-538 (device parity, src/agent/orchestrator.cpp): a head that calls a name
  // that is not a tool gets a way back. A configured connector names the provider
  // whose sub-agents run it (this instance's own registry), and a spawn field of
  // the reply carries the REAL remaining spawn room (the [SPAWN CAPACITY] number).
  // Both read live state and run only on an unknown name, never on a real tool call.
  void installToolHints() {
    orch::ToolRegistry::UnknownToolHints hints;
    hints.connectorProvider = [this](const std::string& name) {
      return nimbus::orch::connectorProviderForName(conns_.parsed(), name);
    };
    hints.spawnRoom = [this] { return orch::spawnRoomFor(jobs_ ? jobs_->pendingCount() : 0); };
    reg_.setUnknownToolHints(hints);
  }

  void registerDeviceStatusTool() {
    reg_.add("device.status", "Report the instance's current state.",
             [this](ArduinoJson::JsonObjectConst, const orch::Principal&) {
               JsonDocument d;
               d["name"] = settings_.get().devName;   // owner-set: escaped, never spliced
               d["mode"] = "orchestrator";
               d["host"] = "nimbusd";
               d["vectors"] = vec_.size();
               std::string out;
               serializeJson(d, out);
               return orch::ToolResult::ok(out);
             },
             R"({"type":"object","properties":{}})");
  }

  // Where an embedding call for `provider` goes (CUM-540, device parity with
  // src/agent/adapters/embeddings.cpp + orch::embedRouteFor): a direct key wins;
  // without one, the Cumulo key carries the call through the router on the SAME
  // upstream and model, so the vectors stay comparable if a direct key is added
  // later ("cumulo" itself is the device's router route, /router/openai/v1). False
  // when no key can carry it.
  struct EmbedRoute {
    std::string host, path, key, dialect;   // dialect: the buildEmbeddingRequest provider
    bool viaRouter = false;
  };
  // Where an embedding's spend is filed: a routed one on the router, its upstream named.
  struct EmbedMeter {
    std::string provider, model;
  };
  static EmbedMeter embedMeterOf(const EmbedRoute& route, const std::string& provider, const std::string& model) {
    if (!route.viaRouter) return EmbedMeter{provider, model};
    return EmbedMeter{kCumuloSlug, provider != kCumuloSlug ? provider + "/" + model : model};
  }
  bool embedRoute(const std::string& provider, EmbedRoute& r) const {
    const orch::EmbedRoute er = orch::embedRouteFor(provider);
    if (!er.known) return false;
    r = EmbedRoute{};
    r.dialect = provider;
    if (!er.viaCumuloRouter && !cfg_.providerKey(provider).empty()) {
      r.host = provider == "mistral" ? "api.mistral.ai" : "api.openai.com";
      r.path = er.path;
      r.key = cfg_.providerKey(provider);
      return true;
    }
    if (!hasCumulo()) return false;
    r.host = kCumuloHost;
    r.path = er.viaCumuloRouter ? std::string(er.path) : "/router/" + provider + er.path;
    r.key = cumuloKey();
    r.viaRouter = true;
    return true;
  }

  // Text -> quantized embedding, on the honest 2-arg Embedder contract (CUM-435):
  // an empty return writes the REAL cause into `err` using the SAME token vocabulary
  // the device seam emits (src/agent/adapters/embeddings.cpp), so orch::classifyEmbedFail
  // names the true failure (no key / key rejected / HTTP nnn / parse: ...) instead of a
  // fixed guess. `err` is left empty on success.
  std::vector<int8_t> embed(const std::string& text, std::string& err) {
    err.clear();
    if (!embedShim_.empty()) { err = embedShim_; return {}; }
    if (!opt_.embeddings) { err = "no model"; return {}; }
    return embedWith(text, err, opt_.embedHost, opt_.embedModel, opt_.embedDims);
  }

  // The machine code of a provider error body ({"error":"x"}, or {"error":{"code"
  // or "type"}}): bounded and limited to [a-z0-9_.-], so it is secret-safe.
  static std::string errorCodeOf(const std::string& body) {
    ArduinoJson::JsonDocument d;
    if (deserializeJson(d, body)) return std::string();
    std::string c;
    ArduinoJson::JsonVariantConst e = d["error"];
    if (e.is<const char*>()) c = e.as<const char*>();
    else if (e["code"].is<const char*>()) c = e["code"].as<const char*>();
    else if (e["type"].is<const char*>()) c = e["type"].as<const char*>();
    if (c.size() > 48) c.resize(48);
    for (char ch : c)
      if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-'))
        return std::string();
    return c;
  }

  // One embeddings call with an explicit (possibly candidate) config: the live
  // embed above and the web app's "verify before save" both run through here.
  // Engine thread only (it uses the engine's transport).
 public:
  std::vector<int8_t> embedWith(const std::string& text, std::string& err, const std::string& provider,
                                const std::string& model, int dims) {
    err.clear();
    EmbedRoute route;
    if (!embedRoute(provider, route)) { err = "no embeddings key for " + provider; return {}; }
    if (text.empty())  { err = "empty text"; return {}; }
    if (model.empty()) { err = "no model"; return {}; }
    // Lane DEF: at the monthly spend ceiling no embedding is requested (the device's
    // embeddings ride the per-provider park, which a hosted instance does not consult).
    if (!orch::paidCallAllowed(orch::SpendSeam::Embed)) { err = orch::kSpendFrozenError; return {}; }
    agent::HttpRequest req;
    req.method = "POST";
    req.host = route.host;
    req.path = route.path;
    req.timeoutMs = 30000;
    req.headers.push_back({"Content-Type", "application/json"});
    req.headers.push_back({"Authorization", "Bearer " + route.key});
    // One provider decision drives the body dialect (CUM-469: mistral never gets
    // OpenAI fields; "cumulo" is the OpenAI dialect).
    req.body = orch::buildEmbeddingRequest(route.dialect, model, text, dims);
    agent::HttpResponse resp;
    std::string httpErr;
    const EmbedMeter meter = embedMeterOf(route, provider, model);
    if (!http_->exec(req, resp, httpErr)) {
      // Lane QW4 (CUM-776): sent and no usable answer: it may have been billed, so the
      // text's estimate counts.
      if (orch::sentWithoutAnswer(resp.status, httpErr))
        orch::meterSeamAnswerBody(orch::SpendSeam::Embed, meter.provider.c_str(), meter.model.c_str(), nullptr,
                                  text.size(), 0);
      err = "connect failed";
      return {};
    }
    if (resp.status == 401 || resp.status == 403)    { err = "key rejected"; return {}; }
    if (resp.status < 200 || resp.status >= 300) {
      // Device parity (embeddings.cpp): append the provider's machine code, so a
      // billing refusal (429 credit_balance_exhausted) classifies as billing.
      err = "HTTP " + std::to_string(resp.status);
      const std::string code = errorCodeOf(resp.body);
      if (!code.empty()) err += " " + code;
      return {};
    }
    // Lane DEF: an answered embedding is billed (its reported usage, else an estimate).
    orch::meterSeamAnswerBody(orch::SpendSeam::Embed, meter.provider.c_str(), meter.model.c_str(),
                              resp.body.c_str(), text.size(), 0);
    std::vector<float> f;
    std::string perr;
    if (!orch::parseEmbeddingResponse(resp.body.c_str(), 0, f, perr)) {
      err = "parse: " + perr;
      return {};
    }
    return orch::VectorMemory::quantize(f);
  }

  // The web app's embedding settings save (device parity, web_memory.cpp
  // handleEmbedCfgPost): refused while memories exist unless `reset` erases them
  // (vectors of two models cannot be compared). `width` is the width the live
  // check read from the model (web_api.h embedCfgPost): a width is never saved
  // unchecked, since a wrong one made every later memory write fail. Persisted on
  // the instance volume. Returns "" or the refusal sentence; `status` carries the
  // HTTP status. Engine thread only.
  std::string applyEmbedCfg(const std::string& provider, const std::string& model, int width, bool reset,
                            int& status) {
    status = 400;
    const std::string why = embedCfgRefusal(provider, model, width);
    if (!why.empty()) return why;
    if (width <= 0) return "Couldn't read the model's embedding size. Try again.";
    if (!reset && vec_.size() > 0) {
      status = 409;
      return "Memories exist for the current model. Save again and choose to erase them to switch.";
    }
    if (reset) vec_.flushAll();
    opt_.embedHost = provider;
    opt_.embedModel = model;
    opt_.embedDims = width;
    vec_.configure(width);
    // The store first, the config last: a restart between the two writes finds an
    // EMPTY store at the new width under the old config, which the load simply
    // re-widens (reconcileVectorWidth), never old vectors under a new config.
    flush();
    saveEmbedCfg();
    status = 200;
    return std::string();
  }
  // The shape check for an embedding candidate ("" = valid): a provider this
  // instance can embed with, a model name that fits one line of embed.txt, and a
  // width in range (0 = the model's own).
  static std::string embedCfgRefusal(const std::string& provider, const std::string& model, int dims) {
    if (!orch::embedRouteFor(provider).known || provider == "custom") return "Choose Cumulo Nimbus, OpenAI or Mistral.";
    if (model.empty() || !validSelector(model)) return "Choose a model.";
    if (dims < 0 || dims > 4096) return "Dimensions must be 0 to 4096.";
    return std::string();
  }

 private:
  std::string embedCfgPath() const { return memDir() + "/embed.txt"; }

 public:
  static constexpr const char* kEmbedProbeText = "nimbus embedding check";   // the verify/width probe

 private:
  // True when the saved config set the width (the owner's choice, not the env's).
  bool loadEmbedCfg() {
    std::string blob;
    if (!fsutil::readFile(embedCfgPath(), blob)) return false;
    std::istringstream is(blob);
    std::string line;
    bool savedWidth = false;
    while (std::getline(is, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      if (k == "provider" && orch::embedRouteFor(v).known) opt_.embedHost = v;
      else if (k == "model" && !v.empty() && validSelector(v)) opt_.embedModel = v;
      else if (k == "dims") {
        const int dims = std::atoi(v.c_str());
        if (dims > 0 && dims <= 4096) {
          opt_.embedDims = dims;
          savedWidth = true;
        }
      }
    }
    return savedWidth;
  }
  // The loaded vector store and the embedding config must agree on the width:
  // vectors of another width can never be compared with what the model returns
  // now (every write fails and recall finds nothing, silently).
  //  * an empty store takes the configured width (its blob header can carry the
  //    old one, device CUM-469);
  //  * vectors at a width the owner's saved config does not produce (a restart
  //    inside a model switch) are copied aside to vectors.bin.mismatch-<time> and
  //    memory starts empty, said in the log; if the copy cannot be written, nothing
  //    is erased: the stored width is kept (writes fail honestly) and the log says so;
  //  * with no saved config the stored width wins (the env default never chose it).
  void reconcileVectorWidth(bool savedWidth) {
    const int want = opt_.embedDims, have = vec_.dims();
    if (have == want || vec_.adoptWidthIfEmpty(want)) return;
    const std::string n = std::to_string(vec_.size()), haveS = std::to_string(have), wantS = std::to_string(want);
    if (!savedWidth) {
      opt_.embedDims = have;
      memLog("memory: the stored memories are " + haveS + " wide, so embeddings use that width");
      return;
    }
    const std::string kept = vectorsPath() + ".mismatch-" + std::to_string((long long)time(nullptr));
    if (!fsutil::writeFileAtomic(kept, vec_.serialize())) {
      opt_.embedDims = have;
      memLog("memory: " + n + " stored memories are " + haveS + " wide but the saved embedding model is " + wantS +
             " wide, and a copy could not be written, so they are kept as they are; save the embedding settings " +
             "again with erase to start fresh");
      return;
    }
    memLog("memory: " + n + " stored memories are " + haveS + " wide but the saved embedding model is " + wantS +
           " wide; they cannot be compared, so memory starts empty (the old store is kept as " +
           kept.substr(memDir().size() + 1) + ")");
    vec_.flushAll();
    vec_.configure(want);
    fsutil::writeFileAtomic(vectorsPath(), vec_.serialize());
  }
  static void memLog(const std::string& line) {
    std::fprintf(stderr, "[nimbusd] %s\n", line.c_str());
    agent::hlog::logf("%s", line.c_str());
  }
  void saveEmbedCfg() {
    fsutil::writeFileAtomic(embedCfgPath(), "provider=" + opt_.embedHost + "\nmodel=" + opt_.embedModel +
                                                "\ndims=" + std::to_string(opt_.embedDims) + "\n");
  }

  // ---- config / provider deps ----------------------------------------------
  agent::HarnessConfig config() {
    agent::HarnessConfig c;
    auto& p = c.provider;
    p.hasKey = [this](const std::string& h) { return keyedForEngine(h); };
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
    p.orchHost = [this] { return settings_.get().orchHost; };
    p.providerPriority = [this] { return settings_.get().provPrio; };
    p.subPriority = [this] { return subPriority(); };
    p.orchModel = [this](const std::string& h) { return modelFor(h); };
    p.subModel = [this](const std::string& h) { return subModelFor(h); };
    p.modelChoices = [this](const std::string& h) { return modelChoicesFor(h); };
    p.convId = [this] { return convId_; };
    p.setConvId = [this](const std::string& v) { convId_ = v; };
    p.customBase = [] { return std::string(); };
    p.customKey = [] { return std::string(); };
    p.customConv = [] { return std::string("openai"); };
    p.customModel = [] { return std::string(); };

    c.loop.toolLoopOn = [this] { return settings_.get().toolLoop; };
    c.loop.rounds = [] { return 12; };
    c.loop.deadlineS = [] { return 600; };
    c.loop.resultCap = [] { return 4096; };
    c.loop.totalCap = [] { return 24576; };

    c.budget.overBudget = [](const std::string&) { return false; };
    c.budget.recordTokens = [](const std::string&, uint32_t, uint32_t, uint32_t, uint32_t,
                               const std::string&, const std::string&) {};
    // Lane DEF: every turn's spend counts toward the monthly spend ceiling (a fold runs
    // inside a turn and does not end its pending rounds).
    c.budget.recordSpend = [this](const orch::TurnSpend& spend, const std::string& tag) {
      ceiling_->commit(spend, tag != "compact");
    };
    c.ttsEnabled = [] { return false; };
    c.deviceName = [this] { return settings_.get().devName; };
    return c;
  }

  agent::providers::ProviderDeps providerDeps() {
    agent::providers::ProviderDeps pd;
    pd.http = http_;
    pd.key = [this](const char* h) { return cfg_.providerKey(h); };
    pd.orchModel = [this](const char* h) { return modelFor(h); };
    pd.toolLoopOn = [this] { return settings_.get().toolLoop; };
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
    pd.monoMs = pd.nowMs;   // lane QW4: a request's held time (the no-answer estimate)
    pd.freeHeap = [this] { return mem_.freeBytes(); };
    // Connector attach seams (CUM-424): BYOK heads advertise the instance's
    // configured connectors exactly as a device with direct keys does
    // (builtinsOnly=false; the portable guard already keeps disabled, wrong-prov
    // and private-URL entries off the wire). They read connectorsView(): no
    // code_interpreter (no capture here) and no Studio connector the workspace
    // answer does not list, the same view the catalog describes. The cumulo router head overrides
    // these with builtinsOnly=true (routerHeadDeps): router lanes carry no private
    // connectors, the same boundary the device draws on shared keys.
    pd.attachOpenAI = [this](JsonDocument& doc) {
      nimbus::orch::attachOpenAIWire(doc, connectorsView(), bearerFn(), false);
    };
    pd.attachMistral = [this](JsonDocument& doc) {
      nimbus::orch::attachMistralWire(doc, connectorsView(), false);
    };
    pd.attachAnthropic = [this](JsonDocument& doc) {
      nimbus::orch::attachAnthropicWire(doc, connectorsView(), bearerFn(), false);
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
      nimbus::orch::attachMistralWire(doc, connectorsView(), /*builtinsOnly=*/true);
    };
    return pd;
  }

  // The provider deps for one Cumulo router head attempt on `model` (a bare id
  // after the "<upstream>/" split): device parity with src/agent/orchestrator.cpp's
  // cumulo head (CUM-242). The router override (routerBase/routerKey) makes the
  // NATIVE upstream wire travel through /router/<upstream> with the Cumulo key as
  // the bearer; the upstream loop steps read key()/orchModel() for their own host
  // arg, so both resolve to the router credential and the selected model. The head
  // advertises provider BUILT-IN tools only: the owner's private connectors stay
  // off the shared router key (attach*Wire builtinsOnly=true). Every request goes
  // through tap_, which records the router's reason when it refuses one.
  agent::providers::ProviderDeps routerHeadDeps(const std::string& model) {
    auto pd = providerDeps();
    pd.http = &tap_;
    pd.routerBase = [] { return std::string(kCumuloHost); };
    pd.routerKey = [this] { return cumuloKey(); };
    pd.key = [this](const char*) { return cumuloKey(); };
    pd.orchModel = [model](const char*) { return model; };
    pd.attachOpenAI = [this](JsonDocument& doc) {
      nimbus::orch::attachOpenAIWire(doc, connectorsView(), nullptr, /*builtinsOnly=*/true);
    };
    pd.attachMistral = [this](JsonDocument& doc) {
      nimbus::orch::attachMistralWire(doc, connectorsView(), /*builtinsOnly=*/true);
    };
    pd.attachAnthropic = [this](JsonDocument& doc) {
      nimbus::orch::attachAnthropicWire(doc, connectorsView(), nullptr, /*builtinsOnly=*/true);
    };
    return pd;
  }

  // One head attempt on the router `selector` ("<upstream>/<model>", or a bare id
  // for the openai upstream): the SAME portable split the device head and the
  // sub-session adapter use (resolveRouterRoute). anthropic/openai/mistral run the
  // upstream's native head (its real tool loop when `tools` is set and the loop is
  // on); zai and any upstream the native fabric does not drive keep the single-shot
  // chat-completions path over /router/<upstream> with json_object (GLM behind the
  // router ignores json_schema, CUM-495).
  //
  // Every router selector shares the engine's ONE conversation slot for the
  // "cumulo" host, so the marker an upstream leaves is stored tagged with that
  // upstream and handed back only to it: a Mistral conversation id or an Anthropic
  // "messages" marker never rides to the OpenAI Responses wire as
  // previous_response_id after a pick switch.
  bool runRouterHead(const std::string& selector, HeadCall& c, HeadOut& o) {
    const nimbus::orch::RouterRoute rr = nimbus::orch::resolveRouterRoute(selector);
    std::string conv = routerConvFor(c.conv, rr.upstream);
    const bool ok = runRouterUpstream(rr, conv, c, o);
    c.conv = conv.empty() ? std::string() : rr.upstream + kRouterConvSep + conv;
    return ok;
  }
  static constexpr const char* kRouterConvSep = "~";
  // The marker `upstream` may continue: only one it wrote itself, and never a
  // stateless marker (the providers' "responses" / "messages" / "chat" name no
  // server-side thread; the loop's "responses" would otherwise become the
  // single-shot turn's previous_response_id once the tool loop is switched off).
  static std::string routerConvFor(const std::string& stored, const std::string& upstream) {
    const std::string tag = upstream + kRouterConvSep;
    if (stored.compare(0, tag.size(), tag) != 0) return std::string();
    const std::string marker = stored.substr(tag.size());
    for (const char* stateless : {"responses", "messages", "chat"})
      if (marker == stateless) return std::string();
    return marker;
  }
  bool runRouterUpstream(const nimbus::orch::RouterRoute& rr, std::string& conv, HeadCall& c, HeadOut& o) {
    const std::string& ins = c.ins;
    const std::string& inp = c.inp;
    const agent::HeadTools* tools = c.tools;
    auto pd = routerHeadDeps(rr.model);
    if (rr.upstream == "anthropic")
      return agent::providers::orchTurnAnthropic(pd, conv, ins, inp, o.out, o.err, tools, o.usage);
    if (rr.upstream == "mistral") {
      // The router admits only POST /v1/conversations (a new conversation), never an
      // append to /v1/conversations/<id>: start fresh every turn. The prompt already
      // carries the recent conversation, so nothing is lost.
      conv.clear();
      return agent::providers::orchTurnMistral(pd, conv, ins, inp, o.out, o.err, tools, o.usage);
    }
    if (rr.upstream == "openai")
      return agent::providers::orchTurnOpenAI(pd, conv, ins, inp, o.out, o.err, tools, o.usage);
    // zai / any other upstream: the device's ONE seam (CUM-544) - base and key from
    // the router override above, /router/<upstream>, json_object, and for zai the
    // bounded reasoning effort + 50 s head budget.
    agent::providers::configureRouterCompatHead(pd, rr.upstream, rr.model);
    return agent::providers::orchTurnCustom(pd, conv, ins, inp, o.out, o.err, tools, o.usage);
  }

  // The Cumulo head turn (CUM-539). A refusal never leaves the assistant dead:
  //   * the picked model is not offered (model_not_priced, a 404, or the upstream's
  //     own 400 "Invalid model" passed through): THIS turn is
  //     answered on the last selector that worked, else (none, or it is refused
  //     too) on the router default, with a first line naming the refusal and the
  //     fix; the pick moves to the model that answered - so the next turn answers
  //     too, and GET /api/orch shows the model actually in use;
  //   * that upstream is paused or its route is not allowed (funding_cap_reached,
  //     provider_disabled, endpoint_not_allowed): this turn is answered on the
  //     router default, the pick is kept;
  //   * account-wide refusals (credits, spending limit) cannot be answered on any
  //     model, so the failure reply says what to do instead of "rejected this
  //     device's key".
  // Only a refusal of the attempt's FIRST request is re-run: no tool has executed
  // yet, so the retry cannot repeat a side effect.
  bool cumuloTurn(HeadCall& c, HeadOut& o) {
    const std::string picked = cumuloModel();
    const LadderResult r =
        runLadder(tap_, picked, lastGoodCumulo_, kCumuloModel, [&](const std::string& sel, bool fresh) {
          if (fresh) {   // a fresh conversation and a clean output for another model
            c.conv.clear();
            o.out.clear();
            o.err.clear();
          }
          return runRouterHead(sel, c, o);
        });
    if (!r.ok) {
      noteRefusal(r.lastKind, r.lastTried, o);
      return false;
    }
    lastGoodCumulo_ = r.used;
    if (!r.fellBack()) return true;
    // A background compaction call (the fold runs through this same head, with no
    // turn in flight) gets the answer only: no notice prefixed into the summary,
    // and the owner's pick is never moved by housekeeping.
    if (!eng_ || !eng_->turnInFlight()) return true;
    // Only a model the router does not offer moves the pick, and only once the
    // fallback has answered (a failed fallback leaves the owner's choice alone).
    settleFallback(r, picked, kCumuloModel, o.out, [this] { return cumuloModel(); },
                   [this](const std::string& p) { setModelPick(kCumuloSlug, p); });
    return true;
  }

  // A refusal no other model can answer: the badge learns what it means, and the
  // failure reply names the cause and the fix instead of "rejected this device's key".
  void noteRefusal(RefusalKind kind, const std::string& selector, HeadOut& o) {
    if (kind == RefusalKind::Auth) verify_.record(kCumuloSlug, 0, "");
    if (kind == RefusalKind::Account) verify_.record(kCumuloSlug, -1, "nocredits");
    const std::string cause = refusalCause(kind, selector);
    if (!cause.empty()) o.err = cause;
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
    // CUM-544: the ONE Z.ai head policy the device heads use (json_object,
    // reasoning_effort "low", the 50 s head budget under Z.ai's gateway cut).
    agent::providers::applyZaiHeadPolicy(pd);
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
    d.recall = [this](const std::string& q, const orch::Principal& who) { return recallFor(q, who); };
    // Queued memory writes stored since this chat's last turn (lane P).
    d.memNotes = [this](const std::string& chat) {
      return pending_.takeNotes(access_->principalFor(chat).ns);
    };
    d.composeInputs = [this](const std::string& chat) {
      agent::ComposeInputs in;
      const HostedSettings st = settings_.get();
      in.devName = st.devName;
      in.directive = st.directive;   // "" = the shipped default (effectiveDirective)
      in.hostLabel = lastHost_.empty() ? std::string("(picking)") : lastHost_;
      in.recentConversation = recentWindow(chat);
      in.runningMemory = runningMemory(chat);
      // Who this message is from (device W10): the prompt names the speaker's real
      // role, so an approved member is never addressed as the owner.
      in.speakerRole = orch::roleName(access_->roleOf(chat));
      in.speakerLabel = access_->labelOf(chat);
      fillHostedFacts(in);
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
    d.connectorsCatalog = [this](bool headTools) { return connectorsCatalog(headTools); };
    d.spawnPause = [](const std::string& p) { return pauseOf(p, nimbus::orch::PaidSeam::Spawn); };
    d.headRunsTools = [this](const std::string& h) { return headRunsTools(h); };
    d.modelChoices = [this](const std::string& p) { return config().provider.modelChoices(p); };
    d.episodicCaptureUser = [this](const std::string& c, const std::string& t,
                                   const std::string& tag) {
      curUserRowId_ = capture(c, "user", t, tag);   // the in-flight row rides [USER], not the window
    };
    d.firstAllowedChat = [] { return std::string("owner"); };
    d.journalGc = [] {};

    d.hooks.onToolCall = [this](const orch::HeadToolCall& c) {
      cur_.toolCalls.push_back(c.name + "(" + trunc(c.argsJson, 200) + ")");
    };
    d.hooks.onToolResult = [this](const orch::HeadToolResult& r) {
      cur_.toolResults.push_back(trunc(r.output, 300));
    };
    d.hooks.onTurnDebug = [this](const agent::TurnDebugEv& ev) { captureTurn(ev); };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) {
      lastHost_ = ev.host;
      if (ev.ok && !ev.host.empty()) verify_.markWorking(ev.host);   // the key answered (CUM-545)
      if (ev.ok) lastUsage_ = ev.usage;   // survives an engine rebuild, unlike the engine's own
      // The in-flight row id must not outlive its turn: a later synthesis or
      // scheduled turn (no user row of its own) would hide that message from its
      // window (device parity, orchestrator.cpp onTurnEnd).
      curUserRowId_.clear();
      // Served-by disclosure (CUM-236): remember whether this turn was served by a
      // fallback provider/model and how to say it, so the web chat can surface it.
      lastFallback_ = ev.fallback;
      lastServedBy_ = ev.host + (ev.servedModel.empty() ? std::string() : (" " + ev.servedModel));
    };

    wireApply(d);

    registerHeads(d);
    // A rebuild (a key or model save) must not zero the session usage (CUM-546):
    // the counters live in the TurnEngine, so carry the old engine's totals over.
    if (eng_) {
      carriedUsage_ += eng_->sessionUsage();
      carriedTurns_ += eng_->turnCount();
    }
    eng_.reset(new agent::TurnEngine(std::move(d)));
  }

  // What the prompt says about THIS instance (CUM-542): the real clock in the owner's
  // time zone (the host clock is NTP-synced; the composer used to say "date-time
  // UNKNOWN" on every hosted turn, so the model refused date questions), the engine
  // version, and the hardware a hosted instance really has (none of the desk
  // device's ring, screen, mic, speaker or battery; always online).
  void fillHostedFacts(agent::ComposeInputs& in) const {
    in.now = hosted::localTime(time(nullptr), "%Y-%m-%d %a %H:%M %Z");
    in.fw = NIMBUS_FW_VERSION " (" NIMBUS_FW_BUILD ")";
    in.hw.ring = false;
    in.hw.touch = false;
    in.hw.mic = false;
    in.hw.speaker = false;
    in.hw.battery = false;
    in.hw.sd = false;
    in.hw.wifi = true;
    in.hw.ble = false;
    in.hw.telegram = cfg_.has("TELEGRAM_BOT_TOKEN");
    in.hw.files = true;
    in.hw.voiceReplies = false;
  }

  // The composer's identity and capabilities lines describe the desk device; a
  // hosted head reads the hosted truth instead (CUM-542), so it stops calling
  // itself an offline ESP32. Applied to the instructions of every head this
  // instance registers (registerHeads). The phrases are pinned by
  // tests/test_hosted_prompt.cpp, so a change to the composer's wording fails a
  // test instead of silently shipping the device text again.
  static std::string hostedPrompt(std::string ins) {
    static const char* const kSwap[][2] = {
        {"(a Nimbus device), running in Orchestrator mode on a Solide S3 (ESP32-S3-DevKitC-1) desk device.",
         "(a Virtual Nimbus), running as a hosted cloud instance that is always online; it has no screen, "
         "microphone, speaker or LED ring."},
        {" desk device (ESP32-S3). Hardware present: ",
         " hosted Virtual Nimbus (a cloud instance, not a desk device). Hardware present: "},
        {"Hardware present: .\n", "Hardware present: none; memory and files live on the instance disk.\n"},
        {"Connectivity: WiFi", "Connectivity: online"},
    };
    for (const auto& sw : kSwap) {
      const size_t at = ins.find(sw[0]);
      if (at != std::string::npos) ins.replace(at, std::strlen(sw[0]), sw[1]);
    }
    return ins;
  }

  // The apply layer's deps (device buildApplyDeps): delivery, the turn principal,
  // running memory and the sub-agent session ops. Split from buildEngine so each
  // stays within the complexity gate.
  void wireApply(agent::TurnEngine::Deps& d) {
    d.apply.deliver = d.deliver;
    // The head guard and [THIS TURN RUNS ON] read the Chat seam (device buildApplyDeps).
    d.apply.headPause = [](const std::string& p) { return pauseOf(p, nimbus::orch::PaidSeam::Chat, false); };
    d.apply.stageDevice = [](const orch::ValidatedAction&) {};  // hosted: no device actions
    // The WHOLE principal from the tenant table (device principalFor): the owner's
    // own surfaces are Admin, an approved Telegram chat runs as its RBAC role.
    d.apply.principalFor = [this](const std::string& chat) {
      return access_->principalFor(chat);
    };
    // The reply's mem_write[] / mem_query[] fields (device memDispatch): through the
    // same registry and principal as the memory tools, then persisted. Unbound, the
    // fields were dropped in silence on a hosted instance (lane P live run, 19 Oct).
    d.apply.memDispatch = [this](const char* name, ArduinoJson::JsonObjectConst a,
                                 const orch::Principal& who) { return reg_.dispatch(name, a, who); };
    d.apply.persistMemory = [this] { flush(); };
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
      return enqueueSpawnFor(s, chat, quiet);
    };
    // Stopping a sub-agent is the owner's, or its own namespace's, to do (a member
    // turn only runs while the in-flight work is its own - see turnMustWait).
    d.apply.cancelSession = [this](const std::string& id) {
      const orch::Principal who = access_->principalFor(cur_.chatId);
      if (!who.owner && who.ns != bgNs_) return false;
      return jobs_ && jobs_->cancel(id.c_str());
    };
    d.apply.sessionStopping = [this](const std::string& id) { return jobs_ && jobs_->stopping(id.c_str()); };
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
  }

  // A spawn from `chat`: refused (with the reason, never silently) when the owner has
  // not approved the chat, or while another person's sub-agent work is in flight (the
  // one-namespace rule, see turnMustWait); otherwise queued on the JobEngine.
  bool enqueueSpawnFor(const orch::Spawn& s, const std::string& chat, bool quiet) {
    if (!access_->mayConverse(chat)) {
      record(chat, "Sub-agents are off for this chat: the owner has not approved it.");
      return false;
    }
    const std::string ns = access_->principalFor(chat).ns;
    if (backgroundBusy() && ns != bgNs_) {
      record(chat, "Sub-agents are busy with another person's request. Try again in a few minutes.");
      return false;
    }
    bgNs_ = ns;
    // Only the owner chooses a spawn's model on the owner's keys: a member's spawn
    // runs the configured sub model (the JobEngine coerces an empty pick). Naming a
    // provider with no key of its own would pick that provider's model on the
    // router (CUM-541), so a member's such spawn runs the configured sub-agent
    // order instead (a connector task keeps its provider and is refused there).
    orch::Spawn spawn = s;
    if (!access_->principalFor(chat).owner) {
      spawn.model.clear();
      if (!spawn.provider.empty() && !keyedForEngine(spawn.provider) &&
          connectorProviderFor(spawn.skill).empty())
        spawn.provider.clear();
    }
    return jobs_ && jobs_->enqueueSpawn(spawn, chat, quiet);
  }

  // Register the provider heads for the current key set. Kept out of buildEngine so
  // each is one small, named unit: the direct BYOK heads (openai/anthropic/mistral),
  // then the router-style OpenAI-compatible heads (cumulo, Z.ai) that a keyed VN with
  // no direct key still runs on.
  void registerHeads(agent::TurnEngine::Deps& d) {
    for (const char* h : {"openai", "anthropic", "mistral"}) {
      if (cfg_.providerKey(h).empty()) continue;
      const std::string host = h;
      d.hosts.add(host, [this, host](std::string& conv, const std::string& rawIns,
                                     const std::string& inp, std::string& out,
                                     std::string& err, const agent::HeadTools* tools,
                                     orch::TokenUsage* usage) -> bool {
        auto pd = headProviderDeps();
        const std::string ins = hostedPrompt(rawIns);
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
    if (hasCumulo())
      d.hosts.add(kCumuloSlug, [this](std::string& conv, const std::string& ins, const std::string& inp,
                                      std::string& out, std::string& err, const agent::HeadTools* tools,
                                      orch::TokenUsage* usage) -> bool {
        const std::string hosted = hostedPrompt(ins);
        HeadCall c{conv, hosted, inp, tools};
        HeadOut o{out, err, usage};
        return cumuloTurn(c, o);
      });
    // CUM-445: the Z.ai direct-BYOK head, when Z_AI_TOKEN is present. It runs through
    // orchTurnCustom (OpenAI-compatible, single-shot), NOT the loop above whose else-
    // branch is Mistral - routing a Z.ai key there would post it to Mistral, exactly
    // the silent-misroute class the UI comment ("wrote a cumulo key into Mistral") warns
    // against.
    if (hostAvailable(kZaiSlug)) addCustomHead(d, kZaiSlug, [this] { return zaiProviderDeps(); });
  }

  // Register one OpenAI-compatible single-shot head (Z.ai) that resolves its wire
  // deps from `depsFn` at turn time.
  void addCustomHead(agent::TurnEngine::Deps& d, const std::string& slug,
                     std::function<agent::providers::ProviderDeps()> depsFn) {
    d.hosts.add(slug, [depsFn](std::string& conv, const std::string& ins, const std::string& inp,
                               std::string& out, std::string& err, const agent::HeadTools* tools,
                               orch::TokenUsage* usage) -> bool {
      auto pd = depsFn();
      return agent::providers::orchTurnCustom(pd, conv, hostedPrompt(ins), inp, out, err, tools, usage);
    });
  }

  // ---- sub-agent fabric (device parity: adapters/adapter_factory.cpp) -------
  // The backends the device registers, all of them whether keyed or not, since a
  // key can arrive in-app later and dispatch answers Auth without one (the device
  // also registers its custom endpoint; a hosted instance has none). Each call
  // resolves the CURRENT deps, key and model, mirroring the device adapters line
  // for line.
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
        host = zaiHost();
        ep.basePath = kZaiPathPrefix;
        ep.key = cfg_.providerKey(kZaiSlug);
        ep.model = modelFor(kZaiSlug);
        ep.reasoningEffort = agent::providers::reasoningEffortFor("zai");   // CUM-544
      } else {
        // The model the JobEngine resolved for this spawn (the configured sub model
        // when the pick was not one it accepts), routed by the same split as the head.
        const std::string sel = (d.model && d.model[0]) ? std::string(d.model) : cumuloModel();
        const nimbus::orch::RouterRoute route = nimbus::orch::resolveRouterRoute(sel);
        if (route.model.empty()) return agent::FabricErr::BadRequest;
        host = kCumuloHost;
        ep.basePath = route.basePath;
        ep.key = cumuloKey();
        ep.model = route.model;
        ep.reasoningEffort = agent::providers::reasoningEffortFor(route.upstream);   // CUM-544
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
    // A sub-agent inherits the principal of the chat that started it (CUM-828/830): the
    // owner's carries the owner's connectors, anyone else's the built-ins only.
    jd.principalFor = [this](const std::string& chat) { return access_->principalFor(chat); };
    jd.subPriority = [this] { return subLadder(); };
    jd.providerHasKey = [this](const std::string& p) { return keyedForEngine(p); };
    jd.subModel = [this](const std::string& p) { return subModelFor(p); };
    // A spawn may name a router model the harvested Cumulo catalog lists (the
    // router prices it, so the key is entitled to it); everything else runs the
    // configured sub model (the engine coerces) - never a model the key may not be
    // entitled to. BYOK providers have no per-spawn routing here, so they coerce.
    jd.modelIsValid = [this](const std::string& p, const std::string& m) {
      if (p != kCumuloSlug || m.empty()) return false;
      return m == subModelFor(p) || catalogHas(verify_.get(p).models, m, nimbus::orch::RoleSubAgent);
    };
    jd.connectorProvider = [this](const std::string& skill) { return connectorProviderFor(skill); };
    // A file task follows the same PDF rule as the device: no capture here, so no
    // sandbox route, and it never lands on OpenAI only because a key is set.
    jd.fileSpawnProvider = [this] {
      return std::string(nimbus::orch::pdfRouteProvider(nimbus::orch::pdfRoute(connectorsView(), providerState())));
    };
    jd.nowString = [] { return localNowString(); };
    jd.chatContext = [this](const std::string& chat) { return spawnContext(chat); };
    // The report turn for a chat the owner removed or revoked meanwhile never runs:
    // its results are dropped instead of delivered.
    jd.synthesize = [this](const std::string& chat) {
      if (!access_->mayConverse(chat)) {
        if (jobs_) jobs_->takeFreshResultsFor(jobs_->freshScopeFor(chat), nullptr, nullptr, nullptr);
        return;
      }
      if (eng_) eng_->maybeConsolidate(chat);
    };
    jd.turnInFlight = [this] { return eng_ && eng_->turnInFlight(); };
    // Lane DEF: a finished sub-agent's reported usage counts toward the monthly spend
    // ceiling (device parity: orchestrator.cpp recordSpawnUsage).
    jd.recordSpawnUsage = [this](const std::string& backend, const std::string& model,
                                 const orch::TokenUsage& u) {
      const std::string m = (model.empty() || model == "default") ? subModelFor(backend) : model;
      orch::meterSeamUsage(orch::SpendSeam::Sub, backend.c_str(), m.c_str(), u);
    };
    // Lane QW4 round 3: the background stop list on disk (device parity), so a restart
    // keeps asking unconfirmed stops and counts them.
    jd.loadStopping = [this] {
      std::string out;
      fsutil::readFile(opt_.dataDir + "/jobs_stopping.txt", out);
      return out;
    };
    jd.saveStopping = [this](const std::string& text) {
      return fsutil::writeFileAtomic(opt_.dataDir + "/jobs_stopping.txt", text);
    };
  }

  // Connector-aware routing (device d.connectorProvider): a spawn whose skill names
  // an enabled connector runs on the provider that hosts it.
  std::string connectorProviderFor(const std::string& skill) const {
    for (const auto& c : conns_.parsed()) {
      if (!c.enabled) continue;
      const bool match = skill == c.name || (!c.type.empty() && skill == c.type);
      if (match) return c.prov;   // "any": no routing, never the router
    }
    return std::string();
  }

  static std::string localNowString() {
    return hosted::localTime(time(nullptr), "%Y-%m-%d %H:%M %Z (%A)");
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
  // Append one chat row and return its id. Every row gets the device's id shape
  // ("m%08x", memory_subsystem captureMessage): the window excludes the in-flight
  // row by id, and memory.episodic pages by it.
  std::string capture(const std::string& chat, const char* role, const std::string& text,
                      const std::string& tags) {
    char id[16];
    snprintf(id, sizeof id, "m%08x", (unsigned)(++epiSeq_));
    orch::EpisodicMessage m;
    m.id = id;
    m.sessionId = chat;
    m.kind = orch::MsgKind::Message;
    m.role = role;
    m.text = text;
    m.tags = tags;
    m.tsHours = nowHours();
    epi_->addMessage(m);
    return m.id;
  }

  // The chat's recent window, built the device's way (agent::recentConversationBlock,
  // CUM-511 + CUM-534): it stops at the chat's newest /clear boundary, leaves out
  // the in-flight message (it is the turn's [USER]) and job deliveries, renders
  // each row by its stored role ("user" / "assistant", never "owner") with every
  // line break neutralized, so a message can never forge a conversation line, and
  // keeps the newest rows whole within the window budget.
  std::string recentWindow(const std::string& chat) {
    if (chat.empty() || chat == "system") return std::string();
    orch::MsgQuery q;
    q.sessionId = chat;
    q.haveKind = true;
    q.kind = orch::MsgKind::Message;
    q.alsoKinds = {orch::MsgKind::Image, orch::MsgKind::File, orch::MsgKind::Audio};
    q.limit = 2 * orch::kRecentTurnsMax + 1;   // headroom for the skipped rows
    return agent::recentConversationBlock(epi_->query(q), curUserRowId_, kRecentWindowBytes);
  }
  static constexpr size_t kRecentWindowBytes = 8192;   // the device budget at the 200K anchor

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
  std::unique_ptr<SpendCeilingVn> ceiling_;   // lane DEF: before the engine (its config reads it)
  // Telegram allowlist + RBAC (CUM-459). Declared early so it outlives the engine and
  // job engine whose closures consult it.
  std::unique_ptr<TelegramAccess> access_;
  CgroupMemory mem_;
  std::unique_ptr<DaemonHttpTransport> ownedHttp_;  // owned unless a test injects one
  agent::HttpTransport* http_ = nullptr;            // the live transport (owned or injected)
  RouterTap tap_{nullptr, kCumuloHost};             // the cumulo head's transport (refusal reasons)
  std::string lastGoodCumulo_;   // the last router selector a turn succeeded on (engine thread)
  mutable std::mutex modelsMu_;  // guards opt_.models + subModels_ (HTTP reads, engine writes)
  std::map<std::string, std::string> subModels_;   // sub-agent model picks (device subM_<slug>)
  SettingsStore settings_;       // the owner's settings (CUM-543), durable, thread-safe
  ProviderVerifyStore verify_;   // per-provider verify verdict + harvested catalog (CUM-545)
  std::mutex verifyMu_;
  std::set<std::string> verifyQueued_;   // providers with a verify queued
  std::string envTz_;            // the operator's TZ at start ("" = none)
  std::string curUserRowId_;     // the in-flight user row (excluded from the window)
  uint32_t epiSeq_ = 0;          // episodic row id sequence (engine thread)
  orch::TokenUsage carriedUsage_;   // session usage of engines a rebuild replaced (CUM-546)
  uint32_t carriedTurns_ = 0;
  orch::TokenUsage lastUsage_;      // the most recent completed turn's usage
  mutable std::mutex diagMu_;    // guards lastTurn_ (engine writes, HTTP reads)
  std::string lastTurn_;         // the /api/lastturn page

  PosixEpiFs         epiFs_;
  orch::VectorMemory vec_;
  orch::PendingEmbeds pending_;   // memory writes waiting for an embedding (lane P)
  orch::EmbedPark     park_;
  orch::MemoryContext memCtx_;    // the tools' context (the drain reuses it)
  uint32_t            lastDrainMs_ = 0;
  std::string         embedShim_; // test seam: the embed call fails with this token
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
  std::atomic<uint32_t> keyGen_{0};   // bumps on each in-app key change (engine thread; the web layer reads it)
};

}  // namespace nimbusd
