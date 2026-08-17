#include "scenarios.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nimbus/harness/websearch.h"

namespace lab {
namespace {

// ---- tiny assertion harness ------------------------------------------------
struct Ctx {
  std::string name;
  std::vector<std::string> failures;
  std::vector<std::string> notes;

  void check(bool cond, const std::string& what) {
    if (cond) std::printf("    ok   %s\n", what.c_str());
    else { std::printf("    FAIL %s\n", what.c_str()); failures.push_back(what); }
  }
  void note(const std::string& n) {
    std::printf("    ...  %s\n", n.c_str());
    notes.push_back(n);
  }
};

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
bool has(const std::string& hay, const std::string& needle) {
  return lower(hay).find(lower(needle)) != std::string::npos;
}
bool calledTool(const TurnRecord& t, const std::string& name) {
  for (const auto& c : t.toolCalls)
    if (c.rfind(name, 0) == 0) return true;
  return false;
}
int toolCallCount(const TurnRecord& t, const std::string& name) {
  int n = 0;
  for (const auto& c : t.toolCalls)
    if (c.rfind(name, 0) == 0) n++;
  return n;
}
void showTurn(const TurnRecord& t) {
  std::printf("    > %s\n", t.userText.substr(0, 110).c_str());
  for (size_t i = 0; i < t.toolCalls.size(); i++)
    std::printf("      [tool] %s\n             -> %s\n", t.toolCalls[i].c_str(),
                i < t.toolResults.size() ? t.toolResults[i].substr(0, 160).c_str() : "?");
  std::printf("    < %s\n", t.reply.empty() ? "(no reply)" : t.reply.substr(0, 400).c_str());
}

// ---- scenarios -------------------------------------------------------------

// 1. The regression that started all of this. A live search must succeed, and
//    the model must actually USE what came back.
void scWebSearch(LabRig& rig, Ctx& c) {
  auto t = rig.say("lab", "Search the web for today's top world news headline, then tell me "
                          "the single most important story in one sentence. Cite the URL.");
  showTurn(t);
  c.check(calledTool(t, "web_search") || calledTool(t, "web.search"),
          "the model called web.search");
  bool searchFailed = false;
  for (const auto& r : t.toolResults)
    if (has(r, "web search failed")) searchFailed = true;
  c.check(!searchFailed, "web.search returned results (not an error)");
  c.check(!t.reply.empty(), "the model produced a reply");
  c.check(has(t.reply, "http") || has(t.reply, "www."),
          "the reply cites a source URL from the search");
}

// 2. web.search failure must NAME its cause. This is the honesty check: with a
//    deliberately bad key the tool has to say "401", not "no results".
void scSearchErrorHonesty(LabRig& rig, Ctx& c) {
  (void)rig;
  CurlHttpTransport http;
  auto bad = agent::websearch::search(http, "tvly-dev-obviously-not-a-real-key",
                                      "anything", 3);
  std::printf("    bad-key error: \"%s\"\n", bad.err.c_str());
  c.check(!bad.ok, "a bad key fails");
  c.check(has(bad.err, "401") || has(bad.err, "unauthor") || has(bad.err, "key"),
          "the error names the cause (not a generic 'network / no results')");
  c.check(!has(bad.err, "no results"),
          "a failure is never reported as 'no results'");
}

// 3. Several tool calls inside ONE turn - the owner asked specifically whether
//    this works.
void scMultiToolSingleTurn(LabRig& rig, Ctx& c) {
  auto t = rig.say("lab",
                   "Do all of this in one go: save a note to memory that my favourite "
                   "debugging drink is oolong tea, then save a file called plan.txt in "
                   "project 'lab' containing the text 'step one: reproduce', then list "
                   "the files you have. Report what you did.");
  showTurn(t);
  c.check(t.toolCalls.size() >= 2,
          "at least two tool calls in a single turn (got " +
              std::to_string(t.toolCalls.size()) + ")");
  c.check(calledTool(t, "memory_write") || calledTool(t, "memory.write"),
          "wrote to memory");
  c.check(calledTool(t, "artifact_save") || calledTool(t, "artifact.save"),
          "saved a file");
  c.check(rig.files().has("lab/plan.txt"), "the file really landed in the store");
}

// 4. Memory across turns: write in one turn, recall in a LATER one. The second
//    turn must not be able to see the first in its transcript window alone -
//    so a distinctive token proves genuine retrieval.
void scMemoryRoundtrip(LabRig& rig, Ctx& c) {
  const std::string secret = "the workshop spare key is behind the blue ceramic owl";
  auto t1 = rig.say("mem", "Remember this permanently: " + secret);
  showTurn(t1);
  c.check(calledTool(t1, "memory_write") || calledTool(t1, "memory.write"),
          "turn 1 wrote the fact to memory");
  c.check(rig.vectors().size() > 0, "a vector actually landed in the store");

  // A different chat: no shared conversation window, so only real associative
  // recall (or an explicit memory.search) can answer this.
  auto t2 = rig.say("other", "Where is the workshop spare key kept?");
  showTurn(t2);
  c.check(has(t2.reply, "owl") || has(t2.reply, "ceramic"),
          "turn 2 recalled the fact in a DIFFERENT chat");
}

// 5. memory.update / pin - the owner asked whether the memory structure works,
//    not just whether writes land.
void scMemoryLifecycle(LabRig& rig, Ctx& c) {
  std::string w = rig.callTool("memory.write",
                               R"({"content":"The lab kettle is model KX-40.","ttl":"weeks"})");
  c.check(!has(w, "\"isError\":true"), "memory.write succeeded");

  std::string s = rig.callTool("memory.search", R"({"query":"lab kettle model","n_results":5})");
  c.check(has(s, "KX-40"), "memory.search found the written fact");

  // pin addresses the entry by exact content (or id) and needs an explicit
  // action - pin / unpin / delete.
  std::string p = rig.callTool(
      "memory.pin", R"({"action":"pin","content":"The lab kettle is model KX-40."})");
  c.check(!has(p, "\"isError\":true"), "memory.pin pinned the entry: " + p.substr(0, 120));

  // update: 'old' describes the fact being REPLACED, 'content' is the new one.
  // Getting this wrong stores a second vector instead of replacing the first,
  // which is the exact duplicate-memory bug doUpdate was written to prevent.
  std::string u = rig.callTool("memory.update",
      R"({"old":"the lab kettle model","content":"The lab kettle is model KX-41."})");
  c.check(has(u, "replaced the prior memory"),
          "memory.update REPLACED rather than duplicating: " + u.substr(0, 140));

  std::string s2 = rig.callTool("memory.search", R"({"query":"lab kettle model","n_results":5})");
  c.check(has(s2, "KX-41"), "the updated value is what search now returns");
  c.check(!has(s2, "KX-40"), "the superseded value is gone (no duplicate left behind)");

  std::string e = rig.callTool("memory.episodic", R"({"limit":5})");
  c.check(!has(e, "\"isError\":true"), "memory.episodic is queryable");
}

// 6. Multi-round tool loop: the answer requires the result of one call to make
//    the next, so a single round cannot finish it.
void scToolLoopDepth(LabRig& rig, Ctx& c) {
  rig.files().seed("archive/notes-a.txt", "Meeting notes. The chosen codename is FALCON.");
  rig.files().seed("archive/notes-b.txt", "Shopping list. Milk, bread.");
  rig.files().seed("archive/notes-c.txt", "Reminder: renew the domain in March.");

  auto t = rig.say("loop",
                   "Look through the files in project 'archive' and tell me which one "
                   "contains a codename, and what the codename is.");
  showTurn(t);
  c.check(calledTool(t, "files_list") || calledTool(t, "files.list"),
          "listed the project first");
  c.check(toolCallCount(t, "files_read") + toolCallCount(t, "files.read") >= 1,
          "read at least one file after listing (a second, dependent round)");
  c.check(has(t.reply, "FALCON"), "found the codename");
  c.check(has(t.reply, "notes-a"), "named the right file");
}

// 7. Multi-turn continuity - does the chat remember three turns later?
void scMultiTurnContinuity(LabRig& rig, Ctx& c) {
  auto t1 = rig.say("cont", "I'm planning a trip to Lisbon in October. Just noting it.");
  c.check(!t1.reply.empty(), "turn 1 answered");
  rig.say("cont", "What's a good way to keep notes while travelling?");
  auto t3 = rig.say("cont", "Remind me which city I said I was visiting, and in which month.");
  showTurn(t3);
  c.check(has(t3.reply, "lisbon"), "recalled the city three turns later");
  c.check(has(t3.reply, "october"), "recalled the month three turns later");
}

// 8. An unattended (scheduled/routine) turn - the path a briefing actually runs
//    on. This is the one that has been failing in the field.
void scScheduledTurn(LabRig& rig, Ctx& c) {
  auto r = rig.scheduled("lab",
                         "Search the web for one notable technology story from the last "
                         "few days and summarize it in two sentences for the owner.",
                         "news-brief");
  const auto& t = rig.turns().back();
  showTurn(t);
  c.check(r.ok, "the scheduled turn reported ok");
  c.check(!t.reply.empty(), "the routine produced something to deliver");
  bool failedSearch = false;
  for (const auto& tr : t.toolResults)
    if (has(tr, "web search failed")) failedSearch = true;
  c.check(!failedSearch, "no web.search failure inside the routine");
}

// 9. The quiet (dream-style) consolidation turn: it must be allowed to say
//    nothing without that counting as a failure.
void scQuietTurn(LabRig& rig, Ctx& c) {
  auto r = rig.scheduled("lab",
                         "Review what you learned today and store at most one durable "
                         "insight with memory.write. If there is nothing worth keeping, "
                         "reply with nothing at all.",
                         "dream", /*quietOk=*/true);
  const auto& t = rig.turns().back();
  showTurn(t);
  c.check(r.ok, "the quiet turn completed");
  c.note(t.reply.empty() ? "stayed silent (allowed)" : "spoke: " + t.reply.substr(0, 120));
}

// 10. DEGRADED HONESTY. When memory pressure takes the tools away, the reply must
//     say so - never invent work that will never run.
//
//     This is the scenario the whole investigation turned on. With the tools
//     silently removed, every provider promised a future it did not have:
//       mistral   "Searching the 'archive' project... (this will complete next turn)"
//       openai    "I'll check the archive files in the background and report back"
//       anthropic "I've queued a memory query and will have the tools next turn"
//     Nothing was queued and nothing ran later; the device just went quiet, which
//     the owner experienced as the model ignoring the request entirely.
void scDegradedHonesty(LabRig& rig, Ctx& c) {
  rig.files().seed("archive/notes-a.txt", "Meeting notes. The chosen codename is FALCON.");

  auto t = rig.say("degraded",
                   "Look through the files in project 'archive' and tell me which one "
                   "contains a codename, and what the codename is.");
  showTurn(t);

  c.check(!t.reply.empty(), "a degraded turn still answers rather than going silent");

  // The load-bearing assertion: no claim of work that will never happen. These
  // are the exact shapes the three providers produced before the fix.
  const char* fabrications[] = {
      "report back", "in the background", "background check", "background scan",
      "complete next turn", "when the scan finishes", "once the scan",
      "i'll check", "i will check", "i'll look", "i will look",
      "i'll search", "i will search", "i'll fetch", "i will fetch",
      "get back to you", "will complete", "coming up shortly",
  };
  for (const char* f : fabrications)
    c.check(!has(t.reply, f),
            std::string("reply does not promise future work (\"") + f + "\")");

  // ...and it has to actually SAY it is limited, not just stay vague.
  const bool admits = has(t.reply, "can't") || has(t.reply, "cannot") ||
                      has(t.reply, "unavailable") || has(t.reply, "not callable") ||
                      has(t.reply, "loop is off") || has(t.reply, "don't have") ||
                      has(t.reply, "do not have") || has(t.reply, "another turn") ||
                      has(t.reply, "one more turn") || has(t.reply, "ask again");
  c.check(admits, "the reply states plainly that it could not do the work");

  // And the harness really did degrade - otherwise this scenario proves nothing.
  c.check(LabRig::loggedContaining("tool-loop DEFERRED") ||
              LabRig::loggedContaining("cap=heap"),
          "the run actually hit the degraded path (else this test is vacuous)");
}

// 11. Every keyed provider must handle a plain turn AND a tool-using turn. The
//     owner's rule: no provider is allowed to be the one that only half works.
void scProviderParity(LabRig& rig, Ctx& c) {
  (void)rig;
  c.note("(driven per-provider by runScenarios; see the parity section)");
}

struct Scenario {
  const char* name;
  const char* what;
  void (*fn)(LabRig&, Ctx&);
  bool needsSearch;
  // Force a device-like, FALLING internal heap so the real turn/loop gates fire.
  // A static low value cannot reproduce this: the turn is admitted above
  // ORCH_TURN_HARD_FLOOR and only crosses the loop floor part-way through.
  bool degradeHeap = false;
};

const Scenario kScenarios[] = {
    {"web-search",    "live web.search end to end, and the model uses the result", scWebSearch, true},
    {"search-errors", "a failing search names its cause instead of lying",         scSearchErrorHonesty, false},
    {"multi-tool",    "several tool calls inside one turn",                        scMultiToolSingleTurn, false},
    {"memory",        "write in one chat, recall in another",                      scMemoryRoundtrip, false},
    {"memory-ops",    "write / search / pin / episodic round-trip",                scMemoryLifecycle, false},
    {"tool-loop",     "multi-round loop where round 2 depends on round 1",         scToolLoopDepth, false},
    {"continuity",    "the chat still remembers three turns later",                scMultiTurnContinuity, false},
    {"scheduled",     "an unattended routine turn (the news-briefing path)",       scScheduledTurn, true},
    {"dream",         "a quiet consolidation turn may say nothing",                scQuietTurn, false},
    {"degraded",      "a memory-degraded turn admits it, and promises nothing",   scDegradedHonesty, false, true},
    {"parity",        "every keyed provider handles plain + tool turns",           scProviderParity, false},
};

}  // namespace

void listScenarios() {
  std::puts("scenarios:");
  for (const auto& s : kScenarios)
    std::printf("  %-14s %s%s\n", s.name, s.what,
                s.needsSearch ? "  [needs TAVILY_API_KEY]" : "");
}

int cmdProviders(const Env& env, const LabRig::Options& opt) {
  std::puts("provider check - one minimal real turn each\n");
  int failures = 0;
  for (const char* host : {"mistral", "openai", "anthropic"}) {
    if (env.providerKey(host).empty()) {
      std::printf("  %-10s no key configured - skipped\n", host);
      continue;
    }
    LabRig::Options o = opt;
    o.priority = host;      // pin it: no failover, so a failure is attributable
    LabRig rig(env, o);
    auto t = rig.say("probe", "Reply with exactly the word: pong");
    const bool ok = !t.reply.empty();
    std::printf("  %-10s %-4s %-6.1fs  %5u in /%5u out  %s\n", host, ok ? "OK" : "FAIL",
                t.seconds, t.tokensIn, t.tokensOut,
                t.reply.empty() ? "(no reply)" : t.reply.substr(0, 60).c_str());
    if (!ok) {
      failures++;
      for (const auto& call : rig.http().calls)
        if (call.status == 0 || call.status >= 400)
          std::printf("             %s%s -> %d %s\n", call.host.c_str(), call.path.c_str(),
                      call.status, call.respBody.substr(0, 240).c_str());
    }
  }
  std::printf("\n%s\n", failures ? "SOME PROVIDERS FAILED" : "all keyed providers answered");
  return failures ? 1 : 0;
}

int cmdSearch(const Env& env, const LabRig::Options& opt, const std::string& query) {
  (void)opt;
  const std::string key = env.get("TAVILY_API_KEY");
  if (key.empty()) {
    std::fprintf(stderr, "no TAVILY_API_KEY\n");
    return 2;
  }
  CurlHttpTransport http;
  http.verbose = true;
  auto r = agent::websearch::search(http, key, query, 5);
  if (!r.ok) {
    std::printf("FAILED: %s\n", r.err.c_str());
    return 1;
  }
  std::printf("\n%s\n", r.digest.c_str());
  std::printf("[response was %zu bytes; the pre-fix adapter truncated at 6000]\n",
              http.calls.empty() ? 0 : http.calls.back().respBody.size());
  return 0;
}

// Provider parity gets its own driver: the same two turns on every keyed host,
// each pinned so a failure is attributable rather than silently failed over.
static int runParity(const Env& env, const LabRig::Options& opt) {
  std::puts("\n=== parity: every keyed provider, plain turn + tool turn ===");
  int failures = 0;
  for (const char* host : {"mistral", "openai", "anthropic"}) {
    if (env.providerKey(host).empty()) {
      std::printf("  %-10s no key - skipped\n", host);
      continue;
    }
    LabRig::Options o = opt;
    o.priority = host;
    LabRig rig(env, o);

    auto plain = rig.say("parity", "In one short sentence, what is an ESP32?");
    const bool plainOk = !plain.reply.empty();

    auto tool = rig.say("parity",
                        "Save a note to memory that the parity check ran, then tell me "
                        "you did it.");
    const bool toolOk = !tool.reply.empty() &&
                        (calledTool(tool, "memory_write") || calledTool(tool, "memory.write"));

    std::printf("  %-10s plain:%-4s tool:%-4s  (%u+%u tokens)\n", host,
                plainOk ? "OK" : "FAIL", toolOk ? "OK" : "FAIL",
                rig.totalIn(), rig.totalOut());
    if (!plainOk || !toolOk) {
      failures++;
      if (!toolOk && plainOk)
        std::printf("             tool turn reply: %s\n", tool.reply.substr(0, 200).c_str());
      for (const auto& call : rig.http().calls)
        if (call.status == 0 || call.status >= 400)
          std::printf("             %s%s -> %d %s\n", call.host.c_str(), call.path.c_str(),
                      call.status, call.respBody.substr(0, 240).c_str());
    }
  }
  return failures;
}

int runScenarios(const Env& env, const LabRig::Options& opt,
                 const std::vector<std::string>& names) {
  const bool haveSearch = !env.get("TAVILY_API_KEY").empty();
  int failed = 0, ran = 0, skipped = 0;
  uint32_t tokIn = 0, tokOut = 0;

  for (const auto& s : kScenarios) {
    if (!names.empty() &&
        std::find(names.begin(), names.end(), s.name) == names.end()) continue;
    if (std::strcmp(s.name, "parity") == 0) {
      if (names.empty() || std::find(names.begin(), names.end(), "parity") != names.end())
        failed += runParity(env, opt);
      continue;
    }
    if (s.needsSearch && !haveSearch) {
      // Skip LOUDLY: a silently-skipped scenario reads as a pass, which is how
      // a broken capability stays broken.
      std::printf("\n=== %s ===\n    SKIPPED - no TAVILY_API_KEY (this scenario is the "
                  "one that catches the search regression)\n", s.name);
      skipped++;
      continue;
    }

    std::printf("\n=== %s - %s ===\n", s.name, s.what);
    LabRig::Options o = opt;
    if (s.degradeHeap && !o.heapBytes) { o.heapBytes = 31000; o.heapDecay = 400; }
    LabRig rig(env, o);
    Ctx c;
    c.name = s.name;
    s.fn(rig, c);
    tokIn += rig.totalIn();
    tokOut += rig.totalOut();
    ran++;
    if (!c.failures.empty()) {
      failed++;
      std::printf("    --> %zu failure(s)\n", c.failures.size());
    }
  }

  std::printf("\n---------------------------------------------\n");
  std::printf("%d scenario(s) run, %d failed, %d skipped\n", ran, failed, skipped);
  std::printf("tokens: %u in / %u out\n", tokIn, tokOut);
  if (skipped)
    std::printf("⚠ %d scenario(s) were SKIPPED - that is not a pass.\n", skipped);
  return failed ? 1 : 0;
}

}  // namespace lab
