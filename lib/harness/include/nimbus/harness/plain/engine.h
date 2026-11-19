#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/item.h"

namespace agent {
struct Platform;
struct Channel;
struct HarnessConfig;
class HttpTransport;
}  // namespace agent
namespace nimbus {
namespace orch {
namespace tx {
class TxArena;
class TxFiles;
class TxLog;
struct HeadState;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/engine.h - the plain engine (PRD section 1). One turn is the simplest agent loop
// with durability around it:
//
//   store the owner message; open the turn (t item, the stub pointer's move, the system
//   text and tools fixed for the turn)
//   loop: render the stored transcript, post it, read the answer (plain/exchange.h);
//         store what the model said (k, a, c items) before anything runs;
//         no call: that text is the reply;
//         else each call: beforeDispatch, an intent item for an effect tool (it runs only
//         when the intent is stored), one dispatch, its result stored before the next
//         request leaves, afterResult
//   the guards (rounds, wall clock; RB-E1b scales them and adds spend and size): one
//   '[device] limit reached (...)' item and one last round with tool choice none
//   beforeDeliver (the truth seam: at most one hold, one more round), deliver, the d and e
//   items, afterDeliver
//
// Nothing in the loop reads, edits or appends to the model's text: the text it returns is
// the text delivered. A failed exchange ends the turn with the exchange's one plain line; a
// cut answer is delivered with the cut line and its calls are answered "not run".
// Durability: every item goes through TxLog::store before the next request leaves; an item
// the card refused stays in RAM (not stored), the model is told once and the owner once per
// episode; a call stored without a result (the turn stopped while it ran) gets an "outcome
// unknown" result at the next turn open and is never run again.
//
// It runs on the existing single task and TLS slot behind the existing contracts
// (Platform, HttpTransport, Channel) and the seams of plain/hooks.h. runTurn's own frame
// stays at most 512 B: the turn's state lives on the heap (PSRAM under the device's
// PSRAM-first scope). A refused allocation ends the turn with a status, never an abort.
//
// Seam declared by lane RB-B0a; the body (plain_engine.cpp) is written only by lanes
// RB-E1a and RB-E1b.
namespace agent {
namespace plain {

// One registry dispatch, with the caller's Principal (TurnCtx::who).
using ToolDispatchFn = std::function<ToolOutput(const ToolCall& call, const TurnCtx& turn)>;

// What the binding gives for one turn, asked once at turn open. The engine sets
// facts.turn, facts.digestBlock and facts.recentFiles itself (it owns the digests and the
// recent-files list); the rest is the binding's: the registry (classOf decides effect and
// pure), notes, skills, the time zone, the person's flags.
struct TurnSetup {
  RequestFacts facts;
  HeadEndpoint head;                 // host, key, effort, routed
  const WireCodec* wire = nullptr;   // nullptr: wireFor(turn.dialect)
  PhotoFn photo;                     // pixels of the visible photos (may be empty)
  int storedRounds = 0;              // the stored orchLoopRnds (guardLimitsFor)
  HeadCost cost;                     // what the spend and size guards scale from (RB-E1b)
  bool qualified = false;            // the head is qualified on engine 2 (tx::budgetFor)
};
using TurnSetupFn = std::function<Status(const TurnCtx& turn, TurnSetup& out)>;

struct EngineDeps {
  const Platform* platform = nullptr;        // nowMs, delayMs and nowEpoch are required
  const HarnessConfig* config = nullptr;     // not read: the limits come from TurnSetup
  HttpTransport* transport = nullptr;
  const Channel* channel = nullptr;
  PlainHooks* hooks = nullptr;               // may be null: no seam runs
  nimbus::orch::tx::TxLog* log = nullptr;    // the chat's log (one engine per chat)
  nimbus::orch::tx::TxArena* arena = nullptr;
  nimbus::orch::tx::TxFiles* files = nullptr;   // the same storage, for the digests; null: none
  std::string chatKey;                       // the chat the log and arena belong to
  ToolDispatchFn dispatch;
  TurnSetupFn setup;
  nimbus::orch::tx::RecentFilesBlock::Source recentFiles;   // the live file store; null: no list
};

// What a turn starts from. A person's message (Owner, Console) is stored as a u item before
// any model call; a Routine, AgentResult or Notice text as one device note of its class; a
// Resume carries no text (the message is already stored).
struct TurnInput {
  TurnCtx ctx;                   // turn 0: the next turn number
  std::string text;
  std::vector<nimbus::orch::tx::Part> parts;
  std::vector<uint64_t> src;     // Telegram update ids folded into the message
  std::string ch = "tg";         // u.ch: the channel slug
  int16_t tzMinutes = 0;         // local offset from UTC: the message stamp and stub times
  uint64_t m = 0;                // u.m: the day-stream row the message mirrors
};

struct TurnOutcome {
  bool delivered = false;        // the model's text went out
  std::string text;              // what was delivered (the model's text, unchanged)
  uint16_t rounds = 0;           // requests that returned an answer
  uint16_t toolCalls = 0;        // calls dispatched
  uint16_t headRequests = 0;
  GuardTrip trip = GuardTrip::None;
  StatusLine status = StatusLine::None;   // the one harness-written line, when one went out
  std::string statusText;
  bool parkHost = false;         // the head's key or credit was refused: the binding parks it
  bool failOver = false;         // routed: the router said fail over to the next target
  nimbus::orch::tx::TurnEndFacts end;
};

class PlainEngine {
 public:
  explicit PlainEngine(const EngineDeps& deps);
  ~PlainEngine();
  PlainEngine(const PlainEngine&) = delete;
  PlainEngine& operator=(const PlainEngine&) = delete;

  // One whole turn, from the owner item to the e item. InvalidArgument for missing deps or
  // a turn before boot; not ok when composition, the wire or the arena refused (the turn is
  // closed with an e item naming the cause where storage allows).
  Status runTurn(const TurnInput& in, TurnOutcome& out);
  // One unit of idle work through the idleStep seam.
  Status idle(const IdleCtx& ctx, IdleOutcome& out);
  // Once at boot: open the log (Io: run from RAM), fill the arena after thru, load the
  // digests, then the bootResume seam.
  Status boot(const BootCtx& ctx);

  const EngineDeps& deps() const { return deps_; }
  // The chat's head state, shared with the compaction driver (RB-C2), which moves thru and
  // nextQ and writes it; the engine writes it at every turn close and takes its next q from
  // max(its own, head.nextQ, the arena's last q + 1) before each turn. Null before boot.
  nimbus::orch::tx::HeadState* head();
  // The digests in force (RB-C2 replaces them after a compaction; the next turn shows them).
  std::vector<nimbus::orch::tx::DigestEntry>* digests();
  // The newest q the restart fill gave up (stored, not resident; 0: none): the compaction
  // driver's CompactDeps::omittedThru.
  uint64_t omittedThru() const;

  struct ChatState;   // the chat between turns (plain_engine.cpp)

 private:
  EngineDeps deps_;
  std::unique_ptr<ChatState> chat_;
};

}  // namespace plain
}  // namespace agent
