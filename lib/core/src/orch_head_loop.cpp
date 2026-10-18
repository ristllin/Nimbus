#include "nimbus/orch/head_loop.h"

#include "nimbus/orch/billing_refusal.h"   // isBillingRefusal - the Billing step kind
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

const char kFinalCheckCallId[] = "orchcheck";

FinalCheck chainFinalChecks(std::vector<FinalCheck> checks) {
  std::vector<FinalCheck> live;
  for (auto& c : checks)
    if (c) live.push_back(std::move(c));
  if (live.empty()) return FinalCheck();
  return [live](const std::string& finalTurn) {
    std::string out;
    for (const FinalCheck& c : live) {
      const std::string fix = c(finalTurn);
      if (fix.empty()) continue;
      if (!out.empty()) out += "\n";
      out += fix;
    }
    return out;
  };
}

const char* capReasonText(const std::string& reason) {
  if (reason == "heap")     return "the device is low on memory";
  if (reason == "rounds")   return "this turn ran out of its tool-call budget";
  if (reason == "deadline") return "this turn ran out of time";
  if (reason == "bytes")    return "the tool-output budget for this turn was reached";
  if (reason == "stalled")  return "the turn stalled without requesting anything";
  return "a limit for this turn was reached";
}

// ---- typed step errors ----------------------------------------------------------

const char* stepErrSlug(StepErr k) {
  switch (k) {
    case StepErr::None:        return "none";
    case StepErr::Transport:   return "transport";
    case StepErr::Timeout:     return "timeout";
    case StepErr::RateLimited: return "rate_limited";
    case StepErr::BadRequest:  return "bad_request";
    case StepErr::Auth:        return "auth";
    case StepErr::Billing:     return "billing";
    case StepErr::LowMemory:   return "low_memory";
    case StepErr::ServerError: return "server_error";
    case StepErr::Rejected:    return "rejected";
    case StepErr::Cut:         return "cut";
  }
  return "none";
}

// The fabric table. Retrying the same host helps only when the cause was in
// transit (a cut connection, an overloaded server, a short rate window); a refusal
// of THIS request (its content, its key, its account, its model) comes back
// identical, so it switches at once - or, when the request itself is what is
// wrong (BadRequest) or the device is (LowMemory), fails without burning a switch.
FabricRule fabricRuleFor(StepErr k) {
  switch (k) {
    case StepErr::Transport:
    case StepErr::RateLimited:
    case StepErr::ServerError: return {true, true, false};
    case StepErr::Timeout:
    case StepErr::Rejected:
    case StepErr::Cut:         return {false, true, false};
    case StepErr::Auth:
    case StepErr::Billing:     return {false, true, true};
    case StepErr::BadRequest:
    case StepErr::LowMemory:   return {false, false, false};
    case StepErr::None:        break;
  }
  return {true, true, false};   // an untyped failure: the pre-typing behavior
}

namespace {
bool hasNoCase(const std::string& hay, const char* needle) {
  return detail::containsNoCase(hay, needle);
}
// A provider saying OUR body is unreadable: the same body fails on every host.
// Recorded 18 Oct: Anthropic "The request body is not valid JSON: invalid unicode
// code point", OpenAI invalid_json "unicode decode error", Mistral "There was an
// error parsing the body", Z.ai "JSON parse error: Invalid UTF-8 middle byte".
bool unreadableBody(const std::string& text) {
  static const char* const kMarks[] = {
      "not valid json", "invalid unicode", "unicode decode", "invalid utf-8",
      "error parsing the body", "json parse error", "invalid_json", "request_too_large",
      "request too large", "invalid body", "request field over", "request text is not valid utf-8",
  };
  for (const char* m : kMarks)
    if (hasNoCase(text, m)) return true;
  return false;
}
// The status a "<what> HTTP <ddd>" error line names, 0 when it names none.
int httpStatusIn(const std::string& err) {
  const size_t at = err.find("HTTP ");
  if (at == std::string::npos || at + 8 > err.size()) return 0;
  int st = 0;
  for (size_t i = at + 5; i < at + 8; i++) {
    if (err[i] < '0' || err[i] > '9') return 0;
    st = st * 10 + (err[i] - '0');
  }
  return st;
}
}  // namespace

StepErr classifyHttpStatus(int status, const std::string& text) {
  if (isBillingRefusal(status, text)) return StepErr::Billing;
  if (status == 401 || status == 403) return StepErr::Auth;
  if (status == 429) return StepErr::RateLimited;
  if (status >= 500) return StepErr::ServerError;
  if (status == 408) return StepErr::Transport;
  if (status == 413 || unreadableBody(text)) return StepErr::BadRequest;
  if (status >= 400) return StepErr::Rejected;
  return StepErr::ServerError;   // an odd status on an answer we could not use
}

bool isTimeoutError(const std::string& err) { return err.rfind("timeout:", 0) == 0; }

StepErr classifyStepError(const std::string& err, int* httpStatus) {
  const int st = httpStatusIn(err);
  if (httpStatus) *httpStatus = st;
  if (err.rfind("low memory", 0) == 0 || err.find("out of memory") != std::string::npos)
    return StepErr::LowMemory;
  if (isTimeoutError(err)) return StepErr::Timeout;
  if (err.rfind("network", 0) == 0) return StepErr::Transport;
  if (err == "schema parse" || err.rfind("request field over", 0) == 0 ||
      err.rfind("request text is not valid UTF-8", 0) == 0)
    return StepErr::BadRequest;
  if (err.rfind("cut at the output limit", 0) == 0 || err.find("truncated at max_tokens") == 0 ||
      err.rfind("response too large for the device", 0) == 0)
    return StepErr::Cut;
  if (st) return classifyHttpStatus(st, err);
  if (isBillingRefusal(0, err)) return StepErr::Billing;
  return StepErr::Transport;
}

std::string stepErrCause(StepErr k, const std::string& host, int httpStatus) {
  const std::string st = httpStatus > 0 ? " (HTTP " + std::to_string(httpStatus) + ")" : "";
  switch (k) {
    case StepErr::Transport:   return host + " could not be reached";
    case StepErr::Timeout:     return host + " did not answer in time";
    case StepErr::RateLimited: return host + " is rate limited" + st;
    case StepErr::BadRequest:  return host + " could not read the request" + st;
    case StepErr::Auth:        return host + " rejected the key" + st;
    case StepErr::Billing:     return host + " says the account is out of credit" + st;
    case StepErr::LowMemory:   return "the device ran low on memory";
    case StepErr::ServerError: return host + " had a server error" + st;
    case StepErr::Rejected:    return host + " refused the request" + st;
    case StepErr::Cut:         return host + " ran out of room for its answer";
    case StepErr::None:        break;
  }
  return host + " hit trouble";
}

namespace {
// Clamp one tool result to cfg.maxToolResultBytes (0 = no clamp), spilling the
// FULL result first so the truncation marker can carry a widenable fetch handle
// (Context Fabric).
void clampResult(const HeadLoopConfig& cfg, const HeadLoopHooks& hooks, HeadToolResult& r) {
  if (!cfg.maxToolResultBytes || r.output.size() <= cfg.maxToolResultBytes) return;
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
}  // namespace

HeadOutcome runHeadLoop(const HeadLoopConfig& cfg, const HeadLoopHooks& hooks) {
  HeadOutcome out;
  const uint32_t start = hooks.nowMs ? hooks.nowMs() : 0;
  std::vector<HeadToolResult> prior;  // empty on the first step
  size_t totalBytes = 0;
  bool forceFinal = false;            // set by a stall / the byte budget
  std::string forceReason;

  // Dispatch one round's calls: clamp + spill each result, record it, and
  // accumulate it for the next step. Returns true when every call succeeded.
  // Shared by an ordinary tool round and the siblings of a final orch_turn
  // (CUM-506), so both get the same clamp, spill, transcript and byte budget.
  auto dispatchAll = [&](const std::vector<HeadToolCall>& calls, int round) -> bool {
    bool allOk = true;
    for (const HeadToolCall& call : calls) {
      HeadToolResult r = hooks.dispatch(call);
      if (r.id.empty()) r.id = call.id;      // default: echo the call id
      if (r.name.empty()) r.name = call.name;
      if (r.isError) allOk = false;
      clampResult(cfg, hooks, r);
      totalBytes += r.output.size();
      if (hooks.transcript) hooks.transcript->addToolResult(r, round);
      prior.push_back(std::move(r));
    }
    if (cfg.maxTotalToolBytes && totalBytes >= cfg.maxTotalToolBytes) {
      forceFinal = true;
      forceReason = "bytes";
    }
    return allOk;
  };

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
    } else if (round > 0 && cfg.roundMinLargest && hooks.largestBlock &&
               hooks.largestBlock() < cfg.roundMinLargest) {
      // CUM-404: fragmentation, not total free, is what fails a round's mbedTLS
      // handshake alloc - cut the loop honestly (same "heap" cap reason -> the
      // owner-visible "the device is low on memory" notice) before it OOMs.
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
      out.kind = sr.kind != StepErr::None ? sr.kind : classifyStepError(out.error, &out.httpStatus);
      if (sr.httpStatus) out.httpStatus = sr.httpStatus;
      // A round cut at the output limit keeps what it wrote, so the engine can
      // salvage the reply's prefix with the honest marker (parsing P3).
      out.cut = sr.cut;
      if (!sr.cut.empty()) out.partialTurn = sr.finalTurn;
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
      if (!sr.meta.empty()) hooks.transcript->attachMeta(round, sr.meta);
    }
    if (sr.finished) {  // terminal orch_turn - the ONLY success exit
      // CUM-506: tool calls the model batched WITH its final orch_turn (five
      // memory_write calls beside "I've saved these permanently") were dropped,
      // so the reply claimed saves that never happened. They run NOW, before any
      // reply is delivered - and since that reply was written before their
      // results existed (a failed save, or a read whose data it never saw), it
      // is discarded: the next round hands the results back and the answer the
      // owner gets is written with them in hand. Each call runs exactly once.
      // On the forced final round tools were withdrawn (the model was told
      // nothing it asks for will run), so stray calls there are not executed.
      if (!sr.toolCalls.empty() && !lastChance) {
        prior.clear();
        const bool allOk = dispatchAll(sr.toolCalls, round);
        if (hooks.log)
          hooks.log("headloop: ran " + std::to_string(sr.toolCalls.size()) +
                    " tool call(s) sent with the final answer" + (allOk ? "" : " (one failed)") +
                    " - asking again with the results");
        continue;
      }
      if (!sr.toolCalls.empty() && hooks.log)
        hooks.log("headloop: dropped " + std::to_string(sr.toolCalls.size()) +
                  " tool call(s) sent with the final answer on the forced round");
      // The final-answer check (validateFinal): a turn whose shape lost work the
      // reply promises (a spawn list the device cannot read, a reply that says
      // sub-agents started while none is in session_ops) gets ONE correction
      // round, never more, and never on the forced round or for a cut answer
      // (the engine salvages that one). The rejected answer is recorded as the
      // orch_turn call the correction answers, so every provider's replay stays
      // well-formed (an unanswered tool_use 400s on Anthropic).
      if (cfg.validateFinal && !lastChance && out.corrections == 0 && sr.cut.empty()) {
        const std::string fix = cfg.validateFinal(sr.finalTurn);
        if (!fix.empty()) {
          out.corrections++;
          HeadToolCall call;
          call.id = kFinalCheckCallId;
          call.name = "orch_turn";
          call.argsJson = sr.finalTurn.empty() ? "{}" : sr.finalTurn;
          HeadToolResult res;
          res.id = kFinalCheckCallId;
          res.name = "orch_turn";
          res.output = fix;
          res.isError = true;
          prior.clear();
          if (hooks.transcript) {
            hooks.transcript->addToolCall(call, round, "");
            hooks.transcript->addToolResult(res, round);
            if (!sr.meta.empty()) hooks.transcript->attachMeta(round, sr.meta);
          }
          prior.push_back(std::move(res));
          if (hooks.log) hooks.log("headloop: final answer sent back for one correction - " + fix.substr(0, 120));
          continue;
        }
      }
      out.ok = true;
      out.finalTurn = sr.finalTurn;
      out.prose = sr.prose;
      if (sr.prose && hooks.log)
        hooks.log("headloop: final answer was prose (" + std::to_string(sr.finalTurn.size()) +
                  " B), delivered as the reply only");
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
    dispatchAll(sr.toolCalls, round);
  }
}

}  // namespace orch
}  // namespace nimbus
