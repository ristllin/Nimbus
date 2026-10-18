#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/orch/failure_chain.h"
#include "nimbus/orch/turn_spend.h"

// failure_rows - the rows a finished turn leaves behind, rendered in one portable
// place so host tests pin them (the device writes them to the episodic store).
//
//  - turnEndRowJson: the per-turn summary row (ev:turnend). The device used to drop
//    the WHOLE row silently when it ran over its 256 B ceiling; a cause and a per-host
//    split make long rows common, so the row now SHRINKS to fit and is never dropped.
//  - errorEventText: the timeline row of a failed turn (ev:error): which turn (its
//    source), where it started and ended, the first cause first, and how many tool
//    calls ran (it used to say "tool round(s)" for a count of calls).
//  - dreamRowText: the nightly dream's row: the cause when it failed, and the spend
//    labelled as what it is (the reflection turn's own billed tokens).
// Every output is valid UTF-8 within its cap. Portable (NO Arduino).

namespace nimbus {
namespace orch {

struct TurnEndRowIn {
  bool        ok = false;
  std::string src;            // "owner" | "serial" | "synthesis" | "loop" | "dream"
  std::string host;           // the host that served (or failed) last
  std::string model;          // the model the turn is shown under ("" = none billed)
  bool        fallback = false;
  int         tools = 0;      // executed tool calls
  uint32_t    in = 0, out = 0, cacheR = 0, cacheW = 0;   // the turn's billed total
  size_t      reply = 0;      // delivered reply bytes
  const TurnSpend*    spend = nullptr;   // "via" when more than one slot billed
  const FailureChain* chain = nullptr;   // "err": the first cause first
  std::string error;          // the last error text, used when there is no chain
};

// The ceiling the device stores a turn-end row under (ORCH_TRACE_END_MAX).
inline constexpr size_t kTurnEndRowMax = 256;
// The smallest cap the row is always produced within (the bare row: host, ok, in, out).
inline constexpr size_t kTurnEndRowMin = 96;

// The row as JSON, at most `cap` bytes when cap >= kTurnEndRowMin. Keys: host, model,
// fallback (only when true), src, tools, ok, in, out, cr / cw (only when non-zero),
// reply, err (only on failure), via (only when more than one slot billed:
// [[host, model, in, out, cr, cw], ...]). When it does not fit it shrinks in this
// order: via model names, the cause (later hosts, then the first text down to 40 B),
// cr/cw, via cache counts, model, the cause down to its first host's short form,
// via, then everything but host/ok/in/out.
std::string turnEndRowJson(const TurnEndRowIn& r, size_t cap = kTurnEndRowMax);

// The ev:error timeline text, at most `cap` bytes:
//   "Turn FAILED (dream) on anthropic -> mistral: anthropic (x2): messages HTTP 400:
//    The request body ... > openai 400 (x2) > mistral 400 (x2); 4 tool calls ran"
// The prefix "Turn FAILED" is stable (the head and the web timeline search for it).
inline constexpr size_t kErrorEventMax = 240;
std::string errorEventText(const std::string& src, const std::string& requestedHost,
                           const std::string& finalHost, const FailureChain* chain,
                           const std::string& error, int toolCalls,
                           size_t cap = kErrorEventMax);

// The ev:dream timeline text, at most `cap` bytes:
//   "Nightly dream FAILED (anthropic 400: ...) - spent 15438 tokens; maintenance
//    pruned=0 deduped=0"
// `cause` is the first failure ("" on success; "cause not recorded" when a failure
// has none). The prefix "Nightly dream ok" / "Nightly dream FAILED" is stable.
std::string dreamRowText(bool ok, const std::string& cause, uint32_t spentTokens, int pruned,
                         int deduped, size_t cap = kErrorEventMax);

}  // namespace orch
}  // namespace nimbus
