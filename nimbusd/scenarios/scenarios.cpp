// nimbusd-scenarios - the harness-lab scenario suite, run AGAINST THE DAEMON.
//
// These are the same outcome-asserting scenarios harness-lab uses (web.search
// end to end, memory round-trip, multi-tool, tool-loop, scheduled/dream turns,
// provider parity), but driven through NimbusdRig - the real TurnEngine composed
// with the daemon's DURABLE POSIX stores - so a pass proves the hosted daemon
// runs a real assistant. It adds one scenario the in-memory lab cannot have:
// memory survives a genuine daemon RESTART (write, tear the rig down, rebuild it
// on the same data dir, recall).
//
// Real provider calls cost real money; each scenario asserts an OUTCOME, not
// "HTTP 200". Scenarios needing a capability with no key SKIP LOUDLY (a silent
// skip reads as a pass). Keys come from the environment or an --env dotenv file
// (default: NIMBUS_ENV_FILE, then ~/.env), same as the lab + HIL suite.
//
//   nimbusd-scenarios                 run all applicable scenarios
//   nimbusd-scenarios web-search mem  run only the named ones
//   nimbusd-scenarios --env /path     use a specific dotenv file
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "daemon_config.h"
#include "nimbus/harness/websearch.h"
#include "daemon_http.h"
#include "rig.h"

using namespace nimbusd;

namespace {

struct Ctx {
  std::string name;
  int failures = 0;
  void check(bool cond, const std::string& what) {
    std::printf(cond ? "    ok   %s\n" : "    FAIL %s\n", what.c_str());
    if (!cond) failures++;
  }
  void note(const std::string& n) { std::printf("    ...  %s\n", n.c_str()); }
};

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
bool has(const std::string& hay, const std::string& n) { return lower(hay).find(lower(n)) != std::string::npos; }
bool calledTool(const TurnRecord& t, const std::string& name) {
  for (const auto& c : t.toolCalls) if (c.rfind(name, 0) == 0) return true;
  return false;
}
int toolCallCount(const TurnRecord& t, const std::string& name) {
  int n = 0; for (const auto& c : t.toolCalls) if (c.rfind(name, 0) == 0) n++; return n;
}
void showTurn(const TurnRecord& t) {
  std::printf("    > %s\n", t.userText.substr(0, 110).c_str());
  for (size_t i = 0; i < t.toolCalls.size(); i++)
    std::printf("      [tool] %s\n", t.toolCalls[i].c_str());
  std::printf("    < %s\n", t.reply.empty() ? "(no reply)" : t.reply.substr(0, 300).c_str());
}

// Fresh data dir per scenario so memory from one does not mask another.
NimbusdRig::Options baseOpt(const Config& cfg, const std::string& tag) {
  NimbusdRig::Options o;
  const char* base = std::getenv("TMPDIR");
  o.dataDir = std::string(base && *base ? base : "/tmp") + "/nimbusd-scn-" + tag;
  o.embedHost = "mistral";
  o.embeddings = !cfg.providerKey("mistral").empty();
  if (!o.embeddings && !cfg.providerKey("openai").empty()) { o.embedHost = "openai"; o.embeddings = true; }
  o.embedModel = o.embedHost == "mistral" ? "mistral-embed" : "text-embedding-3-small";
  o.embedDims = o.embedHost == "mistral" ? 1024 : 1536;
  return o;
}
void wipe(const std::string& dir) { std::string c = "rm -rf '" + dir + "'"; (void)std::system(c.c_str()); }

// ---- scenarios (adapted from tools/harness-lab/src/scenarios.cpp) ----------

void scWebSearch(NimbusdRig& rig, Ctx& c) {
  auto t = rig.say("owner", "Search the web for today's top world news headline, then tell me "
                            "the single most important story in one sentence. Cite the URL.");
  showTurn(t);
  c.check(calledTool(t, "web.search") || calledTool(t, "web_search"), "the model called web.search");
  bool failed = false;
  for (const auto& r : t.toolResults) if (has(r, "web search failed")) failed = true;
  c.check(!failed, "web.search returned results (not an error)");
  c.check(!t.reply.empty(), "the model produced a reply");
  c.check(has(t.reply, "http") || has(t.reply, "www."), "the reply cites a source URL");
}

void scSearchErrorHonesty(NimbusdRig&, Ctx& c) {
  DaemonHttpTransport http;
  auto bad = agent::websearch::search(http, "tvly-dev-obviously-not-a-real-key", "anything", 3);
  std::printf("    bad-key error: \"%s\"\n", bad.err.c_str());
  c.check(!bad.ok, "a bad key fails");
  c.check(has(bad.err, "401") || has(bad.err, "unauthor") || has(bad.err, "key"),
          "the error names the cause (not 'no results')");
  c.check(!has(bad.err, "no results"), "a failure is never reported as 'no results'");
}

void scMultiTool(NimbusdRig& rig, Ctx& c) {
  auto t = rig.say("owner",
                   "Do all of this in one go: save a note to memory that my favourite "
                   "debugging drink is oolong tea, then save a file called plan.txt in "
                   "project 'lab' containing 'step one: reproduce', then list your files. "
                   "Report what you did.");
  showTurn(t);
  c.check(t.toolCalls.size() >= 2, "at least two tool calls in one turn (" +
                                       std::to_string(t.toolCalls.size()) + ")");
  c.check(calledTool(t, "memory.write") || calledTool(t, "memory_write"), "wrote to memory");
  c.check(calledTool(t, "artifact.save") || calledTool(t, "artifact_save"), "saved a file");
  c.check(rig.files().has("lab/plan.txt"), "the file really landed on disk");
}

void scMemoryRoundtrip(NimbusdRig& rig, Ctx& c) {
  const std::string secret = "the workshop spare key is behind the blue ceramic owl";
  auto t1 = rig.say("mem", "Remember this permanently: " + secret);
  showTurn(t1);
  c.check(calledTool(t1, "memory.write") || calledTool(t1, "memory_write"), "turn 1 wrote the fact");
  c.check(rig.vectors().size() > 0, "a vector landed in the store");
  auto t2 = rig.say("other", "Where is the workshop spare key kept?");
  showTurn(t2);
  c.check(has(t2.reply, "owl") || has(t2.reply, "ceramic"), "recalled the fact in a DIFFERENT chat");
}

// The daemon-only scenario: memory survives a real RESTART with live embeddings.
void scRestartPersistence(const Config& cfg, Ctx& c) {
  auto opt = baseOpt(cfg, "restart");
  wipe(opt.dataDir);
  {
    NimbusdRig rig(cfg, opt);
    auto t = rig.say("owner", "Remember permanently: the vault code is emerald-seventeen.");
    showTurn(t);
    c.check(rig.vectors().size() > 0, "the fact was written before the restart");
  }  // rig destroyed -> flushed -> "process exit"
  {
    NimbusdRig rig(cfg, opt);  // fresh process on the same data dir
    c.check(rig.vectors().size() > 0, "vector memory rehydrated after the restart");
    auto t = rig.say("owner", "What is the vault code?");
    showTurn(t);
    c.check(has(t.reply, "emerald") || has(t.reply, "seventeen"),
            "the assistant recalled the fact from BEFORE the restart");
  }
  wipe(opt.dataDir);
}

void scScheduled(NimbusdRig& rig, Ctx& c) {
  auto r = rig.scheduled("owner",
                         "Search the web for one notable technology story from the last few "
                         "days and summarize it in two sentences for the owner.", "news-brief");
  const auto& t = rig.turns().back();
  showTurn(t);
  c.check(r.ok, "the scheduled turn reported ok");
  c.check(!t.reply.empty(), "the routine produced something to deliver");
}

struct Scenario { const char* name; const char* what; void (*fn)(NimbusdRig&, Ctx&); bool needsSearch; };
const Scenario kScenarios[] = {
    {"web-search",    "live web.search end to end, model uses the result", scWebSearch, true},
    {"search-errors", "a failing search names its cause",                  scSearchErrorHonesty, false},
    {"multi-tool",    "several tool calls in one turn",                    scMultiTool, false},
    {"memory",        "write in one chat, recall in another",             scMemoryRoundtrip, false},
    {"scheduled",     "an unattended routine turn",                       scScheduled, true},
};

}  // namespace

struct RunTotals { int failed = 0, ran = 0, skipped = 0; };
struct Keys { bool provider = false; bool search = false; };

// dotenv precedence: explicit --env, then NIMBUS_ENV_FILE, then ~/.env.
void loadEnv(Config& cfg, const std::string& envPath) {
  if (!envPath.empty()) cfg.loadFile(envPath);
  if (const char* nf = std::getenv("NIMBUS_ENV_FILE")) cfg.loadFile(nf);
  if (const char* home = std::getenv("HOME")) cfg.loadFile(std::string(home) + "/.env");
}

bool selected(const std::vector<std::string>& names, const char* name) {
  return names.empty() || std::find(names.begin(), names.end(), name) != names.end();
}

void printBanner(Config& cfg, const Keys& keys) {
  auto mark = [&](const char* h) { return cfg.providerKey(h).empty() ? "-" : "y"; };
  std::printf("nimbusd scenarios - providers[mistral:%s openai:%s anthropic:%s] tavily:%s\n\n",
              mark("mistral"), mark("openai"), mark("anthropic"), keys.search ? "y" : "-");
}

// Run one scenario (or record it as SKIPPED), updating the totals.
void runOne(Config& cfg, const Scenario& s, const Keys& keys, RunTotals& t) {
  const bool providerNeeded = std::strcmp(s.name, "search-errors") != 0;  // only that one is provider-free
  if (providerNeeded && !keys.provider) {
    std::printf("=== %s ===\n    SKIPPED - no provider key (a live turn needs one)\n\n", s.name);
    t.skipped++;
    return;
  }
  if (s.needsSearch && !keys.search) {
    std::printf("=== %s ===\n    SKIPPED - no TAVILY_API_KEY (this is the search-regression catcher)\n\n", s.name);
    t.skipped++;
    return;
  }
  std::printf("=== %s - %s ===\n", s.name, s.what);
  auto opt = baseOpt(cfg, s.name);
  wipe(opt.dataDir);
  NimbusdRig rig(cfg, opt);
  Ctx c;
  c.name = s.name;
  s.fn(rig, c);
  wipe(opt.dataDir);
  t.ran++;
  if (c.failures) { t.failed++; std::printf("    --> %d failure(s)\n", c.failures); }
  std::printf("\n");
}

// The restart scenario runs whenever a provider (for embeddings) is present.
void runRestart(Config& cfg, const Keys& keys, RunTotals& t) {
  if (!keys.provider) {
    std::printf("=== restart ===\n    SKIPPED - no provider key for live embeddings "
                "(the offline restart proof is tests/test_rig + test_posix_stores)\n\n");
    t.skipped++;
    return;
  }
  std::printf("=== restart - memory survives a real daemon restart (live embeddings) ===\n");
  Ctx c;
  c.name = "restart";
  scRestartPersistence(cfg, c);
  t.ran++;
  if (c.failures) { t.failed++; std::printf("    --> %d failure(s)\n", c.failures); }
  std::printf("\n");
}

int main(int argc, char** argv) {
  Config cfg;
  std::string envPath;
  std::vector<std::string> names;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--env=", 0) == 0) envPath = a.substr(6);
    else if (a == "--env" && i + 1 < argc) envPath = argv[++i];
    else names.push_back(a);
  }
  loadEnv(cfg, envPath);

  Keys keys;
  keys.search = !cfg.get("TAVILY_API_KEY").empty();
  keys.provider = !cfg.providerKey("mistral").empty() || !cfg.providerKey("openai").empty() ||
                  !cfg.providerKey("anthropic").empty();
  printBanner(cfg, keys);

  RunTotals t;
  for (const auto& s : kScenarios)
    if (selected(names, s.name)) runOne(cfg, s, keys, t);
  if (selected(names, "restart")) runRestart(cfg, keys, t);

  std::printf("---------------------------------------------\n");
  std::printf("%d scenario(s) run, %d failed, %d skipped\n", t.ran, t.failed, t.skipped);
  if (t.skipped) std::printf("WARNING: %d scenario(s) were SKIPPED - that is not a pass.\n", t.skipped);
  return t.failed ? 1 : 0;
}
