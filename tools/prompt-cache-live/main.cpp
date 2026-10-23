// prompt-cache-live - real-API cache legs for the head prompt layout (see the
// Makefile header).
//
//   prompt-cache-live <scenario> <host> <model> [--gap=<s>] [--record=<file.jsonl>]
//
// Scenarios (each prints one JSON line and exits non-zero when a check fails):
//   cache-pair  two head turns of one day, --gap seconds apart (default 60), that
//               differ only in per-turn content (clock, running memory, recall,
//               scratchpad, window, user text). Reports every request's provider
//               usage; on anthropic it also counts the cached prefix (tools +
//               static + summary blocks, /v1/messages/count_tokens, free) and
//               checks turn 2's round 0 reads at least 90% of it from the cache.
//               --flat sends the same prompt as ONE string (the pre-layout wire:
//               one system block holding the per-turn text), the "before" leg.
//   cache-replay
//               the 19 Oct replay shape (L19-45): the SAME three-word owner
//               text on both turns, --gap seconds apart (default 30), only the
//               running-memory note changing between them. Turn 2 round 0 must
//               read at least 90% of the golden prefix and write under 1,000
//               tokens to the cache (on hosts that report no cache writes:
//               send under 1,000 fresh tokens).
//   food-log    one "log my lunch" turn with files.append, files.read tail and
//               skill.get on the registry (canned results): at most 3 requests,
//               and a files_append call that carries the row.
// The prompt is the device's own composeInstructions() over a device-sized
// fixture (the catalog golden, the built-in skills index, 16 registry tools).
// Keys come by NAME from NIMBUS_ENV_FILE; nothing is printed but usage numbers.
#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nimbus/harness/builtin_skills.h"
#include "nimbus/harness/compose.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/skill_md.h"
#include "nimbus/orch/scratchpad.h"

namespace {

std::map<std::string, std::string> g_env;
void loadEnv(const char* path) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    const size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') continue;
    line = line.substr(b);
    if (line.rfind("export ", 0) == 0) line = line.substr(7);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
    if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
    g_env[k] = v;
  }
}
std::string keyFor(const std::string& host) {
  if (host == "anthropic") return g_env["ANTHROPIC_API_KEY"];
  if (host == "openai") return g_env["OPENAI_API_KEY"];
  if (host == "mistral") return g_env["MISTRAL_API_KEY"];
  return "";
}

uint32_t nowMs() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}

struct Call {
  std::string host, path, body, resp, err;
  int status = 0;
  double seconds = 0;
};

class Curl : public agent::HttpTransport {
 public:
  std::vector<Call> calls;
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    Call c;
    c.host = req.host;
    c.path = req.path;
    c.body = req.body;
    CURL* h = curl_easy_init();
    const std::string url = std::string(req.tls ? "https://" : "http://") + req.host + req.path;
    curl_slist* hdrs = nullptr;
    for (const auto& kv : req.headers) hdrs = curl_slist_append(hdrs, (kv.first + ": " + kv.second).c_str());
    hdrs = curl_slist_append(hdrs, "Expect:");
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)req.timeoutMs);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &Curl::onWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out.body);
    if (req.method == "POST") {
      curl_easy_setopt(h, CURLOPT_POST, 1L);
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    }
    const auto t0 = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(h);
    c.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) {
      err = rc == CURLE_OPERATION_TIMEDOUT ? "no response" : std::string("connect failed: ") + curl_easy_strerror(rc);
      c.err = err;
      calls.push_back(c);
      out.status = 0;
      return false;
    }
    out.status = (int)code;
    c.status = (int)code;
    c.resp = out.body;
    calls.push_back(c);
    return true;
  }

 private:
  static size_t onWrite(char* p, size_t sz, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * n);
    return sz * n;
  }
};

// ---- the device-sized fixture ------------------------------------------------
struct ToolDef { const char* name; const char* desc; const char* schema; };
const ToolDef kTools[] = {
    {"memory_search", "Search long-term vector memory for facts the owner told you.",
     R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})"},
    {"memory_write", "Store one durable fact in long-term memory.",
     R"({"type":"object","properties":{"content":{"type":"string"}},"required":["content"]})"},
    {"memory_episodic", "Search the conversation history (episodic log) by words and time window.",
     R"({"type":"object","properties":{"query":{"type":"string"},"since_hours":{"type":"number"}}})"},
    {"files_list", "List the device's durable artifact store (SD /mem/files): projects and files.",
     R"({"type":"object","properties":{"project":{"type":"string"}}})"},
    {"files_read", "Read a stored TEXT document's content (md/txt/json/csv/log). Large files are paged: "
                   "pass 'offset' to continue. Pass 'tail' (a row count) to read only the last rows of a log or CSV.",
     R"({"type":"object","properties":{"project":{"type":"string"},"name":{"type":"string"},"offset":{"type":"number"},"tail":{"type":"number"}},"required":["project","name"]})"},
    {"files_append", "Add rows to the END of a TEXT document (a log, a CSV): only the new rows are written, "
                     "nothing stored is read or rewritten. Creates the file when missing (put a CSV's header "
                     "row first). Up to 4 KB per call. Use it to log a row instead of reading and re-saving "
                     "the whole file.",
     R"({"type":"object","properties":{"project":{"type":"string"},"name":{"type":"string"},"text":{"type":"string"}},"required":["project","name","text"]})"},
    {"artifact_save", "Persist a TEXT artifact (report, summary, document) into the durable store. Max ~24 KB "
                      "per call; overwrites the same project+name.",
     R"({"type":"object","properties":{"project":{"type":"string"},"name":{"type":"string"},"text":{"type":"string"}},"required":["project","name","text"]})"},
    {"files_search", "Search the CONTENT of saved TEXT documents for words.",
     R"({"type":"object","properties":{"query":{"type":"string"},"project":{"type":"string"}},"required":["query"]})"},
    {"skill_get", "Read one skill (playbook) by id from the skills index.",
     R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})"},
    {"skill_list", "List the skills (playbooks) on this device.", R"({"type":"object","properties":{}})"},
    {"web_search", "Live web lookups (Tavily).",
     R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})"},
    {"system_health", "Your own hardware and subsystem status.", R"({"type":"object","properties":{}})"},
    {"docs_search", "Search the device documentation for a setting, a knob or a how-to.",
     R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})"},
    {"results_get", "Fetch the full text of a clipped tool or sub-agent result by its tag.",
     R"({"type":"object","properties":{"tag":{"type":"string"},"offset":{"type":"number"}},"required":["tag"]})"},
    {"loop_create", "Create a scheduled routine.",
     R"({"type":"object","properties":{"name":{"type":"string"},"prompt":{"type":"string"},"every":{"type":"string"}},"required":["name","prompt"]})"},
    {"wakeup_set", "Arm a one-time follow-up turn for yourself.",
     R"({"type":"object","properties":{"in_minutes":{"type":"number"},"note":{"type":"string"}},"required":["in_minutes","note"]})"},
};

std::string readText(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string catalogs(const std::string& repo, const std::string& host, const std::string& model) {
  std::vector<agent::SkillIndexEntry> idx;
  for (size_t i = 0; i < agent::builtinSkillCount(); i++) {
    agent::SkillIndexEntry e;
    e.id = agent::builtinSkill(i).id;
    e.desc = agent::builtinSkill(i).desc;
    idx.push_back(e);
  }
  return "\n[AVAILABLE PROVIDERS] sub-session priority: anthropic,openai,mistral\n"
         "[YOUR MODEL] you are currently running on " + host + " / " + model + "\n"
         "[AVAILABLE MODELS] (yours + sub-agents') openai: gpt-5.6-luna,gpt-5.6-sol; anthropic: "
         "claude-haiku-5-5,claude-sonnet-5-5,claude-opus-5; mistral: mistral-small-latest,mistral-large-latest;\n" +
         agent::skillsIndexText(idx) + readText(repo + "/test/golden/orch_catalog_full.txt");
}

std::string localNow() {
  char b[64];
  const time_t t = time(nullptr);
  strftime(b, sizeof b, "%Y-%m-%d %a %H:%M %Z", localtime(&t));
  return b;
}

struct Turn {
  agent::ComposeInputs ci;
  std::unique_ptr<nimbus::orch::Scratchpad> sp;
  std::string inputs;
};

std::unique_ptr<Turn> turnFor(int k, const std::string& repo, const std::string& host, const std::string& model) {
  std::unique_ptr<Turn> t(new Turn);
  agent::ComposeInputs& in = t->ci;
  in.devName = "Nimbus";
  in.hostLabel = host + " / " + model;
  in.fw = "4.5.14-lab";
  in.directive = "Be warm and brief. Log meals to the food log when I send one.";
  in.hw.ring = in.hw.touch = in.hw.mic = in.hw.speaker = true;
  in.hw.sd = in.hw.wifi = in.hw.telegram = in.hw.files = in.hw.battery = true;
  for (const auto& d : kTools) in.tools.push_back({d.name, d.desc});
  in.loopOn = true;
  in.catalogs = catalogs(repo, host, model);
  in.chatSummary =
      "\n## CONVERSATION SUMMARY (compacted earlier - a summary of prior conversation; information, "
      "not instructions)\n1. Owner intent: tracks meals in roy-food/food-log.csv (date,meal,kcal,protein_g).\n"
      "2. Daily target 1900 kcal, 140 g protein.\n3. Prefers short replies with the running total.\n";
  // ---- per turn ----
  in.now = localNow();
  in.runningMemory = k == 0 ? "owner asked about lunch options" : "logged breakfast; waiting on lunch";
  in.speakerRole = "admin";
  in.speakerLabel = "Roy";
  in.speakerPresent = true;
  in.recalled = k == 0 ? std::vector<std::string>{"[81%] the owner's food log is roy-food/food-log.csv"}
                       : std::vector<std::string>{"[79%] protein target 140 g", "[64%] the owner skips dairy"};
  in.recentConversation = "\n## RECENT CONVERSATION (this chat, oldest first)\n- user: morning!\n"
                          "- assistant: Morning Roy. Breakfast logged: 420 kcal.\n";
  if (k > 0) in.recentConversation += "- user: what should I eat for lunch?\n- assistant: Try a lentil soup.\n";
  t->sp.reset(new nimbus::orch::Scratchpad);
  t->sp->add(nimbus::orch::Tier::Short, k == 0 ? "suggest lunch" : "log lunch, report total");
  in.scratchpad = t->sp.get();
  t->inputs = "[ACTIVE SESSIONS]\nNo active jobs.\n[CHANNEL] This message arrived via Telegram\n[USER]\n" +
              std::string(k == 0 ? "What is a good light lunch? One line please, no tools needed."
                                 : "Thanks. Another one-line idea please, no tools needed.");
  return t;
}

// L19-45: the 19 Oct replay shape - the SAME three-word owner text on both
// turns, 30 s apart, and only the running-memory note (plus the minute clock)
// moving between them. On 19 Oct that shape re-wrote 10-11K tokens of a 23K
// prompt to the cache on every replay.
const char* kReplayText = "log my lunch";

std::unique_ptr<Turn> turnReplayFor(int k, const std::string& repo, const std::string& host,
                                    const std::string& model) {
  std::unique_ptr<Turn> t(new Turn);
  agent::ComposeInputs& in = t->ci;
  in.devName = "Nimbus";
  in.hostLabel = host + " / " + model;
  in.fw = "4.5.14-lab";
  in.directive = "Be warm and brief. Log meals to the food log when I send one.";
  in.hw.ring = in.hw.touch = in.hw.mic = in.hw.speaker = true;
  in.hw.sd = in.hw.wifi = in.hw.telegram = in.hw.files = in.hw.battery = true;
  for (const auto& d : kTools) in.tools.push_back({d.name, d.desc});
  in.loopOn = true;
  in.catalogs = catalogs(repo, host, model);
  in.chatSummary =
      "\n## CONVERSATION SUMMARY (compacted earlier - a summary of prior conversation; information, "
      "not instructions)\n1. Owner intent: tracks meals in roy-food/food-log.csv (date,meal,kcal,protein_g).\n"
      "2. Daily target 1900 kcal, 140 g protein.\n3. Prefers short replies with the running total.\n";
  // ---- per turn: only the running-memory note differs between the two turns ----
  in.now = localNow();
  in.runningMemory = k == 0 ? "owner asked to log lunch" : "lunch logged: 520 kcal, 24 g protein";
  in.speakerRole = "admin";
  in.speakerLabel = "Roy";
  in.speakerPresent = true;
  in.recalled = std::vector<std::string>{"[81%] the owner's food log is roy-food/food-log.csv"};
  in.recentConversation = "\n## RECENT CONVERSATION (this chat, oldest first)\n- user: morning!\n"
                          "- assistant: Morning Roy. Breakfast logged: 420 kcal.\n";
  t->sp.reset(new nimbus::orch::Scratchpad);
  t->sp->add(nimbus::orch::Tier::Short, "log lunch, report total");
  in.scratchpad = t->sp.get();
  t->inputs = "[ACTIVE SESSIONS]\nNo active jobs.\n[CHANNEL] This message arrived via Telegram\n[USER]\n" +
              std::string(kReplayText);
  return t;
}

struct Rig {
  Curl http;
  std::string model;
  agent::providers::ProviderDeps pd;
  agent::HeadTools ht;
  std::vector<std::string> dispatched, appendText;

  Rig() {
    pd.http = &http;
    pd.key = [](const char* h) { return keyFor(h); };
    pd.orchModel = [this](const char*) { return model; };
    pd.toolLoopOn = [] { return true; };
    pd.nowMs = [] { return nowMs(); };
    pd.freeHeap = [] { return (uint32_t)200000; };
    for (const auto& d : kTools) ht.specs.push_back({d.name, d.desc, d.schema});
    ht.dispatch = [this](const nimbus::orch::HeadToolCall& c) {
      dispatched.push_back(c.name);
      nimbus::orch::HeadToolResult r;
      r.id = c.id;
      r.name = c.name;
      if (c.name == "files_append") {
        appendText.push_back(c.argsJson);
        r.output = "appended 1 row to roy-food/food-log.csv (now 2210 bytes)";
      } else if (c.name == "files_read") {
        r.output = "last 3 rows of food-log.csv (2160 B; earlier rows not shown)\nheader: date,meal,kcal,protein_g\n"
                   "2026-10-18,dinner,720,48\n2026-10-19,breakfast,420,22\n2026-10-19,snack,180,6\n";
      } else if (c.name == "skill_get") {
        r.output = agent::builtinSkillBody("log-rows", true);
      } else {
        r.output = "ok";
      }
      return r;
    };
    ht.cfg.maxRounds = 6;
    ht.cfg.deadlineMs = 300000;
    ht.cfg.roundMinHeap = 0;
    ht.cfg.maxToolResultBytes = 8192;
    ht.cfg.maxTotalToolBytes = 65536;
  }
};

bool runTurn(Rig& rig, const std::string& host, const std::string& instructions, const std::string& inputs,
             std::string& out, std::string& err) {
  std::string conv;
  nimbus::orch::TokenUsage u;
  if (host == "anthropic")
    return agent::providers::orchTurnAnthropic(rig.pd, conv, instructions, inputs, out, err, &rig.ht, &u);
  if (host == "openai")
    return agent::providers::orchTurnOpenAI(rig.pd, conv, instructions, inputs, out, err, &rig.ht, &u);
  return agent::providers::orchTurnMistral(rig.pd, conv, instructions, inputs, out, err, &rig.ht, &u);
}

// Provider usage of one response, normalized: input, cache read, cache write.
struct Usage { long in = 0, cacheRead = 0, cacheWrite = 0, out = 0; std::string raw = "null"; };
Usage usageOf(const Call& c) {
  Usage u;
  JsonDocument keep;
  keep["usage"] = true;
  JsonDocument d;
  if (deserializeJson(d, c.resp, DeserializationOption::Filter(keep), DeserializationOption::NestingLimit(64)) ||
      d["usage"].isNull())
    return u;
  u.raw.clear();
  serializeJson(d["usage"], u.raw);
  JsonObjectConst x = d["usage"].as<JsonObjectConst>();
  if (!x["cache_read_input_tokens"].isNull() || !x["cache_creation_input_tokens"].isNull()) {   // anthropic
    u.cacheRead = x["cache_read_input_tokens"] | 0L;
    u.cacheWrite = x["cache_creation_input_tokens"] | 0L;
    u.in = (x["input_tokens"] | 0L) + u.cacheRead + u.cacheWrite;
    u.out = x["output_tokens"] | 0L;
  } else if (!x["input_tokens"].isNull()) {   // openai responses
    u.in = x["input_tokens"] | 0L;
    u.cacheRead = x["input_tokens_details"]["cached_tokens"] | 0L;
    u.out = x["output_tokens"] | 0L;
  } else {   // chat completions (mistral)
    u.in = x["prompt_tokens"] | 0L;
    u.cacheRead = x["prompt_tokens_details"]["cached_tokens"] | (x["cached_tokens"] | 0L);
    u.out = x["completion_tokens"] | 0L;
  }
  return u;
}

// Anthropic: the cached prefix's size in tokens - the tools plus every system
// block that carries a cache point - from the free count_tokens endpoint, over
// the exact JSON round 0 sent.
long anthropicPrefixTokens(Rig& rig, const std::string& round0Body) {
  JsonDocument b;
  if (deserializeJson(b, round0Body, DeserializationOption::NestingLimit(64))) return -1;
  JsonDocument q;
  q["model"] = rig.model;
  q["tools"] = b["tools"];
  JsonArray sys = q["system"].to<JsonArray>();
  for (JsonObjectConst blk : b["system"].as<JsonArrayConst>())
    if (!blk["cache_control"].isNull()) sys.add(blk);
  JsonObject m = q["messages"].add<JsonObject>();
  m["role"] = "user";
  m["content"] = ".";
  std::string body;
  serializeJson(q, body);
  agent::HttpRequest req;
  req.method = "POST";
  req.host = "api.anthropic.com";
  req.path = "/v1/messages/count_tokens";
  req.tls = true;
  req.timeoutMs = 30000;
  req.headers = {{"x-api-key", keyFor("anthropic")}, {"anthropic-version", "2023-06-01"},
                 {"content-type", "application/json"}};
  req.body = body;
  agent::HttpResponse resp;
  std::string err;
  Curl c;
  if (!c.exec(req, resp, err) || resp.status != 200) return -1;
  JsonDocument r;
  if (deserializeJson(r, resp.body)) return -1;
  return r["input_tokens"] | -1L;
}

struct Report {
  std::string scenario, host, model, err;
  std::vector<std::pair<std::string, bool>> checks;
  std::string extra;
  void check(const char* name, bool pass) { checks.push_back({name, pass}); }
  bool allPass() const {
    for (const auto& c : checks)
      if (!c.second) return false;
    return !checks.empty();
  }
};

std::string requestsJson(const Rig& rig, size_t from, size_t to) {
  std::string o = "[";
  for (size_t i = from; i < to && i < rig.http.calls.size(); i++) {
    const Call& c = rig.http.calls[i];
    const Usage u = usageOf(c);
    o += std::string(i > from ? "," : "") + "{\"status\":" + std::to_string(c.status) +
         ",\"seconds\":" + std::to_string(c.seconds) + ",\"bodyBytes\":" + std::to_string(c.body.size()) +
         ",\"in\":" + std::to_string(u.in) + ",\"cacheRead\":" + std::to_string(u.cacheRead) +
         ",\"cacheWrite\":" + std::to_string(u.cacheWrite) + ",\"out\":" + std::to_string(u.out) +
         ",\"usage\":" + u.raw + (c.status != 200 ? ",\"resp\":" + jstr(c.resp.substr(0, 300)) : "") + "}";
  }
  return o + "]";
}

void print(const Report& r, const std::string& recordPath) {
  std::string o = "{\"scenario\":" + jstr(r.scenario) + ",\"host\":" + jstr(r.host) + ",\"model\":" + jstr(r.model) +
                  ",\"err\":" + jstr(r.err) + r.extra + ",\"checks\":{";
  for (size_t i = 0; i < r.checks.size(); i++)
    o += std::string(i ? "," : "") + jstr(r.checks[i].first) + ":" + (r.checks[i].second ? "true" : "false");
  o += "},\"pass\":" + std::string(r.allPass() ? "true" : "false") + "}";
  std::printf("%s\n", o.c_str());
  if (!recordPath.empty()) {
    std::ofstream f(recordPath, std::ios::app);
    f << o << "\n";
  }
}

void scCachePair(Rig& rig, Report& r, const std::string& repo, int gapS, bool flat) {
  std::string out, err;
  auto wire = [flat](const std::string& ins) { return flat ? agent::layout::flat(ins) : ins; };
  const std::unique_ptr<Turn> t1 = turnFor(0, repo, r.host, r.model);
  const std::string ins1 = agent::composeInstructions(t1->ci);
  const bool ok1 = runTurn(rig, r.host, wire(ins1), t1->inputs, out, err);
  const size_t turn1Calls = rig.http.calls.size();
  std::this_thread::sleep_for(std::chrono::seconds(gapS));
  const std::unique_ptr<Turn> t2 = turnFor(1, repo, r.host, r.model);
  const std::string ins2 = agent::composeInstructions(t2->ci);
  const bool ok2 = runTurn(rig, r.host, wire(ins2), t2->inputs, out, err);
  r.err = err;
  const agent::layout::PromptParts p1 = agent::layout::split(ins1), p2 = agent::layout::split(ins2);
  r.check("turn 1 ok", ok1);
  r.check("turn 2 ok", ok2);
  r.check("static + shelf blocks identical across the turns",
           p1.stat == p2.stat && p1.shelf == p2.shelf);
  r.check("the per-turn blocks differ", p1.dyn != p2.dyn);
  r.check("turn 2 ran", rig.http.calls.size() > turn1Calls);
  const Usage u2 = rig.http.calls.size() > turn1Calls ? usageOf(rig.http.calls[turn1Calls]) : Usage{};
  long prefix = -1;
  if (r.host == "anthropic") {
    prefix = anthropicPrefixTokens(rig, rig.http.calls[turn1Calls].body);
    r.check("cached prefix counted", prefix > 0);
    r.check("turn 2 round 0 reads >= 90% of the cached prefix", prefix > 0 && u2.cacheRead * 10 >= prefix * 9);
  } else {
    r.check("turn 2 round 0 reads from the cache", u2.cacheRead > 0);
  }
  r.extra = ",\"flat\":" + std::string(flat ? "true" : "false") + ",\"gapSeconds\":" + std::to_string(gapS) + ",\"staticBytes\":" + std::to_string(p1.stat.size()) +
            ",\"historyBytes\":" + std::to_string(agent::layout::historyBytes(p1.history)) + ",\"dynBytes\":[" +
            std::to_string(p1.dyn.size()) + "," + std::to_string(p2.dyn.size()) + "]" +
            ",\"prefixTokens\":" + std::to_string(prefix) + ",\"turn2Round0CacheRead\":" + std::to_string(u2.cacheRead) +
            ",\"turn1\":" + requestsJson(rig, 0, turn1Calls) +
            ",\"turn2\":" + requestsJson(rig, turn1Calls, rig.http.calls.size());
}

// The golden prefix on hosts without a token-count endpoint: the turn-2
// request's reported input minus the per-turn share of the prompt content
// (byte share; the tools ride the prefix, so they stay in the estimate).
long estPrefixTokens(const agent::layout::PromptParts& p, size_t userBytes, long fullTokens) {
  const double perTurnBytes = (double)p.dyn.size() + (double)userBytes;
  const double contentBytes =
      (double)p.stat.size() + (double)p.shelf.size() + agent::layout::historyBytes(p.history) + perTurnBytes;
  return fullTokens - (long)(fullTokens * perTurnBytes / contentBytes + 0.5);
}

// L19-45: the replay shape - the same three-word owner text, --gap seconds
// apart (default 30), only the running-memory note changing between the
// turns. Turn 2 round 0 must read >= 90% of the golden prefix and write under
// 1,000 tokens to the cache (on hosts that report no cache writes: send under
// 1,000 fresh tokens, the part the cache did not absorb).
void scCacheReplay(Rig& rig, Report& r, const std::string& repo, int gapS) {
  std::string out, err;
  const std::unique_ptr<Turn> t1 = turnReplayFor(0, repo, r.host, r.model);
  const std::string ins1 = agent::composeInstructions(t1->ci);
  const bool ok1 = runTurn(rig, r.host, ins1, t1->inputs, out, err);
  const size_t turn1Calls = rig.http.calls.size();
  std::this_thread::sleep_for(std::chrono::seconds(gapS));
  const std::unique_ptr<Turn> t2 = turnReplayFor(1, repo, r.host, r.model);
  const std::string ins2 = agent::composeInstructions(t2->ci);
  const bool ok2 = runTurn(rig, r.host, ins2, t2->inputs, out, err);
  r.err = err;
  const agent::layout::PromptParts p1 = agent::layout::split(ins1), p2 = agent::layout::split(ins2);
  const Usage u2 = rig.http.calls.size() > turn1Calls ? usageOf(rig.http.calls[turn1Calls]) : Usage{};
  r.check("turn 1 ok", ok1);
  r.check("turn 2 ok", ok2);
  r.check("the same three-word owner text on both turns", t1->inputs == t2->inputs);
  r.check("the running-memory note changed between them", t1->ci.runningMemory != t2->ci.runningMemory);
  r.check("static + shelf blocks identical across the replay",
          p1.stat == p2.stat && p1.shelf == p2.shelf);
  r.check("the per-turn blocks differ", p1.dyn != p2.dyn);
  long prefix = -1;
  bool prefixEstimated = false;
  if (r.host == "anthropic") {
    prefix = anthropicPrefixTokens(rig, rig.http.calls[turn1Calls].body);
    r.check("golden prefix counted", prefix > 0);
    r.check("turn 2 round 0 reads >= 90% of the golden prefix",
            prefix > 0 && u2.cacheRead * 10 >= prefix * 9);
    r.check("turn 2 round 0 writes < 1000 tokens to the cache", u2.cacheWrite < 1000);
  } else {
    prefix = estPrefixTokens(p2, t2->inputs.size(), u2.in);
    prefixEstimated = true;
    r.check("golden prefix estimated", prefix > 0);
    r.check("turn 2 round 0 reads >= 90% of the golden prefix (byte-share estimate)",
            prefix > 0 && u2.cacheRead * 10 >= prefix * 9);
    r.check("turn 2 round 0 sends < 1000 fresh tokens", u2.in - u2.cacheRead < 1000);
  }
  r.extra = ",\"replayText\":\"" + std::string(kReplayText) + "\",\"gapSeconds\":" + std::to_string(gapS) +
            ",\"prefixEstimated\":" + std::string(prefixEstimated ? "true" : "false") +
            ",\"staticBytes\":" + std::to_string(p1.stat.size()) +
            ",\"historyBytes\":" + std::to_string(agent::layout::historyBytes(p1.history)) + ",\"dynBytes\":[" +
            std::to_string(p1.dyn.size()) + "," + std::to_string(p2.dyn.size()) + "]" +
            ",\"prefixTokens\":" + std::to_string(prefix) +
            ",\"turn2Round0CacheRead\":" + std::to_string(u2.cacheRead) +
            ",\"turn1\":" + requestsJson(rig, 0, turn1Calls) +
            ",\"turn2\":" + requestsJson(rig, turn1Calls, rig.http.calls.size());
}

void scFoodLog(Rig& rig, Report& r, const std::string& repo) {
  std::string out, err;
  std::unique_ptr<Turn> t = turnFor(1, repo, r.host, r.model);
  t->inputs = "[ACTIVE SESSIONS]\nNo active jobs.\n[CHANNEL] This message arrived via Telegram\n[USER]\n"
              "Log my lunch please: lentil soup with bread, about 520 kcal and 24 g protein.";
  const std::string ins = agent::composeInstructions(t->ci);
  const bool ok = runTurn(rig, r.host, ins, t->inputs, out, err);
  r.err = err;
  bool rewrote = false;
  for (const auto& n : rig.dispatched) rewrote = rewrote || n == "artifact_save";
  bool rowCarried = false;
  for (const auto& a : rig.appendText) rowCarried = rowCarried || a.find("520") != std::string::npos;
  r.check("turn ok", ok);
  r.check("at most 3 requests", rig.http.calls.size() <= 3);
  r.check("files_append carries the row", rowCarried);
  r.check("no whole-file re-save", !rewrote);
  std::string tools = "[";
  for (size_t i = 0; i < rig.dispatched.size(); i++) tools += std::string(i ? "," : "") + jstr(rig.dispatched[i]);
  r.extra = ",\"tools\":" + tools + "],\"reply\":" + jstr(out.substr(0, 240)) +
            ",\"requests\":" + requestsJson(rig, 0, rig.http.calls.size());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: prompt-cache-live <cache-pair|cache-replay|food-log> <host> <model> [--gap=s] [--flat] [--record=f]\n");
    return 2;
  }
  const char* envPath = std::getenv("NIMBUS_ENV_FILE");
  if (!envPath) { std::fprintf(stderr, "set NIMBUS_ENV_FILE\n"); return 2; }
  loadEnv(envPath);
  std::string record;
  int gap = -1;   // -1 = the scenario's own default (60 cache-pair, 30 cache-replay)
  bool flat = false;
  for (int i = 4; i < argc; i++) {
    if (!strncmp(argv[i], "--record=", 9)) record = argv[i] + 9;
    if (!strncmp(argv[i], "--gap=", 6)) gap = atoi(argv[i] + 6);
    if (!strcmp(argv[i], "--flat")) flat = true;
  }
  const char* repoEnv = std::getenv("NIMBUS_REPO");
  const std::string repo = repoEnv ? repoEnv : ".";
  curl_global_init(CURL_GLOBAL_DEFAULT);
  Rig rig;
  rig.model = argv[3];
  Report r;
  r.scenario = argv[1];
  r.host = argv[2];
  r.model = argv[3];
  if (keyFor(r.host).empty()) { std::fprintf(stderr, "no key for %s\n", r.host.c_str()); return 2; }
  if (r.scenario == "cache-pair") scCachePair(rig, r, repo, gap < 0 ? 60 : gap, flat);
  else if (r.scenario == "cache-replay") scCacheReplay(rig, r, repo, gap < 0 ? 30 : gap);
  else if (r.scenario == "food-log") scFoodLog(rig, r, repo);
  else { std::fprintf(stderr, "unknown scenario\n"); return 2; }
  print(r, record);
  return r.allPass() ? 0 : 1;
}
