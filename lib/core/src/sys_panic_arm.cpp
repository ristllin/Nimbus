#include "nimbus/sys/panic_arm.h"

#include <sstream>

namespace nimbus::sys {

const char* panicKindName(PanicKind k) {
  switch (k) {
    case PanicKind::NullStore: return "nullptr";
    case PanicKind::Abort:     return "abort";
    case PanicKind::Canary:    return "canary";
    case PanicKind::None:      break;
  }
  return "none";
}

static bool parseRound(const std::string& tok, int& round) {
  const std::string key = "round=";
  if (tok.compare(0, key.size(), key) != 0 || tok.size() == key.size() || tok.size() > key.size() + 2)
    return false;
  int r = 0;
  for (size_t i = key.size(); i < tok.size(); ++i) {
    if (tok[i] < '0' || tok[i] > '9') return false;
    r = r * 10 + (tok[i] - '0');
  }
  if (r > 50) return false;
  round = r;
  return true;
}

bool parsePanicArm(const std::string& spec, PanicArm& out) {
  PanicArm arm;
  bool haveKind = false, haveRound = false;
  std::istringstream in(spec);
  std::string tok;
  while (in >> tok) {
    PanicKind k = PanicKind::None;
    if (tok == "nullptr") k = PanicKind::NullStore;
    else if (tok == "abort") k = PanicKind::Abort;
    else if (tok == "canary") k = PanicKind::Canary;
    if (k != PanicKind::None) {
      if (haveKind) return false;
      arm.kind = k;
      haveKind = true;
      continue;
    }
    if (haveRound || !parseRound(tok, arm.round)) return false;
    haveRound = true;
  }
  if (!haveKind) arm.kind = PanicKind::NullStore;
  out = arm;
  return true;
}

bool panicArmFires(const PanicArm& arm, bool turnActive, int turnRound) {
  return arm.kind != PanicKind::None && turnActive && turnRound >= arm.round;
}

std::string panicArmReply(const PanicArm& arm) {
  return std::string("PANIC armed: ") + panicKindName(arm.kind) + " at round " +
         std::to_string(arm.round) + " of the next turn";
}

}  // namespace nimbus::sys
