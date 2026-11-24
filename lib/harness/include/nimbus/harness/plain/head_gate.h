#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/harness/plain/engine.h"   // TurnOutcome
#include "nimbus/orch/billing_refusal.h"   // billingShortAnswer
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/provider_slots.h"    // findProviderSlot: the provider's display name

// plain/head_gate.h - engine 1's gates in front of an engine-2 head (lane E2-FIX14).
//
// Engine 1 asks three things before a person's message costs a request, all shared code:
//   - the inbound moderation gate for a member's message (orchestrator_common.cpp moderateGate,
//     ModGate::InboundText): a block starts no turn and the person gets engine 1's line;
//   - the provider's own monthly limit (store::providerOverBudget: the dollar limit's pause and the
//     token and call budgets): engine 1's line, every message, until the limit is raised or resets;
//   - the shared provider park (nimbus/orch/provider_park.h): a refused key or an empty credit closes
//     the provider for every seam, re-checked by one request after ProviderPark::kRecheckMs, ended at
//     once by a changed key or an answered request; a pause (the owner, a limit) closes it until its
//     cause lifts. A closed head sends no request; the person gets engine 1's words by the park's own
//     latches (refusalTurnAnswer: the notice once an episode, then one short answer, then nothing).
// After a turn, noteHeadOutcome tells the park what the head's answer said, as engine 1's wire does:
// a key or credit refusal parks (the exchange's own line told the person: the episode's notice is
// taken), an answered request ends the episode, a request with no answer re-arms a re-check.
//
// The lines are device copy for the person (sent with system provenance, never to a model); each is
// engine 1's own (lib/harness/src/engine.cpp deliverParkedRefusal and its budget gate, which the
// engine-1 image keeps as they are), so a person reads the same words on either engine.
namespace agent {
namespace plain {

enum class GateVerdict : uint8_t {
  Run = 0,       // a turn may run
  Blocked = 1,   // the inbound moderation gate blocked a member's message: no turn, nothing kept
  Held = 2,      // the provider is over its limit, parked or paused: no request
};

struct MessageGateDeps {
  std::string provider;                 // the head's provider slot (the park's and the limit's name; E2-R8a: every slot)
  std::function<std::string()> key;      // the key a request would carry (its fingerprint only)
  std::function<uint32_t()> nowMs;       // the park's clock (device: millis)
  // The inbound gate for `chat` and `text`: the person's line when it blocks, "" when the message may
  // go (engine 1's moderateGate; it screens only a member's message). Null: no gate.
  std::function<std::string(const std::string& chat, const std::string& text)> inbound;
  // The provider's own monthly limit is reached (engine 1's overBudget). Null: no limit.
  std::function<bool(const std::string& provider)> overBudget;
};

struct MessageGate {
  GateVerdict verdict = GateVerdict::Run;
  std::string line;          // the person's line; "" = nothing (engine 1's silence for the rest of an episode)
  const char* why = "";      // log slug: "inbound", "limit", "parked", "paused"
  bool probe = false;        // Run as the park's one re-check request (ChatMessage::probe)
};

// The provider's display name, as engine 1 names it in a notice ("Anthropic").
inline std::string providerLabelOf(const std::string& slug) {
  const nimbus::orch::ProviderSlot* ps = nimbus::orch::findProviderSlot(slug.c_str());
  return ps ? std::string(ps->label) : slug;
}

// Engine 1's line for an owner turn its budget gate stopped (engine.cpp, the per-provider budget).
inline std::string overBudgetLine(const std::string& provider) {
  return std::string("\xE2\x9A\xA0\xEF\xB8\x8F ") + provider +
         " has hit its monthly token budget. Raise the limit in Usage & Budget or wait for the reset.";
}

// Engine 1's words for a turn the park closed with no request (engine.cpp deliverParkedRefusal over
// deliverFailure, nothing ran, no helper running), by the park's latches; "" = nothing this time.
inline std::string parkedLine(const std::string& provider, const nimbus::orch::ParkVerdict& v, bool personWrote) {
  namespace orch = nimbus::orch;
  const std::string label = providerLabelOf(provider);
  if (v.paused) return "Your message was not answered: " + orch::parkWords(label.c_str(), v) + ".";
  const orch::ParkCause cause = orch::providerPark().causeOf(provider.c_str());
  const bool key = v.kind == orch::ParkKind::Auth && cause == orch::ParkCause::Unspecified;
  switch (orch::refusalTurnAnswer(provider.c_str(), personWrote)) {
    case orch::RefusalTurnAnswer::Notice: {
      const std::string head =
          cause != orch::ParkCause::Unspecified ? "That didn't finish - " + orch::parkCauseNotice(label, cause, std::string())
          : key ? "That didn't finish - " + label + " rejected its key. Update it under Assistant > Models."
                : "That didn't finish - " + label +
                      " says the account is out of credit. Add credit on its billing page, or switch providers.";
      return head + " Your message was not answered. Nothing is still running.";
    }
    case orch::RefusalTurnAnswer::Short:
      return key ? orch::keyShortAnswer(label)
                 : orch::billingShortAnswer(label, orch::providerPark().recheckInMs(provider.c_str()));
    case orch::RefusalTurnAnswer::Nothing: break;
  }
  return std::string();
}

// The gates in engine 1's order: the inbound screen, the provider's limit, then the park. `personWrote`:
// a person sent this message (a routine never takes the short answer). Asks the park as a seam about to
// send: a due re-check is handed out (probe), and a changed key ends a park at once.
inline MessageGate gateMessage(const MessageGateDeps& d, const std::string& chat, const std::string& text,
                               bool personWrote) {
  namespace orch = nimbus::orch;
  MessageGate g;
  if (d.inbound) {
    g.line = d.inbound(chat, text);
    if (!g.line.empty()) {
      g.verdict = GateVerdict::Blocked;
      g.why = "inbound";
      return g;
    }
  }
  if (d.overBudget && d.overBudget(d.provider)) {
    g.verdict = GateVerdict::Held;
    g.why = "limit";
    g.line = overBudgetLine(d.provider);
    return g;
  }
  const std::string key = d.key ? d.key() : std::string();
  const orch::ParkVerdict v = orch::providerPark().available(d.provider.c_str(), orch::PaidSeam::Chat,
                                                             orch::parkKeyFingerprint(key), d.nowMs ? d.nowMs() : 0);
  g.probe = v.probe;
  if (v.open) return g;
  g.verdict = GateVerdict::Held;
  g.why = v.paused ? "paused" : "parked";
  g.line = parkedLine(d.provider, v, personWrote);
  return g;
}

// A turn no person waits for (a routine fire): closed when the limit is reached or the park holds the
// provider. Reads only: never takes the re-check, never a latch. `why`: the park's words, or the limit.
inline bool headHeld(const std::string& provider, const std::string& key, bool overBudget, uint32_t nowMs,
                     std::string& why) {
  namespace orch = nimbus::orch;
  why.clear();
  if (overBudget) {
    why = provider + " has hit its monthly limit";
    return true;
  }
  const orch::ParkVerdict v =
      orch::providerPark().peek(provider.c_str(), orch::PaidSeam::Chat, orch::parkKeyFingerprint(key), nowMs);
  if (v.open) return false;
  why = orch::parkWords(provider.c_str(), v);
  return true;
}

// After a turn on `provider` with `key`: the park learns what the head's answer said. `probe`: the
// turn was the park's re-check (a turn that sent nothing hands it back). `told`: the turn's own line
// reached the person (an owner turn's status line); a routine's does not, so the owner's next message
// still gets the episode's notice (engine 1: a routine may carry it, never take it).
inline void noteHeadOutcome(const std::string& provider, const std::string& key, const TurnOutcome& out, bool probe,
                            bool told, uint32_t nowMs) {
  namespace orch = nimbus::orch;
  orch::ProviderPark& park = orch::providerPark();
  const char* p = provider.c_str();
  // E2-REP1: a fallback head that refused its key or credit is parked as the head is (no latch: the
  // turn's line named the switch, not the refusal, so its notice is still due if it becomes the head).
  for (const TurnOutcome::FallbackRefusal& r : out.fallbackRefusals)
    (void)park.noteAnswer(r.provider.c_str(), orch::PaidSeam::Chat, r.keyFp, r.status, r.text, nowMs);
  if (out.parkHost) {
    const orch::ParkKind k =
        park.noteAnswer(p, orch::PaidSeam::Chat, orch::parkKeyFingerprint(key), out.parkStatus, out.parkText, nowMs);
    // The exchange's own line told the person why: that was this episode's notice, so the next
    // message gets the short answer and the ones after it nothing (engine 1's rule).
    if (k != orch::ParkKind::None && told) (void)park.noticeDue(p);
    return;
  }
  if (out.switches) {   // E2-P-FO: the head failed and another answered: no answer of its own ends an episode
    if (probe) park.releaseProbe(p);
    return;
  }
  if (out.rounds > 0) {
    (void)park.noteSuccess(p, orch::PaidSeam::Chat);
    return;
  }
  if (out.headRequests > 0) {
    park.noteFailure(p, nowMs);
    return;
  }
  if (probe) park.releaseProbe(p);
}

}  // namespace plain
}  // namespace agent
