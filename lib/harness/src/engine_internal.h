#pragma once
// engine_internal - the helpers the TurnEngine's source files share. engine.cpp holds
// the turn core, engine_fold.cpp the compaction fold and engine_history.cpp the
// turn-structured history (split out of engine.cpp with no behavior change, lane P0
// of the history fix, 16 Nov 2026). Private to lib/harness/src: no header includes it.

#include <cstdint>
#include <string>

#include "nimbus/harness/engine.h"
#include "nimbus/orch/compact.h"           // modelCtxTokens - the head's window
#include "nimbus/orch/connectors_wire.h"   // resolveHeadHost - the one head-host rule

namespace agent {
namespace detail {

inline void trimInPlace(std::string& s) {
  const char* ws = " \t\r\n";
  size_t b = s.find_first_not_of(ws);
  size_t e = s.find_last_not_of(ws);
  s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

// The head the next turn will ACTUALLY run on - the one resolution the turn core
// reads: nimbus::orch::resolveHeadHost,
// the rule the device's screen, catalog and web page share. The pinned orchHost while
// it has a key; else the first KEYED provider in priority order (CUM-201: BYOK-override
// first); else the verified router fallback head (Cumulo, then Z.ai - CUM-242); else
// any other keyed provider; only with no key anywhere the pin or the bare first token
// (the turn is then answered locally, never dialed). HOST-FALLBACK (22 Oct bench): a
// pin whose key was cleared won here and the turn dialed it keyless (HTTP 401); with
// no keyed provider in the priority list the turn ran on nothing and said "Done."
// One rule on purpose: when the context line read the raw first token instead, a
// hosted anthropic-only instance INTRODUCED ITSELF as "mistral /
// mistral-large-latest" - the model parrots this line, so it must name the real head
// (CUM-374 e2e, 2026-09-18).
inline std::string resolveTurnHost(const TurnEngine::Deps& d) {
  const auto& p = d.cfg.provider;
  return nimbus::orch::resolveHeadHost(p.orchHost ? p.orchHost() : std::string(),
                                       p.providerPriority ? p.providerPriority() : std::string(), p.hasKey,
                                       p.routerFallbackHost ? p.routerFallbackHost() : std::string());
}

// The model the head host runs (its orchModel; "" when none is set or the closure is unwired).
inline std::string headModelOf(const TurnEngine::Deps& d, const std::string& host) {
  return d.cfg.provider.orchModel ? d.cfg.provider.orchModel(host) : std::string();
}

// The head the next turn runs on, as a context window in tokens.
inline uint32_t headCtxTokens(const TurnEngine::Deps& d) {
  const std::string host = resolveTurnHost(d);
  return nimbus::orch::modelCtxTokens(host, headModelOf(d, host));
}

}  // namespace detail
}  // namespace agent
