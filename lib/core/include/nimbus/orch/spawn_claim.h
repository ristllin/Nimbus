#pragma once
// spawn_claim - does a reply tell the owner that sub-agents were started?
//
// The promise-without-spawn class: on 17-18 Oct six replies said 3 to 6
// sub-agents were started "this turn" and none was. The device now checks the
// reply against what the turn actually enqueued; this is the reply half of that
// check (the spawn class of the general claim scanner lane S builds).
//
// A sentence claims a spawn when it pairs a start verb (started, spawned,
// kicked off, launched, ...) with a sub-agent noun (sub-agent, "three agents",
// research task, background job, ...) within a few words. It is NOT a claim
// when it is a question, an offer or a condition ("want me to", "if you",
// "I can"), when it negates anything ("did not start", "are not running") or
// speaks of earlier work ("still", "already"), or when the words are inside
// quotes or code. English only, deterministic, no regex, linear in the reply.
// Deliberately narrow: a missed claim costs the owner nothing new, a false one
// would add a wrong correction to a true reply.
//
// Portable, Arduino-free, host-tested (test_spawn_claim).

#include <string>

namespace nimbus {
namespace orch {

bool replyClaimsSpawn(const std::string& reply);

// The owner line a reply gets when it promised sub-agents that did not start
// (none, or only some, of them). Shared with the claim check so both say it the
// same way.
extern const char kSpawnNotStartedLine[];    // none started
extern const char kSpawnSomeNotStartedLine[];  // some started, some did not

}  // namespace orch
}  // namespace nimbus
