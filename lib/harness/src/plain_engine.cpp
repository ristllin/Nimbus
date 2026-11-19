// The plain engine (plain/engine.h): the simplest agent loop with durability around it.
// Lane RB-E1a; RB-E1b is the second and last holder (guards scaled to the head,
// suppression, deferred effects).
#include "nimbus/harness/plain/engine.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

#include "nimbus/harness/channel.h"
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
  bool noteDue = false;                // the not-stored note waits for the next round boundary
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
  uint32_t bodyCap = 0;                // 0: the exchange's 85,000 B floor
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
  bool storeFailed = false;            // the arena refused an item: the turn ends
  bool storageLineDue = false;
  bool fileSetChanged = false;
  bool sawOutside = false;
  bool sawMeta = false;
  GuardTrip trip = GuardTrip::None;
  bool reply = false;                  // `text` is the model's reply
  bool sent = false;                   // the reply went out
  std::string text;
  StatusLine status = StatusLine::None;
  std::string statusText;
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
        if (!(st = finalAnswer(step)).ok() || !t_.held || t_.reply) break;
        continue;                                                          // held once: one more round
      }
      if (!(st = storeAnswer(step, false)).ok()) break;
      for (size_t i = 0; st.ok() && i < step.calls.size(); ++i)   // past the soft wall no call starts
        st = elapsedMs() < t_.limits.softWallMs ? runCall(step.calls[i])
                                                : answerUnrun(step.calls[i], texts::tool_results::kNotRunLimit);
      ++t_.toolRounds;
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
    const Status st = d_.log->store(it, *d_.arena, state);
    if (!st.ok()) {
      t_.storeFailed = true;
      return st;
    }
    const bool ok = state == tx::StoreState::Stored;
    if (stored) *stored = ok;
    if (!ok && !c_.storageEpisode) {
      c_.storageEpisode = c_.noteDue = t_.storageLineDue = true;
      event("not-stored");
    }
    if (ok) c_.storageEpisode = false;
    return Status::success();
  }

  Status note(const std::string& text, tx::NoteClass cls) {
    tx::Item& h = blank(tx::ItemKind::DeviceNote);
    h.x = text;
    h.note = cls;
    return put(h);
  }

  // ---- turn open ----------------------------------------------------------------------
  Status open(const TurnInput& in) {
    t_.ctx = in.ctx;
    if (!t_.ctx.turn) t_.ctx.turn = c_.lastTurn + 1;
    c_.lastTurn = std::max(c_.lastTurn, t_.ctx.turn);
    t_.ctx.startedAt = epoch();
    t_.ctx.startedMs = t_.startMs = d_.platform->nowMs();
    t_.tz = in.tzMinutes;
    Status st = closeDangling();
    if (st.ok()) st = storeInput(in);
    if (!st.ok()) return st;
    if (!d_.setup) return fail(Status::fail(Err::InvalidArgument, "plain::PlainEngine::runTurn.setup"));
    if (!(st = d_.setup(t_.ctx, t_.setup)).ok()) return fail(st);
    budget();
    if (!(st = storeOpen(in)).ok() || !(st = movePointer()).ok()) return st;
    if (!(st = compose()).ok()) return fail(st);
    if (!t_.wire && !(st = wireFor(t_.ctx.dialect, t_.wire)).ok()) return fail(st);
    if (!t_.wire) return fail(Status::fail(Err::NotFound, "plain::wireFor"));
    if (!guardLimitsFor(t_.setup.storedRounds, t_.setup.cost, t_.limits).ok()) t_.limits = GuardLimits{};   // RB-E1b
    // Whatever the scale, every round has a bound and the last round has time before the hard wall.
    t_.limits.rounds = std::max<uint16_t>(1, std::min<uint16_t>(t_.limits.rounds, kMaxRounds));
    t_.limits.softWallMs = std::min<uint32_t>(t_.limits.softWallMs, kHardWallMs - 2 * kMinRequestMs);
    return Status::success();
  }

  // A turn that cannot start (setup, composition or the wire refused) is closed with one
  // plain line: a binding fault, never a partial request.
  Status fail(const Status& st) {
    t_.cause = std::string("setup:") + tx::errSlug(st.err);
    t_.status = StatusLine::ProviderError;
    t_.statusText = exchangeLine(ExchangeErr::Other, providerSlugOf(t_.ctx, t_.setup.head), 0);
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
        for (size_t k = open.size(); k-- > 0;)   // the newest open call of that id
          if (open[k].id == it.id) {
            open.erase(open.begin() + (long)k);
            break;
          }
    }
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
    return Status::success();
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

  void budget() {
    tx::HeadKey key;
    key.host = providerSlugOf(t_.ctx, t_.setup.head);
    key.model = t_.ctx.model;
    key.routed = t_.setup.head.routed;
    key.qualified = t_.setup.qualified;
    tx::HeadBudget b;
    if (!tx::budgetFor(key, t_.ctx.profile, b).ok()) return;   // RB-T4: until then the floor cap
    t_.bodyCap = b.bodyCap;
    t_.ctx.profile = b.profile;
  }
  bool floorHead() const { return t_.bodyCap == 0 || t_.bodyCap <= tx::kFloorHeadBodyCap; }

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
    if (c_.noteDue) {
      c_.noteDue = false;
      Status st = note(texts::device_notes::kNotStored, tx::NoteClass::Gap);
      if (!st.ok()) return st;
    }
    if (t_.lastRound) return Status::success();
    GuardState g;
    g.rounds = t_.toolRounds;
    g.elapsedMs = elapsedMs();
    g.promptTokens = t_.end.promptTokens;
    g.spendMicroUsd = t_.end.microUsd;
    GuardTrip trip = GuardTrip::None;
    if (!checkGuards(t_.limits, g, trip).ok())   // RB-E1b; until then rounds and time only
      trip = g.rounds >= t_.limits.rounds ? GuardTrip::Rounds
             : g.elapsedMs + kMinRequestMs >= t_.limits.softWallMs ? GuardTrip::Time : GuardTrip::None;
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
    req.climb = [this](Climb why) { return climb(why); };
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
      t_.end.exchanges.push_back(tx::ExchangeFacts{ex.requestBytes, ex.ms, (uint16_t)ex.httpStatus});
    }
    const orch::TokenUsage& u = ex.step.usage;
    t_.end.promptTokens += u.promptTokens;
    t_.end.outputTokens += u.completionTokens;
    t_.end.cacheReadTokens += u.cacheReadTokens;
    t_.end.cacheWriteTokens += u.cacheWriteTokens;
    t_.parkHost |= ex.parkHost;
    t_.failOver |= ex.failOver;
  }

  // Nothing came from the model: the exchange's one plain line ends the turn.
  Status failed(ExchangeErr err, const Status& st) {
    t_.cause = std::string("provider:") + exchangeErrSlug(err);
    t_.status = StatusLine::ProviderError;
    t_.statusText = t_.ex.errorLine.empty() ? exchangeLine(err, providerSlugOf(t_.ctx, t_.setup.head), 0)
                                            : t_.ex.errorLine;
    if (err != t_.ex.err) t_.ex.err = err;
    return st;
  }

  // A cut answer: its text is delivered with the cut line; its calls are never run.
  Status cut() {
    ++t_.rounds;
    t_.cause = "cut";
    t_.status = t_.ex.status;
    t_.statusText = t_.ex.errorLine;
    Status st = storeAnswer(t_.ex.step, true);
    for (size_t i = 0; st.ok() && i < t_.ex.step.calls.size(); ++i)
      st = answerUnrun(t_.ex.step.calls[i], texts::tool_results::kNotRunCut);
    t_.reply = st.ok();
    t_.text = t_.ex.step.text;
    return st;
  }

  // The overflow ladder (tx::nextRung), one rung per call; false when no rung is left.
  bool climb(Climb why) {
    LadderState& l = t_.ladder;
    bool climbed = false;
    if (why == Climb::OverCap && !l.closedThrough && c_.lastClosedQ > c_.head.stub) {
      l.closedThrough = c_.lastClosedQ;   // (1) every closed turn as stubs
      climbed = true;
    } else if (why == Climb::OverCap && !l.stubThrough && t_.roundStart.size() > 2) {
      l.stubThrough = t_.roundStart[t_.roundStart.size() - 2] - 1;   // (2) bulk older than two rounds
      climbed = true;
    } else if (!l.omitThrough) {
      climbed = omitOldest();   // (3) one omission line, never the newest 6 owner turns
    }
    if (climbed) event("change:ladder");
    return climbed;
  }

  bool omitOldest() {
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
      spans.back().bytes += e.lineBytes;
      lastQ.back() = e.q;
    }
    uint32_t n = 0;
    if (!tx::omissionSpan(spans, std::numeric_limits<size_t>::max(), n).ok() || n == 0) return false;
    std::string dates;
    (void)tx::omissionDates(spans[0].firstAt, spans[n - 1].firstAt, t_.tz, dates);
    if (!tx::omissionLine(n, dates, t_.ladder.omissionLine).ok()) return false;
    t_.ladder.omitThrough = lastQ[n - 1];
    return true;
  }

  // ---- what the model said ----------------------------------------------------------------
  // The reply, after the truth seam: held once, it is stored as an interim text with the
  // device's facts item after it and the loop asks once more.
  Status finalAnswer(const ParsedStep& step) {
    DeliverDecision dd;
    if (d_.hooks) {
      DeliverCtx dc;
      dc.turn = &t_.ctx;
      dc.text = &step.text;
      dc.held = t_.held;
      dd = d_.hooks->runBeforeDeliver(dc);
    }
    const bool hold = dd.verdict == DeliverVerdict::Hold && !t_.held;   // at most one hold a turn
    Status st = storeAnswer(step, !hold);
    for (size_t i = 0; st.ok() && i < step.calls.size(); ++i)   // the last round still called
      st = answerUnrun(step.calls[i], texts::tool_results::kNotRunLimit);
    if (!st.ok()) return st;
    if (hold) {
      t_.held = true;
      event("hold");
      return note(dd.deviceNote, tx::NoteClass::Notice);
    }
    t_.reply = true;
    t_.text = step.text;
    t_.cause = t_.trip == GuardTrip::None ? "reply" : std::string("limit:") + guardTripSlug(t_.trip);
    return Status::success();
  }

  // k, a and c items in the order the model gave them, before any call runs.
  Status storeAnswer(const ParsedStep& step, bool fin) {
    t_.roundStart.push_back(c_.nextQ);
    Status st = meta(step, 0);
    if (st.ok() && (!step.text.empty() || step.calls.empty())) {
      tx::Item& a = blank(tx::ItemKind::Assistant);
      a.x = step.text;
      a.rd = (uint8_t)std::min<uint16_t>(t_.rounds, 255);
      a.fin = fin;
      st = put(a);
    }
    for (size_t i = 0; st.ok() && i < step.calls.size(); ++i) {
      if (i && !(st = meta(step, (uint16_t)i)).ok()) break;
      tx::Item& c = blank(tx::ItemKind::Call);
      c.id = step.calls[i].id;
      c.n = step.calls[i].name;
      c.args = step.calls[i].argsJson;
      c.eff = classOf(step.calls[i]).effect == orch::EffectClass::Effect;
      c.rd = a_round();
      st = put(c);
      if (st.ok()) t_.writes.push_back(c);   // pruned to successful effects when their result lands
    }
    if (st.ok() && !step.calls.empty()) st = meta(step, (uint16_t)step.calls.size());
    return st;
  }
  uint8_t a_round() const { return (uint8_t)std::min<uint16_t>(t_.rounds, 255); }

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

  // The class of one call as the rails see it: for `more`, the class of the tool it reaches
  // (callClass); arguments that do not parse reach nothing knowable, so a router reads as an
  // effect (the fail-closed default).
  orch::ToolClass classOf(const ToolCall& call) const {
    const orch::ToolRegistry* reg = t_.setup.facts.reg;
    if (!reg) return orch::toolClassFor(call.name);
    ArduinoJson::JsonDocument args;
    if (deserializeJson(args, call.argsJson, ArduinoJson::DeserializationOption::NestingLimit(64)) ||
        !args.is<ArduinoJson::JsonObjectConst>() || !t_.ctx.who)
      return reg->isRouter(call.name) ? orch::ToolClass{} : reg->classOf(call.name);
    return orch::callClass(*reg, call.name, args.as<ArduinoJson::JsonObjectConst>(), *t_.ctx.who);
  }
  // Outside content by class: a tool with a taint row that says so, or a tool nothing
  // classes at all (unknown tools read as outside). The engine-2 names have class rows but
  // no taint rows yet: their own result says whether it is outside (ToolOutput::outside).
  static bool outsideByClass(const std::string& name, const orch::ToolClass& cls) {
    if (cls.taint != orch::Taint::External) return false;
    return orch::toolTaintRow(name) != nullptr || orch::toolClassRow(name) == nullptr;
  }

  // ---- one call -----------------------------------------------------------------------------
  Status runCall(const ToolCall& call) {
    const orch::ToolClass cls = classOf(call);
    DispatchDecision dd;
    if (d_.hooks) {
      DispatchCtx dc;
      dc.turn = &t_.ctx;
      dc.call = &call;
      dc.pure = cls.effect == orch::EffectClass::Pure;
      dc.effect = cls.effect == orch::EffectClass::Effect;
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
        out = d_.dispatch(call, t_.ctx);
        ++t_.end.tools;
      } else {
        out.text = texts::tool_results::kNotRunUnrecorded;
        out.isError = true;
      }
    }
    out.id = call.id;
    out.name = call.name;
    out.outside |= outsideByClass(call.name, cls);
    return storeResult(call, out, cls);
  }

  Status answerUnrun(const ToolCall& call, const char* why) {
    ToolOutput out;
    out.id = call.id;
    out.name = call.name;
    out.text = why;
    out.isError = true;
    return storeResult(call, out, orch::ToolClass{"", orch::Taint::Internal, orch::EffectClass::Read});
  }

  Status storeResult(const ToolCall& call, const ToolOutput& out, const orch::ToolClass& cls) {
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
    if (r.ok && orch::tx::changesFileSet(call.name)) t_.fileSetChanged = true;
    keepWrite(call, r, cls);
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

  void keepWrite(const ToolCall& call, const tx::Item& r, const orch::ToolClass& cls) {
    auto it = std::find_if(t_.writes.begin(), t_.writes.end(),
                           [&](const tx::Item& c) { return c.kind == tx::ItemKind::Call && c.id == call.id; });
    if (it == t_.writes.end()) return;
    if (!r.ok || cls.effect != orch::EffectClass::Effect) {
      t_.writes.erase(it);
      return;
    }
    t_.writes.insert(it + 1, r);
  }

  // ---- turn close -----------------------------------------------------------------------------
  Status close() {
    if (t_.cause.empty()) t_.cause = t_.storeFailed ? "storage" : "stopped";
    if (t_.storeFailed || (t_.storageLineDue && t_.status == StatusLine::None)) {
      t_.status = StatusLine::StorageState;
      t_.statusText = texts::device_notes::kStorageLine;
    }
    if (!t_.storeFailed) supersede();
    if (t_.sawOutside) event("change:outside");
    if (t_.sawMeta) event("change:thinking");   // closed-turn thinking is not replayed (ruling 13)
    if (d_.recentFiles && t_.fileSetChanged) {
      const std::string before = c_.recent.text();
      (void)c_.recent.atClose(true, d_.recentFiles, t_.tz);
      if (c_.recent.text() != before) event("change:recent-files");
    }
    DeliverOutcome sent;
    deliver(sent);
    t_.sent = sent.sent;
    Status st = Status::success();
    if (!t_.storeFailed && sent.sent) {
      tx::Item& dItem = blank(tx::ItemKind::Delivery);
      dItem.msgIds = sent.msgIds;
      st = put(dItem);
    }
    if (d_.hooks) {
      DeliveredCtx dc;
      dc.turn = &t_.ctx;
      dc.text = &t_.text;
      dc.msgIds = &sent.msgIds;
      dc.sent = sent.sent;
      d_.hooks->runAfterDeliver(dc);
    }
    if (!t_.storeFailed) st = storeEnd();
    if (!t_.storeFailed) c_.head.openTurn = 0;   // a turn with no e item stays open
    c_.head.nextQ = c_.nextQ;
    if (d_.log->isOpen()) (void)d_.log->writeHead(c_.head);   // a cache: Io loses nothing
    return st;
  }

  // Rule (4): a successful write supersedes the earlier whole reads it made stale.
  void supersede() {
    if (t_.writes.empty()) return;
    std::vector<tx::Item> earlier(d_.arena->count());
    for (size_t i = 0; i < earlier.size(); ++i) (void)d_.arena->get(i, earlier[i]);
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

  // One portable deliver function (RB-E3); until it lands, the channel's send, the reply
  // first and the status line as its own message.
  void deliver(DeliverOutcome& out) {
    DeliverInput in;
    in.turn = &t_.ctx;
    if (t_.reply) in.text = t_.text;
    in.status = t_.status;
    in.statusText = t_.statusText;
    const Status st = deliverTurn(DeliverDeps{d_.channel, d_.hooks, d_.log}, in, out);
    if (!st.notImplemented() || !d_.channel || !d_.channel->send) return;
    out = DeliverOutcome{};
    if (!in.text.empty() && d_.channel->send(t_.ctx.chatId, in.text)) {
      out.sent = true;
      out.bytes = (uint32_t)in.text.size();
    }
    if (!in.statusText.empty()) (void)d_.channel->send(t_.ctx.chatId, in.statusText);
  }

  Status storeEnd() {
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
    return st;
  }

  const EngineDeps& d_;
  PlainEngine::ChatState& c_;
  Turn& t_;
};

bool depsReady(const EngineDeps& d) {
  return d.platform && d.platform->nowMs && d.platform->delayMs && d.transport && d.log && d.arena && d.dispatch;
}

}  // namespace

PlainEngine::PlainEngine(const EngineDeps& deps) : deps_(deps), chat_(new (std::nothrow) ChatState()) {}
PlainEngine::~PlainEngine() = default;

tx::HeadState* PlainEngine::head() { return chat_ && chat_->booted ? &chat_->head : nullptr; }
std::vector<tx::DigestEntry>* PlainEngine::digests() { return chat_ && chat_->booted ? &chat_->live : nullptr; }

Status PlainEngine::runTurn(const TurnInput& in, TurnOutcome& out) {
  out = TurnOutcome{};
  if (!depsReady(deps_) || !chat_ || !chat_->booted) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::runTurn");
  std::unique_ptr<Turn> t(new (std::nothrow) Turn());
  if (!t) return Status::fail(Err::TooLarge, "plain::PlainEngine::runTurn.memory");
  try {
    const Status st = TurnRun(deps_, *chat_, *t).run(in);
    out.delivered = t->sent;
    out.text = t->reply ? t->text : std::string();
    out.rounds = t->rounds;
    out.toolCalls = t->end.tools;
    out.headRequests = t->end.headRequests;
    out.trip = t->trip;
    out.status = t->status;
    out.statusText = t->statusText;
    out.parkHost = t->parkHost;
    out.failOver = t->failOver;
    out.end = t->end;
    return st;
  } catch (const std::bad_alloc&) {
    // A refused allocation ends the turn here: what was stored stays, and a call left
    // without its result is answered "outcome unknown" at the next turn open.
    return Status::fail(Err::TooLarge, "plain::PlainEngine::runTurn.memory");
  } catch (const std::exception&) {   // a tool or a binding threw: the same designed state
    return Status::fail(Err::Io, "plain::PlainEngine::runTurn.exception");
  }
}

Status PlainEngine::idle(const IdleCtx& ctx, IdleOutcome& out) {
  out = IdleOutcome{};
  if (!chat_ || !chat_->booted) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::idle");
  if (deps_.hooks) out = deps_.hooks->runIdleStep(ctx);
  return Status::success();
}

Status PlainEngine::boot(const BootCtx& ctx) {
  if (!depsReady(deps_) || !chat_) return Status::fail(Err::InvalidArgument, "plain::PlainEngine::boot");
  try {
    ChatState& c = *chat_;
    c = ChatState{};
    Status st = deps_.log->open(c.head);   // Io: no card, the chat runs from RAM
    if (!st.ok() && st.err != Err::Io) return st;
    if (st.ok()) {
      tx::FillReport report;
      if (!(st = deps_.log->fill(c.head, *deps_.arena, report)).ok()) return st;
    }
    c.nextQ = std::max<uint64_t>(std::max<uint64_t>(c.head.nextQ, deps_.arena->lastQ() + 1), 1);
    for (size_t i = 0; i < deps_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!deps_.arena->header(i, e).ok()) continue;
      c.lastTurn = std::max(c.lastTurn, e.turn);
      if (e.kind == tx::ItemKind::TurnEnd) c.lastClosedQ = e.q;
    }
    if (deps_.files && deps_.log->isOpen() && tx::loadDigests(*deps_.files, deps_.chatKey, c.head, c.live).ok()) {
      std::string block;
      tx::SettleReport rep;
      (void)tx::settleDigests(*deps_.files, deps_.chatKey, tx::Profile::Standard, c.live, block, rep);
    }
    c.booted = true;
  } catch (const std::bad_alloc&) {
    return Status::fail(Err::TooLarge, "plain::PlainEngine::boot.memory");
  }
  return deps_.hooks ? deps_.hooks->runBootResume(ctx) : Status::success();
}

}  // namespace plain
}  // namespace agent
