#pragma once
#include <string>

// panic_arm - the test-build "crash inside the next turn" switch (the crash-forensics
// HIL suite and the human gate's "crash mid-photo at round 2").
//
// The test console's `PANIC <kind> arm=turn [round=N]` arms a panic that fires on the
// turn task at the transport's body write in round N of the next turn (round 0 = its
// first exchange). Firing before any request byte is sent keeps a provider from billing
// that exchange, and a crash at that depth is the worst case the 64 KB core dump
// partition must hold. If the turn ends before round N the arm is dropped, so it never
// fires in a later, unrelated turn.
//
// Only the spec parser and the fire decision live here (host-tested); the device seam
// (src/sys/crash_report.cpp) is compiled into NIMBUS_TEST builds only, and a release
// gate checks that no production image links it.
namespace nimbus::sys {

enum class PanicKind : char { None = 0, NullStore = 'n', Abort = 'a', Canary = 'c' };

struct PanicArm {
  PanicKind kind = PanicKind::None;
  int round = 0;
};

// "nullptr", "abort" or "canary", then an optional "round=N" (0..50), any order,
// whitespace separated. An empty spec means "nullptr round=0". False for anything else.
bool parsePanicArm(const std::string& spec, PanicArm& out);

// Fire now? Only inside a turn, once the turn has reached the armed round.
bool panicArmFires(const PanicArm& arm, bool turnActive, int turnRound);

// The console reply for an accepted arm ("PANIC armed: nullptr at round 2 of the next turn").
std::string panicArmReply(const PanicArm& arm);
const char* panicKindName(PanicKind k);

}  // namespace nimbus::sys
