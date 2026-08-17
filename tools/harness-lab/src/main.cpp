// nimbus-lab - drive the Nimbus agent harness from a terminal.
//
//   nimbus-lab providers              which providers are keyed, and do they answer
//   nimbus-lab search <query>         exercise web.search end to end
//   nimbus-lab tool <name> <json>     call one registry tool directly
//   nimbus-lab chat [host]            interactive REPL against the real harness
//   nimbus-lab scenarios [names...]   the scenario suite (see scenarios.cpp)
//
// Flags: --host=<h> --model=<m> --verbose --no-tools --no-embed
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "lab_rig.h"
#include "scenarios.h"

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
      "  list-scenarios            show what the suite covers\n"
      "\n"
      "Options:\n"
      "  --host=<openai|anthropic|mistral>   force the provider (default: priority order)\n"
      "  --model=<id>                        override the model for --host\n"
      "  --priority=<a,b,c>                  provider failover order\n"
      "  --verbose                           echo every HTTP exchange and tool call\n"
      "  --no-tools                          disable the agentic tool loop\n"
      "  --no-embed                          disable embeddings (no associative recall)\n"
      "  --role=<admin|user|guest|unknown>   run the chat as this role, so the\n"
      "                                      principal-scoped tool advertisement is\n"
      "                                      exercised (default: admin)\n"
      "  --heap=<bytes>                      report a device-like internal heap, so the\n"
      "                                      real turn/loop heap gates fire (e.g. 30000)\n"
      "  --heap-decay=<bytes>                subtract this per heap read, so the value\n"
      "                                      FALLS mid-loop like it does on the board\n"
      "  --env=<path>                        dotenv file (default: NIMBUS_ENV_FILE or ~/.env)\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  lab::LabRig::Options opt;
  std::string envPath = lab::Env::defaultDotenvPath();
  std::string forceHost, forceModel;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--host=", 0) == 0)          forceHost = a.substr(7);
    else if (a.rfind("--model=", 0) == 0)    forceModel = a.substr(8);
    else if (a.rfind("--priority=", 0) == 0) opt.priority = a.substr(11);
    else if (a.rfind("--env=", 0) == 0)      envPath = a.substr(6);
    else if (a == "--verbose")               opt.verboseHttp = true;
    else if (a == "--no-tools")              opt.toolLoop = false;
    else if (a == "--no-embed")              opt.embeddings = false;
    else if (a.rfind("--role=", 0) == 0)     opt.role = a.substr(7);
    else if (a.rfind("--heap=", 0) == 0)     opt.heapBytes = (uint32_t)std::stoul(a.substr(7));
    else if (a.rfind("--heap-decay=", 0) == 0) opt.heapDecay = (uint32_t)std::stoul(a.substr(13));
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

  lab::Env env;
  env.loadDotenv(envPath);

  const std::string cmd = args[0];

  if (cmd == "list-scenarios") { lab::listScenarios(); return 0; }

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

  if (cmd == "scenarios") {
    std::vector<std::string> names(args.begin() + 1, args.end());
    return lab::runScenarios(env, opt, names);
  }

  std::fprintf(stderr, "unknown command '%s'\n\n", cmd.c_str());
  usage();
  return 2;
}
