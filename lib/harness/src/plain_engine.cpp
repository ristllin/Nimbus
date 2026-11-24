// The plain engine (plain/engine.h): the simplest agent loop with durability around it.
// Lane RB-E1a; RB-E1b is the second and last holder (guards scaled to the head,
// suppression, deferred effects).
#include "nimbus/harness/plain/engine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "nimbus/harness/channel.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/prefix.h"   // the frozen system part (lane E2-CACHE)
#include "nimbus/harness/plain/rails.h"   // the item taint rule (RB-RL)
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/platform.h"
#include "nimbus/orch/mcp_client.h"   // serverOf (E2-REP2)
#include "nimbus/orch/provider_park.h"   // parkKeyFingerprint: a fallback head's refusal (E2-REP1)
#include "nimbus/orch/path_rules.h"   // storePathFor: an own path as the store lists it (E2-FIX8)
#include "nimbus/orch/psram_alloc.h"   // WorkingVector: the argument walks keep their list off the stack
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/rules.h"
#include "providers/image_parts.h"

namespace agent {
namespace plain {

namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;

// The chat between turns: what the log and the arena do not say by themselves.
struct PlainEngine::ChatState {
  tx::HeadState head;
  std::vector<tx::DigestEntry> live;   // the digests in force (loaded at boot)
  tx::RecentFilesBlock recent;
  uint64_t nextQ = 1;                  // the next item's q (also given to items not stored)
  uint64_t lastClosedQ = 0;            // the last item of the last closed turn (its e item)
  // Overflow ladder rung 1's line, kept from turn to turn (lane E2-FIX9): the closed bulk at or below it renders as
  // stubs until a compaction stores (or a restart), so the turns after it share one cached prefix; it moves only
  // when a request is over its cap again, and then down to kLadderLowPercent of the cap.
  uint64_t ladderLine = 0;
  uint32_t lastTurn = 0;
  std::unique_ptr<LastTurn> last;      // the last closed turn, for the turn inspector (lane E2-P-SURF)
  bool booted = false;
  bool afterRestart = true;            // the first turn after boot opens cold
  bool storageEpisode = false;         // storage refused an item and has stored none since
  bool noteDue = false;                // a stored gap note is still owed for the last episode
  bool noteShown = false;              // the model has seen the note in this episode (RAM at least)
  uint64_t droppedThru = 0;            // the newest q given up from RAM (the restart fill, a full arena)
  // A full arena (ruling 60): the oldest closed turns left RAM (storage keeps them). The
  // omission line names them until a digest covers them.
  uint32_t droppedTurns = 0;
  uint64_t droppedFirstAt = 0, droppedLastAt = 0;   // the first item of the first and last dropped turn
  uint32_t lastDroppedTurn = 0;        // the newest turn a drop reached (counted once)
  // The memory line, built at boot: on a refused allocation it goes out with no allocation.
  std::string memoryLine;
  // The stored conversation could not be loaded (lane E2-STORE, finding S5): open() answered Io
  // on a device with a card fitted. The chat runs from RAM, the log stays closed, and reopen()
  // stores the RAM items after the stored conversation once the card answers. mergedThru is
  // the newest RAM q already appended; qShift and turnShift move RAM numbers past the stored
  // ones (set at the first append, kept across retries so a partial merge resumes in order).
  bool unloaded = false;
  // The frozen system part (lane E2-CACHE, EngineDeps::stablePrefix): what the requests send and what
  // was last seen of its parts; its record beside the transcript. prefixStore: Untried (not read yet),
  // Known (read, or listed as absent: it may be written), Unread (the card did not answer: never
  // written over, read again at the next turn open). prefixDirty: RAM is ahead of the record.
  FrozenPrefix prefix;
  enum class PrefixStore : uint8_t { Untried = 0, Known = 1, Unread = 2 } prefixStore = PrefixStore::Untried;
  bool prefixDirty = false;
  bool shiftSet = false;
  uint64_t mergedThru = 0;
  uint64_t qShift = 0;
  uint32_t turnShift = 0;
};

namespace {

uint32_t fnv1a(uint32_t h, const std::string& s) {
  for (unsigned char ch : s) h = (h ^ ch) * 16777619u;
  return h;
}

// The model the provider named last (E2-P-SURF); an answer that names none keeps the one before.
void keepServed(std::string& kept, const std::string& named) {
  if (!named.empty()) kept = named;
}

// One turn's state. It lives on the heap so runTurn's frame stays small.
struct Turn {
  TurnCtx ctx;
  TurnSetup setup;
  SystemBlocks system;
  std::vector<ToolSpec> tools;
  const WireCodec* wire = nullptr;
  uint32_t bodyCap = 0;                // the head's body cap (budget()); 0 only before it runs
  uint32_t messageCap = tx::kInlineMessageCap;   // text kept inline (tx::HeadBudget::messageCap)
  uint32_t bulkWholeCap = tx::kBulkWholeMax;     // tier 2 whole in its own turn (tx::HeadBudget::bulkWholeCap)
  GuardLimits limits;
  GuardProgress progress;              // progressing or a loop (ruling 84 item 2; lane RB-X3)
  LadderState ladder;
  RenderInput render;
  ExchangeResult ex;
  int16_t tz = 0;
  uint32_t startMs = 0;
  uint16_t rounds = 0;                 // answered requests
  uint16_t toolRounds = 0;             // rounds whose calls ran (the round guard counts these)
  std::vector<uint64_t> roundStart;    // the first q of each answered round (ladder rung 2)
  bool lastRound = false;              // a guard tripped: this request goes with tool choice none
  std::vector<std::string> photosShown;   // the open turn's visible photo refs (its t item; E2-P-PHOTO)
  bool photoRefusalResent = false;     // the round was re-sent once without photos a provider refused
  bool held = false;                   // beforeDeliver held the reply once
  std::string heldText;                // the held reply: delivered if the extra round fails
  std::string interim;                 // text the model wrote beside its calls this turn (RB-H81)
  std::vector<uint64_t> callQ;         // the q of each call of the round being run (pairing)
  uint64_t curCallQ = 0;               // the q of the call whose result is being stored
  bool storeFailed = false;            // the arena refused an item: the turn ends
  bool storageLineDue = false;
  bool fileSetChanged = false;
  bool sawOutside = false;
  bool sawMeta = false;
  bool longArgs = false;               // a call kept a long argument whole: its stub shows from the close
  // Lane E2-FIX6: what this turn's own calls did, for the frozen system part (seeOwnChanges). notStored:
  // an item of this turn did not reach the card (its own changes are not taken as held by the
  // conversation). ownTexts: the successful, stored results (short ones: a note's stored line);
  // ownPaths: the paths their calls named; ownFromQ: the turn's first item.
  bool notStored = false;
  std::vector<std::string> ownTexts;
  std::vector<std::string> ownPaths;
  bool callRan = false;                // the call being stored ran here (a hook's answer never ran it: not own)
  // Lane E2-FIX10: the paths the device stored for the message that opens this turn (TurnInput::stored),
  // kept only once that message is stored; taken as the turn's own at the open, once.
  std::vector<std::string> inbound;
  bool inboundSeen = false;
  bool inboundMoved = false;           // the open moved the seen list: undone at the close if an item was refused
  std::string inboundSeenBefore;       // the seen list before that move
  uint64_t inboundOwnFromBefore = 0;
  uint8_t compactAsks = 0;             // compaction first, asked once a turn per urgency (lane RB-C3)
  uint32_t sizeTokens = 0;             // the size guard's input: request sizes summed, cache included
  DeferredEffects deferred;            // effects to run after the turn (TurnOutcome::deferred)
  GuardTrip trip = GuardTrip::None;
  bool reply = false;                  // replyText is the model's reply
  bool answered = false;               // a final answer came back (possibly empty)
  bool emptyRefusal = false;           // that answer was an empty refusal
  // The final answer's text, moved out of the exchange before anything is stored: the reply
  // is never copied on its way to the owner (ruling 59 item 1).
  std::string answer;
  const std::string* replyText = nullptr;   // what goes out: answer, else the held reply
  DeliverOutcome out;                  // delivery progress, recorded at each send
  bool deliverDone = false;            // delivery finished
  bool dAttempted = false;             // the d item was tried (never twice)
  bool afterRan = false;               // afterDeliver ran (once, whatever the d item did)
  bool endStored = false;              // the e item is stored
  bool endAttempted = false;           // the e item was tried (never twice)
  uint64_t firstQ = 0;                 // the open turn's first own item (a full arena never drops it)
  Err storeErr = Err::None;            // why the arena refused an item (storeFailed)
  const char* storeWhere = "";
  tx::ItemKind refusedKind = tx::ItemKind::User;
  StatusLine status = StatusLine::None;
  std::string statusText;
  std::string statusStored;            // statusText as it may be kept (no provider words); empty: the same
  const std::string* statusRef = nullptr;   // the line that goes out: statusText or a kept line
  // The line as it may be stored or reach a model (TurnOutcome::statusStored); statusRef set.
  const std::string& storedLine() const { return statusStored.empty() || statusRef != &statusText ? *statusRef : statusStored; }
  bool parkHost = false, failOver = false;
  bool modelUnusable = false;          // the provider does not know the model (E2-R8a: the store marks it)
  // Lane E2-P-FO: rounds moved to another head, the heads tried (slot/model), the answering head's slot
  // (set at the first switch), the person's line per switch, what each head left was billed, and the
  // turn's totals when the head now answering took over (its share is the rest).
  uint8_t switches = 0;
  std::vector<std::string> tried;
  std::string slot;
  std::string switchLines;
  std::vector<HeadShare> shares;
  HeadShare base;
  int parkStatus = 0;                  // the park's refusal: its status and body (E2-FIX14)
  std::string parkText;
  std::vector<TurnOutcome::FallbackRefusal> fallbackRefusals;   // E2-REP1
  std::string cause;
  tx::TurnEndFacts end;
  std::vector<tx::Item> writes;        // c, r pairs of successful effect calls (supersede rule)
  // Scratch items, reused: an Item is about 0.5 KB, too big for the task stack's frames.
  tx::Item item;                       // the item being built and stored
  tx::Item read;                       // an item decoded from the arena
  tx::StubFacts stub;
  std::string again;                   // the long arguments' way back (tx::wayBack), while tierArgs runs
};

class TurnRun {
 public:
  TurnRun(const EngineDeps& d, PlainEngine::ChatState& c, Turn& t) : d_(d), c_(c), t_(t) {}

  // The loop. Everything around it is durability: each answer and each result is stored
  // before the next request leaves. Kept out of line: runTurn's own frame stays small.
  __attribute__((noinline)) Status run(const TurnInput& in) {
    Status st = open(in);
    while (st.ok() && !t_.storeFailed) {
      if ((st = boundary()).ok()) st = ask();                              // render, post, read
      if (!st.ok() || t_.ex.err != ExchangeErr::None) break;               // failed or cut: ended
      const ParsedStep& step = t_.ex.step;
      if (step.outside) t_.sawOutside = t_.ctx.tainted = true;            // a provider-side connector answered
      if (step.calls.empty() || t_.lastRound) {                            // no call: the text is the reply
        if (!(st = finalAnswer(step)).ok() || t_.answered) break;
        continue;                                                          // held once: one more round
      }
      if (!step.text.empty()) keepInterim(step.text);                      // delivered if no reply follows
      if ((st = storeAnswer(step, false)).ok()) st = runCalls(step);       // each result stored in turn
    }
    const Status closed = close();
    return st.ok() ? closed : st;
  }

 private:
  // Text the model already wrote in this turn beside its calls: when the last round answers with
  // nothing, it is the reply, never lost behind a provider line (RB-W1b's report, lane RB-H81).
  void keepInterim(const std::string& text) {
    t_.progress.noteText(text);   // new text is progress (RB-X3)
    if (!t_.interim.empty()) t_.interim += "\n\n";
    t_.interim += text;
  }

  // ---- storage --------------------------------------------------------------------------
  uint64_t epoch() const { return d_.platform->nowEpoch ? d_.platform->nowEpoch() : 0; }
  uint32_t elapsedMs() const { return d_.platform->nowMs() - t_.startMs; }
  void event(const std::string& e) { t_.end.events.push_back(e); }
  // A scratch value emptied in place: no temporary on the stack.
  template <class T>
  static T& renew(T& v) {
    v.~T();
    return *new (&v) T();
  }
  // The scratch item as a fresh item of `kind`.
  tx::Item& blank(tx::ItemKind kind) {
    renew(t_.item).kind = kind;
    return t_.item;
  }

  // Store one item: it takes the next q; an item the card refused stays in RAM and the
  // episode is named once (the model at the next round boundary, the owner at the end).
  Status put(tx::Item& it, bool* stored = nullptr) {
    it.q = c_.nextQ++;
    if (!it.turn) it.turn = t_.ctx.turn;
    it.t = epoch();
    tx::StoreState state = tx::StoreState::Stored;
    Status st = d_.log->store(it, *d_.arena, state);
    while (arenaFull(st) && (dropOldestClosed() || shedReference(it))) st = d_.log->store(it, *d_.arena, state);   // ruling 60; RB-E3
    if (!st.ok()) {
      t_.storeFailed = true;
      t_.storeErr = st.err;
      t_.storeWhere = st.where;
      t_.refusedKind = it.kind;
      return st;
    }
    const bool ok = state == tx::StoreState::Stored;
    if (stored) *stored = ok;
    if (!ok) t_.notStored = true;
    if (!ok && !c_.storageEpisode) {
      c_.storageEpisode = c_.noteDue = t_.storageLineDue = true;
      c_.noteShown = false;
      event("not-stored");
    }
    if (ok) c_.storageEpisode = false;
    return Status::success();
  }

  Status note(const std::string& text, tx::NoteClass cls, bool* stored = nullptr) {
    tx::Item& h = blank(tx::ItemKind::DeviceNote);
    h.x = text;
    h.note = cls;
    return put(h, stored);
  }

  // ---- the full arena (ruling 60) -------------------------------------------------------
  // The loop never calls the summarizer to make room. At turn open (free bytes under the turn's
  // reserve) and between rounds (the arena refused a store) the oldest closed resident turn
  // leaves RAM; storage keeps it, the omission line names it, omittedThru rises to its last q,
  // and the compaction driver (idle step only) folds it from storage later.
  static constexpr size_t kTurnArenaReserve = 32 * 1024;
  size_t arenaReserve() const { return std::min(kTurnArenaReserve, d_.arena->capacity() / 2); }
  static bool arenaFull(const Status& st) {
    return st.err == Err::TooLarge && std::strcmp(st.where, "tx::TxArena::append") == 0;
  }

  // The prefix through the oldest turn's last item, and through any later item of a turn at or
  // below it (a late "outcome unknown" result): no result is left without its call. Never an
  // item of the open turn (q at or above firstQ; before the turn stores anything, every resident
  // item is closed). False when nothing closed is resident.
  bool dropOldestClosed() {
    if (c_.unloaded) return false;   // RAM is the only copy: nothing leaves it (ruling 81)
    uint32_t through = 0;
    const size_t end = dropEnd(closedPrefix(through), through);
    uint64_t lastQ = 0;
    tx::ArenaEntry e;
    if (!end || !d_.arena->header(end - 1, e).ok() || !(lastQ = e.q)) return false;
    countDropped(end);   // before the drop: it reads the items' headers
    if (!d_.arena->dropThrough(lastQ).ok()) return false;
    c_.droppedThru = std::max(c_.droppedThru, lastQ);
    nameDropped();
    return true;
  }
  // How many resident items belong to closed turns (a prefix), and the oldest turn's number.
  size_t closedPrefix(uint32_t& oldest) const {
    const uint64_t keepFrom = t_.firstQ ? t_.firstQ : c_.nextQ;
    size_t n = 0;
    tx::ArenaEntry e;
    for (; n < d_.arena->count() && d_.arena->header(n, e).ok() && e.q < keepFrom; ++n)
      if (!oldest) oldest = e.turn;
    return n;
  }
  // The drop's end within the closed prefix: through every item of the turns it covers, growing
  // while a covered item belongs to a newer turn.
  size_t dropEnd(size_t n, uint32_t& through) const {
    size_t end = 0;
    tx::ArenaEntry e;
    for (bool grew = n != 0; grew;) {
      grew = false;
      for (size_t i = end; i < n; ++i)
        if (d_.arena->header(i, e).ok() && (!through || (e.turn && e.turn <= through))) end = i + 1;
      for (size_t i = 0; i < end; ++i) {
        if (!d_.arena->header(i, e).ok() || e.turn <= through) continue;
        through = e.turn;
        grew = true;
      }
    }
    return end;
  }
  // The dropped turns' count and dates, for the omission line.
  void countDropped(size_t end) {
    tx::ArenaEntry e;
    for (size_t i = 0; i < end; ++i) {
      if (!d_.arena->header(i, e).ok() || !e.turn || e.turn == c_.lastDroppedTurn) continue;
      c_.lastDroppedTurn = e.turn;   // kept across drops: a turn left in two drops counts once
      ++c_.droppedTurns;
      if (!c_.droppedFirstAt) c_.droppedFirstAt = e.t;
      c_.droppedLastAt = e.t;
    }
  }

  // The ladder starts from the dropped turns: whole turns through droppedThru are left out (they
  // are not resident) and the omission line names them; rung 3 adds resident turns to it.
  void nameDropped() {
    if (c_.droppedTurns) (void)omit(std::max(t_.ladder.omitThrough, c_.droppedThru));
  }

  // One omission line for all that is left out through `through`: the dropped turns (no longer
  // resident) and the resident whole turns at or below it.
  bool omit(uint64_t through) {
    uint32_t n = c_.droppedTurns, last = 0;
    uint64_t first = n ? c_.droppedFirstAt : 0, lastAt = c_.droppedLastAt;
    tx::ArenaEntry e;
    for (size_t i = 0; i < d_.arena->count() && d_.arena->header(i, e).ok() && e.q <= through; ++i) {
      if (!e.turn || e.turn == last) continue;
      last = e.turn;
      ++n;
      if (!first) first = e.t;
      lastAt = e.t;
    }
    std::string dates;
    (void)tx::omissionDates(first, lastAt, t_.tz, dates);
    if (!n || !tx::omissionLine(n, dates, t_.ladder.omissionLine, bound()).ok()) return false;
    t_.ladder.omitThrough = through;
    return true;
  }

  // At turn open: what earlier drops left out is named, and room is made for this turn.
  void makeRoom() {
    if (c_.droppedTurns && c_.head.thru >= c_.droppedThru) {   // a digest names them now
      c_.droppedTurns = 0;
      c_.droppedFirstAt = c_.droppedLastAt = 0;
    }
    nameDropped();
    while (d_.arena->freeBytes() < arenaReserve() && dropOldestClosed()) {
    }
  }

  // ---- turn open ----------------------------------------------------------------------
  Status open(const TurnInput& in) {
    t_.ctx = in.ctx;
    if (!t_.ctx.turn) t_.ctx.turn = c_.lastTurn + 1;
    c_.lastTurn = std::max(c_.lastTurn, t_.ctx.turn);
    t_.ctx.startedAt = epoch();
    t_.ctx.startedMs = t_.startMs = d_.platform->nowMs();
    t_.tz = in.tzMinutes;
    // A resumed run's turn is open from its first item (its owner message, calls and results of the
    // run a cut stopped): a drop to make room never takes them (lane RB-X1).
    const uint64_t own = t_.ctx.trigger == Trigger::Resume ? firstQOfTurn(t_.ctx.turn) : 0;
    t_.firstQ = own;
    Status st = closeDangling();   // first: a drop never takes a call before its late result is stored
    t_.firstQ = own;
    if (st.ok()) makeRoom();
    t_.firstQ = own ? own : c_.nextQ;
    if (st.ok()) st = storeInput(in);
    if (!st.ok()) return st;
    if (!d_.setup) return fail(Status::fail(Err::InvalidArgument, "plain::PlainEngine::runTurn.setup"));
    if (!(st = d_.setup(t_.ctx, t_.setup)).ok() || !(st = sized()).ok()) return fail(st);
    if (!(st = storeOpen(in)).ok() || !(st = openChanges()).ok()) return st;
    if (!(st = compose()).ok() || !(st = wired()).ok()) return fail(st);
    clampLimits();
    return Status::success();
  }

  // Whatever the scale, every round has a bound and the last round has time before the hard wall.
  // An attended turn (a person's message started it) has the soft and hard guards; any other keeps
  // today's limits (ruling 84 items 2 and 9).
  void clampLimits() {
    const bool attended = personTrigger(t_.ctx.trigger);
    if (!guardLimitsFor(t_.setup.storedRounds, t_.setup.cost, attended, t_.limits).ok()) t_.limits = GuardLimits{};
    GuardLimits& l = t_.limits;
    // An unattended turn's own bounds (RB-U1a; a routine's ceiling), applied over the head's limits;
    // the clamps below only bound them further.
    applyCeiling(t_.setup.ceiling, l, l.lastRoundWallMs);
    l.lastRoundWallMs = std::max<uint32_t>(std::min<uint32_t>(l.lastRoundWallMs, attended ? kAttendedHardWallMs : kHardWallMs),
                                           3 * kMinRequestMs);
    l.rounds = std::max<uint16_t>(1, std::min<uint16_t>(l.rounds, kMaxRounds));
    l.hardRounds = std::max<uint16_t>(l.rounds, std::min<uint16_t>(l.hardRounds, kHardRounds));
    l.timeLimitMs = std::min<uint32_t>(l.timeLimitMs, l.lastRoundWallMs - 2 * kMinRequestMs);
    l.softWallMs = std::min<uint32_t>(l.softWallMs, l.timeLimitMs);
  }

  // A turn that cannot start (setup, composition or the wire refused) is closed with one
  // plain line: a binding fault, never a partial request.
  Status fail(const Status& st) {
    t_.cause = std::string("setup:") + tx::errSlug(st.err);
    setStatus(StatusLine::ProviderError, exchangeLine(ExchangeErr::Other, providerSlugOf(t_.ctx, t_.setup.head), 0));
    return st;
  }

  // A call stored with no result (the turn stopped while it ran: a cut, a refused
  // allocation) gets an "outcome unknown" result before anything else of this turn, so every
  // request stays paired; it is never run again.
  Status closeDangling() {
    std::vector<tx::Item> open;
    tx::Item& it = t_.read;
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok()) continue;
      if (e.kind != tx::ItemKind::Call && e.kind != tx::ItemKind::Result) continue;
      if (!d_.arena->get(i, it).ok()) continue;
      if (it.kind == tx::ItemKind::Call) open.push_back(it);
      else
        for (size_t k = 0; k < open.size(); ++k)   // the oldest open call of that id, as the wire pairs
          if (open[k].id == it.id) {
            open.erase(open.begin() + (long)k);
            break;
          }
    }
    // A full arena may drop closed turns while these results are stored, never a call still
    // waiting for its result: the oldest dangling call bounds the drop until all are answered.
    if (!open.empty()) t_.firstQ = firstQOfTurn(open.front().turn);
    for (const tx::Item& call : open) {
      tx::Item& r = blank(tx::ItemKind::Result);
      r.turn = call.turn;
      r.id = call.id;
      r.n = call.n;
      r.ok = false;
      r.x = texts::tool_results::kOutcomeUnknown;
      Status st = capture(r, r.x, nullptr);
      if (!st.ok()) return st;
    }
    t_.firstQ = 0;
    return Status::success();
  }

  // The first resident item of a turn: a drop bounded by a dangling call keeps that call's whole turn,
  // so a round of parallel calls is never split (ruling 79 item 4).
  uint64_t firstQOfTurn(uint32_t turn) const {
    tx::ArenaEntry e;
    for (size_t i = 0; i < d_.arena->count(); ++i)
      if (d_.arena->header(i, e).ok() && e.turn == turn) return e.q;
    return c_.nextQ;
  }

  // An outside item stored since the last closed turn (a routine's reply after outside content,
  // appended between turns): this turn is the first whose request carries it, so it is tainted, as
  // the turn a sub-agent's report arrives in is (lane RB-RL: plain/rails.h itemOutside).
  bool outsideSinceLastTurn() const {
    tx::Item& it = t_.read;
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok() || e.q <= c_.lastClosedQ || e.kind != tx::ItemKind::DeviceNote) continue;
      if (d_.arena->get(i, it).ok() && itemOutside(it)) return true;
    }
    return false;
  }

  // The owner message (u) before any model call; another trigger's text as its device note.
  Status storeInput(const TurnInput& in) {
    t_.ctx.tainted = t_.ctx.tainted || inputOutside(t_.ctx.trigger, in.parts) || outsideSinceLastTurn();   // RB-RL
    if (in.text.empty() && in.parts.empty()) return Status::success();
    const Trigger tr = t_.ctx.trigger;
    if (tr == Trigger::Routine) return note(in.text, tx::NoteClass::Routine);
    if (tr == Trigger::AgentResult) return note(in.text, tx::NoteClass::Agent);
    if (tr == Trigger::Notice) return note(in.text, tx::NoteClass::Notice);
    tx::Item& u = blank(tx::ItemKind::User);
    u.x = in.text;
    u.tzMinutes = in.tzMinutes;
    u.ch = in.ch;
    u.src = in.src;
    u.m = in.m;
    u.parts = in.parts;
    bool stored = false;
    const Status st = put(u, &stored);
    if (!st.ok()) return st;
    if (stored) t_.inbound = in.stored;   // a message the card refused carries nothing in (ruling 116)
    for (uint64_t s : in.src) c_.head.hw = std::max(c_.head.hw, s);
    c_.head.mHi = std::max(c_.head.mHi, in.m);
    return st;
  }

  // The head's budget (tx::budgetFor). The head is what the setup configured: its provider slug, else
  // a known API host, else its dialect (one log line: lane RB-T4b). A turn whose setup names no host
  // or no model is a binding fault, refused loudly (rulings 89 item 3 and 91), never sized as a custom
  // floor head. budgetFor fails only on such a key, so there is no other fallback.
  // The turn's wire: the setup's, else the dialect's.
  Status wired() {
    if (t_.wire) return Status::success();
    const Status st = wireFor(t_.ctx.dialect, t_.wire);
    if (!st.ok()) return st;
    return t_.wire ? Status::success() : Status::fail(Err::NotFound, "plain::wireFor");
  }

  // After the setup: the day start the wires stamp every message with, then the head's budget, then
  // compaction first when it is due (rulings 98 item 1, 105 item 3, 106 item 3; lane RB-C3): before the
  // system text is composed, so the request carries the new summary.
  Status sized() {
    t_.ctx.dayStartMin = t_.setup.dayStartMin;
    t_.ladder.closedThrough = c_.ladderLine;   // rung 1's line from the turns before (lane E2-FIX9)
    const Status st = budget();
    if (st.ok()) (void)compactFirst(Urgency::IfDue);
    return st;
  }

  // Compaction first: the chat's compaction slot asked by the engine itself (never another idle slot),
  // at most once a turn. IfDue at turn open (a summary prepared ahead is swapped in with no wait), Now
  // when a request does not fit; a wait for a request shows the status line. True when thru moved.
  bool compactFirst(Urgency urgency) {
    const uint8_t bit = (uint8_t)(1u << (uint8_t)urgency);
    if (!d_.hooks || (t_.compactAsks & bit)) return false;
    t_.compactAsks |= bit;
    IdleCtx ctx;
    ctx.now = epoch();
    ctx.nowMs = d_.platform->nowMs();
    ctx.tzMinutes = t_.tz;
    ctx.clockSet = ctx.now != 0;
    ctx.urgency = urgency;
    ctx.waiting = [this] {
      event("wait:summary");
      report(ProgressKind::Summary, "");
    };
    if (!d_.hooks->runIdleSlot(HookSlot::Compaction, ctx).worked) return false;
    c_.ladderLine = t_.ladder.closedThrough = 0;   // a new summary: the request is rebuilt, nothing stays stubbed for it
    // The summary's s item took a q past everything stored: this turn's next items go after it.
    c_.nextQ = std::max({c_.nextQ, c_.head.nextQ, d_.arena->lastQ() + 1});
    event("change:compaction");
    event(urgency == Urgency::IfDue ? "compact:open" : "compact:turn");   // where it ran (lane RB-C3)
    return true;
  }

  Status budget() {
    bool byDialect = false;
    tx::HeadKey key;
    key.host = headSlugOf(t_.ctx, t_.setup.head, &byDialect);
    key.model = t_.ctx.model;
    key.routed = t_.setup.head.routed;
    tx::HeadBudget b;
    if (key.host.empty() || key.model.empty() || !tx::budgetFor(key, t_.ctx.profile, b).ok()) {
      agent::hlog::logf("plain(%s): no head named for the turn: refused", t_.ctx.chatKey.c_str());
      return Status::fail(Err::InvalidArgument, "plain::PlainEngine::budget.head");
    }
    if (byDialect)
      agent::hlog::logf("plain(%s): host %s is not a known provider: sized by its dialect as %s", t_.ctx.chatKey.c_str(),
                        (t_.setup.head.host.empty() ? t_.ctx.host : t_.setup.head.host).c_str(), key.host.c_str());
    t_.bodyCap = t_.setup.bodyCapLimit ? std::min(b.bodyCap, t_.setup.bodyCapLimit) : b.bodyCap;
    t_.messageCap = std::max<uint32_t>(b.messageCap, tx::kInlineMessageCap);
    if (b.bulkWholeCap) t_.bulkWholeCap = b.bulkWholeCap;
    t_.ctx.profile = b.profile;
    return Status::success();
  }

  // A photo part as a candidate, `since` person turns after it arrived (each ref once).
  static void photoCandidate(tx::PhotoFacts& f, const tx::Part& p, uint32_t since) {
    if (p.kind != tx::PartKind::Image || std::find(f.candidates.begin(), f.candidates.end(), p.ref) != f.candidates.end())
      return;
    f.candidates.push_back(p.ref);
    f.ownerTurnsSince.push_back(since);
    f.bytes.push_back(p.bytes);
  }

  // The earlier messages' photos the request shows (resident, after compaction's pointer, as the
  // wire renders them), newest first, until the caps could take no more (lane E2-FIX4). Only user
  // items are read; a resumed turn's own stored message counts as this turn's (since 0).
  void earlierPhotos(tx::PhotoFacts& f) const {
    const size_t enough = f.candidates.size() + tx::photosInViewMax(f.mode);
    uint64_t thru = c_.head.thru;
    for (size_t i = d_.arena->count(); i-- > 0 && f.candidates.size() < enough;) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok()) continue;
      const bool digest = e.kind == tx::ItemKind::Digest;
      if (!digest && e.kind != tx::ItemKind::User) continue;
      if (!digest && e.q <= thru) break;   // a digest covers it and everything before it
      tx::Item it;
      if (!d_.arena->get(i, it).ok()) continue;
      if (digest) thru = std::max(thru, it.thru);
      else
        for (const tx::Part& p : it.parts) photoCandidate(f, p, e.turn == t_.ctx.turn ? 0 : 1);
    }
  }

  // The photos this turn shows as pixels (tx::visiblePhotos, PRD section 5): its own, and those of
  // the earlier person turns still in the conversation the request shows, newest first, within the
  // caps (Inline: base64, no provider file ids yet). No clock and no restart take part (lane
  // E2-FIX4). Any other photo renders as its line.
  std::vector<std::string> photosAtOpen(const TurnInput& in) const {
    tx::PhotoFacts f;
    f.noCard = !d_.log->isOpen();
    if (t_.ctx.photosBlind) return {};   // E2-P-PHOTO: a head without image input sees none as pixels
    for (const tx::Part& p : in.parts) photoCandidate(f, p, 0);
    earlierPhotos(f);
    // E2-P-PHOTO: a photo a provider refused takes no place in view.
    const std::vector<std::string>& no = c_.head.refusedPhotos;
    for (size_t i = f.candidates.size(); i-- > 0;)
      if (std::find(no.begin(), no.end(), f.candidates[i]) != no.end()) {
        f.candidates.erase(f.candidates.begin() + (long)i);
        f.ownerTurnsSince.erase(f.ownerTurnsSince.begin() + (long)i);
        f.bytes.erase(f.bytes.begin() + (long)i);
      }
    std::vector<std::string> out;
    if (tx::visiblePhotos(f, out).ok()) return out;
    out.clear();
    for (const tx::Part& p : in.parts)
      if (p.kind == tx::PartKind::Image) out.push_back(p.ref);
    return out;
  }

  Status storeOpen(const TurnInput& in) {
    tx::Item& t = blank(tx::ItemKind::TurnOpen);
    t.trigger = triggerSlug(t_.ctx.trigger);
    t.head = t_.ctx.host + "/" + t_.ctx.model;
    t.profile = tx::headProfileSlug(t_.ctx.profile);   // a smaller profile is the owner's pin (RB-H81)
    t.attempt = t_.ctx.attempt;
    t.photos = photosAtOpen(in);
    t_.photosShown = t.photos;
    c_.head.openTurn = t_.ctx.turn;
    c_.head.attempts = t_.ctx.attempt;
    c_.head.profile = t.profile;
    return put(t);
  }

  bool cold() const {
    const uint64_t now = epoch(), last = c_.head.lastReqAt;
    return c_.afterRestart || now == 0 || last == 0 || now < last || now - last > tx::kColdGapS;
  }

  // The turn open's change points, stored before the first request. The stub pointer does not move
  // here (lane E2-VERB, rulings 81, 82 and 106): only the overflow path makes stubs.
  Status openChanges() {
    const bool wasCold = cold();
    c_.afterRestart = false;
    if (d_.recentFiles && !d_.stablePrefix) {   // stablePrefix: compose() reads the list itself
      const std::string before = c_.recent.text();
      (void)c_.recent.atOpen(wasCold, d_.recentFiles, t_.tz);
      if (c_.recent.text() != before) event("change:recent-files");
    }
    return Status::success();
  }

  // The system text and the tools, composed once and kept for every round of the turn.
  Status compose() {
    RequestFacts& f = t_.setup.facts;
    f.turn = &t_.ctx;
    std::vector<tx::DigestEntry> archive;
    f.digestBlock.clear();
    if (!c_.live.empty()) (void)tx::digestBlock(c_.live, t_.ctx.profile, f.digestBlock, archive);
    if (d_.stablePrefix) loadFrozen();   // first: an unanswered file list falls back to what it saw
    f.recentFiles = d_.stablePrefix ? filesNow() : c_.recent.text();
    Status st = composeSystem(f, t_.system);
    if (st.ok()) st = toolManifest(f, t_.tools);
    if (!st.ok()) return st;
    t_.wire = t_.setup.wire;
    uint32_t h = 2166136261u;
    for (const ToolSpec& s : t_.tools) h = fnv1a(fnv1a(fnv1a(h, s.name), s.description), s.schemaJson);
    t_.end.manifestHash = h;
    if (d_.stablePrefix) freeze(f, h);
    t_.end.promptHash = fnv1a(fnv1a(fnv1a(2166136261u, t_.system.core), t_.system.notes), t_.system.digests);
    return Status::success();
  }

  // ---- the frozen system part (lane E2-CACHE, plain/prefix.h) -------------------------
  // The recent-files list as the store holds it now ("" with no list). A store that does not answer is
  // never an empty list: the list last seen stands for it (ruling 116 item 2).
  std::string filesNow() const {
    if (!d_.recentFiles) return std::string();
    tx::RecentFilesBlock b;
    if (!b.atOpen(true, d_.recentFiles, t_.tz).ok()) return c_.prefix.valid ? c_.prefix.seenFiles : c_.recent.text();
    return b.text();
  }
  PrefixNow prefixNow(const RequestFacts& f, uint32_t manifest) const {
    PrefixNow n;
    n.blocks = t_.system;
    n.manifestHash = manifest;
    n.digestText = f.digestBlock;
    n.files = f.recentFiles;
    n.stub = c_.head.stub;
    n.dropped = c_.droppedThru;
    n.omitted = std::max(c_.droppedThru, t_.ladder.omitThrough);
    n.now = epoch();
    n.lastReqAt = c_.head.lastReqAt;
    n.lifetimeS = kCacheLongS;
    return n;
  }
  bool prefixStorable() const { return d_.files && d_.log->isOpen() && !c_.unloaded; }
  // The record, read once it can be: a record read back is what was sent before a chat switch or a
  // restart; one that does not decode is a torn write, replaced; an unanswered read is never "absent".
  void loadFrozen() {
    if (c_.prefixStore == PlainEngine::ChatState::PrefixStore::Known || !prefixStorable()) return;
    FrozenPrefix stored;
    const Status st = loadPrefix(*d_.files, d_.chatKey, stored);
    if (st.err == Err::Io) {
      if (c_.prefixStore != PlainEngine::ChatState::PrefixStore::Unread)
        hlog::logf("plain(%s): the prefix record could not be read (%s): not written until it can be", d_.chatKey.c_str(),
                   st.where);
      c_.prefixStore = PlainEngine::ChatState::PrefixStore::Unread;
      return;
    }
    if (st.err == Err::Torn) hlog::logf("plain(%s): the prefix record does not decode: rendered afresh", d_.chatKey.c_str());
    c_.prefixStore = PlainEngine::ChatState::PrefixStore::Known;
    if (c_.prefix.valid) c_.prefixDirty = true;   // RAM went out after it: RAM is what the provider holds
    else if (st.ok()) c_.prefix = std::move(stored);
  }
  void saveFrozen() {
    if (!c_.prefixDirty || c_.prefixStore != PlainEngine::ChatState::PrefixStore::Known || !prefixStorable()) return;
    const Status st = savePrefix(*d_.files, d_.chatKey, c_.prefix);
    if (st.ok()) c_.prefixDirty = false;   // else kept dirty: written again at the next turn open
    else hlog::logf("plain(%s): the prefix record was not written (%s): tried again next turn", d_.chatKey.c_str(), st.where);
  }
  // The turn sends the frozen blocks unless a listed event happened; then they are rendered afresh,
  // named as a change point.
  void freeze(const RequestFacts& f, uint32_t manifest) {
    const PrefixNow now = prefixNow(f, manifest);
    seeInbound(now.files);
    const PrefixRender why = decidePrefix(c_.prefix, now);
    if (why != PrefixRender::None) {
      renderPrefix(c_.prefix, now);
      c_.prefixDirty = true;
      event(std::string("change:") + prefixRenderSlug(why));
    }
    t_.system = c_.prefix.blocks;
    saveFrozen();
  }
  // At the close of a turn that ran calls: a change the turn's own calls made (a pinned note added, a
  // new file) is seen now, so the next open does not take it for an outside change; the blocks stay
  // as sent. Only a pure addition that this turn's own stored results account for is taken
  // (plain/prefix.h, lane E2-FIX6); anything else leaves the seen marks, and the next open renders.
  // Notes changed in a turn whose long arguments show as stubs from now are not taken either: the
  // conversation may not hold the note's words whole (ruling 81).
  void seeOwnChanges() {
    if (!d_.stablePrefix || !c_.prefix.valid || !t_.end.tools || !d_.setup) return;
    if (t_.storeFailed || t_.notStored) {   // ruling 116: what did not land moves nothing
      hlog::logf("plain(%s): an item of this turn was not stored: its own changes render at the next open",
                 d_.chatKey.c_str());
      return;
    }
    std::string notesNow, files;
    if (!notesBlockNow(notesNow)) return;
    const bool notes = ownNotes(notesNow);
    const bool filesOwn = ownFiles(files);
    if (!notes && !filesOwn) return;
    if (notes) c_.prefix.seenNotes = notesNow;
    if (filesOwn) c_.prefix.seenFiles = files;
    if (!c_.prefix.ownFrom || t_.firstQ < c_.prefix.ownFrom) c_.prefix.ownFrom = t_.firstQ;
    c_.prefixDirty = true;
    saveFrozen();
  }
  // Lane E2-FIX10, at the open: the files the device stored for the message that opens this turn are
  // the turn's own (the conversation holds that message), by the same path rule as a call's. The seen
  // list moves and the turn's first item becomes what the blocks lean on (ownFrom); anything else in the
  // list's change still renders. Once a turn, and only when every item so far was stored.
  void seeInbound(const std::string& files) {
    if (t_.inboundSeen) return;   // the turn's first composition only (later ones are mid-turn)
    t_.inboundSeen = true;
    if (!d_.stablePrefix || !c_.prefix.valid || t_.inbound.empty()) return;
    if (t_.storeFailed || t_.notStored || files == c_.prefix.seenFiles) return;
    std::string text;
    std::vector<ListedFile> listing;
    const bool own = filesListing(text, listing) && text == files &&
                     ownFilesChange(c_.prefix.seenFiles, files, listing, t_.inbound, t_.ctx.startedAt);
    if (!own) {
      hlog::logf("plain(%s): the file list changed beyond this message's own files: rendered now", d_.chatKey.c_str());
      return;
    }
    t_.inboundMoved = true;
    t_.inboundSeenBefore = c_.prefix.seenFiles;
    t_.inboundOwnFromBefore = c_.prefix.ownFrom;
    c_.prefix.seenFiles = files;
    if (!c_.prefix.ownFrom || t_.firstQ < c_.prefix.ownFrom) c_.prefix.ownFrom = t_.firstQ;
    c_.prefixDirty = true;
  }
  // At the close: a turn with an item the card refused moves nothing (ruling 116), the open's move
  // included; the next open renders the list afresh.
  void undoInboundIfRefused() {
    if (!t_.inboundMoved || !(t_.storeFailed || t_.notStored)) return;
    c_.prefix.seenFiles = t_.inboundSeenBefore;
    c_.prefix.ownFrom = t_.inboundOwnFromBefore;
    c_.prefixDirty = true;
    saveFrozen();
  }
  // The file list changed only in entries of paths this turn's own calls named (`files`: the list now).
  bool ownFiles(std::string& files) const {
    std::vector<ListedFile> listing;
    if (!filesListing(files, listing) || files == c_.prefix.seenFiles) return false;
    const bool own = ownFilesChange(c_.prefix.seenFiles, files, listing, t_.ownPaths, t_.ctx.startedAt);
    if (!own)
      hlog::logf("plain(%s): the file list changed beyond this turn's own files: rendered at the next open", d_.chatKey.c_str());
    return own;
  }
  // The notes block as the binding composes it now (false when it cannot be read).
  bool notesBlockNow(std::string& out) const {
    std::unique_ptr<TurnSetup> s(new (std::nothrow) TurnSetup());
    if (!s || !d_.setup(t_.ctx, *s).ok()) return false;
    s->facts.turn = &t_.ctx;
    s->facts.digestBlock.clear();
    s->facts.recentFiles.clear();
    SystemBlocks b;
    if (!composeSystem(s->facts, b).ok()) return false;
    out = b.notes;
    return true;
  }
  // The notes changed only by this turn's own additions (and none of its long arguments became a stub).
  bool ownNotes(const std::string& now) const {
    if (now == c_.prefix.seenNotes) return false;
    const bool own = !t_.longArgs && ownNotesAddition(c_.prefix.blocks.notes, c_.prefix.seenNotes, now, t_.ownTexts,
                                                       {texts::core::kNotes, texts::core::kSkills});
    if (!own)
      hlog::logf("plain(%s): the notes changed beyond this turn's own additions: rendered at the next open", d_.chatKey.c_str());
    return own;
  }
  // The recent-files list now and, for each file the store holds, its line, path and write time.
  // False when the store does not answer (nothing is taken as seen then).
  bool filesListing(std::string& text, std::vector<ListedFile>& listing) const {
    text.clear();
    listing.clear();
    if (!d_.recentFiles) return true;
    std::vector<tx::RecentFile> all;
    if (!d_.recentFiles(all)) return false;
    if (!tx::recentFilesList(all, t_.tz, text).ok()) return false;
    std::string one;
    for (const tx::RecentFile& f : all)
      if (tx::recentFilesList({f}, t_.tz, one).ok() && !one.empty()) listing.push_back(ListedFile{one, f.path, f.lastWrite});
    return true;
  }
  // Mid-turn: the ladder or a full arena left out the turn the frozen blocks lean on; the rest of the
  // turn goes with blocks rendered afresh (lane E2-FIX6, plain/prefix.h ownFrom).
  void refreezeIfOmitted() {
    if (!d_.stablePrefix || !c_.prefix.valid || !c_.prefix.ownFrom) return;
    if (std::max(c_.droppedThru, t_.ladder.omitThrough) < c_.prefix.ownFrom) return;
    (void)(compose().ok() && wired().ok());
  }

  // ---- one request ----------------------------------------------------------------------
  // The round boundary: the not-stored note, then the guards (one device item and one last
  // round with tool choice none on a trip).
  Status boundary() {
    t_.ctx.roundNo = t_.rounds;
    refreezeIfOmitted();   // a full arena let the turn the frozen blocks lean on go (E2-FIX6)
    // The gap note: shown once when an episode starts, and owed until one is stored, so the
    // stored transcript names the hole once storage is back.
    if (c_.noteDue && !c_.noteShown) {   // the model sees it once, in RAM if need be
      bool stored = false;
      Status st = note(texts::device_notes::kNotStored, tx::NoteClass::Gap, &stored);
      if (!st.ok()) return st;
      c_.noteShown = true;
      c_.noteDue = !stored;
    } else if (c_.noteDue && !c_.storageEpisode) {   // storage is back: the owed note to storage only
      tx::Item& h = blank(tx::ItemKind::DeviceNote);
      h.x = texts::device_notes::kNotStored;
      h.note = tx::NoteClass::Gap;
      h.q = c_.nextQ++;
      h.turn = t_.ctx.turn;
      h.t = epoch();
      tx::StoreState state = tx::StoreState::NotStored;
      if (d_.log->append(h, state).ok() && state == tx::StoreState::Stored) c_.noteDue = false;
    }
    if (t_.lastRound) return Status::success();
    const GuardTrip trip = guardTrip();
    if (trip == GuardTrip::None) {
      report(ProgressKind::ProviderWait, "");
      return Status::success();
    }
    t_.trip = trip;
    t_.lastRound = true;
    event(std::string("trip:") + guardTripSlug(trip));
    return note(std::string(texts::device_notes::kLimitReached) + guardTripSlug(trip) +
                    texts::device_notes::kLimitReachedEnd,
                tx::NoteClass::Limit);
  }

  GuardState guardState() const {
    GuardState g;
    g.rounds = t_.toolRounds;
    g.elapsedMs = elapsedMs();
    g.promptTokens = t_.sizeTokens;
    g.spendMicroUsd = t_.end.microUsd;
    g.idleRounds = t_.progress.idle;
    return g;
  }
  // The hard guards and the loop first; then, on an attended turn, the soft check: a progressing
  // turn shows a status line and goes on, one that is not stops at the soft guard it reached.
  GuardTrip guardTrip() {
    const GuardState g = guardState();
    GuardTrip trip = GuardTrip::None;
    (void)checkGuards(t_.limits, g, trip);   // the limits are clamped at open: never InvalidArgument
    if (trip != GuardTrip::None) return trip;
    GuardTrip soft = GuardTrip::None;
    const SoftCheck c = checkSoftGuards(t_.limits, g, t_.progress, soft);
    if (c == SoftCheck::Stop) return soft;
    if (c == SoftCheck::GoOn) {
      event(std::string("soft:") + guardTripSlug(soft));
      report(ProgressKind::SoftGuard, guardTripSlug(soft));
    }
    return GuardTrip::None;
  }
  // Progress the engine reports itself (EngineDeps::progress; RB-E3's status-line class).
  void report(ProgressKind kind, const char* detail) const {
    if (!d_.progress) return;
    ProgressEvent e;
    e.kind = kind;
    e.nowMs = d_.platform->nowMs();
    e.detail = detail;
    d_.progress(e);
  }

  Status ask() {
    if (paidStopped()) return Status::success();
    RenderInput& r = t_.render;
    r.turn = &t_.ctx;
    r.head = &t_.setup.head;
    r.system = &t_.system;
    r.tools = &t_.tools;
    r.items = d_.arena;
    r.stubPointer = c_.head.stub;
    r.toolChoiceNone = t_.lastRound;
    r.photo = t_.setup.photo;
    r.bound = bound();
    r.refusedPhotos = &c_.head.refusedPhotos;   // E2-P-PHOTO
    r.ladder = &t_.ladder;
    r.longCache = d_.stablePrefix;   // the stable points keep 1 hour (lane E2-CACHE)
    tx::TxLog* log = d_.log;
    r.bulk = [log](const std::string& handle, std::string& out) { return log->readBulk(handle, 0, tx::kBulkMaxBytes, out); };
    // The time limit for every round; the guard's last round may run to the turn's hard wall.
    const uint32_t wall = t_.lastRound ? t_.limits.lastRoundWallMs : t_.limits.timeLimitMs, used = elapsedMs();
    ExchangeRequest req;
    req.wire = t_.wire;
    req.render = &r;
    req.deadlineMs = std::min<uint32_t>(used < wall ? wall - used : 1, kExchangeMaxMs);
    req.bodyCap = t_.bodyCap;
    req.climb = [this](const ClimbAsk& ask) { return climb(ask); };
    if (r.photo) {   // a photo's file id the provider no longer holds: forgotten, sent once more without it
      const std::string chat = t_.ctx.chatKey;
      req.refusedRef = [chat](int st, const std::string& body) {
        return providers::imageparts::forgetRefusedFileId(chat, st, body);
      };
    }
    t_.ex = ExchangeResult{};
    const Status st = runExchange(ExchangeDeps{d_.transport, d_.platform}, req, t_.ex);
    account(t_.ex);
    if (!st.ok()) return failed(ExchangeErr::Other, st);
    if (photosRefused()) return ask();   // E2-P-PHOTO: once, the refused photos as their lines
    if (failOver()) {   // E2-P-FO: the same round on the next head, the conversation unchanged
      const Status cs = compose();
      if (!cs.ok() || !wired().ok()) return failed(ExchangeErr::Other, cs.ok() ? Status::fail(Err::NotFound, "plain::wireFor") : cs);
      return ask();
    }
    if (t_.ex.err == ExchangeErr::Cut) return cut();
    if (t_.ex.err != ExchangeErr::None) return failed(t_.ex.err, Status::success());
    ++t_.rounds;
    return Status::success();
  }

  // Lane E2-REP1 (review F): a monthly limit crossed by this turn's own answered requests (or reached
  // meanwhile) stops the turn before its next request, with the device's true line; what it did stays.
  bool paidStopped() {
    if (!d_.paidHeld || !t_.end.headRequests) return false;   // the turn's first request: the message gate asked
    const std::string line = d_.paidHeld(slotNow());
    if (line.empty()) return false;
    heldRoundFailed();
    t_.ex = ExchangeResult{};
    t_.ex.err = ExchangeErr::Other;
    t_.cause = "paused";
    event("paused:" + slotNow());
    setStatus(StatusLine::PausedSpend, line);
    return true;
  }

  // ---- fail-over at the round boundary (lane E2-P-FO, router design section 9) ------------------
  // The answering head's slot: the setup's, else the target's once a round moved.
  std::string slotNow() const { return t_.slot.empty() ? headSlugOf(t_.ctx, t_.setup.head) : t_.slot; }
  // A round that failed on a class engine 1 switches on moves to the first head of the binding's order
  // that was not tried this turn and whose body budget holds the request it replaces (design 6 item 7);
  // at most kMaxFailovers a turn. Nothing stored changes: the round is rendered again for the new head
  // (its own reasoning tag and call ids: the planner), and the person gets one line per switch.
  bool failOver() {
    const ExchangeResult& ex = t_.ex;
    if (!d_.failover || ex.err == ExchangeErr::None || t_.switches >= kMaxFailovers) return false;
    if (!ex.failOver && !switchesHost(ex.err, ex.httpStatus, ex.providerBody)) return false;
    FailoverAsk ask;
    ask.provider = slotNow();
    ask.model = t_.ctx.model;
    ask.err = ex.err;
    ask.httpStatus = ex.httpStatus;
    std::vector<FailoverTarget> order;
    d_.failover(ask, order);
    if (t_.tried.empty()) t_.tried.push_back(ask.provider + "/" + ask.model);
    for (FailoverTarget& tg : order) {
      const std::string id = tg.provider + "/" + tg.model;
      if (std::find(t_.tried.begin(), t_.tried.end(), id) != t_.tried.end()) continue;
      tx::HeadBudget b;
      if (!tx::budgetFor(tx::HeadKey{tg.provider, tg.model, tg.head.routed}, t_.ctx.profile, b).ok()) continue;
      const uint32_t cap = t_.setup.bodyCapLimit ? std::min(b.bodyCap, t_.setup.bodyCapLimit) : b.bodyCap;
      if (cap < ex.requestBytes) {   // never to a smaller body
        hlog::logf("plain(%s): fail-over skips %s: body %u B over its cap %u B", d_.chatKey.c_str(), id.c_str(),
                   (unsigned)ex.requestBytes, (unsigned)cap);
        continue;
      }
      moveTo(tg, cap, ask);
      return true;
    }
    hlog::logf("plain(%s): %s failed (%s): no head to fail over to", d_.chatKey.c_str(), ask.provider.c_str(),
               exchangeErrSlug(ex.err));
    return false;
  }
  void moveTo(FailoverTarget& tg, uint32_t cap, const FailoverAsk& ask) {
    closeShare(ask.provider, ask.model);
    if (!t_.switchLines.empty()) t_.switchLines += "\n";
    t_.switchLines += failoverLine(ask.provider, ask.err, tg.model);
    event("failover:" + ask.provider + ">" + tg.provider + ":" + exchangeErrSlug(ask.err));
    hlog::logf("plain(%s): %s/%s failed (%s): the round goes to %s/%s", d_.chatKey.c_str(), ask.provider.c_str(),
               ask.model.c_str(), exchangeErrSlug(ask.err), tg.provider.c_str(), tg.model.c_str());
    t_.tried.push_back(tg.provider + "/" + tg.model);
    ++t_.switches;
    t_.slot = tg.provider;
    t_.ctx.host = tg.host;
    t_.ctx.model = tg.model;
    t_.ctx.dialect = tg.dialect;
    if (!tg.seesPhotos) t_.ctx.photosBlind = true;   // each photo as its line, never pixels (design 9)
    t_.setup.head = std::move(tg.head);
    t_.setup.cost = tg.cost;   // the spend guard prices what follows at the new head's rates
    t_.setup.wire = nullptr;
    t_.bodyCap = cap;
  }
  // What the head left was billed since it took over (the turn's totals less `base`).
  void closeShare(const std::string& provider, const std::string& model) {
    HeadShare s;
    s.provider = provider;
    s.model = model;
    const tx::TurnEndFacts& e = t_.end;
    s.promptTokens = e.promptTokens - t_.base.promptTokens;
    s.outputTokens = e.outputTokens - t_.base.outputTokens;
    s.cacheReadTokens = e.cacheReadTokens - t_.base.cacheReadTokens;
    s.cacheWriteTokens = e.cacheWriteTokens - t_.base.cacheWriteTokens;
    s.cacheWrite1hTokens = e.cacheWrite1hTokens - t_.base.cacheWrite1hTokens;
    s.requests = (uint16_t)(e.headRequests - t_.base.requests);
    t_.base.promptTokens = e.promptTokens;
    t_.base.outputTokens = e.outputTokens;
    t_.base.cacheReadTokens = e.cacheReadTokens;
    t_.base.cacheWriteTokens = e.cacheWriteTokens;
    t_.base.cacheWrite1hTokens = e.cacheWrite1hTokens;
    t_.base.requests = e.headRequests;
    t_.shares.push_back(std::move(s));
  }

  // Lane E2-P-PHOTO: the provider refused the request for its image (imageparts::refusedImages: a
  // 400 or 422 naming the image, a format it does not take). Engine 1 re-sends with the photos as
  // text; here the open turn's visible photos join the chat's refused list, render as their true line
  // (tx::photoAbsentLine Refused) and the round is sent once more, so the turn ends in a reply.
  bool photosRefused() {
    if (t_.ex.err != ExchangeErr::Other || t_.photoRefusalResent || t_.photosShown.empty() ||
        !providers::imageparts::refusedImages(t_.ex.httpStatus, t_.ex.providerBody))
      return false;
    t_.photoRefusalResent = true;
    for (const std::string& ref : t_.photosShown)
      if (std::find(c_.head.refusedPhotos.begin(), c_.head.refusedPhotos.end(), ref) == c_.head.refusedPhotos.end()) {
        // E2-REP4: kept in head.json (written at the close), so a restart does not try it again; bounded.
        if (c_.head.refusedPhotos.size() >= tx::kHeadMaxRefusedPhotos) c_.head.refusedPhotos.erase(c_.head.refusedPhotos.begin());
        c_.head.refusedPhotos.push_back(ref);
      }
    hlog::logf("plain(%s): the provider refused the photos (HTTP %d): sent again with %u as their line",
               d_.chatKey.c_str(), t_.ex.httpStatus, (unsigned)t_.photosShown.size());
    t_.photosShown.clear();
    return true;
  }

  void account(const ExchangeResult& ex) {
    t_.end.headRequests += ex.requests;
    if (ex.requests) {
      c_.head.lastReqAt = epoch();
      t_.end.exchanges.push_back(exchangeFacts(ex));   // with the request by reference (RB-E3)
    }
    const orch::TokenUsage& u = ex.step.usage;
    if (ex.requests)   // one line per answered request: the cache split as the provider billed it (E2-CACHE)
      hlog::logf("plain(%s): usage in=%u cacheR=%u cacheW=%u cacheW1h=%u out=%u", d_.chatKey.c_str(), (unsigned)u.promptTokens,
                 (unsigned)u.cacheReadTokens, (unsigned)u.cacheWriteTokens, (unsigned)u.cacheWrite1hTokens,
                 (unsigned)u.completionTokens);
    t_.end.promptTokens += u.promptTokens;
    t_.end.outputTokens += u.completionTokens;
    t_.end.cacheReadTokens += u.cacheReadTokens;
    t_.end.cacheWriteTokens += u.cacheWriteTokens;
    t_.end.cacheWrite1hTokens += std::min(u.cacheWrite1hTokens, u.cacheWriteTokens);
    keepServed(t_.end.servedModel, u.servedModel);   // E2-P-SURF: the usage rows name it
    // The guards' inputs: size counts every prompt token (cached ones too); spend prices them. An
    // answer that reports no usage at all still counts: a floor from the request's bytes (about 3.5 B
    // a token, kDefaultRequestTokens' rule) at the input rate, named by an event (ruling 79 item 1).
    RequestTokens rt{u.promptTokens, u.completionTokens, u.cacheReadTokens, u.cacheWriteTokens, u.cacheWrite1hTokens};
    const bool answered = ex.requests && (ex.err == ExchangeErr::None || ex.err == ExchangeErr::Cut);
    // Compaction's hot trigger reads the last answered request's whole prompt, never the turn's sum
    // (ruling 91 T4-ADV-01): maxRequestIn is input plus cache in every provider's meaning; a wire
    // that reports no whole prompt leaves its input count.
    if (answered && (u.maxRequestIn || u.promptTokens)) t_.end.lastPromptTokens = u.maxRequestIn ? u.maxRequestIn : u.promptTokens;
    if (answered && !rt.prompt && !rt.output && !rt.cacheRead && !rt.cacheWrite) {
      rt.prompt = (uint32_t)std::min<uint64_t>((uint64_t)ex.requestBytes * 2 / 7, UINT32_MAX);
      event("usage:missing");
    }
    const uint64_t size = (uint64_t)t_.sizeTokens + rt.prompt + rt.cacheRead + rt.cacheWrite;
    t_.sizeTokens = (uint32_t)std::min<uint64_t>(size, UINT32_MAX);
    t_.end.microUsd = (uint32_t)std::min<uint64_t>((uint64_t)t_.end.microUsd + requestMicroUsd(t_.setup.cost, rt), UINT32_MAX);
    takePark(ex);
    notePending(ex);
    t_.failOver |= ex.failOver;
    t_.modelUnusable |= ex.modelUnusable;
  }

  // E2-REP1 (review F): an answered exchange's spend counts toward the limits now (EngineDeps::pendingSpend).
  void notePending(const ExchangeResult& ex) {
    const orch::TokenUsage& u = ex.step.usage;
    if (!d_.pendingSpend || !ex.requests || (u.empty() && !u.cacheReadTokens && !u.cacheWriteTokens)) return;
    d_.pendingSpend(slotNow(), t_.ctx.model, u);
  }

  // E2-FIX14: the refusal the binding hands the shared park. E2-P-FO: the first head's in parkHost;
  // E2-REP1: a fallback head's in fallbackRefusals, so the binding parks it too.
  void takePark(const ExchangeResult& ex) {
    if (!ex.parkHost) return;
    if (t_.switches) {
      event("park:fallback");
      TurnOutcome::FallbackRefusal r;
      r.provider = slotNow();
      r.keyFp = t_.setup.head.key ? orch::parkKeyFingerprint(t_.setup.head.key()) : 0;
      r.status = ex.httpStatus;
      r.text = ex.parkBody;
      t_.fallbackRefusals.push_back(std::move(r));
      return;
    }
    t_.parkHost = true;
    t_.parkStatus = ex.httpStatus;
    t_.parkText = ex.parkBody;
  }

  // Nothing came from the model: the exchange's one plain line ends the turn.
  Status failed(ExchangeErr err, const Status& st) {
    heldRoundFailed();
    if (t_.lastRound && t_.trip != GuardTrip::None) {   // the guard stopped the turn, not the provider
      t_.cause = std::string("limit:") + guardTripSlug(t_.trip);
      setStatus(StatusLine::LimitReached, limitLine());
      if (err != t_.ex.err) t_.ex.err = err;
      return st;
    }
    t_.cause = std::string("provider:") + exchangeErrSlug(err);   // a held reply goes out: settle()
    if (t_.ex.errorLine.empty()) setStatus(StatusLine::ProviderError, exchangeLine(err, providerSlugOf(t_.ctx, t_.setup.head), 0));
    else setStatus(StatusLine::ProviderError, t_.ex.errorLine, &t_.ex.errorLineStored);
    if (err != t_.ex.err) t_.ex.err = err;
    return st;
  }

  // A held reply whose extra round failed goes out (settle()): the truth seam sees it once more, as
  // the reply that is delivered, so a failed-write receipt it owes still rides out (RB-G1).
  void heldRoundFailed() {
    if (!t_.held || !d_.hooks) return;
    DeliverCtx dc;
    dc.turn = &t_.ctx;
    dc.text = &t_.heldText;
    dc.held = true;
    dc.cut = true;
    takeStatus(d_.hooks->runBeforeDeliver(dc));
  }

  // A cut answer: its text is delivered with the cut line; its calls are never run.
  Status cut() {
    ++t_.rounds;
    t_.answer.swap(t_.ex.step.text);   // in hand before anything is stored, never copied
    t_.cause = "cut";
    setStatus(t_.ex.status, t_.ex.errorLine, &t_.ex.errorLineStored);
    if (d_.hooks) {   // the truth seam sees a cut answer too; it cannot hold one
      DeliverCtx dc;
      dc.turn = &t_.ctx;
      dc.text = &t_.answer;
      dc.held = t_.held;
      dc.cut = true;
      takeStatus(d_.hooks->runBeforeDeliver(dc));
    }
    Status st = storeAnswer(t_.ex.step, true, t_.answer);
    for (size_t i = 0; st.ok() && i < t_.ex.step.calls.size(); ++i) {
      t_.curCallQ = callQOf(i);
      st = answerUnrun(t_.ex.step.calls[i], texts::tool_results::kNotRunCut);
    }
    return st;
  }

  // The overflow ladder (tx::nextRung), one rung per call; false when no rung is left. Rung 1
  // and 2 climb only when they change what renders. Rung 3 omits the fewest oldest turns that
  // free what the body is over its cap (and the omission line's own bytes), and climbs again
  // with more turns while the body is still over and omissible turns remain; a refusal knows
  // no size, so there it omits every omissible turn.
  bool climb(const ClimbAsk& ask) {
    LadderState& l = t_.ladder;
    const bool over = ask.why == Climb::OverCap;
    // Compaction first, once a turn, then the prioritized trim (ruling 105 item 3); on a provider's
    // "too long" it is the one compaction before the one retry (ruling 106 item 4).
    if (compactInTurn()) return true;
    bool climbed = false;
    const uint64_t closedTo = over && closedBulkAbovePointer() ? closedStubThrough(ask) : 0;
    if (closedTo > l.closedThrough) {
      l.closedThrough = c_.ladderLine = closedTo;   // (1) the closed turns' bulk as stubs, oldest first, as far as needed
      climbed = true;
    } else if (over && openBulkBelow(rungTwoThrough())) {
      l.stubThrough = rungTwoThrough();   // (2) this turn's bulk older than the newest two rounds
      climbed = true;
    } else {
      const size_t need = over && ask.bodyBytes > ask.bodyCap ? ask.bodyBytes - ask.bodyCap + kOmissionLineRoom
                                                             : std::numeric_limits<size_t>::max();
      climbed = omitOldest(need);   // (3) one omission line, never the newest 6 owner turns
      if (climbed) refreezeIfOmitted();
    }
    if (climbed) event("change:ladder");
    return climbed;
  }
  static constexpr size_t kOmissionLineRoom = 160;   // the line and its block (under 120 B of text)

  // Inside a turn: compaction first, once, before rung 3 omits turns or a provider's "too long" is
  // retried; the system text is composed again with the new summary (a change point, logged).
  bool compactInTurn() {
    if (!compactFirst(Urgency::Now)) return false;
    return compose().ok() && wired().ok();
  }

  // Rung 2's line: the item before the newest two rounds of the open turn (0: none yet).
  uint64_t rungTwoThrough() const {
    const size_t n = t_.roundStart.size();
    return n > 2 ? t_.roundStart[n - 2] - 1 : 0;
  }
  // Open-turn bulk still whole between rung 2's line and `through`: what rung 2 would stub now.
  bool openBulkBelow(uint64_t through) const {
    if (through <= t_.ladder.stubThrough) return false;
    tx::Item& it = t_.read;
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok() || e.turn != t_.ctx.turn) continue;
      if (e.kind != tx::ItemKind::Result && e.kind != tx::ItemKind::Call) continue;
      if (e.q <= t_.ladder.stubThrough || e.q > through || !d_.arena->get(i, it).ok()) continue;
      if (e.kind == tx::ItemKind::Call ? !it.st.empty() : it.cls == tx::ResultClass::Bulk) return true;   // RB-H81: long arguments too
    }
    return false;
  }

  // Tier-2 bulk or a long call argument of a closed turn shown whole above the pointer and rung 1's line: what
  // rung 1 would stub (lane E2-VERB: long arguments stay whole until the overflow path stubs them).
  bool closedBulkAbovePointer() const {
    tx::Item& it = t_.read;
    const uint64_t from = std::max(c_.head.stub, t_.ladder.closedThrough);
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok() || (e.kind != tx::ItemKind::Result && e.kind != tx::ItemKind::Call)) continue;
      if (e.q <= from || e.q > c_.lastClosedQ || !d_.arena->get(i, it).ok()) continue;
      if (e.kind == tx::ItemKind::Call ? !it.st.empty() : it.cls == tx::ResultClass::Bulk) return true;
    }
    return false;
  }

  // Rung 1's line (lane E2-FIX9, ruling 81: nothing leaves the request that does not have to): the closed turns'
  // bulk results and long arguments become stubs oldest first, only until what they free brings the body down to
  // kLadderLowPercent of its cap (each counted as its text, a lower bound of what the wire frees: escapes only add);
  // the next climb goes further when that was not enough. The line then stays (ChatState::ladderLine) while the
  // turns after it grow back to the cap, so they share one cached prefix instead of a line that moves every turn.
  // The last closed item when the size is not known.
  static constexpr size_t kLadderLowPercent = 75;
  uint64_t closedStubThrough(const ClimbAsk& ask) const {
    if (ask.bodyBytes <= ask.bodyCap) return c_.lastClosedQ;
    const size_t need = ask.bodyBytes - ask.bodyCap * kLadderLowPercent / 100;
    const uint64_t from = std::max(c_.head.stub, t_.ladder.closedThrough);
    const std::vector<uint64_t> listed = foldListed();
    size_t freed = 0;
    uint64_t to = from;
    for (size_t i = 0; i < d_.arena->count() && freed < need; ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok() || e.q <= from || e.q > c_.lastClosedQ) continue;
      const size_t frees = stubFrees(i, e, listed);
      if (frees == kNotBulk) continue;
      freed += frees;
      to = e.q;
    }
    return to;
  }
  static constexpr size_t kNotBulk = std::numeric_limits<size_t>::max();
  // The items a tool fold's list already shows as stubs, sorted: stubbing them again frees nothing.
  std::vector<uint64_t> foldListed() const {
    std::vector<uint64_t> listed;
    tx::Item& it = t_.read;
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (d_.arena->header(i, e).ok() && e.kind == tx::ItemKind::PointerMove && d_.arena->get(i, it).ok() && tx::stubList(it))
        listed.insert(listed.end(), it.sup.begin(), it.sup.end());
    }
    std::sort(listed.begin(), listed.end());
    return listed;
  }
  // What rendering one closed item as its stub frees (its whole text less the stub); kNotBulk for an item rung 1 does
  // not stub (no bulk result, no long argument, or one a fold already lists).
  size_t stubFrees(size_t index, const tx::ArenaEntry& e, const std::vector<uint64_t>& listed) const {
    if (e.kind != tx::ItemKind::Result && e.kind != tx::ItemKind::Call) return kNotBulk;
    tx::Item& it = t_.read;
    if (!d_.arena->get(index, it).ok() || std::binary_search(listed.begin(), listed.end(), e.q)) return kNotBulk;
    const bool bulk = e.kind == tx::ItemKind::Call ? !it.st.empty() : it.cls == tx::ResultClass::Bulk;
    if (!bulk) return kNotBulk;
    const size_t whole = e.kind == tx::ItemKind::Call ? it.args.size() : shownBytes(it);
    return whole > it.st.size() ? whole - it.st.size() : 0;
  }

  // What one stored item adds to the request, at least: the text it shows, with no wire's
  // framing (an owner text with its stamp, an assistant text, a device note, a call's id, name
  // and arguments, a result as it shows: its stub when stubbed or outside, else the receipt or
  // the whole bulk), nothing for the items no wire renders (t, i, x, d, e, s; k of a closed
  // turn). Rung 3 sizes its omission with it: a lower bound on every wire, so the turns it omits
  // always free what the body is over (framing and JSON escapes only add), and never a sum of
  // stored lines, whose keys and stub copies count bytes the request does not carry.
  size_t renderedBytes(size_t index, const tx::ArenaEntry& e) const {
    if (!rendersText(e.kind)) return 0;
    tx::Item& it = t_.read;
    if (!d_.arena->get(index, it).ok()) return 0;
    if (e.kind == tx::ItemKind::User) return it.x.size() + stampBytes(it);
    if (e.kind == tx::ItemKind::Call) return it.id.size() + it.n.size() + tx::shownArgs(it, showFacts()).size();
    if (e.kind == tx::ItemKind::Result) return shownBytes(it);
    return it.x.size();
  }
  static bool rendersText(tx::ItemKind k) {
    return k == tx::ItemKind::User || k == tx::ItemKind::Assistant || k == tx::ItemKind::Call ||
           k == tx::ItemKind::DeviceNote || k == tx::ItemKind::Result;
  }
  size_t stampBytes(const tx::Item& u) const {
    char stamp[tx::kStampMaxBytes];
    return tx::messageStamp(u.t, u.tzMinutes, t_.ctx.dayStartMin, stamp, sizeof stamp).ok() ? std::strlen(stamp) : 0;
  }
  // The stub rule's facts for this request as the ladder stands (the stub lists are not read: a
  // folded item counts whole, as the results always did here).
  tx::ShowFacts showFacts() const {
    tx::ShowFacts f;
    f.pointer = std::max(c_.head.stub, t_.ladder.closedThrough);
    f.openTurn = t_.ctx.turn;
    f.ladderStubThrough = t_.ladder.stubThrough;
    return f;
  }
  // A result as it shows: its stub when stubbed or outside, else the receipt or the whole bulk.
  size_t shownBytes(const tx::Item& r) const {
    const bool stub = r.cls == tx::ResultClass::Outside ||
                      (r.cls == tx::ResultClass::Bulk && (r.q <= c_.head.stub || r.q <= t_.ladder.closedThrough));
    if (stub) return r.st.size();
    return r.cls == tx::ResultClass::Receipt || r.handle.empty() ? r.x.size() : r.bytes;
  }

  // Rung 3: the oldest whole turns that free at least `need` more bytes than the turns it
  // already left out; false when no further turn may go.
  bool omitOldest(size_t need) {
    std::vector<tx::TurnSpan> spans;
    std::vector<uint64_t> lastQ;
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok() || e.turn == 0) continue;
      if (spans.empty() || spans.back().turn != e.turn) {
        if (!spans.empty() && e.turn < spans.back().turn) return false;   // not whole turns in order
        spans.push_back(tx::TurnSpan{e.turn, false, 0, e.t});
        lastQ.push_back(0);
      }
      spans.back().owner |= e.kind == tx::ItemKind::User;
      spans.back().bytes += renderedBytes(i, e);
      lastQ.back() = e.q;
    }
    uint32_t done = 0;   // turns already left out: the body no longer carries them
    for (; done < spans.size() && lastQ[done] <= t_.ladder.omitThrough; ++done)
      if (need != std::numeric_limits<size_t>::max()) need += spans[done].bytes;
    uint32_t n = 0;
    if (!tx::omissionSpan(spans, need, n).ok() || n <= done) return false;
    return omit(lastQ[n - 1]);
  }

  // ---- what the model said ----------------------------------------------------------------
  // The reply, after the truth seam: held once, it is stored as an interim text with the
  // device's facts item after it and the loop asks once more.
  Status finalAnswer(const ParsedStep& step) {
    t_.answer.swap(t_.ex.step.text);   // the reply in hand before anything is stored, never copied
    DeliverDecision dd;
    if (d_.hooks) {
      DeliverCtx dc;
      dc.turn = &t_.ctx;
      dc.text = &t_.answer;
      dc.held = t_.held;
      dc.last = t_.lastRound;
      dd = d_.hooks->runBeforeDeliver(dc);
    }
    // At most one hold a turn, and none on a guard's last round: a hold asks one more request.
    const bool hold = dd.verdict == DeliverVerdict::Hold && !t_.held && !t_.lastRound;
    takeStatus(dd);
    if (hold) {
      t_.held = true;
      t_.heldText.swap(t_.answer);
    } else {
      t_.answered = true;
      t_.emptyRefusal = step.stop == StopReason::Refusal;
      answerCause(step);
    }
    Status st = storeAnswer(step, !hold, hold ? t_.heldText : t_.answer);
    for (size_t i = 0; st.ok() && i < step.calls.size(); ++i) {   // the last round still called
      t_.curCallQ = callQOf(i);
      st = answerUnrun(step.calls[i], texts::tool_results::kNotRunLimit);
    }
    if (!st.ok()) return st;
    if (!hold) return Status::success();
    event("hold");
    return note(dd.deviceNote, tx::NoteClass::Notice);
  }

  // The final answer's cause: the reply, the guard that ended the turn, or an empty answer a person
  // gets a plain line for (a held reply that goes out is a reply: ruling 59 item 5).
  void answerCause(const ParsedStep& step) {
    t_.cause = t_.trip == GuardTrip::None ? "reply" : std::string("limit:") + guardTripSlug(t_.trip);
    const bool heldGoesOut = (t_.held && !t_.heldText.empty()) || !t_.interim.empty();
    if (t_.answer.empty() && step.calls.empty() && personTrigger(t_.ctx.trigger) && !heldGoesOut) t_.cause = "empty";
  }

  // At most one status line a turn, by precedence: a failed-write receipt (a seam's) is never
  // replaced; then the device's own failure (memory, a part too long); then the turn's end (a
  // provider line, the cut line, the limit line); then the storage line. Equal ranks keep the
  // first.
  static int rank(StatusLine s) {
    switch (s) {
      case StatusLine::None: return 0;
      case StatusLine::FailedWrite: return 4;
      case StatusLine::CrashNotice: return 3;
      case StatusLine::StorageState: return 1;
      default: return 2;
    }
  }
  // `stored`: the line as it may be kept or reach a model when it differs from `text` (a provider's
  // reason rides only in the sent line, E2-FIX11).
  void setStatus(StatusLine s, const std::string& text, const std::string* stored = nullptr) {
    if (rank(s) <= rank(t_.status)) return;
    std::string kept = stored && *stored != text ? *stored : std::string();
    t_.statusText = text;   // copied first: a refused copy leaves the earlier line whole
    t_.statusStored.swap(kept);
    t_.status = s;
    t_.statusRef = &t_.statusText;
  }
  // E2-P-FO: the answering head's share, once (settle, also on the refused path).
  void lastShare() {
    if (t_.switches && t_.shares.size() == t_.switches) closeShare(slotNow(), t_.ctx.model);
  }
  // E2-P-FO: a reply that came from a fallback head carries one line per switch, before any other line
  // of the turn (a cut's, a guard's): the status line is one message. A failed last head: the plain
  // failure line alone.
  void switchedLine() {
    const bool fromFallback = t_.ex.err == ExchangeErr::None || t_.ex.err == ExchangeErr::Cut;
    if (t_.switchLines.empty() || !t_.reply || !fromFallback) return;
    if (t_.status == StatusLine::None) return setStatus(StatusLine::ProviderError, t_.switchLines);
    if (t_.statusRef != &t_.statusText) return;
    if (t_.statusStored.empty()) t_.statusStored = t_.statusText;
    t_.statusText.insert(0, t_.switchLines + "\n");
    t_.statusStored.insert(0, t_.switchLines + "\n");
  }
  // A line kept outside the turn (the memory line): taken with no allocation.
  void setStatusRef(StatusLine s, const std::string& kept) noexcept {
    if (rank(s) <= rank(t_.status)) return;
    t_.status = s;
    t_.statusStored.clear();
    t_.statusRef = &kept;
  }
  void takeStatus(const DeliverDecision& dd) {
    t_.end.probeRequests = (uint16_t)std::min<unsigned>(UINT16_MAX, (unsigned)t_.end.probeRequests + dd.probes);
    setStatus(dd.status, dd.statusText);
  }
  // The line for a turn a guard ended before a reply: it names the guard (ruling 79 item 4).
  std::string limitLine() const {
    return std::string(texts::device_notes::kLimitStopped) + guardTripSlug(t_.trip) + texts::device_notes::kLimitStoppedEnd;
  }

  // What the owner gets, decided in one place (close and the refusal path): the model's reply
  // if there is one, else the reply held before a failed or empty extra round, else a plain
  // line: the limit line when the last allowed round only called tools, the provider's line
  // for an empty answer. A routine's empty text stays its silence.
  void settle() {
    t_.replyText = replyOf();
    t_.reply = t_.replyText != nullptr;   // a pointer: the reply is never copied to go out
    lastShare();
    if (t_.reply && t_.trip != GuardTrip::None && personTrigger(t_.ctx.trigger))   // the guard that stopped it
      setStatus(StatusLine::LimitReached, std::string(texts::device_notes::kLimitStopped) + guardTripSlug(t_.trip) +
                                              texts::device_notes::kLimitStoppedEnd);
    if (t_.reply || !personTrigger(t_.ctx.trigger)) return;
    if (t_.answered && t_.trip != GuardTrip::None) setStatus(StatusLine::LimitReached, limitLine());
    else if (t_.answered || t_.status == StatusLine::None)   // never silent: an answer the turn could not use
      setStatus(StatusLine::ProviderError,
                exchangeLine(t_.emptyRefusal ? ExchangeErr::Other : ExchangeErr::Malformed,
                             providerSlugOf(t_.ctx, t_.setup.head), 0));
  }

  // The reply in hand: the answer, else the held reply, else the text the model wrote beside its
  // calls when the last round said nothing (RB-H81).
  const std::string* replyOf() const {
    if (!t_.answer.empty()) return &t_.answer;
    if (t_.held && !t_.heldText.empty()) return &t_.heldText;
    return t_.answered && !t_.interim.empty() ? &t_.interim : nullptr;
  }

  // k, a and c items in the order the model gave them, before any call runs.
  Status storeAnswer(const ParsedStep& step, bool fin) { return storeAnswer(step, fin, step.text); }
  Status storeAnswer(const ParsedStep& step, bool fin, const std::string& text) {
    t_.roundStart.push_back(c_.nextQ);
    Status st = meta(step, 0);
    if (st.ok() && (!text.empty() || step.calls.empty())) {
      tx::Item& a = blank(tx::ItemKind::Assistant);
      a.rd = (uint8_t)std::min<uint16_t>(t_.rounds, 255);
      a.fin = fin;
      st = tierText(text, a);   // over tier 1: bulk and a stub; the delivered text stays whole
      if (st.ok()) st = put(a);
    }
    t_.callQ.clear();
    for (size_t i = 0; st.ok() && i < step.calls.size(); ++i) {
      if (i && !(st = meta(step, (uint16_t)i)).ok()) break;
      tx::Item& c = blank(tx::ItemKind::Call);
      c.id = step.calls[i].id;
      c.n = step.calls[i].name;
      if (!(st = tierArgs(c.n, step.calls[i].argsJson, c.args, c.st)).ok()) break;
      t_.longArgs |= !c.st.empty();
      c.eff = viewOf(step.calls[i]).cls.effect == orch::EffectClass::Effect;
      c.rd = a_round();
      st = put(c);
      t_.callQ.push_back(c.q);
      if (st.ok()) t_.writes.push_back(c);   // pruned to successful effects when their result lands
    }
    if (st.ok() && !step.calls.empty()) st = meta(step, (uint16_t)step.calls.size());
    return st;
  }
  uint8_t a_round() const { return (uint8_t)std::min<uint16_t>(t_.rounds, 255); }
  uint64_t callQOf(size_t i) const { return i < t_.callQ.size() ? t_.callQ[i] : 0; }

  // Tier 2 for a call's arguments (PRD section 2 as amended by rulings 81 to 83): a string argument
  // over kTier1CallArgs is whole in every request, bounded like tier-2 bulk (the head's
  // bulkWholeCap), and shows as its long-argument stub only on the overflow path (lane E2-VERB,
  // tx::shownArgs): the c item keeps the model's arguments in `out` and their stub form in
  // `stub`. The whole arguments go to the bulk file first (the stub's view pointer). Over the
  // bound, or when the line would not hold both, `out` is the stub form (arguments that do not
  // parse, cut by the output limit, as one stub). When the bulk file refuses, the arguments stay
  // inline whole if their line fits, else the stub makes no promise of view. The call itself runs
  // with the model's arguments.
  Status tierArgs(const std::string& name, const std::string& args, std::string& out, std::string& stub) {
    out = args;
    stub.clear();
    if (args.size() <= tx::kTier1CallArgs) return Status::success();
    ArduinoJson::JsonDocument doc;
    const bool parsed = !deserializeJson(doc, args, ArduinoJson::DeserializationOption::NestingLimit(64));
    if (parsed && !hasLongString(doc.as<ArduinoJson::JsonVariantConst>())) return Status::success();
    std::string handle;
    const bool kept = d_.log->appendBulk(t_.ctx.turn, args, handle).ok();
    if (!kept && args.size() + kLineRoom <= tx::kMaxLineBytes) return Status::success();   // inline whole
    std::string stubbed;
    t_.again = parsed ? tx::wayBack(name, args) : std::string();   // a file write: the file it wrote
    if (!parsed) {
      if (!longStub(name, args, kept, stubbed).ok()) return Status::fail(Err::InvalidArgument, "plain::tierArgs");
    } else {
      stubLongStrings(name, doc.as<ArduinoJson::JsonVariant>(), kept);
      serializeJson(doc, stubbed);
    }
    const bool whole = args.size() <= t_.bulkWholeCap && args.size() + stubbed.size() + kLineRoom <= tx::kMaxLineBytes;
    if (whole) stub.swap(stubbed);   // whole in its open turn, the stub from its close
    else out.swap(stubbed);          // over the bound: the stub at once
    return Status::success();
  }
  static constexpr size_t kLineRoom = 1024;   // a line's keys and crc beside its text

  // Both walks keep their own work list in the working set instead of recursing: the arguments nest
  // as deep as the parse allows (64), and a frame per level landed on the Telegram task's deepest
  // chain (lane E2-STACK2). stubLongStrings visits the strings in the order the recursion did.
  static bool hasLongString(ArduinoJson::JsonVariantConst root) {
    orch::WorkingVector<ArduinoJson::JsonVariantConst> todo{root};
    while (!todo.empty()) {
      const ArduinoJson::JsonVariantConst v = todo.back();
      todo.pop_back();
      if (v.is<ArduinoJson::JsonObjectConst>()) {
        for (ArduinoJson::JsonPairConst kv : v.as<ArduinoJson::JsonObjectConst>()) todo.push_back(kv.value());
      } else if (v.is<ArduinoJson::JsonArrayConst>()) {
        for (ArduinoJson::JsonVariantConst e : v.as<ArduinoJson::JsonArrayConst>()) todo.push_back(e);
      } else if (v.is<const char*>() && std::strlen(v.as<const char*>()) > tx::kTier1CallArgs) {
        return true;
      }
    }
    return false;
  }
  void stubLongStrings(const std::string& name, ArduinoJson::JsonVariant root, bool kept) {
    orch::WorkingVector<ArduinoJson::JsonVariant> todo{root};
    while (!todo.empty()) {
      ArduinoJson::JsonVariant v = todo.back();
      todo.pop_back();
      const size_t at = todo.size();
      if (v.is<ArduinoJson::JsonObject>()) {
        for (ArduinoJson::JsonPair kv : v.as<ArduinoJson::JsonObject>()) todo.push_back(kv.value());
      } else if (v.is<ArduinoJson::JsonArray>()) {
        for (ArduinoJson::JsonVariant e : v.as<ArduinoJson::JsonArray>()) todo.push_back(e);
      } else if (v.is<const char*>() && std::strlen(v.as<const char*>()) > tx::kTier1CallArgs) {
        std::string stub;
        if (longStub(name, v.as<const char*>(), kept, stub).ok()) v.set(stub);
      }
      std::reverse(todo.begin() + (std::ptrdiff_t)at, todo.end());   // first child on top
    }
  }
  // A long text's stub: with the bulk file kept, the long-argument stub (view); without it,
  // the internal stub's no-card form, which promises nothing.
  Status longStub(const std::string& name, const std::string& text, bool kept, std::string& out) {
    if (kept) return tx::longArgStub((uint32_t)text.size(), hex8(text), c_.nextQ, out, t_.again, bound());
    return stubOf(name, text, false, out);
  }
  Status stubOf(const std::string& tool, const std::string& text, bool kept, std::string& out) {
    tx::StubFacts& f = renew(t_.stub);
    f.tool = tool;
    f.q = c_.nextQ;
    f.bytes = (uint32_t)text.size();
    f.lines = 1 + (uint32_t)std::count(text.begin(), text.end(), '\n');
    (void)tx::stubTime(epoch(), t_.tz, f.hhmm);
    f.preview = text.substr(0, tx::kStubPreviewBytes + 4);
    f.cls = tx::ResultClass::Bulk;
    f.noCard = !kept;
    f.bound = bound();
    return tx::stubText(f, out);
  }
  static std::string hex8(const std::string& s) {
    static const char kHex[] = "0123456789abcdef";
    uint32_t h = fnv1a(2166136261u, s);
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i, h >>= 4) out[(size_t)i] = kHex[h & 15];
    return out;
  }

  // Tier 1 for assistant text (owner rulings 81 to 83): whole up to the head's v5.1.2 message cap
  // (tx::HeadBudget::messageCap, never under 12,288 B). Over it the whole goes to the bulk file
  // first, the a item names it (handle: a later view and a resume after a crash read the whole) and
  // keeps the first messageCap bytes with a line pointing at the rest (tx::longTextHead), never a
  // 200 B preview. When the bulk file refuses, the text stays whole if its line fits, else the
  // head says the rest was not kept. The delivered text stays whole.
  Status tierText(const std::string& text, tx::Item& a) {
    a.x = text;
    const size_t cap = std::max<size_t>(tx::kTier1AssistantText, t_.messageCap);
    if (text.size() <= cap) return Status::success();
    const bool kept = d_.log->appendBulk(t_.ctx.turn, text, a.handle).ok();
    if (!kept) a.handle.clear();
    if (!kept && text.size() + kLineRoom <= tx::kMaxLineBytes) return Status::success();
    return tx::longTextHead(text, cap, kept ? tx::viewPointer(c_.nextQ, bound()) : std::string(), a.x, kept);
  }

  Status meta(const ParsedStep& step, uint16_t beforeCall) {
    for (const MetaPart& m : step.meta) {
      if (m.beforeCall != beforeCall) continue;
      tx::Item& k = blank(tx::ItemKind::RoundMeta);
      k.x = m.bytes;
      k.rd = a_round();
      k.wr = replayTag(t_.ctx.dialect, t_.setup.head.routed, t_.ctx.model);   // replayed only to its writer (E2-R4b)
      t_.sawMeta = true;
      Status st = put(k);
      if (!st.ok()) return st;
    }
    return Status::success();
  }

  // One call as every rule keyed on the tool must see it (class, taint, the file-set change,
  // superseded reads): a `more` call is the tool it reaches with that tool's arguments
  // (moreTarget). The stored c item keeps the model's own call (`more` and its arguments):
  // that is what the next request renders and what its result pairs with.
  struct CallView {
    std::string name;      // the tool the call reaches
    std::string args;      // its arguments (JSON)
    orch::ToolClass cls;   // its class; a router that reaches nothing knowable: the fail-closed default
    bool unresolved = false;   // a router call with no target (a listing, an unknown or refused name)
  };
  // E2-FIX5: the tools this turn's texts may name: registered for the turn (none without a registry).
  tx::BoundTools bound() const { return boundTools(t_.setup.facts.reg); }
  CallView viewOf(const std::string& name, const std::string& argsJson) const {
    CallView v;
    v.name = name;
    v.args = argsJson;
    const orch::ToolRegistry* reg = t_.setup.facts.reg;
    if (!reg) {
      v.cls = orch::toolClassFor(name);
      return v;
    }
    const std::string regName = reg->resolveName(name);
    if (regName.empty() || !reg->isRouter(regName)) {
      v.cls = reg->classOf(regName.empty() ? name : regName);
      // A tool server's tool spelled directly (mcp_x_y) is judged by its registry name, as through `more` (E2-REP2).
      if (!regName.empty() && !orch::mcp::serverOf(regName).empty()) v.name = regName;
      return v;
    }
    v.unresolved = true;   // until the target is known: the fail-closed class (outside, effect)
    ArduinoJson::JsonDocument args;
    if (deserializeJson(args, argsJson, ArduinoJson::DeserializationOption::NestingLimit(64)) ||
        !args.is<ArduinoJson::JsonObjectConst>() || !t_.ctx.who)
      return v;
    const orch::MoreTarget mt = orch::moreTarget(*reg, args.as<ArduinoJson::JsonObjectConst>(), *t_.ctx.who);
    v.cls = mt.cls;
    if (mt.name.empty()) return v;
    v.unresolved = false;
    v.name = mt.name;
    v.args.clear();
    if (args["args"].is<ArduinoJson::JsonObjectConst>()) serializeJson(args["args"], v.args);
    else v.args = "{}";
    return v;
  }
  CallView viewOf(const ToolCall& call) const { return viewOf(call.name, call.argsJson); }
  // Outside content by class: a tool with a taint row that says so, a tool nothing classes at
  // all, or a router call that reaches nothing knowable (each reads as outside). The engine-2
  // names have class rows but no taint rows yet: their own result says whether it is outside
  // (ToolOutput::outside). A read of a device-written folder (inbox/, agents/) is outside whatever
  // its class (RB-RL: plain/rails.h).
  static bool outsideByClass(const CallView& v) {
    if (readsOutsidePath(v.name, v.args)) return true;
    if (v.cls.taint != orch::Taint::External) return false;
    return v.unresolved || orch::toolTaintRow(v.name) != nullptr || orch::toolClassRow(v.name) == nullptr;
  }

  // ---- one call -----------------------------------------------------------------------------
  // The round's calls in order; past the time limit no further call starts.
  Status runCalls(const ParsedStep& step) {
    Status st = Status::success();
    for (size_t i = 0; st.ok() && i < step.calls.size(); ++i) {
      t_.curCallQ = callQOf(i);
      st = elapsedMs() < t_.limits.timeLimitMs ? runCall(step.calls[i])
                                               : answerUnrun(step.calls[i], texts::tool_results::kNotRunLimit);
    }
    ++t_.toolRounds;
    t_.progress.endRound();
    return st;
  }

  Status runCall(const ToolCall& call) {
    const CallView view = viewOf(call);
    const orch::ToolClass& cls = view.cls;
    DispatchDecision dd;
    if (d_.hooks) {
      ToolCall target;   // the tool a `more` call reaches, as the seams' rules see it
      const bool routed = view.name != call.name;
      if (routed) {
        target.id = call.id;
        target.name = view.name;
        target.argsJson = view.args;
        target.complete = call.complete;
      }
      DispatchCtx dc;
      dc.turn = &t_.ctx;
      dc.call = &call;
      dc.target = routed ? &target : nullptr;
      dc.pure = cls.effect == orch::EffectClass::Pure;
      dc.effect = cls.effect == orch::EffectClass::Effect;
      dc.stubThrough = std::max(t_.ladder.stubThrough, rungTwoThrough());   // as rung 2 may stand after this round
      dd = d_.hooks->runBeforeDispatch(dc);
    }
    ToolOutput out;
    t_.callRan = false;
    if (dd.verdict != DispatchVerdict::Run) {
      out.text = dd.text;
      out.isError = dd.verdict == DispatchVerdict::Refuse;
    } else if (!call.complete) {
      out.text = texts::tool_results::kNotRunCut;
      out.isError = true;
    } else {
      bool recorded = true;   // an effect runs only when its intent is stored
      if (cls.effect == orch::EffectClass::Effect) {
        tx::Item& i = blank(tx::ItemKind::Intent);
        i.id = call.id;
        i.n = call.name;
        Status st = put(i, &recorded);
        if (!st.ok()) return st;
      }
      if (recorded) {
        dispatchOne(call, out);
        t_.callRan = true;
        ++t_.end.tools;
      } else {
        out.text = texts::tool_results::kNotRunUnrecorded;
        out.isError = true;
      }
    }
    out.id = call.id;
    out.name = call.name;
    out.outside |= outsideByClass(view);
    t_.progress.noteCall(view.name, view.args, dd.verdict == DispatchVerdict::Answer && dd.by == HookSlot::Suppression,
                         out.isError, out.text);
    return storeResult(call, out, view);
  }

  // One dispatch. A handler that throws (anything but a refused allocation, which is the
  // turn's designed state) is an error result for this one call; the turn goes on. Nothing
  // wider is caught.
  void dispatchOne(const ToolCall& call, ToolOutput& out) {
    try {
      out = d_.dispatch(call, t_.ctx);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception&) {
      out = ToolOutput{};
      out.isError = true;
      out.text = texts::tool_results::kToolFailed;
    }
  }

  Status answerUnrun(const ToolCall& call, const char* why) {
    ToolOutput out;
    out.id = call.id;
    out.name = call.name;
    out.text = why;
    out.isError = true;
    CallView view;
    view.name = call.name;
    view.cls = orch::ToolClass{"", orch::Taint::Internal, orch::EffectClass::Read};
    return storeResult(call, out, view);
  }

  Status storeResult(const ToolCall& call, const ToolOutput& out, const CallView& view) {
    tx::Item& r = blank(tx::ItemKind::Result);
    r.id = call.id;
    r.n = call.name;
    r.ok = !out.isError;
    r.cls = out.outside ? tx::ResultClass::Outside : tx::ResultClass::Receipt;
    bool stored = true;
    Status st = capture(r, out.text, &call, &stored);
    if (!st.ok()) return st;
    if (out.outside) {
      t_.sawOutside = true;
      t_.ctx.tainted = true;   // the request now carries an outside result whole
    }
    if (r.ok && orch::tx::changesFileSet(view.name)) t_.fileSetChanged = true;
    if (r.ok && stored && t_.callRan && view.cls.effect == orch::EffectClass::Effect) keepOwn(out.text, view.args);
    if (r.ok && stored && out.deferred != Deferred::None) {   // a stored success only; run once the turn is closed
      const size_t kind = static_cast<size_t>(out.deferred);
      if (kind < kDeferredKinds) {
        t_.deferred.arg[kind] = out.deferredArg;
        t_.deferred.mask |= (uint8_t)(1u << kind);
      }
    }
    keepWrite(call, r, view);
    if (d_.hooks) {
      ResultCtx rc;
      rc.turn = &t_.ctx;
      rc.call = &call;
      rc.output = &out;
      rc.q = r.q;
      rc.stored = stored;
      d_.hooks->runAfterResult(rc);
    }
    return Status::success();
  }

  // Tier 1 inline (a receipt of at most 2,048 B), tier 2 to the bulk file (larger internal
  // results, every outside result); the stub is computed once here and stored on the line.
  Status capture(tx::Item& r, const std::string& text, const ToolCall* call, bool* stored = nullptr) {
    r.q = c_.nextQ;   // the stub names it; put() assigns the same q
    r.bytes = (uint32_t)text.size();
    r.lines = text.empty() ? 0 : 1 + (uint32_t)std::count(text.begin(), text.end(), '\n');
    if (r.cls == tx::ResultClass::Outside || text.size() > tx::kTier1Receipt) {
      if (r.cls != tx::ResultClass::Outside) r.cls = tx::ResultClass::Bulk;
      if (text.empty() || !d_.log->appendBulk(r.turn ? r.turn : t_.ctx.turn, text, r.handle).ok()) {
        r.handle.clear();   // no bulk storage: kept inline, up to the whole-result cap
        r.x = text.substr(0, tx::kBulkWholeMax);
      }
    } else {
      r.x = text;
    }
    tx::StubFacts& f = renew(t_.stub);
    f.tool = r.n;
    f.q = r.q;
    f.bytes = r.bytes;
    f.lines = r.lines;
    (void)tx::stubTime(epoch(), t_.tz, f.hhmm);
    f.preview = text.substr(0, tx::kStubPreviewBytes + 4);
    f.args = call ? call->argsJson : std::string();
    // A file read: read_file again (E2-VERB). Lane E2-FIX9: a write's result is not brought back by reading the file
    // (it shows the file as it is, not what the write answered), so only a read's result names it.
    if (r.cls != tx::ResultClass::Outside && r.n == "read_file") f.again = tx::wayBack(r.n, f.args);
    f.cls = r.cls;
    f.noCard = r.cls != tx::ResultClass::Receipt && r.handle.empty();
    f.bound = bound();
    if (!tx::stubText(f, r.st).ok()) r.st.clear();
    return put(r, stored);
  }

  // A successful, stored effect call's evidence for the frozen system part (seeOwnChanges): its result
  // when short (a note's stored line; a long result is never a note line) and the path it wrote. A read
  // or a listing is never evidence: it can show what another writer added.
  static constexpr size_t kOwnTextMax = 1024;
  void keepOwn(const std::string& text, const std::string& args) {
    if (!d_.stablePrefix) return;
    if (text.size() <= kOwnTextMax) t_.ownTexts.push_back(text);
    ArduinoJson::JsonDocument doc;
    if (deserializeJson(doc, args, ArduinoJson::DeserializationOption::NestingLimit(8))) return;
    const char* path = doc["path"] | (const char*)nullptr;
    if (!path || !*path) return;
    t_.ownPaths.push_back(path);
    const nimbus::orch::StorePath stored = nimbus::orch::storePathFor(path);   // the path as the store lists it
    if (stored.ok() && stored.str() != path) t_.ownPaths.push_back(stored.str());
  }

  // A successful effect's call and result, kept for the superseded-read rule in the view of
  // the tool they reached (a `more` call as its target, with the target's arguments).
  void keepWrite(const ToolCall& call, const tx::Item& r, const CallView& view) {
    const uint64_t q = t_.curCallQ;   // by the call's q: dialects reuse or omit call ids
    auto it = std::find_if(t_.writes.begin(), t_.writes.end(),
                           [&](const tx::Item& c) { return c.kind == tx::ItemKind::Call && c.q == q; });
    if (it == t_.writes.end()) return;
    if (!r.ok || view.cls.effect != orch::EffectClass::Effect) {
      t_.writes.erase(it);
      return;
    }
    it->n = view.name;
    it->args = view.args;
    it = t_.writes.insert(it + 1, r);
    it->n = view.name;
  }

  // ---- turn close -----------------------------------------------------------------------------
  // Delivery first: nothing that allocates much (the superseded-read scan) runs before the
  // reply in hand has gone out.
  Status close() {
    settle();
    switchedLine();
    closingFacts();
    deliver();
    Status st = storeDelivery();   // the d item and afterDeliver, before the scan below allocates
    afterDeliver();
    if (!t_.storeFailed) supersede();
    if (!t_.storeFailed) st = storeEnd();
    undoInboundIfRefused();
    seeOwnChanges();   // after delivery: it reads the notes and the file list once more
    if (!t_.storeFailed) c_.head.openTurn = 0;   // a turn with no e item stays open
    c_.head.nextQ = c_.nextQ;
    if (d_.log->isOpen()) (void)d_.log->writeHead(c_.head);   // a cache: Io loses nothing
    return st;
  }

  // The turn's status line, the superseded reads and the change points, before delivery.
  void closingFacts() {
    if (t_.cause.empty()) t_.cause = "stopped";
    if (t_.storeFailed) refusedLine();
    else if (t_.storageLineDue && !c_.unloaded) setStatus(StatusLine::StorageState, texts::device_notes::kStorageLine);
    if (t_.sawOutside) event("change:outside");
    if (t_.sawMeta) event("change:thinking");   // closed-turn thinking is not replayed (ruling 13)
    if (t_.longArgs) event("change:long-args");   // its long arguments show as stubs from now (RB-H81)
    if (d_.recentFiles && t_.fileSetChanged && !d_.stablePrefix) {
      const std::string before = c_.recent.text();
      (void)c_.recent.atClose(true, d_.recentFiles, t_.tz);
      if (c_.recent.text() != before) event("change:recent-files");
    }
  }

  // The arena refused an item: a true line for why. Over the line limit: an owner message has
  // its own line, any other part the too-long line; otherwise the arena is full.
  // The memory line only for a full arena (nothing closed left to drop); an owner message with
  // more parts or update ids than a line holds (InvalidArgument from the encoder) has its own
  // line; anything else the arena refused is a part too long to keep (ruling 59 item 4).
  void refusedLine() {
    if (t_.storeErr == Err::TooLarge && std::strcmp(t_.storeWhere, "tx::TxArena::append") == 0) {
      t_.cause = "memory";
      setStatusRef(StatusLine::CrashNotice, c_.memoryLine);
      return;
    }
    const bool user = t_.refusedKind == tx::ItemKind::User;
    const bool parts = t_.storeErr == Err::InvalidArgument && std::strcmp(t_.storeWhere, "tx::encodeLine") == 0;
    t_.cause = parts ? "too-many-parts" : "too-long";
    if (user && parts) setStatus(StatusLine::CrashNotice, texts::device_notes::kTooManyParts);
    else if (user) setStatus(StatusLine::CrashNotice, texts::device_notes::kMessageTooLong);
    else setStatus(StatusLine::CrashNotice, texts::device_notes::kTooLong);
  }

  // The d item, tried once: a refusal while it is stored never stores it twice.
  Status storeDelivery() {
    if (t_.dAttempted) return Status::success();
    t_.dAttempted = true;
    if (t_.storeFailed || !t_.out.sent) return Status::success();
    tx::Item& dItem = blank(tx::ItemKind::Delivery);
    dItem.msgIds = t_.out.msgIds;
    return put(dItem);
  }

  // afterDeliver, once, whether or not the d item could be stored (ruling 59 item 2).
  void afterDeliver() {
    if (t_.afterRan || !d_.hooks) return;
    t_.afterRan = true;
    static const std::string kNoText;
    DeliveredCtx dc;
    dc.turn = &t_.ctx;
    dc.text = t_.replyText ? t_.replyText : &kNoText;
    dc.msgIds = &t_.out.msgIds;
    dc.sent = t_.out.sent;
    d_.hooks->runAfterDeliver(dc);
  }

  // Rule (4): a successful write supersedes the earlier whole reads it made stale.
  void supersede() {
    if (t_.writes.empty()) return;
    std::vector<tx::Item> earlier(d_.arena->count());
    for (size_t i = 0; i < earlier.size(); ++i) (void)d_.arena->get(i, earlier[i]);
    resolveRouted(earlier);
    std::vector<uint64_t> sup, one;
    for (size_t i = 0; i + 1 < t_.writes.size(); ++i) {
      if (t_.writes[i].kind != tx::ItemKind::Call || t_.writes[i + 1].kind != tx::ItemKind::Result) continue;
      if (tx::supersededBy(t_.writes[i], t_.writes[i + 1], earlier, one).ok()) sup.insert(sup.end(), one.begin(), one.end());
    }
    std::sort(sup.begin(), sup.end());
    sup.erase(std::unique(sup.begin(), sup.end()), sup.end());
    if (sup.empty()) return;
    tx::Item& x = blank(tx::ItemKind::PointerMove);
    x.stub = c_.head.stub;
    x.why = tx::moveWhySlug(tx::MoveWhy::Superseded);
    x.sup = sup;
    if (put(x).ok()) event("change:superseded");
  }

  // Earlier calls through a router, and their results, as the tools they reached.
  // A result is the oldest open call's of its id, as the wire pairs them.
  void resolveRouted(std::vector<tx::Item>& items) const {
    std::vector<std::pair<std::string, std::string>> open;   // call id -> the name it reached
    for (tx::Item& it : items) {
      if (it.kind == tx::ItemKind::Call) {
        const CallView v = viewOf(it.n, it.args);
        open.emplace_back(it.id, v.name);
        it.n = v.name;
        it.args = v.args;
      } else if (it.kind == tx::ItemKind::Result) {
        for (size_t k = 0; k < open.size(); ++k)
          if (open[k].first == it.id) {
            it.n = open[k].second;
            open.erase(open.begin() + (long)k);
            break;
          }
      }
    }
  }

  // One portable deliver function (RB-E3's deliverTurn, or the binding's EngineDeps::deliver);
  // until it lands, the channel's send, the reply first and the status line as its own message.
  // Nothing is copied: the reply and the line are read where they are. Progress lands in t_.out
  // the moment each send succeeds, so the refusal path never sends a part again. withText false:
  // the status line alone (the refusal path, when the reply's own step was refused again).
  void deliver(bool withText = true) {
    if (t_.deliverDone) return;
    DeliverInput in;
    in.turn = &t_.ctx;
    if (withText && t_.replyText && !t_.out.replyDone) {
      in.text = t_.replyText;
      in.fromPart = t_.out.parts;
    }
    if (!t_.out.statusSent && t_.statusRef) {
      in.status = t_.status;
      in.statusText = t_.statusRef;
    }
    const Status st = d_.deliver ? d_.deliver(in, t_.out) : deliverTurn(DeliverDeps{d_.channel, d_.hooks, d_.log}, in, t_.out);
    if (st.notImplemented()) sendEach(in);
    t_.deliverDone = withText;
  }
  void sendEach(const DeliverInput& in) {
    if (!d_.channel || !d_.channel->send) return;
    if (in.text && !in.text->empty() && d_.channel->send(t_.ctx.chatId, *in.text)) {
      t_.out.sent = t_.out.replyDone = true;
      t_.out.parts = 1;
      t_.out.bytes = (uint32_t)in.text->size();
    }
    if (in.statusText && !in.statusText->empty() && d_.channel->send(t_.ctx.chatId, *in.statusText))
      t_.out.statusSent = true;
  }

  Status storeEnd() {
    if (t_.endAttempted) return t_.endStored ? Status::success() : Status::fail(Err::NotStored, "plain::storeEnd");
    t_.endAttempted = true;   // tried once: a refusal inside the store never leaves two e items
    t_.end.cause = t_.cause;
    t_.end.rounds = t_.rounds;
    t_.end.ms = elapsedMs();
    tx::Item& e = blank(tx::ItemKind::TurnEnd);
    if (!turnEndItem(t_.ctx, t_.end, e).ok()) {   // RB-E3; until then the facts as they are
      blank(tx::ItemKind::TurnEnd);
      e.end = t_.end;
    }
    Status st = put(e);
    if (st.ok()) c_.lastClosedQ = e.q;
    t_.endStored = st.ok();
    if (st.ok()) keepForInspector(e);
    return st;
  }

  // Lane E2-P-SURF: what a render of this turn's requests needs, kept until the next turn closes. A
  // refused allocation keeps nothing (the inspector then says there is no turn): never a failed turn.
  void keepForInspector(const tx::Item& e) {
    c_.last.reset();
    try {
      std::unique_ptr<LastTurn> l(new LastTurn());
      l->ctx = t_.ctx;
      l->ctx.who = nullptr;
      l->system = t_.system;
      l->tools = t_.tools;
      l->head = t_.setup.head;
      l->head.key = [] { return std::string(); };   // the key is never copied
      l->head.bearer = nullptr;   // nor a connector's token (lane E2-P-CONN2)
      l->wire = t_.wire;
      l->end = e.end;
      l->end.promptHash = t_.end.promptHash;
      l->end.manifestHash = t_.end.manifestHash;
      l->endQ = e.q;
      l->bound = bound();
      l->photo = t_.setup.photo;
      l->longCache = d_.stablePrefix;
      l->refusedPhotos = c_.head.refusedPhotos;
      c_.last = std::move(l);
    } catch (const std::bad_alloc&) {
      c_.last.reset();
    }
  }

 public:
  // A refused allocation stopped the turn (ruling 30), a designed state: the owner gets the
  // reply in hand (the answer, else the held reply) and the memory line, unless delivery had
  // already finished; nothing already sent is sent again; then the d item and afterDeliver,
  // the e item and head.json where storage allows. Each step stands alone: a second refusal
  // skips only that step, and nothing here lets a refusal leave.
  // Only std::bad_alloc is caught here; anything else leaves as it would from the turn.
  void refused() {
    try {
      settle();
    } catch (const std::bad_alloc&) {
    }
    if (!t_.deliverDone) {   // the memory line, kept since boot: it needs no allocation
      t_.cause.clear();
      t_.cause.append("memory");   // within the string's own buffer
      setStatusRef(StatusLine::CrashNotice, c_.memoryLine);
    }
    deliverAfterRefusal();
    try {
      (void)storeDelivery();
    } catch (const std::bad_alloc&) {
    }
    try {
      afterDeliver();
    } catch (const std::bad_alloc&) {
    }
    try {
      if (!t_.storeFailed && storeEnd().ok()) c_.head.openTurn = 0;
      c_.head.nextQ = c_.nextQ;
      if (d_.log->isOpen()) (void)d_.log->writeHead(c_.head);
    } catch (const std::bad_alloc&) {
    }
  }

 private:
  // The reply and the line as far as they had not gone out; when the reply's own step is
  // refused again, the memory line alone (the status step stands apart from the text step).
  void deliverAfterRefusal() {
    try {
      deliver();
      return;
    } catch (const std::bad_alloc&) {
    }
    if (!t_.out.replyDone && !t_.out.statusSent && rank(t_.status) < rank(StatusLine::FailedWrite)) {
      t_.status = StatusLine::CrashNotice;   // the reply is lost: the memory line, never a weaker one
      t_.statusRef = &c_.memoryLine;
    }
    try {
      deliver(false);
    } catch (const std::bad_alloc&) {
    }
  }

 private:
  const EngineDeps& d_;
  PlainEngine::ChatState& c_;
  Turn& t_;
};

bool depsReady(const EngineDeps& d) {
  return d.platform && d.platform->nowMs && d.platform->delayMs && d.transport && d.log && d.arena && d.dispatch;
}

}  // namespace

namespace {

// A turn end stored between turns (the resume seam closes a turn at boot, lane RB-X1): the last
// closed turn is the newest e item the engine has not seen, wherever it came from.
void syncClosed(PlainEngine::ChatState& c, const tx::TxArena& arena) {
  for (size_t i = arena.count(); i-- > 0;) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok() || e.q < c.nextQ) return;
    if (e.kind == tx::ItemKind::TurnEnd) {
      c.lastClosedQ = std::max(c.lastClosedQ, e.q);
      return;
    }
  }
}

}  // namespace

PlainEngine::PlainEngine(const EngineDeps& deps) : deps_(deps), chat_(new (std::nothrow) ChatState()) {}

bool PlainEngine::unloaded() const { return chat_ && chat_->booted && chat_->unloaded; }
PlainEngine::~PlainEngine() = default;

tx::HeadState* PlainEngine::head() { return chat_ && chat_->booted ? &chat_->head : nullptr; }
std::vector<tx::DigestEntry>* PlainEngine::digests() { return chat_ && chat_->booted ? &chat_->live : nullptr; }
uint64_t PlainEngine::omittedThru() const { return chat_ && chat_->booted ? chat_->droppedThru : 0; }

const LastTurn* PlainEngine::lastTurn() const { return chat_ && chat_->booted ? chat_->last.get() : nullptr; }

Status PlainEngine::rebuildLast(size_t exchange, HttpRequest& out, tx::ArenaAllocFn alloc, tx::ArenaFreeFn release) const {
  const LastTurn* l = lastTurn();
  if (!l || !l->wire || !deps_.arena || !deps_.log) return Status::fail(Err::NotFound, "plain::PlainEngine::rebuildLast");
  RebuildInput in;
  in.end = &l->end;
  in.exchange = exchange;
  in.turn = &l->ctx;
  in.head = &l->head;
  in.wire = l->wire;
  in.system = &l->system;
  in.tools = &l->tools;
  in.items = deps_.arena;
  tx::TxLog* log = deps_.log;
  in.bulk = [log](const std::string& handle, std::string& o) { return log->readBulk(handle, 0, tx::kBulkMaxBytes, o); };
  in.photo = l->photo;
  in.bound = l->bound;
  in.longCache = l->longCache;
  in.refusedPhotos = &l->refusedPhotos;
  in.alloc = alloc;
  in.release = release;
  return rebuildRequest(in, out);
}

Status PlainEngine::runTurn(const TurnInput& in, TurnOutcome& out) {
  out = TurnOutcome{};
  if (!depsReady(deps_) || !chat_ || !chat_->booted) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::runTurn");
  std::unique_ptr<Turn> t(new (std::nothrow) Turn());
  if (!t) {   // not even the turn's state: the owner still hears it (nothing was stored)
    out.status = StatusLine::CrashNotice;
    try {
      if (deps_.channel && deps_.channel->send) (void)deps_.channel->send(in.ctx.chatId, chat_->memoryLine);
    } catch (const std::bad_alloc&) {
    }
    return Status::fail(Err::TooLarge, "plain::PlainEngine::runTurn.memory");
  }
  // Items stored between turns (the compaction driver's digests, the resume seam's items at
  // boot) take their q from head.nextQ: the turn's first item comes after them, and a turn the
  // resume seam closed counts as the last closed turn.
  syncClosed(*chat_, *deps_.arena);
  chat_->nextQ = std::max({chat_->nextQ, chat_->head.nextQ, deps_.arena->lastQ() + 1});
  Status st = Status::success();
  try {
    st = TurnRun(deps_, *chat_, *t).run(in);
  } catch (const std::bad_alloc&) {
    // A refused allocation is a designed state: what was stored stays, the owner is told, a
    // call left without its result is answered "outcome unknown" at the next turn open.
    TurnRun(deps_, *chat_, *t).refused();
    st = Status::fail(Err::TooLarge, "plain::PlainEngine::runTurn.memory");
  }
  // The plain fields first; the copies that allocate each stand alone (a refusal leaves them
  // empty, never the call).
  out.delivered = t->out.sent;
  out.rounds = t->rounds;
  out.toolCalls = t->end.tools;
  out.headRequests = t->end.headRequests;
  out.trip = t->trip;
  out.status = t->status;
  out.parkHost = t->parkHost;
  out.parkStatus = t->parkStatus;
  out.failOver = t->failOver;
  out.modelUnusable = t->modelUnusable;
  out.switches = t->switches;
  // A deferred effect's bit is set only once its argument is copied: never a head switch without its head.
  for (size_t k = 0; k < kDeferredKinds; ++k) {
    if (!((t->deferred.mask >> k) & 1u)) continue;
    try {
      out.deferred.arg[k] = t->deferred.arg[k];
      out.deferred.mask |= (uint8_t)(1u << k);
    } catch (const std::bad_alloc&) {
    }
  }
  try {
    if (t->replyText) out.text = *t->replyText;
    if (t->statusRef) {
      out.statusText = *t->statusRef;
      out.statusStored = t->storedLine();
    }
    out.end = t->end;
    out.end.cause = t->cause;   // also when no e item could be stored
    out.parkText = t->parkText;
    out.shares = t->shares;
    out.fallbackRefusals = t->fallbackRefusals;
  } catch (const std::bad_alloc&) {
  }
  return st;
}

Status PlainEngine::idle(const IdleCtx& ctx, IdleOutcome& out) {
  out = IdleOutcome{};
  if (!chat_ || !chat_->booted) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::idle");
  if (chat_->unloaded) return Status::success();   // no compaction over RAM-only numbers
  if (deps_.hooks) out = deps_.hooks->runIdleStep(ctx);
  return Status::success();
}

namespace {

// What the filled arena says about the chat: the next q, the last turn, the last closed turn
// and the digests in force.
void settleLoaded(const EngineDeps& deps, PlainEngine::ChatState& c) {
  c.nextQ = std::max<uint64_t>(std::max<uint64_t>(c.head.nextQ, deps.arena->lastQ() + 1), 1);
  for (size_t i = 0; i < deps.arena->count(); ++i) {
    tx::ArenaEntry e;
    if (!deps.arena->header(i, e).ok()) continue;
    c.lastTurn = std::max(c.lastTurn, e.turn);
    if (e.kind == tx::ItemKind::TurnEnd) c.lastClosedQ = e.q;
  }
  c.live.clear();
  if (deps.files && deps.log->isOpen() && tx::loadDigests(*deps.files, deps.chatKey, c.head, c.live).ok()) {
    std::string block;
    tx::SettleReport rep;
    (void)tx::settleDigests(*deps.files, deps.chatKey, tx::profileOfHead(c.head.profile), c.live, block, rep);
  }
}

// The chat from storage at boot: the log opened, the arena filled after thru, then
// settleLoaded. Io: on a device with no card fitted (EngineDeps::noCard) the chat runs from RAM
// by design; with a card fitted the chat is marked unloaded (the stored conversation exists or
// cannot be ruled out) and reopen() loads it later.
Status loadChat(const EngineDeps& deps, PlainEngine::ChatState& c) {
  c = PlainEngine::ChatState{};
  c.memoryLine = texts::device_notes::kNoMemory;
  if (deps.holdUnloaded && deps.holdUnloaded()) {   // P3: nothing is stored before the import's Done
    c.unloaded = true;
    hlog::logf("plain(%s): the earlier conversation is not imported yet: held in RAM", deps.chatKey.c_str());
    settleLoaded(deps, c);
    c.booted = true;
    return Status::success();
  }
  Status st = deps.log->open(c.head);
  if (!st.ok() && st.err != Err::Io) return st;
  if (st.ok()) {
    tx::FillReport report;
    if (!(st = deps.log->fill(c.head, *deps.arena, report)).ok()) return st;
    c.droppedThru = report.droppedThru;
  } else {
    c.unloaded = !(deps.noCard && deps.noCard());
    if (c.unloaded) hlog::logf("plain(%s): the stored conversation could not be loaded: held in RAM", deps.chatKey.c_str());
  }
  settleLoaded(deps, c);
  c.booted = true;
  return Status::success();
}

// A RAM item's own numbers moved past the stored ones, and the pointers its stub or long-text
// head wrote ("r<q>", "view id=r<q>") moved with them. Only device-written fields are touched;
// the owner's and the model's words are never rewritten.
bool isDigit(char ch) { return ch >= '0' && ch <= '9'; }

// "r<digits>" at `i`, after a space, '=' or '(' and not followed by a digit: its number and end.
bool pointerAt(const std::string& s, size_t i, size_t& end, uint64_t& q) {
  if (s[i] != 'r' || i == 0 || (s[i - 1] != ' ' && s[i - 1] != '=' && s[i - 1] != '(')) return false;
  end = i + 1;
  while (end < s.size() && isDigit(s[end])) ++end;
  if (end == i + 1 || end - i > 20) return false;
  q = std::strtoull(s.substr(i + 1, end - i - 1).c_str(), nullptr, 10);
  return true;
}

void shiftPointers(std::string& s, uint64_t lastRamQ, uint64_t shift) {
  if (!shift || s.empty()) return;
  std::string out;
  out.reserve(s.size() + 16);
  for (size_t i = 0; i < s.size();) {
    size_t end = 0;
    uint64_t q = 0;
    if (pointerAt(s, i, end, q) && q >= 1 && q <= lastRamQ) {
      out += 'r';
      out += std::to_string(q + shift);
      i = end;
      continue;
    }
    out += s[i++];
  }
  s.swap(out);
}

uint32_t turnRoom(uint64_t shift) { return static_cast<uint32_t>(std::min<uint64_t>(shift, UINT32_MAX / 2)); }

// The RAM items after the stored conversation, in order, each once (lane E2-STORE, finding S5).
// The first append fixes the shifts: q past the stored next q, turn past it too (a turn number
// is never above its items' q). A refusal stops the merge with what is left still in RAM; the
// next reopen() resumes after mergedThru.
Status mergeRam(const EngineDeps& deps, PlainEngine::ChatState& c) {
  const uint64_t lastRamQ = c.nextQ ? c.nextQ - 1 : 0;
  for (size_t i = 0; i < deps.arena->count(); ++i) {
    tx::Item it;
    Status st = deps.arena->get(i, it);
    if (!st.ok()) return st;
    if (it.q <= c.mergedThru) continue;
    const uint64_t storedNext = deps.log->lastQ() + 1;
    if (!c.shiftSet) {
      c.qShift = storedNext > it.q ? storedNext - it.q : 0;
      c.turnShift = it.turn && storedNext > it.turn ? turnRoom(storedNext - it.turn) : 0;
      c.shiftSet = true;
    } else if (it.q + c.qShift < storedNext) {   // a line the log counted that is not ours
      const uint64_t extra = storedNext - (it.q + c.qShift);
      c.qShift += extra;
      c.turnShift = turnRoom(c.turnShift + extra);
    }
    const uint64_t ramQ = it.q;
    it.q += c.qShift;
    if (it.turn) it.turn += c.turnShift;
    shiftPointers(it.st, lastRamQ, c.qShift);
    if (it.kind == tx::ItemKind::Assistant && !it.handle.empty()) shiftPointers(it.x, lastRamQ, c.qShift);
    tx::StoreState state = tx::StoreState::NotStored;
    st = deps.log->append(it, state);
    if (!st.ok()) return st;
    if (state != tx::StoreState::Stored) return Status::fail(Err::NotStored, "plain::reopen.merge");
    c.mergedThru = ramQ;
  }
  return Status::success();
}

// reopen(): open the log, merge, refill the arena from the log. Any refusal closes the log again
// (nothing more is written until the merge can finish) and leaves the chat unloaded.
Status reopenChat(const EngineDeps& deps, PlainEngine::ChatState& c) {
  tx::HeadState h;
  Status st = deps.log->open(h);
  if (st.ok()) st = mergeRam(deps, c);
  if (!st.ok()) {
    deps.log->close();
    return st;
  }
  // Every RAM item is on the card now: the arena is refilled from the log, so it holds the
  // stored conversation and the merged turns under their stored numbers.
  const uint32_t lastTurn = c.lastTurn + c.turnShift;
  h.nextQ = std::max<uint64_t>(h.nextQ, deps.log->lastQ() + 1);
  deps.arena->clear();
  tx::FillReport report;
  try {
    st = deps.log->fill(h, *deps.arena, report);
  } catch (const std::bad_alloc&) {
    st = Status::fail(Err::TooLarge, "plain::reopen.fill");
  }
  if (!st.ok()) {
    // A partial fill: what the arena holds is stored, what it misses is on the card. The chat
    // stays unloaded (closed log) with every q up to the log's last counted as merged.
    deps.log->close();
    c.mergedThru = std::max(deps.log->lastQ(), deps.arena->lastQ());
    c.qShift = 0;
    c.turnShift = 0;
    c.shiftSet = false;
    c.lastTurn = lastTurn;
    c.nextQ = std::max<uint64_t>(c.nextQ, c.mergedThru + 1);
    return st;
  }
  const std::string memoryLine = c.memoryLine;
  tx::RecentFilesBlock recent = c.recent;
  for (const std::string& ref : c.head.refusedPhotos)   // E2-REP4: refused in RAM, kept (bounded, oldest out)
    if (std::find(h.refusedPhotos.begin(), h.refusedPhotos.end(), ref) == h.refusedPhotos.end()) h.refusedPhotos.push_back(ref);
  while (h.refusedPhotos.size() > tx::kHeadMaxRefusedPhotos) h.refusedPhotos.erase(h.refusedPhotos.begin());
  c = PlainEngine::ChatState{};   // afterRestart: the next turn opens cold over the whole conversation
  c.memoryLine = memoryLine;
  c.recent = recent;
  c.head = h;
  c.droppedThru = report.droppedThru;
  settleLoaded(deps, c);
  c.lastTurn = std::max(c.lastTurn, lastTurn);
  c.booted = true;
  (void)deps.log->writeHead(c.head);   // a cache: Io loses nothing
  hlog::logf("plain(%s): the stored conversation is loaded again; the RAM turns follow it", deps.chatKey.c_str());
  return Status::success();
}

}  // namespace

// A refused allocation anywhere in boot, the resume seam included, is a status.
Status PlainEngine::boot(const BootCtx& ctx) {
  if (!depsReady(deps_) || !chat_) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::boot");
  // Ruling 91 blockers/F6: no compaction installed is never silent.
  if (!deps_.noCompaction && (!deps_.hooks || !deps_.hooks->has(Seam::IdleStep, HookSlot::Compaction))) {
    hlog::logf("plain(%s): no compaction installed (HookSlot::Compaction): boot refused", deps_.chatKey.c_str());
    return Status::fail(Err::InvalidArgument, "plain::PlainEngine::boot.compaction");
  }
  if (deps_.noCompaction) hlog::logf("plain(%s): no compaction, by the binding: %s", deps_.chatKey.c_str(), deps_.noCompaction);
  try {
    const Status st = loadChat(deps_, *chat_);
    if (!st.ok()) return st;
    return deps_.hooks ? deps_.hooks->runBootResume(ctx) : Status::success();
  } catch (const std::bad_alloc&) {
    return Status::fail(Err::TooLarge, "plain::PlainEngine::boot.memory");
  }
}

Status PlainEngine::reopen() {
  if (!depsReady(deps_) || !chat_ || !chat_->booted) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::reopen");
  if (!chat_->unloaded) return Status::success();
  try {
    return reopenChat(deps_, *chat_);
  } catch (const std::bad_alloc&) {
    deps_.log->close();   // a refusal before the refill: the arena still holds every RAM item
    return Status::fail(Err::TooLarge, "plain::PlainEngine::reopen.memory");
  }
}

Status PlainEngine::reopenAs(tx::TxLog& log, const std::string& chatKey) {
  if (!depsReady(deps_) || !chat_ || !chat_->booted || !chat_->unloaded || !tx::chatKeyOk(chatKey) || chatKey == deps_.chatKey)
    return Status::fail(Err::InvalidArgument, "plain::PlainEngine::reopenAs");
  std::unique_ptr<EngineDeps> d(new (std::nothrow) EngineDeps(deps_));   // off the poll task's stack
  if (!d) return Status::fail(Err::TooLarge, "plain::PlainEngine::reopenAs.memory");
  d->log = &log;
  d->chatKey = chatKey;
  try {
    return reopenChat(*d, *chat_);
  } catch (const std::bad_alloc&) {
    log.close();
    return Status::fail(Err::TooLarge, "plain::PlainEngine::reopenAs.memory");
  }
}

}  // namespace plain
}  // namespace agent
