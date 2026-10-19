// The turn-contract live suites. See lane_l_live.h for what each one asserts.
//
// Everything above the HTTP seam is the shipping code: the TurnEngine, the
// provider adapters, the JobEngine with its journal, the sub-agent dispatch and
// poll functions, the registry's memory tools and the on-device docs pack. Two
// stand-ins only: embeddings are a local hash (the scenarios assert that a
// memory was STORED, not how well it recalls - so no embedding spend), and the
// effort the adapter's cap table sends on Anthropic request bodies is pinned
// (default low, the cheapest) at the transport. Only an effort the adapter sent
// is replaced: a model family the cap table sends none to refuses the field.
#include "lane_l_live.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "curl_transport.h"
#include "nimbus/docs_pack.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/journal.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/reply_markup.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_memory.h"
#include "nimbus/tg_html.h"

namespace lab {

namespace orch = nimbus::orch;

namespace {

uint32_t nowMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---- spend: provider-reported usage, priced per model (USD per million tokens) ----
struct Price { double in, out, cacheRead, cacheWrite; bool known; };
Price priceOf(const std::string& model) {
  if (model.rfind("claude-haiku-5-5", 0) == 0)  return {0.10, 0.50, 0.01, 0.125, true};
  if (model.rfind("claude-sonnet-5-5", 0) == 0) return {2.0, 10.0, 0.20, 2.50, true};
  if (model.rfind("claude-opus-5", 0) == 0)     return {5.0, 25.0, 0.50, 6.25, true};
  if (model.rfind("mistral-small", 0) == 0)     return {0.10, 0.30, 0.0, 0.0, true};
  return {0, 0, 0, 0, false};
}

struct Usage {
  uint64_t in = 0, out = 0, cacheRead = 0, cacheWrite = 0;
  double usd = 0;
  bool unpriced = false;
  int calls = 0;
};

// Sum the "usage" object of every recorded response, keyed provider/model.
// `sessionModel` names the model of a Managed Agents session (its object carries
// usage but no model); its running time bills at kSessionUsdPerHour too.
constexpr double kSessionUsdPerHour = 0.08;
std::string providerOfHost(const std::string& host) {
  if (host.find("anthropic") != std::string::npos) return "anthropic";
  if (host.find("mistral") != std::string::npos) return "mistral";
  if (host.find("z.ai") != std::string::npos) return "zai";
  if (host.find("openai") != std::string::npos) return "openai";
  return host;
}

// The model a recorded call billed: the response's echo, else the request's.
std::string modelOfCall(const CurlHttpTransport::Call& c, JsonDocument& resp, bool session,
                        const std::string& sessionModel) {
  if (session) return sessionModel;
  std::string model = resp["model"] | "";
  if (!model.empty()) return model;
  JsonDocument rq;
  if (!deserializeJson(rq, c.reqBody)) model = rq["model"] | "";
  return model;
}

void addOne(Usage& a, JsonVariantConst u, const std::string& model, bool session) {
  const uint64_t in = u["input_tokens"] | u["prompt_tokens"] | 0;
  const uint64_t out = u["output_tokens"] | u["completion_tokens"] | 0;
  const uint64_t cr = u["cache_read_input_tokens"] | 0;
  const uint64_t cw = u["cache_creation_input_tokens"] | 0;
  a.in += in; a.out += out; a.cacheRead += cr; a.cacheWrite += cw; a.calls++;
  const Price p = priceOf(model);
  if (!p.known) a.unpriced = true;
  a.usd += (in * p.in + out * p.out + cr * p.cacheRead + cw * p.cacheWrite) / 1e6;
  if (session) a.usd += (u["active_seconds"] | 0.0) / 3600.0 * kSessionUsdPerHour;
}

void addUsage(const CurlHttpTransport& http, size_t from, std::map<std::string, Usage>& acc,
              const std::string& sessionModel = std::string()) {
  for (size_t i = from; i < http.calls.size(); i++) {
    const auto& c = http.calls[i];
    if (c.status < 200 || c.status >= 300) continue;
    JsonDocument d;
    if (deserializeJson(d, c.respBody) || d["usage"].isNull()) continue;
    const bool session = c.method == "GET" && c.path.rfind("/v1/sessions/", 0) == 0;
    const std::string model = modelOfCall(c, d, session, sessionModel);
    addOne(acc[providerOfHost(c.host) + "/" + (model.empty() ? "?" : model)], d["usage"], model, session);
  }
}

double totalFor(const std::map<std::string, Usage>& acc, const std::string& prov) {
  double t = 0;
  for (const auto& kv : acc)
    if (kv.first.rfind(prov + "/", 0) == 0) t += kv.second.usd;
  return t;
}

void printUsage(const std::map<std::string, Usage>& acc, FILE* f) {
  for (const auto& kv : acc)
    std::fprintf(f, "  usage %-40s calls=%d in=%llu out=%llu cacheR=%llu cacheW=%llu usd=%.4f%s\n",
                 kv.first.c_str(), kv.second.calls, (unsigned long long)kv.second.in,
                 (unsigned long long)kv.second.out, (unsigned long long)kv.second.cacheRead,
                 (unsigned long long)kv.second.cacheWrite, kv.second.usd,
                 kv.second.unpriced ? " (no list price known: tokens only)" : "");
}

// ---- the transport: effort pin on Anthropic message bodies ---------------------
// The adapter's cap table sends output_config.effort only to a family that takes
// it (medium on a head round, low on the retry after a cut); the pin replaces
// that value and never adds the field.
class PinTransport : public agent::HttpTransport {
 public:
  CurlHttpTransport inner;
  std::string effort;
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    if (effort.empty() || req.method != "POST" || req.path != "/v1/messages" ||
        req.host.find("anthropic") == std::string::npos)
      return inner.exec(req, out, err);
    agent::HttpRequest r = req;
    JsonDocument d;
    if (!deserializeJson(d, r.body) && !d["output_config"]["effort"].isNull()) {
      d["output_config"]["effort"] = effort;
      r.body.clear();
      serializeJson(d, r.body);
    }
    return inner.exec(r, out, err);
  }
};

// ---- a local hash embedder (the scenarios assert storage, not recall quality) ----
constexpr int kDims = 64;
std::vector<int8_t> hashEmbed(const std::string& text) {
  std::vector<float> v(kDims, 0.0f);
  for (size_t i = 0; i + 2 < text.size(); i++) {
    uint32_t h = 2166136261u;
    for (size_t k = i; k < i + 3; k++) { h ^= (uint8_t)std::tolower((unsigned char)text[k]); h *= 16777619u; }
    v[h % kDims] += 1.0f;
  }
  return orch::VectorMemory::quantize(v);
}

// ---- journal store with failure injection (leg D) ---------------------------------
struct LabJournalStore : orch::JournalStore {
  std::map<int, std::string> slots;
  bool refuseWrites = false;
  int puts = 0, refused = 0;
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  void put(int slot, const std::string& v) override {
    if (refuseWrites) { refused++; return; }
    puts++;
    slots[slot] = v;
  }
  void remove(int slot) override { if (!refuseWrites) slots.erase(slot); }
  void clearNs() override { if (!refuseWrites) slots.clear(); }
};

// ---- sub-agent adapters over the shipping portable dispatch/poll ------------------
struct SubAdapter : agent::ManagedAgentAdapter {
  std::string backend;
  std::function<agent::FabricErr(const agent::Directive&, char[72])> doDispatch;
  std::function<agent::FabricErr(const char*, agent::ResultEnvelope&)> doPoll;
  const char* backendId() const override { return backend.c_str(); }
  agent::Capabilities capabilities() const override { return {}; }
  agent::FabricErr dispatch(const agent::Directive& d, char out[72]) override { return doDispatch(d, out); }
  agent::FabricErr poll(const char* id, agent::ResultEnvelope& env) override { return doPoll(id, env); }
  agent::FabricErr cancel(const char*) override { return agent::FabricErr::Unsupported; }
};

std::string nonceNow() {
  std::mt19937 rng((uint32_t)nowMs());
  return std::to_string(100000 + rng() % 900000);
}

bool contains(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

// Delivered text must carry no markup: raw, and as the Telegram HTML it becomes.
bool cleanDelivery(const std::string& text, std::string& why) {
  if (orch::hasStrayMarkup(text, &why)) return false;
  const std::string html = nimbus::tgHtml(text);
  for (const char* esc : {"&lt;parameter", "&lt;/parameter", "&lt;/reply", "&lt;think", "&lt;invoke",
                          "&lt;function_calls", "&lt;orch_turn", "&lt;tool_call"}) {
    if (contains(html, esc)) { why = std::string("escaped ") + esc + " in the Telegram HTML"; return false; }
  }
  return true;
}

// ---- the rig ------------------------------------------------------------------------
class LRig {
 public:
  LRig(const Env& env, const LaneLOptions& opt, bool withFabric, bool refuseJournal = false)
      : env_(env), opt_(opt) {
    http_.effort = opt.effort;
    http_.inner.verbose = opt.verbose;
    agent::hlog::setSink(+[](const char* l) {
      logs().push_back(l);
      if (echo()) std::fprintf(stderr, "[harness] %s\n", l);
    });
    echo() = opt.verbose;
    logs().clear();
    store_.refuseWrites = refuseJournal;
    buildRegistry();
    buildJobs(withFabric);
    buildEngine();
  }

  static std::vector<std::string>& logs() { static std::vector<std::string> v; return v; }
  static bool& echo() { static bool e = false; return e; }
  static int logCount(const char* needle) {
    int n = 0;
    for (const auto& l : logs()) n += l.find(needle) != std::string::npos;
    return n;
  }

  agent::TurnEngine& eng() { return *eng_; }
  agent::JobEngine& jobs() { return *jobs_; }
  CurlHttpTransport& http() { return http_.inner; }
  LabJournalStore& store() { return store_; }
  orch::Journal& journal() { return journal_; }
  orch::VectorMemory& vectors() { return vec_; }
  std::vector<std::pair<std::string, std::string>>& deliveries() { return delivered_; }
  int synths = 0;

  // A scripted head: a fixed orch_turn, and for the synthesis turn a reply that
  // repeats the fresh result (so the nonce's path to the owner is the engine's).
  void scriptHead(const std::string& turn) { scripted_ = turn; }

 private:
  std::string modelFor(const std::string& h) const {
    if (h == opt_.headHost) return opt_.headModel;
    if (h == "anthropic") return "claude-haiku-5-5";
    if (h == "mistral") return "mistral-small-latest";
    if (h == "zai") return "glm-5.3-flash";
    return "";
  }
  std::string subModelFor(const std::string& p) const {
    const size_t c = opt_.sub.find(':');
    if (c != std::string::npos && opt_.sub.substr(0, c) == p) return opt_.sub.substr(c + 1);
    return modelFor(p);
  }
  bool keyed(const std::string& h) const {
    if (h == "scripted") return true;
    if (h == "zai") return env_.has("Z_AI_TOKEN");
    return !env_.providerKey(h).empty();
  }

  agent::providers::ProviderDeps deps() {
    agent::providers::ProviderDeps pd;
    pd.http = &http_;
    pd.key = [this](const char* h) {
      return std::string(h) == "zai" ? env_.get("Z_AI_TOKEN") : env_.providerKey(h);
    };
    pd.orchModel = [this](const char* h) { return modelFor(h); };
    pd.toolLoopOn = [] { return true; };
    pd.antEnvId = [this] { return antEnv_; };
    pd.setAntEnvId = [this](const std::string& v) { antEnv_ = v; };
    pd.antAgentMap = [this] { return antAgents_; };
    pd.setAntAgentMap = [this](const std::string& v) { antAgents_ = v; };
    pd.nowMs = nowMs;
    pd.freeHeap = [] { return (uint32_t)4u * 1024u * 1024u; };
    return pd;
  }

  void buildRegistry() {
    vec_.configure(kDims);
    vec_.setMaxEntries(500);
    orch::MemoryContext mc;
    mc.vec = &vec_;
    mc.scratch = &scratch_;
    mc.cfg = &memCfg_;
    mc.embed = [](const std::string& t, std::string& err) { err.clear(); return hashEmbed(t); };
    orch::registerMemoryTools(reg_, mc);
    reg_.add("docs.search", "Search your own device documentation (ranked keyword match).",
             [](ArduinoJson::JsonObjectConst a, const orch::Principal&) -> orch::ToolResult {
               const std::string q = a["query"] | "";
               const nimbus::docs::DocSection* hits[8];
               const size_t n = nimbus::docs::search(q, hits, 8);
               std::string out;
               for (size_t i = 0; i < n; i++)
                 out += std::string(hits[i]->id) + " | " + hits[i]->title + "\n";
               return orch::ToolResult::ok(out.empty() ? std::string("no sections match") : out);
             },
             R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})");
    reg_.add("docs.read", "Read ONE section of your own device documentation by id.",
             [](ArduinoJson::JsonObjectConst a, const orch::Principal&) -> orch::ToolResult {
               const nimbus::docs::DocSection* s = nimbus::docs::find(a["id"] | "");
               if (!s) return orch::ToolResult::fail("unknown doc id");
               return orch::ToolResult::ok(std::string("## ") + s->title + "\n\n" + s->body);
             },
             R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");
  }

  void buildJobs(bool withFabric) {
    agent::JobEngine::Deps jd;
    jd.platform.nowMs = nowMs;
    jd.platform.freeHeap = [] { return (uint32_t)4u * 1024u * 1024u; };
    jd.deliver = [this](const std::string& c, const std::string& t) { delivered_.push_back({c, t}); };
    jd.subPriority = [this] { return opt_.sub.substr(0, opt_.sub.find(':')); };
    jd.providerHasKey = [this](const std::string& p) { return keyed(p); };
    jd.subModel = [this](const std::string& p) { return subModelFor(p); };
    jd.modelChoices = [this](const std::string& p) { return choices(p); };
    jd.modelIsValid = [this](const std::string& p, const std::string& m) {
      return !m.empty() && contains("," + choices(p) + ",", "," + m + ",");
    };
    jd.synthesize = [this](const std::string& chat) { synths++; eng_->maybeConsolidate(chat); };
    jd.turnInFlight = [this] { return eng_ && eng_->turnInFlight(); };
    if (withFabric) {
      journal_.begin(&store_);
      jd.journal = &journal_;
      jd.fabric = &fabric_;
      ant_.backend = "anthropic";
      ant_.doDispatch = [this](const agent::Directive& d, char out[72]) {
        return agent::providers::antDispatch(deps(), "claude-haiku-5-5", d, out);
      };
      ant_.doPoll = [this](const char* id, agent::ResultEnvelope& e) {
        return agent::providers::antPoll(deps(), id, e);
      };
      mis_.backend = "mistral";
      mis_.doDispatch = [this](const agent::Directive& d, char out[72]) {
        return agent::providers::mistralDispatch(deps(), d.model ? d.model : "mistral-small-latest", d, out);
      };
      mis_.doPoll = [this](const char* id, agent::ResultEnvelope& e) {
        return agent::providers::mistralPoll(deps(), id, e);
      };
      zai_.backend = "zai";
      zai_.doDispatch = [this](const agent::Directive& d, char out[72]) {
        agent::providers::CompatEndpoint ep;
        ep.host = "api.z.ai";
        ep.basePath = "/api/paas/v4";
        ep.key = env_.get("Z_AI_TOKEN");
        ep.model = d.model ? d.model : "glm-5.3-flash";
        ep.backendTag = "zai";
        ep.reasoningEffort = agent::providers::reasoningEffortFor("zai");
        return agent::providers::openaiCompatDispatch(deps(), ep, d, out);
      };
      zai_.doPoll = [this](const char* id, agent::ResultEnvelope& e) {
        return agent::providers::openaiCompatPoll(deps(), "zai", id, e);
      };
      fabric_.registerAdapter(&ant_);
      fabric_.registerAdapter(&mis_);
      fabric_.registerAdapter(&zai_);
    }
    agent::JobEngine::Tuning t;
    t.pollIntervalMs = 4000;
    jobs_.reset(new agent::JobEngine(std::move(jd), t));
  }

  std::string choices(const std::string& p) const {
    if (p == "anthropic") return "claude-haiku-5-5,claude-sonnet-5-5,claude-opus-5";
    if (p == "mistral") return "mistral-small-latest,mistral-large-latest";
    if (p == "zai") return "glm-5.3-flash,glm-5.3";
    return "";
  }

  void buildEngine() {
    agent::TurnEngine::Deps d;
    auto& p = d.cfg.provider;
    p.hasKey = [this](const std::string& h) { return keyed(h); };
    p.key = [this](const std::string& h) { return env_.providerKey(h); };
    p.orchHost = [this] { return scripted_.empty() ? opt_.headHost : std::string("scripted"); };
    p.providerPriority = [this] { return opt_.headHost; };
    p.subPriority = [this] { return opt_.sub.substr(0, opt_.sub.find(':')); };
    p.orchModel = [this](const std::string& h) { return modelFor(h); };
    p.subModel = [this](const std::string& h) { return subModelFor(h); };
    p.modelChoices = [this](const std::string& h) { return choices(h); };
    p.convId = [this] { return conv_; };
    p.setConvId = [this](const std::string& v) { conv_ = v; };
    d.cfg.loop.toolLoopOn = [] { return true; };
    d.cfg.loop.rounds = [] { return 8; };
    d.cfg.loop.deadlineS = [] { return 300; };
    d.cfg.budget.overBudget = [](const std::string&) { return false; };
    d.cfg.ttsEnabled = [] { return false; };
    d.platform.nowMs = nowMs;
    d.platform.freeHeap = [] { return (uint32_t)4u * 1024u * 1024u; };
    d.platform.delayMs = sleepMs;
    d.jobs = jobs_.get();
    d.deliver = [this](const std::string& c, const std::string& t) { delivered_.push_back({c, t}); };
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus-Lab";
      in.hostLabel = "lab";
      return in;
    };
    d.toolSpecs = [this](const orch::Principal& who) { return reg_.toolSpecsFor(who); };
    d.mcpDispatch = [this](const std::string& req, const orch::Principal& who) {
      return reg_.handleRpc(req, who);
    };
    d.modelChoices = [this](const std::string& h) { return choices(h); };
    d.firstAllowedChat = [] { return std::string("lab"); };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) {
      return orch::principalForRole(chat, orch::Role::Admin);
    };
    d.apply.memDispatch = [this](const char* name, ArduinoJson::JsonObjectConst args,
                                 const orch::Principal& who) {
      return reg_.dispatch(name, args, who);
    };
    d.apply.setModelMemory = [](const std::string&, const std::string&) { return false; };
    d.apply.enqueueSpawn = [this](const orch::Spawn& s, const std::string& chat, bool quiet) {
      return jobs_->enqueueSpawn(s, chat, quiet);
    };
    d.apply.noteSpawned = [this] { jobs_->noteSpawned(); };
    for (const char* h : {"anthropic", "mistral"}) {
      if (!keyed(h)) continue;
      const std::string host = h;
      d.hosts.add(host, [this, host](std::string& conv, const std::string& ins, const std::string& inp,
                                     std::string& out, std::string& err, const agent::HeadTools* tools,
                                     orch::TokenUsage* usage) -> bool {
        auto pd = deps();
        if (host == "anthropic")
          return agent::providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, usage);
        return agent::providers::orchTurnMistral(pd, conv, ins, inp, out, err, tools, usage);
      });
    }
    d.hosts.add("scripted", [this](std::string& conv, const std::string&, const std::string& inp,
                                   std::string& out, std::string&, const agent::HeadTools*,
                                   orch::TokenUsage*) -> bool {
      conv = "scripted";
      const size_t f = inp.find("[FRESH RESULTS]");
      if (f == std::string::npos) { out = scripted_; return true; }
      const size_t line = inp.find("): ", f);
      const std::string body = line == std::string::npos ? std::string() : inp.substr(line + 3, 240);
      JsonDocument r;
      r["reply"] = "Result: " + body.substr(0, body.find('\n'));
      out.clear();
      serializeJson(r, out);
      return true;
    });
    eng_.reset(new agent::TurnEngine(std::move(d)));
  }

  const Env& env_;
  LaneLOptions opt_;
  PinTransport http_;
  orch::VectorMemory vec_;
  orch::Scratchpad scratch_;
  orch::MemConfig memCfg_;
  orch::ToolRegistry reg_;
  LabJournalStore store_;
  orch::Journal journal_;
  agent::HeavyFabric fabric_;
  SubAdapter ant_, mis_, zai_;
  std::unique_ptr<agent::JobEngine> jobs_;
  std::unique_ptr<agent::TurnEngine> eng_;
  std::string conv_, antEnv_, antAgents_, scripted_;
  std::vector<std::pair<std::string, std::string>> delivered_;
};

void writeSummary(const LaneLOptions& opt, const char* suite, int failures, int runs,
                  const std::map<std::string, Usage>& usage, const std::string& notes) {
  if (opt.out.empty()) return;
  JsonDocument d;
  d["suite"] = suite;
  d["head"] = opt.headHost + "/" + opt.headModel;
  d["sub"] = opt.sub;
  d["runs"] = runs;
  d["failures"] = failures;
  d["notes"] = notes;
  JsonObject u = d["usage"].to<JsonObject>();
  for (const auto& kv : usage) {
    JsonObject o = u[kv.first].to<JsonObject>();
    o["calls"] = kv.second.calls;
    o["in"] = kv.second.in;
    o["out"] = kv.second.out;
    o["cacheRead"] = kv.second.cacheRead;
    o["cacheWrite"] = kv.second.cacheWrite;
    o["usd"] = kv.second.usd;
    o["priced"] = !kv.second.unpriced;
  }
  if (FILE* f = std::fopen(opt.out.c_str(), "wb")) {
    std::string s;
    serializeJsonPretty(d, s);
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
}

}  // namespace

// ---- L-L1 reply-markup -------------------------------------------------------------

namespace {

std::string markupPrompt(char pc, const std::string& word, const std::string& nonce) {
  if (pc == '1')
    return "Use docs_search and docs_read to read the section of your own docs about "
           "session_ops, then explain it to me in about 300 words. Also remember that my "
           "test token colour is " + word + ", and start one sub-agent on anthropic with "
           "model claude-haiku-5-5 that replies with exactly NIMBUS-" + nonce + ".";
  if (pc == '2')
    return "Give me a formatted comparison of a caravan and a motorhome for a family of "
           "four, with bold headings and a short list for each.";
  return "Think step by step about whether 2027 is a prime number, then answer briefly.";
}

// The class verdict for one turn: every delivery clean, and for prompt 1 both
// side effects landed. "" = pass, else why it failed.
std::string markupVerdict(LRig& rig, char pc, const std::string& word, const std::string& nonce) {
  if (rig.deliveries().empty()) return "no reply delivered";
  std::string why;
  for (const auto& d : rig.deliveries())
    if (!cleanDelivery(d.second, why)) return why;
  if (pc != '1') return std::string();
  bool stored = false;
  for (const auto& e : rig.vectors().getAll()) stored = stored || contains(e.content, word);
  if (!stored) return "the memory write was lost (" + word + " not stored)";
  const bool spawned = contains(rig.jobs().spawnLedgerText("lab"), "NIMBUS-" + nonce) ||
                       rig.jobs().pendingCount() > 0;
  return spawned ? std::string() : "the spawn was lost (no sub-agent queued)";
}

struct MarkupTally {
  int failures = 0, runs = 0, repairs = 0, strips = 0, corrections = 0;
  std::string notes;
};

void markupTurn(const Env& env, const LaneLOptions& opt, int rep, char pc,
                std::map<std::string, Usage>& usage, MarkupTally& t) {
  static const std::vector<std::string> words = {"teal", "amber", "violet", "crimson", "olive"};
  LRig rig(env, opt, /*withFabric=*/false);
  const std::string nonce = nonceNow();
  const std::string word = words[(size_t)(rep + pc) % words.size()] + "-" + nonce.substr(0, 3);
  const size_t mark = rig.http().calls.size();
  const uint32_t t0 = nowMs();
  rig.eng().handleMessage(markupPrompt(pc, word, nonce), "Owner", "lab");
  t.runs++;
  addUsage(rig.http(), mark, usage);
  const std::string why = markupVerdict(rig, pc, word, nonce);
  const int rp = LRig::logCount("leaked tool markup repaired") + LRig::logCount("leaked-markup actions dropped");
  const int st = LRig::logCount("reasoning preamble stripped");
  const int co = LRig::logCount("final answer sent back for one correction");
  t.repairs += rp; t.strips += st; t.corrections += co;
  std::printf("[%s] rep %d prompt %c %.1fs repairs=%d strips=%d corrections=%d %s\n",
              why.empty() ? "PASS" : "FAIL", rep + 1, pc, (nowMs() - t0) / 1000.0, rp, st, co,
              why.c_str());
  if (why.empty()) return;
  t.failures++;
  t.notes += "rep " + std::to_string(rep + 1) + " prompt " + pc + ": " + why + "; ";
  if (!rig.deliveries().empty())
    std::printf("    delivered: %.300s\n", rig.deliveries().back().second.c_str());
}

}  // namespace

int cmdReplyMarkup(const Env& env, const LaneLOptions& opt) {
  if (env.providerKey(opt.headHost).empty()) {
    std::fprintf(stderr, "no key for %s - skipped\n", opt.headHost.c_str());
    return 2;
  }
  std::map<std::string, Usage> usage;
  MarkupTally t;
  for (int rep = 0; rep < opt.reps; rep++)
    for (char pc : opt.prompts)
      if (pc >= '1' && pc <= '3') markupTurn(env, opt, rep, pc, usage, t);
  std::printf("reply-markup %s/%s: %d run(s), %d failure(s); leak repairs %d, reasoning strips %d, "
              "correction rounds %d\n",
              opt.headHost.c_str(), opt.headModel.c_str(), t.runs, t.failures, t.repairs, t.strips,
              t.corrections);
  printUsage(usage, stdout);
  t.notes += "repairs=" + std::to_string(t.repairs) + " strips=" + std::to_string(t.strips) +
             " corrections=" + std::to_string(t.corrections);
  writeSummary(opt, "reply-markup", t.failures, t.runs, usage, t.notes);
  return t.failures ? 1 : 0;
}

// ---- L-L2 spawn-e2e ------------------------------------------------------------------

namespace {

std::string spawnTurn(const std::string& sub, const std::string& nonce, bool stringified) {
  const size_t c = sub.find(':');
  JsonDocument item;
  item["op"] = "spawn";
  item["provider"] = sub.substr(0, c);
  item["model"] = sub.substr(c + 1);
  item["name"] = "e2e";
  item["task"] = "Reply with exactly: NIMBUS-" + nonce;
  JsonDocument d;
  d["reply"] = "Starting one sub-agent.";
  if (stringified) {
    std::string list = "[";
    std::string one;
    serializeJson(item, one);
    list += one + "]";
    d["session_ops"] = list;   // the Anthropic head's string-wrapped shape
  } else {
    d["session_ops"].to<JsonArray>().add(item);
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

// Pump the job engine until the nonce reaches the owner or the deadline passes.
bool pumpUntilDelivered(LRig& rig, const std::string& nonce, int timeoutS) {
  const uint32_t end = nowMs() + (uint32_t)timeoutS * 1000u;
  const size_t from = rig.deliveries().size();
  while (nowMs() < end) {
    rig.jobs().pump();
    for (size_t i = from; i < rig.deliveries().size(); i++)
      if (contains(rig.deliveries()[i].second, "NIMBUS-" + nonce)) {
        for (int i = 0; i < 8; i++) { rig.jobs().pump(); sleepMs(1000); }   // let the reap run
        return true;
      }
    sleepMs(1000);
  }
  return false;
}

// Managed Agents report a sub's token usage on the session object, not in the
// events the poll reads: fetch each session this leg created so its usage is
// counted (provider-reported, like every other call here).
void fetchSessionUsage(LRig& rig, const Env& env, size_t from) {
  std::vector<std::string> ids;
  auto& calls = rig.http().calls;
  for (size_t i = from; i < calls.size(); i++) {
    if (calls[i].method != "POST" || calls[i].path != "/v1/sessions" || calls[i].status != 200) continue;
    JsonDocument d;
    if (!deserializeJson(d, calls[i].respBody) && d["id"].is<const char*>()) ids.push_back(d["id"].as<const char*>());
  }
  for (const auto& id : ids) {
    agent::HttpRequest req;
    req.method = "GET";
    req.host = "api.anthropic.com";
    req.path = "/v1/sessions/" + id;
    req.headers = {{"x-api-key", env.providerKey("anthropic")},
                   {"anthropic-version", "2023-06-01"},
                   {"anthropic-beta", "managed-agents-2026-04-01"}};
    agent::HttpResponse resp;
    std::string err;
    rig.http().exec(req, resp, err);
    (void)resp;
  }
}

}  // namespace

namespace {

// The class verdict for one spawn leg: "" = pass, else why it failed.
std::string spawnVerdict(LRig& rig, char leg, bool delivered, int nonceDeliveries) {
  const int spawned = LRig::logCount("orchestrator: spawned job");
  if (!delivered) return "the nonce never reached the owner";
  if (nonceDeliveries != 1) return "the nonce reached the owner " + std::to_string(nonceDeliveries) + " times";
  if (spawned != 1) return std::to_string(spawned) + " spawns for one request";
  if (leg != 'C' && LRig::logCount("spawn model coerced")) return "the listed model was coerced";
  if (leg != 'D' && rig.store().puts == 0) return "no journal record landed";
  if (rig.journal().count() != 0) return "the journal still holds the finished job";
  if (leg == 'D' && rig.store().refused == 0) return "the failing store was never asked";
  return std::string();
}

struct SpawnTally {
  int failures = 0, runs = 0;
  std::string notes;
};

void spawnLeg(const Env& env, const LaneLOptions& opt, char leg, int rep,
              std::map<std::string, Usage>& usage, SpawnTally& t) {
  const std::string subProv = opt.sub.substr(0, opt.sub.find(':'));
  const std::string subModel = opt.sub.substr(opt.sub.find(':') + 1);
  LRig rig(env, opt, /*withFabric=*/true, /*refuseJournal=*/leg == 'D');
  const std::string nonce = nonceNow();
  const size_t mark = rig.http().calls.size();
  const uint32_t t0 = nowMs();
  if (leg == 'C') {
    rig.eng().handleMessage("Start a sub-agent on " + subProv + " with model " + subModel +
                                " that replies with exactly NIMBUS-" + nonce + ". Do not do it yourself.",
                            "Owner", "lab");
  } else {
    rig.scriptHead(spawnTurn(opt.sub, nonce, /*stringified=*/leg == 'B'));
    rig.eng().handleMessage("go", "Owner", "lab");
  }
  // The head's own reply may repeat the nonce it was asked for; the RESULT is
  // what must reach the owner exactly once, so count from here on.
  const size_t afterTurn = rig.deliveries().size();
  const bool delivered = pumpUntilDelivered(rig, nonce, opt.timeoutS);
  t.runs++;
  if (subProv == "anthropic") fetchSessionUsage(rig, env, mark);
  addUsage(rig.http(), mark, usage, subModel);
  int nonceDeliveries = 0;
  for (size_t i = afterTurn; i < rig.deliveries().size(); i++)
    nonceDeliveries += contains(rig.deliveries()[i].second, "NIMBUS-" + nonce);
  const std::string why = spawnVerdict(rig, leg, delivered, nonceDeliveries);
  std::printf("[%s] leg %c rep %d sub=%s %.1fs spawned=%d coerced=%d repaired=%d salvaged=%d synth=%d%s%s\n",
              why.empty() ? "PASS" : "FAIL", leg, rep + 1, opt.sub.c_str(), (nowMs() - t0) / 1000.0,
              LRig::logCount("orchestrator: spawned job"), LRig::logCount("spawn model coerced"),
              LRig::logCount("turn field session_ops"), LRig::logCount("called as a tool"), rig.synths,
              why.empty() ? "" : " - ", why.c_str());
  if (!why.empty()) {
    t.failures++;
    t.notes += std::string("leg ") + leg + " rep " + std::to_string(rep + 1) + ": " + why + "; ";
    for (const auto& d : rig.deliveries()) std::printf("    delivered: %.200s\n", d.second.c_str());
  }
  if (leg == 'D')
    std::printf("    leg D: %d journal write(s) refused; the reboot half (no re-delivery, no loss "
                "after a restart) needs the durable journal and is not run here\n",
                rig.store().refused);
}

}  // namespace

int cmdSpawnE2e(const Env& env, const LaneLOptions& opt) {
  std::map<std::string, Usage> usage;
  SpawnTally t;
  for (char leg : opt.legs) {
    if (leg != 'A' && leg != 'B' && leg != 'C' && leg != 'D') continue;
    const int reps = leg == 'C' ? opt.reps : 1;
    for (int rep = 0; rep < reps; rep++) spawnLeg(env, opt, leg, rep, usage, t);
  }
  std::printf("spawn-e2e head=%s/%s sub=%s: %d run(s), %d failure(s)\n", opt.headHost.c_str(),
              opt.headModel.c_str(), opt.sub.c_str(), t.runs, t.failures);
  printUsage(usage, stdout);
  writeSummary(opt, "spawn-e2e", t.failures, t.runs, usage, t.notes);
  return t.failures ? 1 : 0;
}

}  // namespace lab
