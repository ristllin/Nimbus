// nimbus-lab - drive the Nimbus agent harness from a terminal.
//
//   nimbus-lab providers              which providers are keyed, and do they answer
//   nimbus-lab search <query>         exercise web.search end to end
//   nimbus-lab tool <name> <json>     call one registry tool directly
//   nimbus-lab chat [host]            interactive REPL against the real harness
//   nimbus-lab scenarios [names...]   the scenario suite (see scenarios.cpp)
//   nimbus-lab sub <task>             one Anthropic sub-agent end to end (sub_cmd.cpp)
//   nimbus-lab reply-markup           turn-contract live suite L-L1 (lane_l_live.cpp)
//   nimbus-lab spawn-e2e              turn-contract live suite L-L2 (lane_l_live.cpp)
//
// Flags: --host=<h> --model=<m> --verbose --no-tools --no-embed, plus the run pins
// (--effort --max-tokens --ceiling-ms --rounds --fault --record; see lab_options.h
// for the NIMBUS_LAB_* environment they override).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "lab_options.h"
#include "lab_rig.h"
#include "honesty_eval.h"
#include "lane_l_live.h"
#include "scenarios.h"
#include "selftest.h"
#include "sub_cmd.h"

namespace {

void usage() {
  std::puts(
      "nimbus-lab - run the Nimbus agent harness on this machine\n"
      "\n"
      "  providers                 report which providers are keyed and reachable\n"
      "  search <query>            run web.search end to end\n"
      "  tool <name> <json-args>   call one registry tool directly\n"
      "  chat                      interactive REPL against the real turn engine\n"
      "  scenarios [name...]       run the scenario suite (no names = all)\n"
      "  honesty [--reps=n] [--only=F1,U2]  the live honesty eval (claim check, 20 conversations;\n"
      "                            --only=C1 adds the unrecorded-cause diagnosis)\n"
      "  list-scenarios            show what the suite covers\n"
      "  selftest                  offline checks of the lab's own seams (no keys)\n"
      "  sub <task>                one Anthropic sub-agent end to end (dispatch + polls);\n"
      "                            --model=<id> picks the agent model, --nvs-full drops\n"
      "                            every cache write (a full NVS partition), --runs=<n>\n"
      "                            repeats it, --timeout=<s> bounds each run,\n"
      "                            --record=<dir> saves every response body\n"
      "  reply-markup              stray tags and lost side effects on real turns;\n"
      "                            --head-model=<id> --reps=<n> --prompts=1,2,3 --out=<json>\n"
      "  spawn-e2e                 a sub-agent end to end (legs A-D); --legs=A,B,C,D\n"
      "                            --sub=<provider:model> --head-model=<id> --reps=<n>\n"
      "                            (leg C) --timeout=<s> --out=<json>\n"
      "                            both: --effort=<low|...|none> pins Anthropic effort\n"
      "                            (none or adapter = the adapter's own effort)\n"
      "\n"
      "Options:\n"
      "  --host=<openai|anthropic|mistral>   force the provider (default: priority order)\n"
      "  --model=<id>                        override the model for --host\n"
      "  --priority=<a,b,c>                  provider failover order\n"
      "  --verbose                           echo every HTTP exchange and tool call\n"
      "  --no-tools                          disable the agentic tool loop\n"
      "  --no-embed                          disable embeddings (no associative recall)\n"
      "  --hist-v1                           the v4.5.15 conversation window, not the\n"
      "                                      turn-structured history (histV2)\n"
      "  --role=<admin|user|guest|unknown>   run the chat as this role, so the\n"
      "                                      principal-scoped tool advertisement is\n"
      "                                      exercised (default: admin)\n"
      "  --heap=<bytes>                      report a device-like internal heap, so the\n"
      "                                      real turn/loop heap gates fire (e.g. 30000)\n"
      "  --heap-decay=<bytes>                subtract this per heap read, so the value\n"
      "                                      FALLS mid-loop like it does on the board\n"
      "  --env=<path>                        dotenv file (default: NIMBUS_ENV_FILE or ~/.env)\n"
      "\n"
      "Run pins (each also read from the environment, see lab_options.h):\n"
      "  --effort=<low|medium|high|xhigh|max|adapter>   Anthropic effort (NIMBUS_LAB_EFFORT, default low)\n"
      "  --max-tokens=<n>                    output cap per request (NIMBUS_LAB_MAXTOK)\n"
      "  --ceiling-ms=<n>                    per-request timeout ceiling (NIMBUS_LAB_CEILING_MS)\n"
      "  --rounds=<n>                        head tool-loop rounds per turn (1..32, default 12)\n"
      "  --no-failover                       turn the mid-turn fabric failover off\n"
      "  --fault=<host:range:status[:detail]>  fail chosen requests (repeatable), e.g.\n"
      "                                      anthropic:2:529:overloaded, mistral:1-3:0:noresp\n"
      "  --record=<dir>                      append usage objects and error bodies to <dir>\n"
      "                                      (price them with spend_ledger.py)\n"
      "  NIMBUS_LAB_MODEL=[host/]model[,...] pin models (host inferred from the id)\n");
}

// The turn-contract live suites' flags (lane_l_live.h). True when `a` was one.
bool laneLFlag(const std::string& a, lab::LaneLOptions& o) {
  auto val = [&a](const char* flag) { return a.substr(std::strlen(flag)); };
  if (a.rfind("--head-host=", 0) == 0)       o.headHost = val("--head-host=");
  else if (a.rfind("--head-model=", 0) == 0) o.headModel = val("--head-model=");
  else if (a.rfind("--effort=", 0) == 0) {
    const std::string e = val("--effort=");
    o.effort = (e == "none" || e == "adapter") ? "" : e;   // "" = the adapter's own effort
  }
  else if (a.rfind("--sub=", 0) == 0)        o.sub = val("--sub=");
  else if (a.rfind("--legs=", 0) == 0)       o.legs = val("--legs=");
  else if (a.rfind("--prompts=", 0) == 0)    o.prompts = val("--prompts=");
  else if (a.rfind("--reps=", 0) == 0)       o.reps = std::stoi(val("--reps="));
  else if (a.rfind("--out=", 0) == 0)        o.out = val("--out=");
  else return false;
  return true;
}

int runLaneL(const std::string& cmd, const lab::Env& env, lab::LaneLOptions o, bool verbose,
             int timeoutS) {
  o.verbose = verbose;
  if (timeoutS > 0) o.timeoutS = timeoutS;
  if (o.reps < 1 || o.sub.find(':') == std::string::npos) {
    std::fprintf(stderr, "--reps must be at least 1 and --sub must read provider:model\n");
    return 2;
  }
  return cmd == "reply-markup" ? lab::cmdReplyMarkup(env, o) : lab::cmdSpawnE2e(env, o);
}

}  // namespace

// A pin flag's value, applied after the environment so the flag wins.
static bool applyPinFlag(const std::string& a, lab::LabRig::Options& opt, std::string& err) {
  if (a.rfind("--effort=", 0) == 0)     return lab::applyEffort(a.substr(9), opt, err);
  if (a.rfind("--max-tokens=", 0) == 0) return lab::applyMaxTokens(a.substr(13), opt, err);
  if (a.rfind("--ceiling-ms=", 0) == 0) return lab::applyCeiling(a.substr(13), opt, err);
  if (a.rfind("--rounds=", 0) == 0)     return lab::applyRounds(a.substr(9), opt, err);
  if (a.rfind("--fault=", 0) == 0)      return lab::applyFault(a.substr(8), opt, err);
  if (a == "--no-failover") { opt.midTurnFailover = false; return true; }
  err = "unknown pin flag " + a;
  return false;
}

static bool isPinFlag(const std::string& a) {
  for (const char* p : {"--effort=", "--max-tokens=", "--ceiling-ms=", "--rounds=", "--fault="})
    if (a.rfind(p, 0) == 0) return true;
  return a == "--no-failover";
}

int main(int argc, char** argv) {
  std::vector<std::string> args;
  lab::LabRig::Options opt;
  std::string envPath = lab::Env::defaultDotenvPath();
  std::string forceHost, forceModel;
  lab::SubOptions sub;
  lab::LaneLOptions laneL;   // --reps is shared: the lane L suites and the honesty eval
  std::string only;

  // The environment first (NIMBUS_LAB_*), so every flag below overrides it.
  for (int i = 1; i < argc; i++)
    if (std::strncmp(argv[i], "--env=", 6) == 0) envPath = argv[i] + 6;
  lab::Env env;
  env.loadDotenv(envPath);
  std::string pinErr;
  if (!lab::applyLabEnv(env, opt, pinErr)) {
    std::fprintf(stderr, "%s\n", pinErr.c_str());
    return 2;
  }

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--host=", 0) == 0)          forceHost = a.substr(7);
    else if (a.rfind("--model=", 0) == 0)    forceModel = a.substr(8);
    else if (a.rfind("--priority=", 0) == 0) opt.priority = a.substr(11);
    else if (a.rfind("--env=", 0) == 0)      {}   // consumed before this loop (env loads first)
    else if (a == "--verbose")               opt.verboseHttp = true;
    else if (a == "--no-tools")              opt.toolLoop = false;
    else if (a == "--no-embed")              opt.embeddings = false;
    else if (a == "--hist-v1")               opt.histV2 = false;
    else if (a.rfind("--role=", 0) == 0)     opt.role = a.substr(7);
    else if (a.rfind("--heap=", 0) == 0)     opt.heapBytes = (uint32_t)std::stoul(a.substr(7));
    else if (a.rfind("--heap-decay=", 0) == 0) opt.heapDecay = (uint32_t)std::stoul(a.substr(13));
    else if (a == "--nvs-full")              sub.nvsFull = true;
    else if (a.rfind("--runs=", 0) == 0)     sub.runs = std::stoi(a.substr(7));
    else if (a.rfind("--only=", 0) == 0)     only = a.substr(7);
    else if (a.rfind("--timeout=", 0) == 0)  sub.timeoutS = std::stoi(a.substr(10));
    else if (a.rfind("--record=", 0) == 0)   sub.recordDir = opt.recordDir = a.substr(9);
    else if (laneLFlag(a, laneL)) {
      // --effort is also a run pin (chat, scenarios): both read it, and the
      // suites' "none" is the pin's "adapter".
      if (a.rfind("--effort=", 0) == 0 &&
          !lab::applyEffort(laneL.effort.empty() ? "adapter" : laneL.effort, opt, pinErr)) {
        std::fprintf(stderr, "%s\n", pinErr.c_str());
        return 2;
      }
    }
    else if (isPinFlag(a)) {
      if (!applyPinFlag(a, opt, pinErr)) { std::fprintf(stderr, "%s\n", pinErr.c_str()); return 2; }
    }
    else if (a == "-h" || a == "--help")   { usage(); return 0; }
    else args.push_back(a);
  }
  if (args.empty()) { usage(); return 2; }

  // ⚠ prism: an unrecognized --role used to fall back to ADMIN silently, so a
  // typo ("--role=Guest") produced a full-visibility session while the operator
  // believed they were testing the guest path. Refuse to start instead.
  if (!lab::LabRig::validRole(opt.role)) {
    std::fprintf(stderr, "unknown --role=%s (use admin | user | guest | unknown)\n",
                 opt.role.c_str());
    return 2;
  }

  if (!forceHost.empty()) opt.priority = forceHost;
  if (!forceModel.empty()) {
    if (forceHost.empty()) {
      std::fprintf(stderr, "--model needs --host\n");
      return 2;
    }
    opt.models[forceHost] = forceModel;
  }

  const std::string cmd = args[0];
  if (!opt.recordDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(opt.recordDir, ec);
    if (ec) { std::fprintf(stderr, "--record: cannot create %s\n", opt.recordDir.c_str()); return 2; }
  }
  if (cmd == "chat" || cmd == "scenarios" || cmd == "providers")
    std::printf("%s\n", lab::labBanner(env, opt).c_str());

  if (cmd == "list-scenarios") { lab::listScenarios(); return 0; }
  if (cmd == "selftest") return lab::cmdSelftest();

  if (cmd == "providers") return lab::cmdProviders(env, opt);

  if (cmd == "search") {
    if (args.size() < 2) { std::fprintf(stderr, "search needs a query\n"); return 2; }
    std::string q = args[1];
    for (size_t i = 2; i < args.size(); i++) q += " " + args[i];
    return lab::cmdSearch(env, opt, q);
  }

  if (cmd == "tool") {
    if (args.size() < 2) { std::fprintf(stderr, "tool needs a name\n"); return 2; }
    lab::LabRig rig(env, opt);
    const std::string a = args.size() > 2 ? args[2] : "{}";
    std::puts(rig.callTool(args[1], a).c_str());
    return 0;
  }

  if (cmd == "chat") {
    lab::LabRig rig(env, opt);
    // Echo the RESOLVED role: a scoping run must be self-documenting, so its
    // transcript proves which principal it exercised (prism).
    std::printf("nimbus-lab chat - priority=%s tools=%s role=%s. Ctrl-D to exit.\n",
                opt.priority.c_str(), opt.toolLoop ? "on" : "off", opt.role.c_str());
    std::string line;
    while (std::fputs("\nyou> ", stdout), std::getline(std::cin, line)) {
      if (line.empty()) continue;
      auto t = rig.say("lab", line);
      if (!t.toolCalls.empty()) {
        std::printf("  [%zu tool call%s]\n", t.toolCalls.size(),
                    t.toolCalls.size() == 1 ? "" : "s");
        for (size_t i = 0; i < t.toolCalls.size(); i++)
          std::printf("    %s\n      -> %s\n", t.toolCalls[i].c_str(),
                      i < t.toolResults.size() ? t.toolResults[i].c_str() : "(no result)");
      }
      std::printf("\nnimbus> %s\n", t.reply.empty() ? "(no reply)" : t.reply.c_str());
      std::printf("  [%.1fs, %u in / %u out tokens]\n", t.seconds, t.tokensIn, t.tokensOut);
    }
    return 0;
  }

  if (cmd == "sub") {
    if (args.size() < 2) { std::fprintf(stderr, "sub needs a task\n"); return 2; }
    for (size_t i = 1; i < args.size(); i++) sub.task += (i > 1 ? " " : "") + args[i];
    sub.model = forceModel;
    sub.verbose = opt.verboseHttp;
    if (sub.runs < 1 || sub.timeoutS < 1) {   // 0 runs would report "reached Done" vacuously
      std::fprintf(stderr, "--runs and --timeout must be at least 1\n");
      return 2;
    }
    return lab::cmdSub(env, sub);
  }

  if (cmd == "reply-markup" || cmd == "spawn-e2e")
    return runLaneL(cmd, env, laneL, opt.verboseHttp, sub.timeoutS);
  if (cmd == "honesty") {
    if (laneL.reps < 1 || laneL.reps > 20) { std::fprintf(stderr, "--reps must be 1..20\n"); return 2; }
    return lab::runHonestyEval(env, opt, laneL.reps, only);
  }

  if (cmd == "scenarios") {
    std::vector<std::string> names(args.begin() + 1, args.end());
    return lab::runScenarios(env, opt, names);
  }

  std::fprintf(stderr, "unknown command '%s'\n\n", cmd.c_str());
  usage();
  return 2;
}
