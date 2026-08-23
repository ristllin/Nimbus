#include "nimbus/orch/head_loop.h"

#include "nimbus/orch/transcript.h"  // the canonical record the controller fills

namespace nimbus {
namespace orch {

// Kept blunt on purpose. A softer "you may not have all the information" let the
// model hedge into a promise anyway; it has to be told that there is no later.
const char* const kFinalRoundNotice =
    "\n\n[SYSTEM] This is your FINAL turn for this request and your tools are no "
    "longer available (%s). Nothing you ask for will run, and there is no later "
    "turn that continues this work. Answer NOW using only what you already have. "
    "State plainly what you could not complete and why. Do NOT say you will check, "
    "search, look into it, report back, continue in the background, or finish next "
    "turn - none of that will happen.";

const char* capReasonText(const std::string& reason) {
  if (reason == "heap")     return "the device is low on memory";
  if (reason == "rounds")   return "this turn ran out of its tool-call budget";
  if (reason == "deadline") return "this turn ran out of time";
  if (reason == "bytes")    return "the tool-output budget for this turn was reached";
  if (reason == "stalled")  return "the turn stalled without requesting anything";
  return "a limit for this turn was reached";
}

HeadOutcome runHeadLoop(const HeadLoopConfig& cfg, const HeadLoopHooks& hooks) {
  HeadOutcome out;
  const uint32_t start = hooks.nowMs ? hooks.nowMs() : 0;
  std::vector<HeadToolResult> prior;  // empty on the first step
  size_t totalBytes = 0;
  bool forceFinal = false;            // set by a stall / the byte budget
  std::string forceReason;

  // round increments only when we actually dispatch tools; once round reaches
  // maxRounds the gate below forces the tool-less final call, so the loop always
  // terminates within maxRounds+1 model turns.
  for (int round = 0;; ++round) {
    // ---- gate: decide whether this call may still use tools -----------------
    // The heap gate applies only to rounds AFTER the first: entry gating is the
    // CALLER's job (the turn-level floor admitted this turn), and blocking round 0
    // would make the loop strictly less available than the single-shot path at the
    // same heap (measured live: prompt composition eats ~4 KB below the floor on a
    // turn the device handles fine). The re-gate exists to stop ADDITIONAL rounds
    // when dispatches are draining heap.
    std::string reason;
    if (round >= cfg.maxRounds) {
      reason = "rounds";
    } else if (cfg.deadlineMs && hooks.nowMs &&
               (hooks.nowMs() - start) >= cfg.deadlineMs) {
      reason = "deadline";
    } else if (round > 0 && cfg.roundMinHeap && hooks.freeHeap &&
               hooks.freeHeap() < cfg.roundMinHeap) {
      reason = "heap";
    } else if (forceFinal) {
      reason = forceReason;
    }
    const bool lastChance = !reason.empty();
    const bool allowTools = !lastChance;

    if (hooks.log) {
      std::string m = "headloop round=" + std::to_string(round) +
                      " tools=" + (allowTools ? "1" : "0");
      // per-round free-INTERNAL-heap trace (the re-gate's input) so a before/after
      // SRAM-reclaim measurement can show exactly which round the loop got cut at.
      if (hooks.freeHeap) m += " heap=" + std::to_string(hooks.freeHeap());
      // free≫largest here = internal fragmentation, the prime suspect for
      // late-round degradation (bodies/TLS are PSRAM; only DMA+small allocs are internal).
      if (hooks.largestBlock) m += " intLargest=" + std::to_string(hooks.largestBlock());
      if (lastChance) m += " cap=" + reason;
      hooks.log(m);
    }

    // ---- one model turn -----------------------------------------------------
    // F25: hand the adapter the turn's REMAINING wall-clock budget so it can clamp
    // this round's socket timeout - a slow round can no longer consume a fresh full
    // exchange budget and push the turn past the deadline. UINT32_MAX when no
    // deadline is configured (clamp becomes a no-op → identical to the old behavior).
    uint32_t budgetMs = UINT32_MAX;
    if (cfg.deadlineMs && hooks.nowMs) {
      const uint32_t elapsed = hooks.nowMs() - start;
      budgetMs = elapsed >= cfg.deadlineMs ? 0u : (cfg.deadlineMs - elapsed);
    }
    // `reason` is non-empty exactly when this is the forced tool-less round, so
    // the adapter can tell the model that this is its last turn (see
    // kFinalRoundNotice - a silent capability removal produced confabulated
    // "I'll report back" replies on every provider).
    HeadStep sr = hooks.step(allowTools, prior, budgetMs, reason);
    if (!sr.ok) {  // transport/parse failure - fail soft to the caller
      out.ok = false;
      out.error = sr.error.empty() ? "provider step failed" : sr.error;
      out.rounds = round;
      // Carry the cap through even though the round FAILED. This path used to
      // drop it, so a turn that was capped and then died on transport logged
      // "rounds=2 cap=" - an empty reason next to a round that had just printed
      // "cap=heap". That is precisely the case worth diagnosing (the loop issues
      // one more full HTTPS request at the heap it just declared too low), and
      // the log said nothing about it.
      out.hitCap = lastChance;
      out.capReason = reason;
      return out;
    }
    // Round prose observer (Glass Box A4) - fires for finished rounds too (the
    // terminal round can carry thinking ahead of the orch_turn call).
    if (hooks.onText && !sr.text.empty()) hooks.onText(sr.text, round);
    // Canonical record (Context Fabric): prose, then the calls this round asked
    // for. Written BEFORE dispatch so a transcript inspected mid-turn is honest
    // about what was requested even if a dispatch never returns.
    if (hooks.transcript) {
      hooks.transcript->addAssistantText(sr.text, round, "");
      for (const HeadToolCall& c : sr.toolCalls) hooks.transcript->addToolCall(c, round, "");
      // Opaque provider payload for this round (e.g. OpenAI reasoning items with
      // encrypted_content) - attached to the round's first item; the writing
      // provider's renderer replays it, everyone else ignores it.
      if (!sr.meta.empty()) hooks.transcript->attachMeta(round, std::move(sr.meta));
    }
    if (sr.finished) {  // terminal orch_turn - the ONLY success exit
      out.ok = true;
      out.finalTurn = sr.finalTurn;
      out.rounds = round;
      out.hitCap = lastChance;
      out.capReason = reason;
      return out;
    }
    if (lastChance) {
      // We already forced the answer round (no tools) and it STILL didn't
      // terminate - stop rather than loop unbounded. Caller emits the error reply.
      out.ok = false;
      out.error = "tool budget reached without a final answer";
      out.rounds = round;
      out.hitCap = true;
      out.capReason = reason;
      return out;
    }

    // ---- dispatch the requested tools, accumulate results for next round ----
    prior.clear();
    if (sr.toolCalls.empty()) {
      // Not finished, yet asked for no tools - a stalled turn. Force one final
      // tool-less round so the model must answer (guards an infinite spin).
      forceFinal = true;
      forceReason = "stalled";
      continue;
    }
    for (const HeadToolCall& call : sr.toolCalls) {
      HeadToolResult r = hooks.dispatch(call);
      if (r.id.empty()) r.id = call.id;      // default: echo the call id
      if (r.name.empty()) r.name = call.name;
      if (cfg.maxToolResultBytes && r.output.size() > cfg.maxToolResultBytes) {
        // Spill the FULL result to the recent-results ring BEFORE clipping, so
        // the marker can carry a widenable fetch handle (Context Fabric).
        const size_t fullSize = r.output.size();
        // ⚠ Never re-spill a results.* fetch: each page would push a fresh
        // near-duplicate entry into the 16-slot ring and could evict the very
        // entry being paged (prism 2026-08-05).
        const bool isFetch = r.name.rfind("results", 0) == 0;
        std::string handle = (hooks.spill && !isFetch) ? hooks.spill(r) : std::string();
        // Clamp at a UTF-8 boundary: if the first dropped byte is a continuation
        // byte (0b10xxxxxx), the cut splits a multi-byte code point - back up so the
        // fed-back string stays valid UTF-8 (invalid bytes can 400 the next request).
        size_t cut = cfg.maxToolResultBytes;
        while (cut > 0 && ((unsigned char)r.output[cut] & 0xC0) == 0x80) --cut;
        r.output.resize(cut);
        if (handle.empty()) {
          r.output += "…[truncated]";   // legacy marker, byte-identical (null hook)
        } else {
          r.output += "…[truncated " + std::to_string((unsigned)cut) + " of " +
                      std::to_string((unsigned)fullSize) +
                      " B - fetch the rest with results.get(\"" + handle + "\")]";
        }
      }
      totalBytes += r.output.size();
      if (hooks.transcript) hooks.transcript->addToolResult(r, round);
      prior.push_back(std::move(r));
    }
    if (cfg.maxTotalToolBytes && totalBytes >= cfg.maxTotalToolBytes) {
      forceFinal = true;
      forceReason = "bytes";
    }
  }
}

}  // namespace orch
}  // namespace nimbus
