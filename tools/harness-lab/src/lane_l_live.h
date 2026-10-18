#pragma once
#include <string>

#include "lab_env.h"

// The turn-contract live suites (real provider calls, cheap models first):
//
//   nimbus-lab reply-markup   a long reply plus a memory write plus a spawn after a
//                             big tool output, a formatted comparison, and a
//                             think-then-answer prompt. Fails on any stray tag in a
//                             delivered string (raw or as Telegram HTML) and on a
//                             lost side effect (the memory write or the spawn).
//   nimbus-lab spawn-e2e      a sub-agent end to end through the real JobEngine,
//                             fabric adapters and journal: A scripted head, B the
//                             same with a stringified session_ops, C a real head
//                             asked in plain words, D a journal store that refuses
//                             every write. Pass = the nonce the sub replies with
//                             reaches the owner's synthesized reply, exactly once.
//
// Keys are read by name from the dotenv file and never printed. Every response's
// provider-reported usage is summed and priced per model, and the totals print at
// the end (and go to --out=<file> as JSON).
namespace lab {

struct LaneLOptions {
  std::string headHost = "anthropic";
  std::string headModel = "claude-haiku-5-5";
  std::string effort = "low";        // pinned on Anthropic bodies ("" = none) until the cap table lands
  std::string sub = "anthropic:claude-haiku-5-5";   // provider:model of the spawned sub-agent
  std::string legs = "A,B,C,D";
  std::string prompts = "1,2,3";
  int reps = 1;
  int timeoutS = 300;                // per spawn leg
  std::string out;                   // JSON summary path ("" = none)
  bool verbose = false;
};

int cmdReplyMarkup(const Env& env, const LaneLOptions& opt);
int cmdSpawnE2e(const Env& env, const LaneLOptions& opt);

}  // namespace lab
