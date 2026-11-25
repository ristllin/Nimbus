#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nimbus/harness/plain/engine.h"   // TurnOutcome
#include "nimbus/orch/billing_refusal.h"   // billingShortAnswer
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/spend_guard.h"     // frozenReplyTextOther: the members' neutral pause line (E2-CFIX3)
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
//     cause lifts. A closed head sends no request; the person gets engine 1's words (lane E2-PARK1: the
//     owner the notice once an episode, then every chat the short line at most once per ten minutes).
// Lane E2-PARK1: a held head with an open fall-back starts its turn there (GateVerdict::StandIn, the
// released engine's startPastParkedHead), the park is asked again by a request of its own between turns
// (recheckParked), and the owner is told once when a provider he was told about answers again (backLines).
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
  // Lane E2-PARK1: the head's provider is held (a refused key or credit, a pause, its own dollar limit) and a
  // fall-back the owner named is open: the turn runs and starts there (ChatMessage::standIn), with no request
  // to the held provider and never as the park's re-check (the idle re-check asks it, recheckParked).
  StandIn = 3,
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
  // E2-CFIX3: where `chat` stands under a dollar limit (LimitChat). Null: a member's chat with no notice.
  std::function<LimitChat(const std::string& chat)> limitChat;
  // Lane E2-PARK1: is a fall-back open for the held head, under the owner's rules for `cls` (the device: its
  // fallbacksFor, empty with mid-turn fail-over off)? Null: none, the gate holds as before.
  std::function<bool(nimbus::orch::ErrorClass cls)> standIn;
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

// Lane E2-PARK1: the chats given the short line while a park holds a provider, each at most once per
// kShortEveryMs (RAM: a restart may tell each once more). A new episode starts every chat afresh.
inline constexpr uint32_t kShortEveryMs = 10u * 60u * 1000u;
class ShortTold {
 public:
  // True when `chat` was not given the short line for `provider` in the last kShortEveryMs (and it is now).
  bool due(const std::string& provider, const std::string& chat, uint32_t nowMs) {
    std::lock_guard<std::mutex> lk(mx_);
    const std::string id = provider + "\n" + chat;
    for (Told& t : told_) {
      if (t.id != id) continue;
      if ((uint32_t)(nowMs - t.atMs) < kShortEveryMs) return false;
      t.atMs = nowMs;
      return true;
    }
    if (told_.size() >= kMax) told_.erase(told_.begin());
    told_.push_back(Told{id, nowMs});
    return true;
  }
  void end(const std::string& provider) {
    std::lock_guard<std::mutex> lk(mx_);
    const std::string head = provider + "\n";
    told_.erase(std::remove_if(told_.begin(), told_.end(),
                               [&head](const Told& t) { return t.id.compare(0, head.size(), head) == 0; }),
                told_.end());
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mx_);
    told_.clear();
  }

 private:
  struct Told {
    std::string id;
    uint32_t atMs;
  };
  static constexpr size_t kMax = 32;
  std::mutex mx_;
  std::vector<Told> told_;
};
inline ShortTold& shortTold() {
  static ShortTold t;
  return t;
}

// Where a held message was written: its chat, whether it is the owner's (his words and the episode's notice;
// any other chat gets the members' neutral line), and the time (E2-PARK1).
struct HeldChat {
  std::string chat;
  bool owner = true;
  uint32_t nowMs = 0;
};

// Lane E2-PARK1: the last refusal that parked each provider, in E2-FIX11's safe form (refusalReason: one ASCII
// line, no markup, no key, at most 160 B) with its status, for the episode's one notice when a later turn starts
// on a fall-back. RAM only: after a restart the notice names the status alone (never the provider's words).
class HeldReasons {
 public:
  struct Reason {
    int status = 0;
    std::string text;
    bool all = false;   // the account check was refused too: it refuses every request (item 6)
  };
  void keep(const std::string& provider, const Reason& r) {
    std::lock_guard<std::mutex> lk(mx_);
    for (Kept& k : kept_)
      if (k.provider == provider) return void(k.reason = r);
    if (kept_.size() >= nimbus::orch::ProviderPark::kMaxProviders) kept_.erase(kept_.begin());
    kept_.push_back(Kept{provider, r});
  }
  // The kept reason, else `status` with no words.
  Reason of(const std::string& provider, int status) {
    std::lock_guard<std::mutex> lk(mx_);
    for (const Kept& k : kept_)
      if (k.provider == provider) return k.reason;
    return Reason{status, std::string(), false};
  }
  void drop(const std::string& provider) {
    std::lock_guard<std::mutex> lk(mx_);
    kept_.erase(std::remove_if(kept_.begin(), kept_.end(), [&provider](const Kept& k) { return k.provider == provider; }),
                kept_.end());
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mx_);
    kept_.clear();
  }

 private:
  struct Kept {
    std::string provider;
    Reason reason;
  };
  std::mutex mx_;
  std::vector<Kept> kept_;
};
inline HeldReasons& heldReasons() {
  static HeldReasons r;
  return r;
}

// The episode's full notice for a refusal park (engine 1's deliverParkedRefusal words).
// Item 6: a provider whose account check was refused too: in its own words, as E2-FIX11's safe form keeps them.
inline std::string parkedNotice(const std::string& provider, const std::string& label, bool key) {
  namespace orch = nimbus::orch;
  const orch::ParkCause cause = orch::providerPark().causeOf(provider.c_str());
  const HeldReasons::Reason r = heldReasons().of(provider, 0);
  if (r.all && cause == orch::ParkCause::Unspecified)
    return "That didn't finish - " + label + " refuses every request from this key" +
           (r.text.empty() ? std::string(".") : ": " + r.text + (r.text.back() == '.' ? "" : ".")) +
           " Your message was not answered. Nothing is still running.";
  const std::string head =
      cause != orch::ParkCause::Unspecified ? "That didn't finish - " + orch::parkCauseNotice(label, cause, std::string())
      : key ? "That didn't finish - " + label + " rejected its key. Update it under Assistant > Models."
            : "That didn't finish - " + label +
                  " says the account is out of credit. Add credit on its billing page, or switch providers.";
  return head + " Your message was not answered. Nothing is still running.";
}

// Engine 1's words for a turn the park closed with no request (engine.cpp deliverParkedRefusal over
// deliverFailure, nothing ran, no helper running); "" = nothing this time. A turn no person wrote (a routine)
// may carry the episode's notice by the park's latch, never a short line. Lane E2-PARK1: a person's message is
// never left without a line: the owner gets the episode's notice once, then every chat the short line at most
// once per kShortEveryMs (its own clock); a member's chat gets the neutral line with no money detail and never
// takes the owner's notice.
inline std::string parkedLine(const std::string& provider, const nimbus::orch::ParkVerdict& v, bool personWrote,
                              const HeldChat& at) {
  namespace orch = nimbus::orch;
  const std::string label = providerLabelOf(provider);
  if (v.paused) return "Your message was not answered: " + orch::parkWords(label.c_str(), v) + ".";
  const orch::ParkCause cause = orch::providerPark().causeOf(provider.c_str());
  const bool key = v.kind == orch::ParkKind::Auth && cause == orch::ParkCause::Unspecified;
  if (!personWrote)
    return orch::refusalTurnAnswer(provider.c_str(), false) == orch::RefusalTurnAnswer::Notice
               ? parkedNotice(provider, label, key)
               : std::string();
  if (at.owner && orch::providerPark().noticeDue(provider.c_str())) return parkedNotice(provider, label, key);
  if (!shortTold().due(provider, at.chat, at.nowMs)) return std::string();
  if (!at.owner) return orch::frozenReplyTextOther();
  return key ? orch::keyShortAnswer(label) : orch::billingShortAnswer(label, orch::providerPark().recheckInMs(provider.c_str()));
}

// E2-CFIX3: the chats told under a provider's own dollar limit, each once per pause (RAM: a restart tells each
// once more). The park's notice latches are never touched here: they belong to its refusal episodes.
class LimitTold {
 public:
  // True the first time `chat` asks while `provider` is paused by its limit.
  bool due(const std::string& provider, const std::string& chat) {
    std::lock_guard<std::mutex> lk(mx_);
    const std::string id = provider + "\n" + chat;
    if (std::find(told_.begin(), told_.end(), id) != told_.end()) return false;
    if (told_.size() >= kMax) told_.erase(told_.begin());   // the oldest may be told once more
    told_.push_back(id);
    return true;
  }
  // The pause is over: every chat is told again next time.
  void end(const std::string& provider) {
    std::lock_guard<std::mutex> lk(mx_);
    const std::string head = provider + "\n";
    told_.erase(std::remove_if(told_.begin(), told_.end(),
                               [&head](const std::string& id) { return id.compare(0, head.size(), head) == 0; }),
                told_.end());
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mx_);
    told_.clear();
  }

 private:
  static constexpr size_t kMax = 32;
  std::mutex mx_;
  std::vector<std::string> told_;
};
inline LimitTold& limitTold() {
  static LimitTold t;
  return t;
}
inline bool pausedByLimit(const std::string& provider) {
  return (nimbus::orch::providerPark().pausedBy(provider.c_str()) & nimbus::orch::kPauseProviderLimit) != 0;
}

// The line for a message the provider's own dollar limit leaves unanswered, once per chat per pause: the
// owner's short pause line (E2-CFIX1), or for any other chat the neutral line the overall ceiling gives
// members (spend::frozenReplyFor). A token or call budget keeps engine 1's line.
inline std::string limitLine(const std::string& provider, const std::string& chat, const LimitChat& where,
                             bool personWrote) {
  namespace orch = nimbus::orch;
  if (!pausedByLimit(provider)) return overBudgetLine(provider);
  if (!personWrote || !limitTold().due(provider, chat)) return std::string();
  if (!where.owner) return orch::frozenReplyTextOther();
  orch::ParkVerdict v;
  v.open = false;
  v.paused = orch::kPauseProviderLimit;
  return "Your message was not answered: " + orch::parkWords(providerLabelOf(provider).c_str(), v) + ".";
}

// The line of a turn the limit stopped before its next request (EngineDeps::paidHeld gave `line`): where the
// freeze notice lands it is the pause's one line, so the turn adds none; any other chat is told here, once.
inline std::string limitTurnLine(const std::string& provider, const std::string& chat, const LimitChat& where,
                                 const std::string& line) {
  if (!pausedByLimit(provider)) return line;
  if (where.notice) return std::string();
  return limitLine(provider, chat, where, /*personWrote=*/true);
}

// Lane E2-PARK1: engine 1's class for a held head's fall-back (engine.cpp errorClassOf over the park's kind):
// no credit Disabled, a refused key Auth, a pause none.
inline nimbus::orch::ErrorClass heldClassOf(nimbus::orch::ParkKind kind, bool paused) {
  namespace orch = nimbus::orch;
  if (paused) return orch::ErrorClass::None;
  return kind == orch::ParkKind::Auth ? orch::ErrorClass::Auth : orch::ErrorClass::Disabled;
}

// The park's record of `provider` (false: none).
inline bool parkEntryOf(const std::string& provider, uint32_t nowMs, nimbus::orch::ParkEntry& out) {
  namespace orch = nimbus::orch;
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  const size_t n = orch::providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, nowMs);
  for (size_t i = 0; i < n; ++i)
    if (provider == e[i].provider) return out = e[i], true;
  return false;
}

// Lane E2-PARK1: the head is held (parked, its re-check due or out, paused, or over its own dollar limit) and
// a fall-back is open: the turn starts there. Reads only: no re-check handed out, no latch taken. A token or
// call budget keeps engine 1's line (no fall-back for it, as on engine 1).
inline bool standInGate(const MessageGateDeps& d, uint32_t keyFp, uint32_t nowMs, MessageGate& g) {
  namespace orch = nimbus::orch;
  if (!d.standIn) return false;
  const bool over = d.overBudget && d.overBudget(d.provider);
  const bool limit = over && pausedByLimit(d.provider);
  if (over && !limit) return false;
  const orch::ParkVerdict v = orch::providerPark().peek(d.provider.c_str(), orch::PaidSeam::Chat, keyFp, nowMs);
  if (!limit && v.open && !v.due) return false;
  orch::ParkEntry e;
  const orch::ParkKind kind = parkEntryOf(d.provider, nowMs, e) ? e.kind : orch::ParkKind::None;
  const bool paused = limit || v.paused != 0 || kind == orch::ParkKind::None;
  if (!d.standIn(heldClassOf(kind, paused))) return false;
  g.verdict = GateVerdict::StandIn;
  g.why = paused ? "paused" : "parked";
  return true;
}

// The gates in engine 1's order: the inbound screen, the provider's limit, then the park. `personWrote`:
// a person sent this message (a routine never takes the short answer). Asks the park as a seam about to
// send: a due re-check is handed out (probe), and a changed key ends a park at once. E2-PARK1: a held head
// with an open fall-back answers StandIn first (no re-check, no line).
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
  if (!pausedByLimit(d.provider)) limitTold().end(d.provider);   // E2-CFIX3: a lifted pause tells again
  const uint32_t now = d.nowMs ? d.nowMs() : 0;
  const uint32_t fp = orch::parkKeyFingerprint(d.key ? d.key() : std::string());
  if (standInGate(d, fp, now, g)) return g;
  if (d.overBudget && d.overBudget(d.provider)) {
    g.verdict = GateVerdict::Held;
    g.why = "limit";
    g.line = limitLine(d.provider, chat, d.limitChat ? d.limitChat(chat) : LimitChat{}, personWrote);
    return g;
  }
  const orch::ParkVerdict v = orch::providerPark().available(d.provider.c_str(), orch::PaidSeam::Chat, fp, now);
  g.probe = v.probe;
  if (v.open) return g;
  g.verdict = GateVerdict::Held;
  g.why = v.paused ? "paused" : "parked";
  g.line = parkedLine(d.provider, v, personWrote, HeldChat{chat, !d.limitChat || d.limitChat(chat).owner, now});
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

// Lane E2-PARK1: the providers the owner is to be told are answering again, in order, each once (RAM: a restart
// before the line went loses it, never invents one). The device sends them outside quiet hours.
class BackLines {
 public:
  void push(const std::string& provider) {
    std::lock_guard<std::mutex> lk(mx_);
    if (std::find(due_.begin(), due_.end(), provider) != due_.end()) return;
    if (due_.size() >= nimbus::orch::ProviderPark::kMaxProviders) due_.erase(due_.begin());
    due_.push_back(provider);
  }
  bool take(std::string& provider) {
    std::lock_guard<std::mutex> lk(mx_);
    if (due_.empty()) return false;
    provider = due_.front();
    due_.erase(due_.begin());
    return true;
  }
  bool empty() {
    std::lock_guard<std::mutex> lk(mx_);
    return due_.empty();
  }
  // The dollar-limit pauses seen (a lift is told once: noteLimitState).
  bool seenPaused(const std::string& provider, bool paused) {
    std::lock_guard<std::mutex> lk(mx_);
    const auto it = std::find(paused_.begin(), paused_.end(), provider);
    const bool was = it != paused_.end();
    if (paused && !was) paused_.push_back(provider);
    if (!paused && was) paused_.erase(it);
    return was;
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mx_);
    due_.clear();
    paused_.clear();
  }

 private:
  std::mutex mx_;
  std::vector<std::string> due_;
  std::vector<std::string> paused_;
};
inline BackLines& backLines() {
  static BackLines b;
  return b;
}

// The owner's one line when `provider` answers again: "Anthropic is answering again. Replies are back on
// claude-sonnet-5-5." (`model` "": the provider is not his head, the first sentence alone).
inline std::string backLine(const std::string& provider, const std::string& model) {
  std::string s = providerLabelOf(provider) + " is answering again.";
  if (!model.empty()) s += " Replies are back on " + model + ".";
  return s;
}

// The provider answered on `seam`: its episode ends (the park's noteSuccess). When the owner was told about
// that park, the back line is due once. True when an episode ended.
inline bool endToldEpisode(const std::string& provider, nimbus::orch::PaidSeam seam, uint32_t nowMs) {
  namespace orch = nimbus::orch;
  orch::ParkEntry e;
  const bool told = parkEntryOf(provider, nowMs, e) && e.kind != orch::ParkKind::None && e.noticed;
  if (!orch::providerPark().noteSuccess(provider.c_str(), seam)) return false;
  shortTold().end(provider);
  heldReasons().drop(provider);
  if (told) backLines().push(provider);
  return true;
}

// The provider's own dollar limit, read between turns: a pause seen and then lifted makes the back line due
// (the limits store's freeze notice told the owner). A restart forgets what it saw: never a false "back".
inline void noteLimitState(const std::string& provider) {
  const bool paused = pausedByLimit(provider);
  if (backLines().seenPaused(provider, paused) && !paused) backLines().push(provider);
}

// E2-CFIX1: a refusal the engine's classifier read as billing (its marker table: a key's own usage limit, an
// HTTP 400 the shared billing words do not name) is parked as no credit; any other through the park's own rule.
// E2-PARK1: a refusal that parks keeps its reason for the episode's notice (HeldReasons).
// `all`: the account check was refused too (item 6): parked as no credit (every seam, the 30-minute re-check),
// in engine 2's words "refuses every request" (RAM: after a restart the park's own words).
struct RefusalSeen {
  int status = 0;
  const std::string& text;
  bool billing = false;
  bool all = false;
};
inline nimbus::orch::ParkKind noteRefusal(const char* provider, uint32_t keyFp, const RefusalSeen& r, uint32_t nowMs) {
  namespace orch = nimbus::orch;
  const int status = r.status;
  const std::string& text = r.text;
  const bool billing = r.billing, all = r.all;
  orch::ParkKind k = orch::ParkKind::Billing;
  if (billing) orch::providerPark().park(provider, orch::PaidSeam::Chat, keyFp, orch::ParkKind::Billing, status, nowMs);
  else k = orch::providerPark().noteAnswer(provider, orch::PaidSeam::Chat, keyFp, status, text, nowMs);
  if (k != orch::ParkKind::None) heldReasons().keep(provider, HeldReasons::Reason{status, refusalReason(text), all});
  return k;
}

// Lane E2-PARK1 item 6: the account checks asked, each provider at most once per kAccountCheckEveryMs (RAM).
inline constexpr uint32_t kAccountCheckEveryMs = 10u * 60u * 1000u;
class AccountChecks {
 public:
  // True (and noted) when `provider`'s account was not asked in the last kAccountCheckEveryMs.
  bool due(const std::string& provider, uint32_t nowMs) {
    std::lock_guard<std::mutex> lk(mx_);
    for (Asked& a : asked_) {
      if (a.provider != provider) continue;
      if ((uint32_t)(nowMs - a.atMs) < kAccountCheckEveryMs) return false;
      a.atMs = nowMs;
      return true;
    }
    if (asked_.size() >= nimbus::orch::ProviderPark::kMaxProviders) asked_.erase(asked_.begin());
    asked_.push_back(Asked{provider, nowMs});
    return true;
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mx_);
    asked_.clear();
  }

 private:
  struct Asked {
    std::string provider;
    uint32_t atMs;
  };
  std::mutex mx_;
  std::vector<Asked> asked_;
};
inline AccountChecks& accountChecks() {
  static AccountChecks a;
  return a;
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
    (void)noteRefusal(r.provider.c_str(), r.keyFp, RefusalSeen{r.status, r.text, r.billing, r.all}, nowMs);
  if (out.parkHost) {
    const orch::ParkKind k =
        noteRefusal(p, orch::parkKeyFingerprint(key), RefusalSeen{out.parkStatus, out.parkText, out.parkBilling, out.parkAll},
                    nowMs);
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
    (void)endToldEpisode(provider, orch::PaidSeam::Chat, nowMs);   // E2-PARK1: the owner told it is back
    return;
  }
  if (out.headRequests > 0) {
    park.noteFailure(p, nowMs);
    return;
  }
  if (probe) park.releaseProbe(p);
}

// ---- lane E2-PARK1: the re-check of its own (engine 1's TurnEngine::recheckParkedHost) ----------------------
// What one small request to a parked provider said: sent (false: nothing left, the re-check goes back),
// answered (2xx with an answer), refused for its key or credit (the exchange's parkHost: its status, body and
// the classifier's billing read), else a failure with no verdict (network, 5xx, rate limit).
struct RecheckAnswer {
  bool sent = false;
  bool answered = false;
  bool refused = false;
  int status = 0;
  std::string body;
  bool billing = false;
};
struct RecheckDeps {
  std::function<bool(const std::string& provider)> askable;    // keyed, a slot engine 2 reaches, under its limit
  std::function<std::string(const std::string& provider)> key;  // the key the request carries (its fingerprint)
  std::function<RecheckAnswer(const std::string& provider)> send;
  uint32_t nowMs = 0;
};

// One parked provider whose re-check is due gets one small request of its own, within the park's probe lease
// (claimProbe), so no person's message is ever the re-check while this runs between turns. Its answer goes to
// the park: answered ends the episode (and the owner is told once: endToldEpisode), a refusal re-parks quietly
// for another kRecheckMs (a refusal bills nothing), anything else re-arms the wait. A park on one seam only (a
// 403 for one model) is that seam's. Returns the provider asked ("" = none was due).
inline std::string recheckParked(const RecheckDeps& d) {
  namespace orch = nimbus::orch;
  orch::ProviderPark& park = orch::providerPark();
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  const size_t n = park.snapshot(e, orch::ProviderPark::kMaxProviders, d.nowMs);
  for (size_t i = 0; i < n; ++i) {
    const bool chatSeam = e[i].seamMask & (1u << (uint8_t)orch::PaidSeam::Chat);
    if (e[i].kind == orch::ParkKind::None || e[i].paused || e[i].probing || e[i].recheckInMs || !chatSeam) continue;
    const std::string p = e[i].provider;
    if ((d.askable && !d.askable(p)) || !park.claimProbe(p.c_str(), orch::PaidSeam::Chat, d.nowMs)) continue;
    const RecheckAnswer a = d.send ? d.send(p) : RecheckAnswer{};
    if (!a.sent) park.releaseProbe(p.c_str());
    else if (a.answered) (void)endToldEpisode(p, orch::PaidSeam::Chat, d.nowMs);
    else if (a.refused)
      (void)noteRefusal(p.c_str(), orch::parkKeyFingerprint(d.key ? d.key(p) : std::string()),
                        RefusalSeen{a.status, a.body, a.billing, false}, d.nowMs);
    else park.noteFailure(p.c_str(), d.nowMs);
    return p;
  }
  return std::string();
}

}  // namespace plain
}  // namespace agent
