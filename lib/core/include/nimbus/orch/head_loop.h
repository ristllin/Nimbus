#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// head_loop - the portable, provider-agnostic controller for the head
// orchestrator's multi-turn agentic tool-use loop (ReAct). The model calls tools,
// sees each result, keeps going across model turns, and STOPS when it emits the
// terminal `orch_turn` (the "final answer" tool). This driver owns ONLY the loop
// control - bounded rounds / wall-clock deadline / per-round heap re-gate, plus the
// accumulator bound - and shuttles opaque JSON strings between the provider and the
// tool dispatcher through injected callbacks. It has NO Arduino / ArduinoJson / TLS
// dependency, so it is unit-tested on the host with fakes (pio test -e native).
//
// Provider continuity (Anthropic local messages[], OpenAI previous_response_id,
// Mistral conversation_id) lives in the adapter's `step` closure - the controller
// never sees it. Tool execution (parse args, registry.dispatch under the memory
// Lock) lives in the device's `dispatch` closure - the controller never parses JSON.
//
// SAFETY (this runs synchronously on the tg_poll task, which is NOT
// watchdog-subscribed; each round is a full TLS handshake through the single
// tls_arbiter work-slot): the caps ARE the safety. Every exit is graceful - a
// transport/parse failure, a hit cap, or a stalled model all return a well-formed
// HeadOutcome (never throws, never reboots). The caller supplies the final error
// reply when ok=false.
namespace nimbus {
namespace orch {

// One tool call the model requested this round. `argsJson` is the raw JSON
// arguments object (the controller does not parse it - the dispatch closure does).
struct HeadToolCall {
  std::string id;        // provider call id (Anthropic tool_use.id / OpenAI call_id …)
  std::string name;      // registry tool name, e.g. "memory.search"
  std::string argsJson;  // raw JSON arguments object ("{}" if none)
};

// The outcome of dispatching one HeadToolCall, fed back to the model next round.
// `output` is the text the model sees; `isError` marks a failed dispatch (the model
// is expected to recover - a tool error is NEVER fatal to the loop).
struct HeadToolResult {
  std::string id;       // echoes the originating HeadToolCall.id
  std::string name;
  std::string output;
  bool        isError = false;
};

// The result of ONE provider model turn (one `step`).
struct HeadStep {
  bool        ok = true;      // transport/parse succeeded
  std::string error;          // set when !ok - aborts the loop (fail-soft to caller)
  bool        finished = false;  // model emitted the terminal orch_turn
  std::string finalTurn;         // the orch_turn JSON (when finished)
  std::vector<HeadToolCall> toolCalls;  // tools to dispatch (when !finished)
  // Round prose ("thinking"): the non-tool text blocks the model emitted this
  // round. OBSERVED, never replayed (adapters keep their own API-pairing rules).
  // Glass Box A4 - this text was previously dropped on the floor at every
  // provider; empty when the provider produced none (or doesn't surface it).
  std::string text;
  // Opaque provider round payload, recorded into the transcript (attachMeta) and
  // replayed ONLY by the provider that wrote it. Carrier for OpenAI reasoning
  // items with encrypted_content - a reasoning model under store:false REQUIRES
  // its reasoning items replayed between tool calls (Stage 2 phase 3); a
  // cross-provider failover render simply ignores it.
  std::string meta;
};

// Perform ONE model turn. `allowTools`=false forces the "answer now" round (the
// adapter advertises NO tools / forces orch_turn) so the model must terminate.
// `priorResults` holds the outcomes of the tool calls returned by the PREVIOUS
// step, in order - empty on the first call (the adapter kicks off from the prompt
// it already holds). The adapter feeds these back through its own continuity path.
// `budgetMs` is the wall-clock budget REMAINING for the whole turn at the start of
// this round (F25): the adapter clamps its per-exchange socket timeout to it
// (floored so a near-exhausted final round still gets a real answer). It is
// UINT32_MAX when no deadline is configured - clamping against it is then a no-op,
// so a healthy turn behaves EXACTLY as before (remaining ≫ the provider timeout
// until the very end). Without it, each round got a fresh full 30–60 s exchange
// budget, so N slow-but-valid rounds stacked past the deadline (the ~10-min wedge
// with a progressively dropping heapMin - one hung read can't drain heap, a
// multi-round loop does).
// `capReason` is empty on a normal round and set ("rounds"|"deadline"|"heap"|
// "bytes"|"stalled") on the forced tool-less round. The adapter MUST tell the
// model why its tools went away - see kFinalRoundNotice.
//
// ⚠ Why this parameter exists. The loop used to remove the tools silently, and a
// model that suddenly cannot act does not say "I couldn't finish"; it invents a
// plausible continuation. Reproduced on the host against all three providers with
// a mid-loop heap cap, on a request to read three files:
//   mistral   "Searching the 'archive' project for files... (this will complete next turn)"
//   openai    "I'll check the archive project files in the background and report back"
//   anthropic "I've queued a memory query and will have the file-browsing tools next turn"
// None of it was true - nothing is queued, nothing runs later, and the device
// simply goes quiet. That is what the owner saw as "the model didn't respond to
// the actual content". A capped turn must fail HONESTLY.
using HeadStepFn = std::function<HeadStep(bool allowTools,
                                          const std::vector<HeadToolResult>& priorResults,
                                          uint32_t budgetMs,
                                          const std::string& capReason)>;

// The instruction adapters append to the forced final round. Shared so all four
// providers say the same thing and a test can pin it.
extern const char* const kFinalRoundNotice;

// Human-readable reason for the notice ("heap" -> "the device is low on memory").
const char* capReasonText(const std::string& reason);

// Execute one tool call and return its result. Never throws - a failed dispatch
// reports isError=true with the error text in `output` (the device wraps
// registry.dispatch under the memory Lock here).
using HeadDispatchFn = std::function<HeadToolResult(const HeadToolCall& call)>;

struct HeadLoopConfig {
  int      maxRounds = 4;             // max tool-dispatch rounds before forcing the answer
  uint32_t deadlineMs = 90000;       // wall-clock budget (0 => no deadline). Checked between rounds.
  uint32_t roundMinHeap = 28000;     // re-gate free internal heap before rounds AFTER the first
                                     // (round 0 is admitted by the caller's turn floor; 0 => skip)
  size_t   maxToolResultBytes = 2048;   // clamp each tool result fed back (0 => no clamp)
  size_t   maxTotalToolBytes = 12288;   // cumulative tool-output budget; exceeded => force the final round (0 => no limit)
};

// Forward decl: transcript.h includes THIS header (it uses HeadToolCall/Result),
// so the dependency runs one way only - hooks hold a pointer, never a value.
class Transcript;

struct HeadLoopHooks {
  HeadStepFn                              step;      // required
  HeadDispatchFn                          dispatch;  // required
  std::function<uint32_t()>               nowMs;     // required: monotonic ms (millis on device)
  std::function<uint32_t()>               freeHeap;  // optional: free INTERNAL heap; null => skip heap gate
  std::function<uint32_t()>               largestBlock; // optional: largest contiguous free INTERNAL
                                                        // block - logged per round so fragmentation
                                                        // (free≫largest) is visible in the trace
  // Optional recent-results spill (Context Fabric): called with the FULL result
  // BEFORE the maxToolResultBytes clamp fires. Returns a fetch handle ("r<seq>")
  // that the truncation marker embeds so the model can widen the view with
  // results.get - a clip becomes a view, never a loss. Null => the legacy
  // "…[truncated]" marker, byte-identical.
  std::function<std::string(const HeadToolResult& full)> spill;
  // Optional CANONICAL transcript (Context Fabric Stage 2). When set, the
  // controller records this turn's user seed, round prose, tool calls and tool
  // results into it - one provider-neutral record the adapters can render from
  // (phase 2+) and a mid-turn provider switch can carry over (phase 5), because
  // the results ride it as DATA and nothing has to be re-run.
  //
  // Purely additive in this phase: providers still keep their own continuity, so
  // the wire is byte-identical whether or not this is set.
  Transcript* transcript = nullptr;
  std::function<void(const std::string&)> log;       // optional trace
  // Optional round-prose observer (Glass Box A4): fired once per step whose
  // HeadStep.text is non-empty, BEFORE tool dispatch - the device persists it
  // as a kind=llm_response episodic row. Observer-only; never affects the loop.
  std::function<void(const std::string& text, int round)> onText;
};

struct HeadOutcome {
  bool        ok = false;      // a terminal orch_turn was produced
  std::string finalTurn;       // the orch_turn JSON (when ok)
  std::string error;           // set when !ok
  int         rounds = 0;      // tool-dispatch rounds executed (0 == single-shot)
  bool        hitCap = false;  // a cap forced the last (tool-less) round
  std::string capReason;       // "rounds"|"deadline"|"heap"|"bytes"|"stalled"|"" - why the loop was capped
};

// Drive the bounded ReAct loop to a terminal orch_turn. Always returns (never
// throws): terminates within maxRounds+1 model calls. See the header comment for
// the safety contract.
HeadOutcome runHeadLoop(const HeadLoopConfig& cfg, const HeadLoopHooks& hooks);

}  // namespace orch
}  // namespace nimbus
