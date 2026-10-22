// The models-on-demand live suite (R5). See lane_h_live.h for what each leg asserts.
//
// Everything above the HTTP seam is the shipping code: the TurnEngine, the apply
// rails (orch_model validation and its "not in <provider> list" answers), the
// models.list tool and the refresh queue, the provider adapters. The model lists
// the validation reads are the providers' own, fetched live (GET /v1/models is
// free). The effort the adapter's cap table sends on Anthropic request bodies is
// pinned at the transport (default low), as in the other live suites.
#include "lane_h_live.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "curl_transport.h"
#include "lab_rig.h"   // labDefaultModel, the Z.ai head endpoint
#include "nimbus/harness/apply.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/model_refresh.h"
#include "nimbus/orch/models_tool.h"
#include "nimbus/orch/tool_registry.h"

namespace lab {

namespace orch = nimbus::orch;

namespace {

uint32_t nowMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t nowEpoch() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}
void sleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

// The effort pin on Anthropic message bodies: only a value the adapter sent is replaced.
class PinnedTransport : public agent::HttpTransport {
 public:
  CurlHttpTransport inner;
  std::string effort;
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    if (effort.empty() || req.method != "POST" || req.path != "/v1/messages") return inner.exec(req, out, err);
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

// ---- spend: provider-reported usage priced with the device's built-in table -------
struct Usage {
  uint64_t in = 0, out = 0, cacheRead = 0, cacheWrite = 0;
  double usd = 0;
  int calls = 0;
};
std::string providerOfHost(const std::string& host) {
  if (has(host, "anthropic")) return "anthropic";
  if (has(host, "mistral")) return "mistral";
  if (has(host, "z.ai")) return "zai";
  if (has(host, "openai")) return "openai";
  return host;
}
void addUsage(const CurlHttpTransport& http, size_t from, std::map<std::string, Usage>& acc) {
  for (size_t i = from; i < http.calls.size(); i++) {
    const auto& c = http.calls[i];
    if (c.status < 200 || c.status >= 300) continue;
    JsonDocument d;
    if (deserializeJson(d, c.respBody) || d["usage"].isNull()) continue;
    std::string model = d["model"] | "";
    if (model.empty()) {
      JsonDocument rq;
      if (!deserializeJson(rq, c.reqBody)) model = rq["model"] | "";
    }
    const std::string prov = providerOfHost(c.host);
    JsonVariantConst u = d["usage"];
    Usage& a = acc[prov + "/" + model];
    const uint64_t in = u["input_tokens"] | u["prompt_tokens"] | 0;
    const uint64_t out = u["output_tokens"] | u["completion_tokens"] | 0;
    const uint64_t cr = u["cache_read_input_tokens"] | 0;
    const uint64_t cw = u["cache_creation_input_tokens"] | 0;
    a.in += in; a.out += out; a.cacheRead += cr; a.cacheWrite += cw; a.calls++;
    const orch::BuiltinPrice* p = orch::builtinModelPrice(prov.c_str(), model.c_str());
    if (p) a.usd += ((double)in * p->microIn + (double)out * p->microOut + (double)cr * p->microIn * 0.1 +
                     (double)cw * p->microIn * 1.25) / 1e12;
  }
}

// ---- the live model lists ------------------------------------------------------
struct LiveList {
  std::vector<orch::ModelInfo> models;
  uint32_t fetchedAt = 0;
  std::string error;
};

agent::HttpRequest listRequest(const Env& env, const std::string& p) {
  agent::HttpRequest r;
  r.method = "GET";
  r.timeoutMs = 30000;
  if (p == "anthropic") {
    r.host = "api.anthropic.com";
    r.path = "/v1/models?limit=100";
    r.headers = {{"x-api-key", env.providerKey(p)}, {"anthropic-version", "2023-06-01"}};
  } else {
    r.host = p == "mistral" ? "api.mistral.ai" : p == "zai" ? kLabZaiHost : "api.openai.com";
    r.path = p == "zai" ? std::string(kLabZaiPath) + "/models" : std::string("/v1/models");
    r.headers = {{"Authorization", "Bearer " + env.providerKey(p)}};
  }
  return r;
}

// ---- the rig -------------------------------------------------------------------
struct LaneHOpts {
  std::string headHost, headModel, effort;
  bool verbose = false;
};

struct TurnOut {
  std::string reply;
  std::vector<std::string> toolCalls, toolResults;
};

class HRig {
 public:
  HRig(const Env& env, const LaneHOpts& o) : env_(env), opt_(o) {
    http_.effort = o.effort;
    http_.inner.verbose = o.verbose;
    agent::hlog::setSink(+[](const char* l) {
      logs().push_back(l);
      if (echo()) std::fprintf(stderr, "[harness] %s\n", l);
    });
    echo() = o.verbose;
    logs().clear();
  }
  static bool& echo() { static bool e = false; return e; }
  static std::vector<std::string>& logs() { static std::vector<std::string> v; return v; }

  bool keyed(const std::string& p) const { return !env_.providerKey(p).empty(); }

  bool fetchList(const std::string& p) {
    LiveList& l = lists_[p];
    agent::HttpResponse resp;
    std::string err;
    if (!http_.inner.exec(listRequest(env_, p), resp, err) || resp.status != 200) {
      l.error = err.empty() ? "HTTP " + std::to_string(resp.status) : err;
      return false;
    }
    orch::parseModelsList(p, resp.body, l.models);
    l.fetchedAt = nowEpoch();
    return !l.models.empty();
  }
  const LiveList& list(const std::string& p) { return lists_[p]; }

  // The chat-capable ids of a provider's live list, in its order: what the device's
  // orch_model and spawn checks would accept.
  std::string choices(const std::string& p) const {
    auto it = lists_.find(p);
    std::string csv;
    if (it == lists_.end()) return csv;
    for (const auto& m : it->second.models)
      if (m.hasRole(orch::RoleOrchestrator)) csv += (csv.empty() ? "" : ",") + m.id;
    return csv;
  }
  bool listed(const std::string& p, const std::string& id) const {
    return orch::modelInChoices(choices(p), id);
  }

  void build() {
    buildApply();
    buildRegistry();
    buildEngine();
  }

  TurnOut say(const std::string& text) {
    cur_ = TurnOut{};
    deliveries_.clear();
    eng_->handleMessage(text, "Owner", "lab");
    if (!deliveries_.empty()) cur_.reply = deliveries_.back();
    return cur_;
  }

  const std::vector<std::pair<std::string, std::string>>& applied() const { return applied_; }
  int refreshesQueued() const { return queued_; }
  PinnedTransport& http() { return http_; }

 private:
  std::string modelFor(const std::string& h) const {
    return h == opt_.headHost ? opt_.headModel : labDefaultModel(h);
  }

  agent::providers::ProviderDeps deps() {
    agent::providers::ProviderDeps pd;
    pd.http = &http_;
    pd.key = [this](const char* h) { return env_.providerKey(h); };
    pd.orchModel = [this](const char* h) { return modelFor(h); };
    pd.toolLoopOn = [] { return true; };
    pd.antEnvId = [] { return std::string(); };
    pd.setAntEnvId = [](const std::string&) {};
    pd.antAgentMap = [] { return std::string(); };
    pd.setAntAgentMap = [](const std::string&) {};
    pd.customBase = [] { return std::string(); };
    pd.customKey = [] { return std::string(); };
    pd.customConv = [] { return std::string("openai"); };
    pd.customModel = [] { return std::string(); };
    pd.nowMs = nowMs;
    pd.freeHeap = [] { return (uint32_t)4u * 1024u * 1024u; };
    return pd;
  }
  agent::providers::ProviderDeps zaiDeps() {
    auto pd = deps();
    pd.customBase = [] { return std::string(kLabZaiHost); };
    pd.customPathPrefix = [] { return std::string(kLabZaiPath); };
    pd.customKey = [this] { return env_.providerKey("zai"); };
    pd.customModel = [this] { return modelFor("zai"); };
    agent::providers::applyZaiHeadPolicy(pd);
    return pd;
  }

  // The apply rails as on the device: orch_model checks the live list, needs a key,
  // and a miss asks the refresh queue (models_tool.h answers).
  void buildApply() {
    ad_.deliver = [this](const std::string&, const std::string& t) { deliveries_.push_back(t); };
    ad_.principalFor = [](const std::string& chat) { return orch::principalForRole(chat, orch::Role::Admin); };
    ad_.setModelMemory = [](const std::string&, const std::string&) { return false; };
    ad_.modelIsValid = [this](const std::string& p, const std::string& m) { return listed(p, m); };
    ad_.providerHasKey = [this](const std::string& p) { return keyed(p); };
    ad_.setOrchHostModel = [this](const std::string& p, const std::string& m) { applied_.push_back({p, m}); };
    ad_.modelMiss = [this](const std::string& p, const std::string&) {
      const orch::RefreshAsk a = q_.request(p.c_str(), keyed(p), nowMs());
      if (a == orch::RefreshAsk::Queued) queued_++;
      return orch::modelMissText(p, lists_[p].fetchedAt, a);
    };
  }

  void buildRegistry() {
    orch::ModelsToolHandlers h;
    h.keyedProviders = [this] {
      std::vector<std::string> out;
      for (const orch::ProviderSlot& s : orch::kProviderSlots)
        if (keyed(s.slug) && lists_.count(s.slug)) out.push_back(s.slug);
      return out;
    };
    h.load = [this](const std::string& p, orch::ProviderModels& out) {
      auto it = lists_.find(p);
      if (it == lists_.end()) return false;
      out.fetched = true;
      out.fetchedAt = it->second.fetchedAt;
      for (const auto& m : it->second.models) {
        if (!m.hasRole(orch::RoleOrchestrator)) continue;
        orch::ModelRow r;
        r.id = m.id;
        r.line = m.family;
        r.size = m.size;
        r.ctx = m.ctxTokens;
        r.roles = m.roles;
        r.lifecycle = m.deprecated ? "deprecated" : "active";
        if (const orch::BuiltinPrice* bp = orch::builtinModelPrice(p.c_str(), m.id.c_str())) {
          r.priceIn = bp->microIn;
          r.priceOut = bp->microOut;
          r.priceSource = "est";
        }
        out.rows.push_back(r);
      }
      return true;
    };
    h.requestRefresh = [this](const std::string& p) {
      const orch::RefreshAsk a = q_.request(p.c_str(), keyed(p), nowMs());
      if (a == orch::RefreshAsk::Queued) queued_++;
      return a;
    };
    orch::registerModelsTool(reg_, h);
    // The device's mid-turn device.control (orchestrator.cpp), on the same apply rails.
    reg_.add("device.control",
             "Apply ONE device action IMMEDIATELY, mid-turn. 'action' is the same JSON shape as a "
             "device[] element: {\"type\":\"config\"|\"led\"|\"lights\"|\"tts\"|\"orch_model\", ...}.",
             [this](ArduinoJson::JsonObjectConst a, const orch::Principal&) -> orch::ToolResult {
               if (!a["action"].is<ArduinoJson::JsonObjectConst>())
                 return orch::ToolResult::fail("missing 'action' object");
               std::string json;
               serializeJson(a["action"], json);
               std::string risk;
               return orch::ToolResult::ok("applied: " +
                                           agent::applyDeviceElement(json, false, "lab", ad_, &risk) + risk);
             },
             R"({"type":"object","properties":{"action":{"type":"object","description":"one device[] element, verbatim"}},"required":["action"]})");
  }

  void buildEngine() {
    agent::JobEngine::Deps jd;
    jd.platform.nowMs = nowMs;
    jd.platform.freeHeap = [] { return (uint32_t)4u * 1024u * 1024u; };
    jd.deliver = [this](const std::string&, const std::string& t) { deliveries_.push_back(t); };
    jd.providerHasKey = [this](const std::string& p) { return keyed(p); };
    jd.modelIsValid = [this](const std::string& p, const std::string& m) { return listed(p, m); };
    jd.modelChoices = [this](const std::string& p) { return choices(p); };
    jobs_.reset(new agent::JobEngine(std::move(jd)));

    agent::TurnEngine::Deps d;
    auto& p = d.cfg.provider;
    p.hasKey = [this](const std::string& h) { return keyed(h) && lists_.count(h); };
    p.key = [this](const std::string& h) { return env_.providerKey(h); };
    p.orchHost = [this] { return opt_.headHost; };
    p.providerPriority = [this] { return opt_.headHost; };
    p.subPriority = [this] { return opt_.headHost; };
    p.orchModel = [this](const std::string& h) { return modelFor(h); };
    p.subModel = [this](const std::string& h) { return modelFor(h); };
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
    d.deliver = [this](const std::string&, const std::string& t) { deliveries_.push_back(t); };
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus-Lab";
      in.hostLabel = "lab";
      return in;
    };
    d.toolSpecs = [this](const orch::Principal& who) { return reg_.toolSpecsFor(who); };
    d.mcpDispatch = [this](const std::string& req, const orch::Principal& who) { return reg_.handleRpc(req, who); };
    d.modelChoices = [this](const std::string& h) { return choices(h); };
    d.firstAllowedChat = [] { return std::string("lab"); };
    d.hooks.onToolCall = [this](const orch::HeadToolCall& c) { cur_.toolCalls.push_back(c.name + "(" + c.argsJson + ")"); };
    d.hooks.onToolResult = [this](const orch::HeadToolResult& r) { cur_.toolResults.push_back(r.output); };
    d.apply = ad_;
    // As on the device: only the three cloud hosts run a tool loop (custom.cpp, the
    // Z.ai head, is single-shot), so a Z.ai turn gets the inline model lists.
    d.hosts.fabricSupports = [](const std::string& h) { return agent::providers::fabricSupports(h); };
    const std::string host = opt_.headHost;
    d.hosts.add(host, [this, host](std::string& conv, const std::string& ins, const std::string& inp, std::string& out,
                                   std::string& err, const agent::HeadTools* tools, orch::TokenUsage* usage) -> bool {
      if (host == "zai") return agent::providers::orchTurnCustom(zaiDeps(), conv, ins, inp, out, err, tools, usage);
      auto pd = deps();
      if (host == "anthropic") return agent::providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, usage);
      return agent::providers::orchTurnMistral(pd, conv, ins, inp, out, err, tools, usage);
    });
    eng_.reset(new agent::TurnEngine(std::move(d)));
  }

  const Env& env_;
  LaneHOpts opt_;
  PinnedTransport http_;
  std::map<std::string, LiveList> lists_;
  orch::ToolRegistry reg_;
  orch::ModelRefreshQueue q_;
  agent::ApplyDeps ad_;
  std::unique_ptr<agent::JobEngine> jobs_;
  std::unique_ptr<agent::TurnEngine> eng_;
  std::string conv_;
  std::vector<std::string> deliveries_;
  std::vector<std::pair<std::string, std::string>> applied_;
  int queued_ = 0;
  TurnOut cur_;
};

void printUsage(const CurlHttpTransport& http) {
  std::map<std::string, Usage> u;
  addUsage(http, 0, u);
  for (auto& kv : u)
    std::printf("    usage %s calls=%d in=%llu out=%llu cacheR=%llu cacheW=%llu usd=%.4f\n", kv.first.c_str(),
                kv.second.calls, (unsigned long long)kv.second.in, (unsigned long long)kv.second.out,
                (unsigned long long)kv.second.cacheRead, (unsigned long long)kv.second.cacheWrite, kv.second.usd);
}

struct Leg {
  std::string name;
  bool pass = false;
  std::string detail;
};

void showTurn(const TurnOut& t) {
  for (size_t i = 0; i < t.toolCalls.size(); i++)
    std::printf("      tool %s\n        -> %.300s\n", t.toolCalls[i].substr(0, 200).c_str(),
                i < t.toolResults.size() ? t.toolResults[i].c_str() : "(no result)");
  std::printf("      nimbus> %.400s\n", t.reply.empty() ? "(no reply)" : t.reply.c_str());
}

bool calledModelsList(const TurnOut& t) {
  for (const auto& c : t.toolCalls)
    if (c.rfind("models_list", 0) == 0 || c.rfind("models.list", 0) == 0) return true;
  return false;
}

Leg legLists(HRig& rig) {
  Leg l{"lists", true, ""};
  for (const char* p : {"anthropic", "mistral", "zai", "openai"}) {
    if (!rig.keyed(p)) continue;
    const bool ok = rig.fetchList(p);
    const std::string ch = rig.choices(p);
    std::printf("    %s: %s, %zu models, %zu chat ids, first: %.120s\n", p, ok ? "ok" : rig.list(p).error.c_str(),
                rig.list(p).models.size(), ch.empty() ? 0 : (size_t)std::count(ch.begin(), ch.end(), ',') + 1,
                ch.c_str());
    l.pass = l.pass && ok;
    l.detail += std::string(p) + (ok ? " ok; " : " FAILED; ");
  }
  if (rig.keyed("anthropic") && !rig.listed("anthropic", "claude-sonnet-5-5")) {
    l.pass = false;
    l.detail += "claude-sonnet-5-5 is not in the live Anthropic list; ";
  }
  return l;
}

Leg legSwitch(HRig& rig) {
  Leg l{"switch", false, ""};
  const TurnOut t = rig.say("Switch to Sonnet 5.5.");
  showTurn(t);
  for (const auto& a : rig.applied())
    if (a.first == "anthropic" && a.second == "claude-sonnet-5-5") l.pass = true;
  l.detail = std::string(l.pass ? "applied anthropic/claude-sonnet-5-5" : "no switch to claude-sonnet-5-5 applied") +
             (calledModelsList(t) ? "; the head called models.list" : "; no models.list call");
  return l;
}

// A router id for `host` whose bare id the live list holds (the refusal then names it).
std::string routerIdFor(HRig& rig, const std::string& host) {
  const char* prefer = host == "anthropic" ? "claude-sonnet-5-5" : host == "mistral" ? "mistral-large-latest" : "glm-5.3";
  if (rig.listed(host, prefer)) return host + "/" + prefer;
  const std::string ch = rig.choices(host);
  return ch.empty() ? std::string() : host + "/" + ch.substr(0, ch.find(','));
}

// Did the device refuse a model id for `host` this run: a mid-turn device.control
// answer, or the end-of-turn device[] summary in the harness log.
bool refusedFor(const TurnOut& t, const std::string& host) {
  const std::string tag = "not in " + host + " list";
  for (const auto& r : t.toolResults)
    if (has(r, tag)) return true;
  for (const auto& l : HRig::logs())
    if (has(l, "orch_model(invalid)") && has(l, tag)) return true;
  return false;
}

Leg legRouterId(HRig& rig, const std::string& host) {
  Leg l{"router-id(" + host + ")", false, ""};
  const std::string rid = routerIdFor(rig, host);
  if (rid.empty()) { l.detail = "no live list for " + host; return l; }
  const bool loops = agent::providers::fabricSupports(host);
  TurnOut t;
  if (loops) {
    // A head with a tool loop hears the refusal mid-turn and fixes it in the same turn.
    t = rig.say("Use the device.control tool now to switch your own model: provider " + host + ", model " + rid +
                ". If the device refuses it, correct the model id from what it tells you and switch again with "
                "device.control.");
  } else {
    // A single-shot head (Z.ai) cannot call a tool: it switches in its end-of-turn
    // device[], and the inline [AVAILABLE MODELS] list it reads marks the router ids
    // as cumulo-only. It passes by naming a listed id; a refused router id reaches
    // it in the next turn's results.
    t = rig.say("Switch your own model now: provider " + host + ", model " + rid + ".");
  }
  showTurn(t);
  if (HRig::echo())   // --verbose: the head's last raw answer, to see the shape it sent
    for (const auto& c : rig.http().inner.calls)
      if (c.path.find("/models") == std::string::npos)
        std::fprintf(stderr, "[raw] %.1500s\n", c.respBody.c_str());
  const bool refused = refusedFor(t, host);
  std::string last;
  for (const auto& a : rig.applied())
    if (a.first == host) last = a.second;
  const bool listedPick = !last.empty() && last != rid && rig.listed(host, last);
  l.pass = listedPick && (refused || !loops);
  l.detail = std::string(refused ? "refused with 'not in " + host + " list'" : "no refusal seen") + "; " +
             (listedPick ? "switched to " + last : "no listed id applied") +
             (loops ? "" : " (single-shot head: the inline list marks router ids; no mid-turn retry)");
  return l;
}

}  // namespace

namespace {
LaneHOpts headOpts(const LaneLOptions& o) {
  LaneHOpts h;
  h.headHost = o.headHost;
  // The lane L default head model is Anthropic's; another head takes its own cheapest.
  h.headModel = (o.headHost != "anthropic" && o.headModel == "claude-haiku-5-5") ? labDefaultModel(o.headHost)
                                                                                   : o.headModel;
  h.effort = o.effort;
  h.verbose = o.verbose;
  return h;
}

int report(const std::vector<Leg>& legs, const std::string& outPath) {
  int failed = 0;
  JsonDocument out;
  for (const auto& l : legs) {
    std::printf("  %s %s - %s\n", l.pass ? "PASS" : "FAIL", l.name.c_str(), l.detail.c_str());
    failed += l.pass ? 0 : 1;
    JsonObject o = out["legs"].add<JsonObject>();
    o["name"] = l.name;
    o["pass"] = l.pass;
    o["detail"] = l.detail;
  }
  if (!outPath.empty()) {
    std::string s;
    serializeJson(out, s);
    if (FILE* f = std::fopen(outPath.c_str(), "w")) {
      std::fputs(s.c_str(), f);
      std::fclose(f);
    }
  }
  std::printf("models-live: %d of %zu legs passed\n", (int)legs.size() - failed, legs.size());
  return failed ? 1 : 0;
}
}  // namespace

int cmdModelsLive(const Env& env, LaneLOptions o, const std::string& leg) {
  const LaneHOpts h = headOpts(o);
  if (env.providerKey(h.headHost).empty()) {
    std::fprintf(stderr, "no key for %s\n", h.headHost.c_str());
    return 2;
  }
  std::printf("models-live: head %s/%s, effort %s, reps %d\n", h.headHost.c_str(), h.headModel.c_str(),
              h.effort.empty() ? "adapter" : h.effort.c_str(), o.reps);
  std::vector<Leg> legs;
  std::printf("  [lists]\n");
  {
    HRig rig(env, h);
    legs.push_back(legLists(rig));
  }
  for (int rep = 0; rep < o.reps; rep++) {
    for (const char* name : {"switch", "router-id"}) {
      if (leg != name && leg != "all") continue;
      std::printf("  [%s] rep %d\n", name, rep + 1);
      HRig r(env, h);   // a fresh conversation and queue per turn
      legLists(r);
      r.build();
      legs.push_back(std::string(name) == "switch" ? legSwitch(r) : legRouterId(r, h.headHost));
      printUsage(r.http().inner);
    }
  }
  return report(legs, o.out);
}

}  // namespace lab
