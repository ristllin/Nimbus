#pragma once
#include <string>

#include "lab_env.h"

// nimbus-lab sub - one Anthropic sub-agent end to end through the shipping
// provider code (providers::antDispatch + antPoll), the exact chain a spawn runs
// on the device: environment -> agent -> session -> user.message -> event polls.
namespace lab {

struct SubOptions {
  std::string task;            // the instruction the sub-agent runs
  std::string model;           // "" = the device default (ANT_MODEL)
  bool        nvsFull = false; // drop every cache write, as a full NVS partition does (CUM-479)
  bool        verbose = false; // echo each HTTP exchange
  int         runs = 1;        // >1 proves the warm path re-creates nothing
  int         timeoutS = 300;  // per-run poll deadline
  int         pollS = 5;       // poll interval (the device polls every ~15 s)
  std::string recordDir;       // non-empty: write every response body here (fixtures)
};

int cmdSub(const Env& env, const SubOptions& opt);

}  // namespace lab
