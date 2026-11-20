// The plain engine (plain/engine.h): the simplest agent loop with durability around it.
// Lane RB-E1a; RB-E1b is the second and last holder (guards scaled to the head,
// suppression, deferred effects).
#include "nimbus/harness/plain/engine.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "nimbus/harness/channel.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/platform.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/rules.h"

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
  uint32_t lastTurn = 0;
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
};

namespace {

uint32_t fnv1a(uint32_t h, const std::string& s) {
  for (unsigned char ch : s) h = (h ^ ch) * 16777619u;
  return h;
}

// One turn's state. It lives on the heap so runTurn's frame stays small.
struct Turn {
  TurnCtx ctx;
  TurnSetup setup;
  SystemBlocks system;
  std::vector<ToolSpec> tools;
  const WireCodec* wire = nullptr;
  uint32_t bodyCap = 0;                // the head's body cap (budget()); 0 only before it runs
  bool floorHead = false;              // the head's class (tx::HeadBudget::floorHead)
  GuardLimits limits;
  LadderState ladder;
  RenderInput render;
  ExchangeResult ex;
  int16_t tz = 0;
  uint32_t startMs = 0;
  uint16_t rounds = 0;                 // answered requests
  uint16_t toolRounds = 0;             // rounds whose calls ran (the round guard counts these)
  std::vector<uint64_t> roundStart;    // the first q of each answered round (ladder rung 2)
  bool lastRound = false;              // a guard tripped: this request goes with tool choice none
  bool held = false;                   // beforeDeliver held the reply once
  std::string heldText;                // the held reply: delivered if the extra round fails
  std::vector<uint64_t> callQ;         // the q of each call of the round being run (pairing)
  uint64_t curCallQ = 0;               // the q of the call whose result is being stored
  bool storeFailed = false;            // the arena refused an item: the turn ends
  bool storageLineDue = false;
  bool fileSetChanged = false;
  bool sawOutside = false;
  bool sawMeta = false;
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
  const std::string* statusRef = nullptr;   // the line that goes out: statusText or a kept line
  bool parkHost = false, failOver = false;
  std::string cause;
  tx::TurnEndFacts end;
  std::vector<tx::Item> writes;        // c, r pairs of successful effect calls (supersede rule)
  // Scratch items, reused: an Item is about 0.5 KB, too big for the task stack's frames.
  tx::Item item;                       // the item being built and stored
  tx::Item read;                       // an item decoded from the arena
  tx::StubFacts stub;
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
      if (step.calls.empty() || t_.lastRound) {                            // no call: the text is the reply
        if (!(st = finalAnswer(step)).ok() || t_.answered) break;
        continue;                                                          // held once: one more round
      }
      if ((st = storeAnswer(step, false)).ok()) st = runCalls(step);       // each result stored in turn
    }
    const Status closed = close();
    return st.ok() ? closed : st;
  }

 private:
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
    if (!n || !tx::omissionLine(n, dates, t_.ladder.omissionLine).ok()) return false;
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
    if (!(st = d_.setup(t_.ctx, t_.setup)).ok()) return fail(st);
    t_.ctx.dayStartMin = t_.setup.dayStartMin;   // the wires stamp every message with it
    if (!(st = budget()).ok()) return fail(st);
    if (!(st = storeOpen(in)).ok() || !(st = movePointer()).ok()) return st;
    if (!(st = compose()).ok()) return fail(st);
    if (!t_.wire && !(st = wireFor(t_.ctx.dialect, t_.wire)).ok()) return fail(st);
    if (!t_.wire) return fail(Status::fail(Err::NotFound, "plain::wireFor"));
    if (!guardLimitsFor(t_.setup.storedRounds, t_.setup.cost, t_.limits).ok()) t_.limits = GuardLimits{};
    // Whatever the scale, every round has a bound and the last round has time before the hard wall.
    t_.limits.rounds = std::max<uint16_t>(1, std::min<uint16_t>(t_.limits.rounds, kMaxRounds));
    t_.limits.softWallMs = std::min<uint32_t>(t_.limits.softWallMs, kHardWallMs - 2 * kMinRequestMs);
    return Status::success();
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

  // The owner message (u) before any model call; another trigger's text as its device note.
  Status storeInput(const TurnInput& in) {
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
    const Status st = put(u);
    if (!st.ok()) return st;
    for (uint64_t s : in.src) c_.head.hw = std::max(c_.head.hw, s);
    c_.head.mHi = std::max(c_.head.mHi, in.m);
    return st;
  }

  // The head's budget (tx::budgetFor). Never the floor by accident: a key the table cannot read
  // takes the head's released row (orch::requestBudgetFor), and a floor head is its class. A turn
  // whose setup names no head is a binding fault, refused loudly (ruling 89 item 3).
  Status budget() {
    tx::HeadKey key;
    key.host = providerSlugOf(t_.ctx, t_.setup.head);
    key.model = t_.ctx.model;
    key.routed = t_.setup.head.routed;
    if (key.host.empty() || key.model.empty()) {
      agent::hlog::logf("plain(%s): no head named for the turn: refused", t_.ctx.chatKey.c_str());
      return Status::fail(Err::InvalidArgument, "plain::PlainEngine::budget.head");
    }
    tx::HeadBudget b;
    if (tx::budgetFor(key, t_.ctx.profile, b).ok()) {
      t_.bodyCap = b.bodyCap;
      t_.floorHead = b.floorHead;
      t_.ctx.profile = b.profile;
      return Status::success();
    }
    const orch::RequestBudget r = orch::requestBudgetFor(key.host, key.model, key.routed);
    t_.bodyCap = r.wireBodyBytes;
    t_.floorHead = r.bound == orch::BudgetBound::Floor || r.bound == orch::BudgetBound::Held;
    return Status::success();
  }
  bool floorHead() const { return t_.floorHead; }

  Status storeOpen(const TurnInput& in) {
    tx::Item& t = blank(tx::ItemKind::TurnOpen);
    t.trigger = triggerSlug(t_.ctx.trigger);
    t.head = t_.ctx.host + "/" + t_.ctx.model;
    t.profile = tx::profileSlug(t_.ctx.profile);
    t.attempt = t_.ctx.attempt;
    for (const tx::Part& p : in.parts)
      if (p.kind == tx::PartKind::Image) t.photos.push_back(p.ref);
    c_.head.openTurn = t_.ctx.turn;
    c_.head.attempts = t_.ctx.attempt;
    c_.head.profile = t.profile;
    return put(t);
  }

  bool cold() const {
    const uint64_t now = epoch(), last = c_.head.lastReqAt;
    return c_.afterRestart || now == 0 || last == 0 || now < last || now - last > tx::kColdGapS;
  }

  // Rules (1) and (2) of the stub pointer, once, stored before the first request.
  Status movePointer() {
    tx::OpenFacts f;
    f.now = epoch();
    f.lastReqAt = c_.head.lastReqAt;
    f.afterRestart = c_.afterRestart;
    f.pointer = c_.head.stub;
    f.lastClosedTurnQ = c_.lastClosedQ;
    f.wholeBulkAbove = bulkAbove(c_.head.stub);
    f.floorHead = floorHead();
    tx::PointerMove m;
    (void)tx::decideTurnOpen(f, m);
    const bool wasCold = cold();
    c_.afterRestart = false;
    if (d_.recentFiles) {
      const std::string before = c_.recent.text();
      (void)c_.recent.atOpen(wasCold, d_.recentFiles, t_.tz);
      if (c_.recent.text() != before) event("change:recent-files");
    }
    if (!m.move) return Status::success();
    tx::Item& x = blank(tx::ItemKind::PointerMove);
    x.stub = m.to;
    x.why = tx::moveWhySlug(m.why);
    Status st = put(x);
    if (st.ok()) {
      c_.head.stub = m.to;
      event("change:pointer");
    }
    return st;
  }

  uint32_t bulkAbove(uint64_t pointer) const {
    uint64_t sum = 0;
    tx::Item& it = t_.read;
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok() || e.q <= pointer || e.kind != tx::ItemKind::Result) continue;
      if (d_.arena->get(i, it).ok() && it.cls == tx::ResultClass::Bulk) sum += it.bytes;
    }
    return (uint32_t)std::min<uint64_t>(sum, UINT32_MAX);
  }

  // The system text and the tools, composed once and kept for every round of the turn.
  Status compose() {
    RequestFacts& f = t_.setup.facts;
    f.turn = &t_.ctx;
    std::vector<tx::DigestEntry> archive;
    f.digestBlock.clear();
    if (!c_.live.empty()) (void)tx::digestBlock(c_.live, t_.ctx.profile, f.digestBlock, archive);
    f.recentFiles = c_.recent.text();
    Status st = composeSystem(f, t_.system);
    if (st.ok()) st = toolManifest(f, t_.tools);
    if (!st.ok()) return st;
    t_.wire = t_.setup.wire;
    t_.end.promptHash = fnv1a(fnv1a(fnv1a(2166136261u, t_.system.core), t_.system.notes), t_.system.digests);
    uint32_t h = 2166136261u;
    for (const ToolSpec& s : t_.tools) h = fnv1a(fnv1a(fnv1a(h, s.name), s.description), s.schemaJson);
    t_.end.manifestHash = h;
    return Status::success();
  }

  // ---- one request ----------------------------------------------------------------------
  // The round boundary: the not-stored note, then the guards (one device item and one last
  // round with tool choice none on a trip).
  Status boundary() {
    t_.ctx.roundNo = t_.rounds;
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
    GuardState g;
    g.rounds = t_.toolRounds;
    g.elapsedMs = elapsedMs();
    g.promptTokens = t_.sizeTokens;
    g.spendMicroUsd = t_.end.microUsd;
    GuardTrip trip = GuardTrip::None;
    (void)checkGuards(t_.limits, g, trip);   // the limits are clamped at open: never InvalidArgument
    if (trip == GuardTrip::None) return Status::success();
    t_.trip = trip;
    t_.lastRound = true;
    event(std::string("trip:") + guardTripSlug(trip));
    return note(std::string(texts::device_notes::kLimitReached) + guardTripSlug(trip) +
                    texts::device_notes::kLimitReachedEnd,
                tx::NoteClass::Limit);
  }

  Status ask() {
    RenderInput& r = t_.render;
    r.turn = &t_.ctx;
    r.head = &t_.setup.head;
    r.system = &t_.system;
    r.tools = &t_.tools;
    r.items = d_.arena;
    r.stubPointer = c_.head.stub;
    r.toolChoiceNone = t_.lastRound;
    r.photo = t_.setup.photo;
    r.ladder = &t_.ladder;
    tx::TxLog* log = d_.log;
    r.bulk = [log](const std::string& handle, std::string& out) { return log->readBulk(handle, 0, tx::kBulkMaxBytes, out); };
    // The soft wall for every round; the guard's last round may run to the hard wall.
    const uint32_t wall = t_.lastRound ? kHardWallMs : t_.limits.softWallMs, used = elapsedMs();
    ExchangeRequest req;
    req.wire = t_.wire;
    req.render = &r;
    req.deadlineMs = used < wall ? wall - used : 1;
    req.bodyCap = t_.bodyCap;
    req.climb = [this](const ClimbAsk& ask) { return climb(ask); };
    t_.ex = ExchangeResult{};
    const Status st = runExchange(ExchangeDeps{d_.transport, d_.platform}, req, t_.ex);
    account(t_.ex);
    if (!st.ok()) return failed(ExchangeErr::Other, st);
    if (t_.ex.err == ExchangeErr::Cut) return cut();
    if (t_.ex.err != ExchangeErr::None) return failed(t_.ex.err, Status::success());
    ++t_.rounds;
    return Status::success();
  }

  void account(const ExchangeResult& ex) {
    t_.end.headRequests += ex.requests;
    if (ex.requests) {
      c_.head.lastReqAt = epoch();
      t_.end.exchanges.push_back(exchangeFacts(ex));   // with the request by reference (RB-E3)
    }
    const orch::TokenUsage& u = ex.step.usage;
    t_.end.promptTokens += u.promptTokens;
    t_.end.outputTokens += u.completionTokens;
    t_.end.cacheReadTokens += u.cacheReadTokens;
    t_.end.cacheWriteTokens += u.cacheWriteTokens;
    // The guards' inputs: size counts every prompt token (cached ones too); spend prices them. An
    // answer that reports no usage at all still counts: a floor from the request's bytes (about 3.5 B
    // a token, kDefaultRequestTokens' rule) at the input rate, named by an event (ruling 79 item 1).
    RequestTokens rt{u.promptTokens, u.completionTokens, u.cacheReadTokens, u.cacheWriteTokens};
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
    t_.parkHost |= ex.parkHost;
    t_.failOver |= ex.failOver;
  }

  // Nothing came from the model: the exchange's one plain line ends the turn.
  Status failed(ExchangeErr err, const Status& st) {
    if (t_.lastRound && t_.trip != GuardTrip::None) {   // the guard stopped the turn, not the provider
      t_.cause = std::string("limit:") + guardTripSlug(t_.trip);
      setStatus(StatusLine::LimitReached, limitLine());
      if (err != t_.ex.err) t_.ex.err = err;
      return st;
    }
    t_.cause = std::string("provider:") + exchangeErrSlug(err);   // a held reply goes out: settle()
    setStatus(StatusLine::ProviderError, t_.ex.errorLine.empty()
                                             ? exchangeLine(err, providerSlugOf(t_.ctx, t_.setup.head), 0)
                                             : t_.ex.errorLine);
    if (err != t_.ex.err) t_.ex.err = err;
    return st;
  }

  // A cut answer: its text is delivered with the cut line; its calls are never run.
  Status cut() {
    ++t_.rounds;
    t_.answer.swap(t_.ex.step.text);   // in hand before anything is stored, never copied
    t_.cause = "cut";
    setStatus(t_.ex.status, t_.ex.errorLine);
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
    bool climbed = false;
    if (over && !l.closedThrough && closedBulkAbovePointer()) {
      l.closedThrough = c_.lastClosedQ;   // (1) every closed turn as stubs
      climbed = true;
    } else if (over && openBulkBelow(rungTwoThrough())) {
      l.stubThrough = rungTwoThrough();   // (2) this turn's bulk older than the newest two rounds
      climbed = true;
    } else {
      const size_t need = over && ask.bodyBytes > ask.bodyCap ? ask.bodyBytes - ask.bodyCap + kOmissionLineRoom
                                                             : std::numeric_limits<size_t>::max();
      climbed = omitOldest(need);   // (3) one omission line, never the newest 6 owner turns
    }
    if (climbed) event("change:ladder");
    return climbed;
  }
  static constexpr size_t kOmissionLineRoom = 160;   // the line and its block (under 120 B of text)

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
      if (!d_.arena->header(i, e).ok() || e.kind != tx::ItemKind::Result || e.turn != t_.ctx.turn) continue;
      if (e.q <= t_.ladder.stubThrough || e.q > through) continue;
      if (d_.arena->get(i, it).ok() && it.cls == tx::ResultClass::Bulk) return true;
    }
    return false;
  }

  // Tier-2 bulk of a closed turn shown whole above the pointer: what rung 1 would stub.
  bool closedBulkAbovePointer() const {
    tx::Item& it = t_.read;
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok() || e.kind != tx::ItemKind::Result) continue;
      if (e.q <= c_.head.stub || e.q > c_.lastClosedQ) continue;
      if (d_.arena->get(i, it).ok() && it.cls == tx::ResultClass::Bulk) return true;
    }
    return false;
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
    if (e.kind == tx::ItemKind::Call) return it.id.size() + it.n.size() + it.args.size();
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
    const bool heldGoesOut = t_.held && !t_.heldText.empty();
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
  void setStatus(StatusLine s, const std::string& text) {
    if (rank(s) <= rank(t_.status)) return;
    t_.statusText = text;   // copied first: a refused copy leaves the earlier line whole
    t_.status = s;
    t_.statusRef = &t_.statusText;
  }
  // A line kept outside the turn (the memory line): taken with no allocation.
  void setStatusRef(StatusLine s, const std::string& kept) noexcept {
    if (rank(s) <= rank(t_.status)) return;
    t_.status = s;
    t_.statusRef = &kept;
  }
  void takeStatus(const DeliverDecision& dd) { setStatus(dd.status, dd.statusText); }
  // The line for a turn a guard ended before a reply: it names the guard (ruling 79 item 4).
  std::string limitLine() const {
    return std::string(texts::device_notes::kLimitLine) + guardTripSlug(t_.trip) + texts::device_notes::kLimitLineEnd;
  }

  // What the owner gets, decided in one place (close and the refusal path): the model's reply
  // if there is one, else the reply held before a failed or empty extra round, else a plain
  // line: the limit line when the last allowed round only called tools, the provider's line
  // for an empty answer. A routine's empty text stays its silence.
  void settle() {
    t_.replyText = !t_.answer.empty() ? &t_.answer : t_.held && !t_.heldText.empty() ? &t_.heldText : nullptr;
    t_.reply = t_.replyText != nullptr;   // a pointer: the reply is never copied to go out
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
      if (!(st = tierArgs(c.n, step.calls[i].argsJson, c.args)).ok()) break;
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

  // Tier 1 for a call's arguments (PRD section 2): over kTier1CallArgs, each long string
  // argument is stored as its long-argument stub (arguments that do not parse, cut by the
  // output limit, as one stub) and the whole arguments go to the bulk file first. When the
  // bulk file refuses, the arguments stay inline if their line fits, else the stub makes no
  // promise of view. The call itself runs with the model's arguments.
  Status tierArgs(const std::string& name, const std::string& args, std::string& out) {
    out = args;
    if (args.size() <= tx::kTier1CallArgs) return Status::success();
    ArduinoJson::JsonDocument doc;
    const bool parsed = !deserializeJson(doc, args, ArduinoJson::DeserializationOption::NestingLimit(64));
    if (parsed && !hasLongString(doc.as<ArduinoJson::JsonVariantConst>())) return Status::success();
    std::string handle;
    const bool kept = d_.log->appendBulk(t_.ctx.turn, args, handle).ok();
    if (!kept && args.size() + kLineRoom <= tx::kMaxLineBytes) return Status::success();   // inline whole
    if (!parsed) return longStub(name, args, kept, out);
    stubLongStrings(name, doc.as<ArduinoJson::JsonVariant>(), kept);
    out.clear();
    serializeJson(doc, out);
    return Status::success();
  }
  static constexpr size_t kLineRoom = 1024;   // a line's keys and crc beside its text

  static bool hasLongString(ArduinoJson::JsonVariantConst v) {
    if (v.is<ArduinoJson::JsonObjectConst>()) {
      for (ArduinoJson::JsonPairConst kv : v.as<ArduinoJson::JsonObjectConst>())
        if (hasLongString(kv.value())) return true;
    } else if (v.is<ArduinoJson::JsonArrayConst>()) {
      for (ArduinoJson::JsonVariantConst e : v.as<ArduinoJson::JsonArrayConst>())
        if (hasLongString(e)) return true;
    } else if (v.is<const char*>()) {
      return std::strlen(v.as<const char*>()) > tx::kTier1CallArgs;
    }
    return false;
  }
  void stubLongStrings(const std::string& name, ArduinoJson::JsonVariant v, bool kept) {
    if (v.is<ArduinoJson::JsonObject>()) {
      for (ArduinoJson::JsonPair kv : v.as<ArduinoJson::JsonObject>()) stubLongStrings(name, kv.value(), kept);
    } else if (v.is<ArduinoJson::JsonArray>()) {
      for (ArduinoJson::JsonVariant e : v.as<ArduinoJson::JsonArray>()) stubLongStrings(name, e, kept);
    } else if (v.is<const char*>() && std::strlen(v.as<const char*>()) > tx::kTier1CallArgs) {
      std::string stub;
      if (longStub(name, v.as<const char*>(), kept, stub).ok()) v.set(stub);
    }
  }
  // A long text's stub: with the bulk file kept, the long-argument stub (view); without it,
  // the internal stub's no-card form, which promises nothing.
  Status longStub(const std::string& name, const std::string& text, bool kept, std::string& out) {
    if (kept) return tx::longArgStub((uint32_t)text.size(), hex8(text), c_.nextQ, out);
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
    return tx::stubText(f, out);
  }
  static std::string hex8(const std::string& s) {
    static const char kHex[] = "0123456789abcdef";
    uint32_t h = fnv1a(2166136261u, s);
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i, h >>= 4) out[(size_t)i] = kHex[h & 15];
    return out;
  }

  // Tier 1 for assistant text: over kTier1AssistantText it goes to the bulk file and the a
  // item holds its stub; when the bulk file refuses, the text stays inline if its line fits.
  Status tierText(const std::string& text, tx::Item& a) {
    a.x = text;
    if (text.size() <= tx::kTier1AssistantText) return Status::success();
    std::string handle;
    const bool kept = d_.log->appendBulk(t_.ctx.turn, text, handle).ok();
    if (!kept && text.size() + kLineRoom <= tx::kMaxLineBytes) return Status::success();
    return stubOf(texts::device_notes::kAssistantSlug, text, kept, a.x);
  }

  Status meta(const ParsedStep& step, uint16_t beforeCall) {
    for (const MetaPart& m : step.meta) {
      if (m.beforeCall != beforeCall) continue;
      tx::Item& k = blank(tx::ItemKind::RoundMeta);
      k.x = m.bytes;
      k.rd = a_round();
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
  // (ToolOutput::outside).
  static bool outsideByClass(const CallView& v) {
    if (v.cls.taint != orch::Taint::External) return false;
    return v.unresolved || orch::toolTaintRow(v.name) != nullptr || orch::toolClassRow(v.name) == nullptr;
  }

  // ---- one call -----------------------------------------------------------------------------
  // The round's calls in order; past the soft wall no further call starts.
  Status runCalls(const ParsedStep& step) {
    Status st = Status::success();
    for (size_t i = 0; st.ok() && i < step.calls.size(); ++i) {
      t_.curCallQ = callQOf(i);
      st = elapsedMs() < t_.limits.softWallMs ? runCall(step.calls[i])
                                              : answerUnrun(step.calls[i], texts::tool_results::kNotRunLimit);
    }
    ++t_.toolRounds;
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
        ++t_.end.tools;
      } else {
        out.text = texts::tool_results::kNotRunUnrecorded;
        out.isError = true;
      }
    }
    out.id = call.id;
    out.name = call.name;
    out.outside |= outsideByClass(view);
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
    f.cls = r.cls;
    f.noCard = r.cls != tx::ResultClass::Receipt && r.handle.empty();
    if (!tx::stubText(f, r.st).ok()) r.st.clear();
    return put(r, stored);
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
    closingFacts();
    deliver();
    Status st = storeDelivery();   // the d item and afterDeliver, before the scan below allocates
    afterDeliver();
    if (!t_.storeFailed) supersede();
    if (!t_.storeFailed) st = storeEnd();
    if (!t_.storeFailed) c_.head.openTurn = 0;   // a turn with no e item stays open
    c_.head.nextQ = c_.nextQ;
    if (d_.log->isOpen()) (void)d_.log->writeHead(c_.head);   // a cache: Io loses nothing
    return st;
  }

  // The turn's status line, the superseded reads and the change points, before delivery.
  void closingFacts() {
    if (t_.cause.empty()) t_.cause = "stopped";
    if (t_.storeFailed) refusedLine();
    else if (t_.storageLineDue) setStatus(StatusLine::StorageState, texts::device_notes::kStorageLine);
    if (t_.sawOutside) event("change:outside");
    if (t_.sawMeta) event("change:thinking");   // closed-turn thinking is not replayed (ruling 13)
    if (d_.recentFiles && t_.fileSetChanged) {
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
    return st;
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
PlainEngine::~PlainEngine() = default;

tx::HeadState* PlainEngine::head() { return chat_ && chat_->booted ? &chat_->head : nullptr; }
std::vector<tx::DigestEntry>* PlainEngine::digests() { return chat_ && chat_->booted ? &chat_->live : nullptr; }
uint64_t PlainEngine::omittedThru() const { return chat_ && chat_->booted ? chat_->droppedThru : 0; }

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
  out.failOver = t->failOver;
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
    if (t->statusRef) out.statusText = *t->statusRef;
    out.end = t->end;
    out.end.cause = t->cause;   // also when no e item could be stored
  } catch (const std::bad_alloc&) {
  }
  return st;
}

Status PlainEngine::idle(const IdleCtx& ctx, IdleOutcome& out) {
  out = IdleOutcome{};
  if (!chat_ || !chat_->booted) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::idle");
  if (deps_.hooks) out = deps_.hooks->runIdleStep(ctx);
  return Status::success();
}

namespace {

// The chat from storage at boot: the log opened (Io: no card, the chat runs from RAM), the arena
// filled after thru, the next q and the last turn, the digests in force.
Status loadChat(const EngineDeps& deps, PlainEngine::ChatState& c) {
  c = PlainEngine::ChatState{};
  c.memoryLine = texts::device_notes::kNoMemory;
  Status st = deps.log->open(c.head);
  if (!st.ok() && st.err != Err::Io) return st;
  if (st.ok()) {
    tx::FillReport report;
    if (!(st = deps.log->fill(c.head, *deps.arena, report)).ok()) return st;
    c.droppedThru = report.droppedThru;
  }
  c.nextQ = std::max<uint64_t>(std::max<uint64_t>(c.head.nextQ, deps.arena->lastQ() + 1), 1);
  for (size_t i = 0; i < deps.arena->count(); ++i) {
    tx::ArenaEntry e;
    if (!deps.arena->header(i, e).ok()) continue;
    c.lastTurn = std::max(c.lastTurn, e.turn);
    if (e.kind == tx::ItemKind::TurnEnd) c.lastClosedQ = e.q;
  }
  if (deps.files && deps.log->isOpen() && tx::loadDigests(*deps.files, deps.chatKey, c.head, c.live).ok()) {
    std::string block;
    tx::SettleReport rep;
    (void)tx::settleDigests(*deps.files, deps.chatKey, tx::profileOf(c.head.profile), c.live, block, rep);
  }
  c.booted = true;
  return Status::success();
}

}  // namespace

// A refused allocation anywhere in boot, the resume seam included, is a status.
Status PlainEngine::boot(const BootCtx& ctx) {
  if (!depsReady(deps_) || !chat_) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::boot");
  try {
    const Status st = loadChat(deps_, *chat_);
    if (!st.ok()) return st;
    return deps_.hooks ? deps_.hooks->runBootResume(ctx) : Status::success();
  } catch (const std::bad_alloc&) {
    return Status::fail(Err::TooLarge, "plain::PlainEngine::boot.memory");
  }
}

}  // namespace plain
}  // namespace agent
