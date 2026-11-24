#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/exchange.h"   // ExchangeErr: the class a fail-over is asked for (E2-P-FO)
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
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

// An unattended turn's own bounds (plain/unattended.h: a routine's 6 rounds, 90 s soft, 180 s
// hard, 0.05 USD a fire; ruling 84 item 9). Each nonzero field lowers the limit the head scales
// to; zero leaves it. Lane RB-U1a.
struct TurnCeiling {
  uint16_t rounds = 0;
  uint32_t softWallMs = 0;
  uint32_t hardWallMs = 0;
  uint32_t spendMicroUsd = 0;
};
// The limits after a ceiling: the lower of each pair, and the hard wall the last round runs to.
inline void applyCeiling(const TurnCeiling& c, GuardLimits& limits, uint32_t& hardWallMs) {
  auto lower = [](auto& v, uint32_t cap) {
    if (cap && v > cap) v = static_cast<std::remove_reference_t<decltype(v)>>(cap);
  };
  lower(limits.rounds, c.rounds);
  lower(limits.hardRounds, c.rounds);         // the round guard itself (lane RB-X3's hard rounds)
  lower(limits.softWallMs, c.softWallMs);
  lower(limits.timeLimitMs, c.softWallMs);    // the time guard itself (lane RB-X3's time limit)
  lower(limits.maxSpendMicroUsd, c.spendMicroUsd);
  lower(hardWallMs, c.hardWallMs);
}

// What the binding gives for one turn, asked at turn open (and, with EngineDeps::stablePrefix, once
// more at the close of a turn that ran calls, to see what the turn itself changed). The engine sets
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
  bool qualified = false;            // qualified on engine 2: routing and truth mode only, never the window
  uint16_t dayStartMin = kDayStartDefaultMin;   // the device's day start (TurnCtx::dayStartMin)
  TurnCeiling ceiling;               // an unattended turn's own bounds (RB-U1a); none by default
  // A test rig's smaller request body cap, bytes (lane E2-FIX9: the overflow ladder on a real provider at a
  // small cost); 0: the head's own (tx::HeadBudget::bodyCap). Never larger than the head's.
  uint32_t bodyCapLimit = 0;
};
using TurnSetupFn = std::function<Status(const TurnCtx& turn, TurnSetup& out)>;

// Lane E2-P-FO (router design section 9, "Fail-over"): a round that failed on a class engine 1 switches
// on (transport, server, rate limit, a refused key or credit, a cut, an unreadable answer, a refusal of
// this request by this host; never a request unreadable everywhere, a context refusal or a moderation
// block) runs again on another head, at most kMaxFailovers times a turn. The binding names the heads in
// the owner's fallback order (only keyed ones, none parked, paused or over its budget; none at all with
// midTurnFailover off); the engine takes the first whose body budget holds the request it replaces.
inline constexpr uint8_t kMaxFailovers = 2;
struct FailoverAsk {
  std::string provider;            // the head that failed: its slot and model
  std::string model;
  ExchangeErr err = ExchangeErr::None;
  int httpStatus = 0;
};
struct FailoverTarget {
  std::string provider;            // its slot: price, spend, park ("openai")
  std::string host;                // TurnCtx::host (HeadSlot::turnHost)
  std::string model;
  Dialect dialect = Dialect::AnthropicMessages;
  HeadEndpoint head;               // with its own key
  HeadCost cost;                   // its rates (a target with no price row is never named)
  bool seesPhotos = false;         // the owner chose this provider for vision: pixels; else each photo's line
};
using FailoverFn = std::function<void(const FailoverAsk& ask, std::vector<FailoverTarget>& out)>;
// What one head of a switched turn was billed (TurnOutcome::shares), booked to the model that answered.
struct HeadShare {
  std::string provider;
  std::string model;
  uint32_t promptTokens = 0, outputTokens = 0, cacheReadTokens = 0, cacheWriteTokens = 0, cacheWrite1hTokens = 0;
  uint16_t requests = 0;
};
// One delivery (plain/deliver.h); unset: deliverTurn over the channel. A test or a host may
// put its own here; it keeps deliverTurn's contract (progress recorded per send, fromPart).
using DeliverFn = std::function<Status(const DeliverInput& in, DeliverOutcome& out)>;

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
  DeliverFn deliver;                         // null: deliverTurn(DeliverDeps{channel, hooks, log})
  // Ruling 91 blockers/F6 (lane RB-H81): boot refuses, with one log line, when no compaction is
  // installed on the idle seam (HookSlot::Compaction): without it the overflow ladder and the
  // full-arena drop would take old turns with no digest. A binding that compacts no chat on
  // purpose (a test of something else, a rig before its compaction lane) says so here, by name.
  const char* noCompaction = nullptr;
  // Progress the engine itself reports (lane RB-X3, ruling 84 items 2 and 3): a soft guard a
  // progressing attended turn passed (ProgressKind::SoftGuard, its slug as detail) and a request
  // about to leave (ProgressKind::ProviderWait). The binding forwards them to its
  // ProgressReporter (RB-E3's status-line class); null: nothing is reported.
  std::function<void(const ProgressEvent& e)> progress;
  // True when the device has no card fitted at all: an Io from the log's open() then means
  // the chat runs from RAM by design. Null or false: an Io is an outage, the chat is unloaded
  // and reopen() loads it when storage answers (lane E2-STORE).
  std::function<bool()> noCard;
  // True when nothing may be stored for this chat yet (lane E2-FIX2B, finding P3: the history
  // import has not answered Done, tx/migrate.h's contract): boot does not open the log and the
  // chat runs unloaded, as after an Io. Null or false: boot opens the log.
  std::function<bool()> holdUnloaded;
  // Lane E2-CACHE (plain/prefix.h): a person's conversation keeps its system blocks frozen between the
  // listed events (a record beside the transcript, so a chat switch or a restart sends the same bytes)
  // and its stable cache points carry the 1-hour lifetime. False (a routine's own run, the battery by
  // default): the blocks are composed afresh at every turn open and every point has 5 minutes.
  bool stablePrefix = false;
  // Lane E2-P-FO: the heads a failed round may move to (FailoverFn). Null: no fail-over.
  FailoverFn failover;
  // Lane E2-REP1 (review F): the monthly limits inside a turn, as engine 1's wire keeps them. pendingSpend:
  // each answered exchange's usage on its head (slot, model), counted toward the limits at once (the turn's
  // own booking at its end replaces it). paidHeld: asked before each further request of the turn; a line
  // ("" = go on) ends the turn there with that line (StatusLine::PausedSpend), keeping what it already did.
  std::function<void(const std::string& provider, const std::string& model, const nimbus::orch::TokenUsage& u)> pendingSpend;
  std::function<std::string(const std::string& provider)> paidHeld;
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
  // Lane E2-FIX10: the file-store paths the device itself stored for this very message (a
  // Telegram photo, an album's photos, a document, a long paste kept in the inbox), only those
  // whose store succeeded. At the open they count as the turn's own change to the recent-files
  // list (plain/prefix.h): the conversation holds the message that carries them.
  std::vector<std::string> stored;
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
  // statusText as it may be stored or reach a model (the chat's history row): without a provider's own
  // reason, which rides only in the line the person gets (E2-FIX11). Equal to statusText otherwise.
  std::string statusStored;
  bool parkHost = false;         // the head's key or credit was refused: the binding parks it
  int parkStatus = 0;            // that refusal's HTTP status and its body (first 4 KB): the shared park reads
  std::string parkText;          // its kind from them (lane E2-FIX14); never stored, never shown
  bool modelUnusable = false;    // the provider does not know the head model (E2-R8a: the device marks it)
  bool failOver = false;         // routed: the router said fail over to the next target
  // Lane E2-P-FO: rounds moved to another head (at most kMaxFailovers), and then what each head was
  // billed, in order (empty with no switch: the whole turn is the head's). parkHost is the first head's.
  uint8_t switches = 0;
  std::vector<HeadShare> shares;
  // Lane E2-REP1: a fallback head that refused its key or credit, for the binding to park as it parks the
  // head (its slot, its key's fingerprint, the refusal's status and body; never stored, never shown).
  struct FallbackRefusal {
    std::string provider;
    uint32_t keyFp = 0;
    int status = 0;
    std::string text;
  };
  std::vector<FallbackRefusal> fallbackRefusals;
  // Effects the turn's tools asked for after the turn (a stored, successful result each): the
  // binding runs them in Deferred order once runTurn returns, the reply and the turn's items
  // already stored. The mask survives a refused allocation; an argument may not.
  DeferredEffects deferred;
  nimbus::orch::tx::TurnEndFacts end;
};

// Lane E2-P-SURF: the last closed turn, kept for the turn inspector. Not the request (that is a
// reference, plain/deliver.h): the system blocks, the tools and the facts a render needs, so
// PlainEngine::rebuildLast renders the request again from the stored items. RAM only; the head's key
// is never copied into it.
struct LastTurn {
  TurnCtx ctx;                    // `who` is null
  SystemBlocks system;
  std::vector<ToolSpec> tools;
  HeadEndpoint head;              // key empty
  const WireCodec* wire = nullptr;
  nimbus::orch::tx::TurnEndFacts end;
  uint64_t endQ = 0;              // the e item's q
  nimbus::orch::tx::BoundTools bound;
  PhotoFn photo;
  bool longCache = false;
  std::vector<std::string> refusedPhotos;
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
  // The stored conversation could not be loaded at boot (open() answered Io and a card is
  // fitted, EngineDeps::noCard): the chat runs from RAM and nothing is written to the log
  // (lane E2-STORE, finding S5).
  bool unloaded() const;
  // While unloaded, before a turn: open the log again, append every item held in RAM after
  // the stored conversation (in order, renumbered past it, each once), then refill the arena
  // from the log. Not ok: still unloaded, nothing held in RAM given up (the next call resumes
  // after the items already appended). Success and no change when the chat is loaded.
  Status reopen();
  // Lane E2-FIX3 (finding R1): reopen() into another transcript (`log`, under `chatKey`): the RAM
  // items are stored there, in order, each once, and nothing is written to this chat's own log.
  // /clear on a chat held by a history import that cannot finish. After success the engine is
  // done (its log is not `log`): the caller boots a binding on `chatKey`. Not ok: still unloaded,
  // the next call into the same log resumes after the items already stored there.
  Status reopenAs(nimbus::orch::tx::TxLog& log, const std::string& chatKey);

  // Lane E2-P-SURF: the last closed turn since boot (null: none yet), and request `exchange` of it
  // rendered again from the stored items (plain::rebuildRequest: byte for byte, or a status that says why
  // not: NotFound when compaction took items of the range, Torn when a photo's pixels are gone).
  // `alloc` and `release` give the range's own block (PSRAM on the device; empty: malloc). Read and call
  // only where the engine runs its turns: the web side asks the turn task (src/net/web_engine2.cpp).
  const LastTurn* lastTurn() const;
  Status rebuildLast(size_t exchange, HttpRequest& out, nimbus::orch::tx::ArenaAllocFn alloc = nullptr,
                     nimbus::orch::tx::ArenaFreeFn release = nullptr) const;

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
