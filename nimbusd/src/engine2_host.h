#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "daemon_http.h"
#include "hosted_file_tools.h"
#include "hosted_fold.h"   // hostedTextIO
#include "hosted_loops.h"   // the schedule tool and its fires (lane E2-SCHED)
#include "hosted_notes.h"
#include "hosted_tx.h"
#include "nimbus/harness/plain/compact.h"
#include "nimbus/harness/plain/exchange.h"   // providerSlugOf
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/history.h"   // search and view (lane E2-SRCH)
#include "nimbus/harness/plain/schedule_binding.h"   // heldFileOn, heldPathFor, drainHeld (lane E2-SCHED)
#include "nimbus/orch/calc_tool.h"
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/spend_ceiling.h"   // kSpendFrozenError (lane E2-FIX7)
#include "nimbus/orch/turn_spend.h"   // TurnSpend (lane E2-CMP)
#include "plain_rig.h"
#include "posix_files.h"
#include "posix_platform.h"

// engine2_host - the owner chat of a Virtual Nimbus on engine 2 (lane E2-HOST). Started once at
// boot when the instance's startup switch says engine 2 (NIMBUSD_ENGINE=2); engine 1 stays the
// default and is untouched when the switch is off.
//
// One PlainRig for the owner's conversation (transcript key "owner", <data>/mem/tx/owner/),
// over the daemon's own platform and a libcurl transport of its own (the engine thread is the
// only caller), the Anthropic wire, and the bindings the engine already has:
//   * compaction: one CompactDriver for the chat, installed before boot; the engine runs it at
//     turn open and from the daemon's idle step (idle()); its control record is
//     <data>/mem/compact_ctl.txt. No v5.1.1 chatsum mirror: engine 1 owns that file.
//   * notes (hosted_notes.h): <data>/mem/notes/owner.md, the pinned block in every request;
//   * calc;
//   * the five file tools and delete_file behind more (hosted_file_tools.h) over the daemon's one file store;
//   * lane E2-SCHED: `schedule` over the instance's routines table (hosted_loops.h, loops.json with
//     loops2.json beside it) when Engine2Options::loops is set, and its fires (fire(), the routines
//     tick's hook while engine 2 runs): a reminder's text with no model request, a routine isolated on
//     the sub-agent model through plain::runRoutine, its state and outbox in <data>/mem/routines/.
// Replies leave through the host's deliver function (the web chat ring and Telegram), to the
// chat the running turn came from. Claude heads only: start() refuses any other head in one
// plain line, and the daemon does not start.
namespace nimbusd {

struct Engine2Options {
  std::string dataDir;
  PosixFiles* files = nullptr;                       // the daemon's file store (engine 1's, shared)
  std::function<std::string()> head;                 // the head provider slug now ("anthropic")
  std::function<std::string()> model;                // the head model now
  std::function<std::string()> summarizerModel;      // the compaction model (the sub-agent pick)
  std::function<std::string()> key;                  // the Anthropic key, read at render time
  std::function<int32_t(uint32_t)> utcOffsetMin;     // minutes east of UTC at an instant
  std::function<uint16_t()> dayStartMin;
  std::string timeZone;                              // IANA name for the system block ("" = none)
  nimbus::orch::tx::Profile profile = nimbus::orch::tx::kDefaultProfile;   // the owner's pinned profile
  uint32_t bodyCapLimit = 0;   // lane E2-FIX9: a test rig's smaller request body cap (NIMBUSD_E2_BODY_CAP); 0: none
  // A reply of the running turn, to the chat it came from (the web ring, Telegram).
  std::function<void(const std::string& chat, const std::string& text)> deliver;
  agent::HttpTransport* transport = nullptr;         // null: the host's own libcurl transport (tests script one)
  std::string chatKey = "owner";                     // /mem/tx/<chatKey>
  std::string chatId = "owner";                      // the engine's routing chat (the web chat id)
  HostedLoops* loops = nullptr;                      // lane E2-SCHED: the routines table; null: no schedule tool
  // Lane E2-CMP: every paid request for the instance's monthly ceiling: each turn's spend ("turn") and
  // each summarizer request of the compaction ("compact"). Null: not counted.
  std::function<void(const nimbus::orch::TurnSpend& spend, const std::string& tag)> spend;
  // Lane E2-FIX7: the instance's monthly ceiling is reached. Then no engine-2 path makes a paid request: a
  // person's turn on any channel gets kPausedLine, routines wait (reminders still go), and the idle step runs
  // no compaction. Null: never paused.
  std::function<bool()> paused;
};

// What a person's message gets while paid calls are paused (the web chat's own refusal, lane DEF).
inline constexpr char kPausedLine[] =
    "Paid AI calls are paused: this month's spend ceiling is reached. Raise it on Assistant > Usage, or send the "
    "/budget command from the pause message.";

// The one plain line a refused switch logs (and the daemon exits on).
inline std::string engine2HeadRefusal(const std::string& head, const std::string& model) {
  return "engine 2 runs on Claude heads only (an Anthropic key and a claude- model); this instance's head is " +
         (head.empty() ? std::string("none") : head) + (model.empty() ? std::string() : " " + model) +
         ": set NIMBUSD_ORCH_HOST=anthropic or turn NIMBUSD_ENGINE off";
}
inline bool engine2HeadOk(const std::string& head, const std::string& model) {
  return head == "anthropic" && model.rfind("claude-", 0) == 0;
}

class Engine2Host {
 public:
  explicit Engine2Host(Engine2Options o)
      : o_(std::move(o)), platform_(makePosixPlatform(&mem_)), txFiles_(o_.dataDir), notes_(o_.dataDir + "/mem"),
        fileTools_(*o_.files, HostedFileTools::Deps{o_.utcOffsetMin, nullptr}),
        routineFiles_(o_.dataDir, nimbus::orch::schedule::kStateDir),
        held_(plain::heldFileOn(routineFiles_, plain::heldPathFor(o_.chatId))), history_(historyDeps()) {}
  Engine2Host(const Engine2Host&) = delete;
  Engine2Host& operator=(const Engine2Host&) = delete;

  // Builds the rig, binds the tools and compaction, boots over the stored transcript. "" on
  // success, else the one plain line that says why engine 2 cannot run.
  std::string start() {
    const std::string head = o_.head ? o_.head() : std::string();
    const std::string model = o_.model ? o_.model() : std::string();
    if (!engine2HeadOk(head, model)) return engine2HeadRefusal(head, model);
    bootModel_ = model;
    rig_.reset();   // a reboot: the old rig and its hooks go before the driver they point at
    PlainRigConfig c;
    c.chatKey = o_.chatKey;
    c.chatId = o_.chatId;
    c.dialect = plain::Dialect::AnthropicMessages;
    c.host = "api.anthropic.com";
    c.model = model;
    c.modelNow = o_.model;
    c.key = o_.key;
    c.effort = "medium";   // PRD section 3: thinking as today (adaptive, effort medium), as engine 1 sends
    c.timeZone = o_.timeZone;
    c.tzNow = [this] { return int16_t(o_.utcOffsetMin ? o_.utcOffsetMin(vclock::epoch32()) : 0); };
    c.dayStartMin = o_.dayStartMin ? o_.dayStartMin() : plain::kDayStartDefaultMin;
    c.profile = o_.profile;
    c.bodyCapLimit = o_.bodyCapLimit;
    c.stablePrefix = true;   // lane E2-CACHE: one long conversation, written to in clusters
    PlainRigBindings b;
    b.platform = &platform_;
    b.transport = transport();
    b.files = &txFiles_;
    b.send = [this](const std::string&, const std::string& text) {
      if (o_.deliver) o_.deliver(curChat_.empty() ? o_.chatId : curChat_, text);
      return true;
    };
    // Lane E2-SRCH: search and view read the turn's own conversation (no earlier-engine rows here).
    plain::ToolDispatchFn inner = o_.loops ? o_.loops->scheduleDispatch(notes_.dispatch()) : notes_.dispatch();
    b.dispatch = [this, inner](const plain::ToolCall& c, const plain::TurnCtx& t) {
      plain::ToolOutput out;
      return history_.answer(c, t, out) ? out : inner(c, t);
    };
    b.setup = [this](const plain::TurnCtx& t, plain::TurnSetup& s) { setupTurn(t, s); };
    b.recentFiles = fileTools_.recentFilesFor(nimbus::orch::principalForRole(o_.chatId, nimbus::orch::Role::Admin));
    b.beforeBoot = [this, model] { return bindCompaction(model); };
    rig_.reset(new PlainRig(c, b));
    notes_.attach(rig_->registry());
    nimbus::orch::registerCalcTool(rig_->registry());
    fileTools_.attach(rig_->registry());
    history_.registerInto(rig_->registry());
    if (o_.loops) {
      o_.loops->attachSchedule(rig_->registry());
      o_.loops->setStateFiles(&routineFiles_);
    }
    (void)held_.load();
    const plain::Status st = rig_->boot();
    if (!st.ok()) {
      std::string why = std::string("engine 2 did not start: ") + nimbus::orch::tx::errSlug(st.err) + "@" + st.where;
      rig_.reset();
      return why;
    }
    return std::string();
  }

  // One owner message from `chat` (the web chat or an owner's Telegram chat). Logs every ending
  // that is not a reply.
  // A head model the owner changed since boot boots the chat again over its stored transcript, so the
  // budget and the compaction marks are the new head's; a model engine 2 cannot run is said in one line.
  PlainTurn say(const std::string& chat, const std::string& text, const char* ch) {
    if (o_.model && o_.model() != bootModel_) {
      const std::string why = start();
      if (!why.empty()) {
        std::fprintf(stderr, "[nimbusd] engine2: %s\n", why.c_str());
        if (o_.deliver) o_.deliver(chat, "This assistant runs on Claude models only. Pick a Claude model and try again.");
        PlainTurn t;
        t.stated = "plain-rig:failed:head";
        return t;
      }
    }
    if (paused()) {   // fail closed: no head or summarizer request past the ceiling, on any channel
      if (o_.deliver) o_.deliver(chat, kPausedLine);
      PlainTurn t;
      t.stated = "plain-rig:paused";
      return t;
    }
    if (!rig_) {
      PlainTurn t;
      t.stated = "plain-rig:not-booted";
      if (o_.deliver) o_.deliver(chat, "The assistant could not start. Check the instance log and restart it.");
      return t;
    }
    curChat_ = chat;
    PlainTurn t = rig_->say(text, ch);
    curChat_.clear();
    if (o_.spend) o_.spend(turnSpend(t.out, bootModel_), "turn");
    lastTurnEnd_ = vclock::epoch();
    if (t.ending != PlainEnding::Reply)
      std::fprintf(stderr, "[nimbusd] engine2: turn ended %s: %s\n", plainEndingSlug(t.ending), t.stated.c_str());
    if (t.out.parkHost) std::fprintf(stderr, "[nimbusd] engine2: the head's key or credit was refused\n");
    const nimbus::orch::tx::TurnEndFacts& e = t.out.end;
    std::fprintf(stderr,
                 "[nimbusd] engine2: turn rounds=%u calls=%u requests=%u in=%u cacheR=%u cacheW=%u cacheW1h=%u out=%u "
                 "uUSD=%u\n",
                 unsigned(t.out.rounds), unsigned(t.out.toolCalls), unsigned(t.out.headRequests), unsigned(e.promptTokens),
                 unsigned(e.cacheReadTokens), unsigned(e.cacheWriteTokens), unsigned(e.cacheWrite1hTokens),
                 unsigned(e.outputTokens), unsigned(e.microUsd));
    return t;
  }

  // The owner's /compact: queued on the driver, answered by it on this chat.
  bool requestCompaction() { return driver_ && driver_->requestManual(); }
  // Lane E2-FIX7: the instance's ceiling is reached (no paid engine-2 request runs).
  bool paused() const { return o_.paused && o_.paused(); }
  // The answer to the owner's /compact: queued (or already queued), or the pause line while paused (no summary
  // runs past the ceiling, so none is promised).
  std::string compactCommand() {
    if (paused()) return kPausedLine;
    return requestCompaction() ? "Compacting this conversation. A message follows when it is done."
                               : "A compaction is already queued. A message follows when it is done.";
  }

  // One unit of idle work (the compaction) between turns, on the engine thread. True when it
  // did work (a paid call may have run).
  bool idle(bool ownerWaiting) {
    if (!rig_ || !rig_->engine()) return false;
    // Lane E2-SCHED: a routine note held for quiet hours goes once they end (one note a step).
    if (o_.loops && plain::drainHeld(held_, rig_->ownerSink(), vclock::epoch(), tzNow(), quiet(),
                                     o_.dayStartMin ? o_.dayStartMin() : plain::kDayStartDefaultMin)) return true;
    if (paused()) return false;   // no summarizer request past the ceiling (an attempt in flight has finished)
    plain::IdleCtx ctx;
    ctx.now = vclock::epoch();
    ctx.nowMs = vclock::steadyMs();
    ctx.ownerWaiting = ownerWaiting;
    ctx.quietForS = lastTurnEnd_ && ctx.now > lastTurnEnd_ ? uint32_t(ctx.now - lastTurnEnd_) : 0;
    ctx.tzMinutes = int16_t(o_.utcOffsetMin ? o_.utcOffsetMin(uint32_t(ctx.now)) : 0);
    ctx.clockSet = true;
    plain::IdleOutcome out;
    const plain::Status st = rig_->engine()->idle(ctx, out);
    if (!st.ok())
      std::fprintf(stderr, "[nimbusd] engine2: idle step failed %s@%s\n", nimbus::orch::tx::errSlug(st.err),
                   st.where);
    if (out.worked && driver_)
      std::fprintf(stderr, "[nimbusd] engine2: compaction step (thru %llu)\n",
                   (unsigned long long)(rig_->engine()->head() ? rig_->engine()->head()->thru : 0));
    return out.worked;
  }

  // Lane E2-SCHED: the routines tick's fire hook while engine 2 runs (the engine thread, between
  // turns). Every entry delivers to this chat's owner conversation, as the hosted owner chat is one.
  nimbus::orch::FireOutcome fire(const nimbus::orch::LoopFireRequest& req) {
    nimbus::orch::FireOutcome o;
    if (!o_.loops || !running()) {
      o.deferred = true;   // not started: tried again, not counted
      o.retryAfterSec = 60;
      return o;
    }
    if (paused() && o_.loops->firesAsRoutine(req.id)) {   // a routine waits while paused; a reminder still goes
      o.deferred = true;
      o.error = nimbus::orch::kSpendFrozenError;
      return o;
    }
    nimbus::orch::schedule::FireBinding b;
    b.owner = rig_->ownerSink();
    b.tzMinutes = tzNow();
    b.dayStartMin = o_.dayStartMin ? o_.dayStartMin() : plain::kDayStartDefaultMin;
    b.files = &routineFiles_;
    b.grants = o_.loops->routineGrants();
    b.run = [this](const plain::RoutineFire& f, bool tainted, plain::FireReport& out) {
      plain::RoutineDeps r = rig_->routineDeps(o_.summarizerModel ? o_.summarizerModel() : std::string());
      r.head.tainted = tainted;
      r.tzMinutes = tzNow();
      r.held = &held_;
      r.quiet = quiet();
      const plain::Status st = plain::runRoutine(r, f, out);
      if (o_.spend) o_.spend(turnSpend(out.outcome, r.head.model), "routine");   // counted like a turn
      return st;
    };
    nimbus::orch::LoopFireRequest owner = req;
    owner.chatId = o_.chatId;
    o = o_.loops->fireE2(owner, b);
    std::fprintf(stderr, "[nimbusd] engine2: fire %s ok=%d deferred=%d answered=%d\n", req.id.c_str(), int(o.ok),
                 int(o.deferred), int(o.answered));
    return o;
  }

  bool running() const { return rig_ && rig_->engine(); }
  PlainRig* rig() { return rig_.get(); }
  HostedNotes& notes() { return notes_; }
  PosixTxFiles& txFiles() { return txFiles_; }

 private:
  agent::HttpTransport* transport() { return o_.transport ? o_.transport : &http_; }
  // Lane E2-SRCH: the transcripts and the owner's unpinned notes; no earlier-engine rows, no photos.
  plain::HistoryDeps historyDeps() {
    plain::HistoryDeps d;
    d.files = &txFiles_;
    d.notes = [this](const nimbus::orch::Principal& who, std::vector<nimbus::orch::notes::Note>& out) {
      return notes_.unpinned(who, out);
    };
    d.tzAt = [this](uint64_t t) { return int16_t(o_.utcOffsetMin ? o_.utcOffsetMin(uint32_t(t)) : 0); };
    d.nowMs = [] { return uint32_t(vclock::steadyMs()); };
    return d;
  }
  int16_t tzNow() const { return int16_t(o_.utcOffsetMin ? o_.utcOffsetMin(vclock::epoch32()) : 0); }
  // The instance keeps no quiet-hours end of its own: the default end (09:00), as plain::QuietHours.
  plain::QuietHours quiet() const { return plain::QuietHours{}; }
  nimbus::orch::tx::HeadKey headKey(const std::string& model) const {
    plain::TurnCtx t;
    t.host = "api.anthropic.com";
    t.model = model;
    plain::HeadEndpoint e;
    e.host = t.host;
    return nimbus::orch::tx::HeadKey{plain::providerSlugOf(t, e), model, false};
  }

  // A turn's spend in the provider's own counts (plain::turnSpendOf's shape): one slot of the head.
  nimbus::orch::TurnSpend turnSpend(const plain::TurnOutcome& out, const std::string& model) const {
    nimbus::orch::TurnSpend s;
    nimbus::orch::TokenUsage u;
    u.promptTokens = out.end.promptTokens;
    u.completionTokens = out.end.outputTokens;
    u.cacheReadTokens = out.end.cacheReadTokens;
    u.cacheWriteTokens = out.end.cacheWriteTokens;
    u.cacheWrite1hTokens = out.end.cacheWrite1hTokens;   // priced at 2x the input rate (lane E2-CACHE)
    u.responses = out.headRequests;
    u.maxRequestIn = out.end.lastPromptTokens;
    s.add("anthropic", model.c_str(), u);   // files nothing when nothing was billed
    return s;
  }

  // The pinned notes, the recent files and the guard costs of this turn's head.
  void setupTurn(const plain::TurnCtx& t, plain::TurnSetup& s) {
    if (t.who) s.facts.pinnedNotes = notes_.pinnedBlock(*t.who);
    nimbus::orch::tx::HeadBudget budget;
    if (!nimbus::orch::tx::budgetFor(headKey(t.model), t.profile, budget).ok()) return;
    plain::HeadRates r;   // the ledger's own price rows, cache and long-prompt tier included (E2-FIX6)
    if (!plain::headRatesFor("anthropic", t.model, r)) {
      nimbus::orch::ModelInfo m;
      m.id = t.model;
      const nimbus::orch::ModelPrice p = nimbus::orch::modelPrice("anthropic", m);
      r.in = p.in;
      r.out = p.out;
      r.cacheRead = p.in / 10;
      r.cacheWrite = p.in * 125 / 100;
    }
    s.cost = plain::guardCostFor(budget, r);
  }

  plain::Status bindCompaction(const std::string& model) {
    plain::CompactDeps d;
    d.platform = &platform_;
    d.transport = transport();
    d.log = rig_->log();
    d.arena = rig_->arena();
    d.files = &txFiles_;
    d.chatKey = o_.chatKey;
    d.chatId = o_.chatId;
    d.headKey = headKey(model);
    d.dayStartMin = o_.dayStartMin ? o_.dayStartMin() : plain::kDayStartDefaultMin;
    PlainRig* rig = rig_.get();
    d.head = [rig] { return rig->engine() ? rig->engine()->head() : nullptr; };
    d.digests = [rig] { return rig->engine() ? rig->engine()->digests() : nullptr; };
    d.omittedThru = [rig] { return rig->engine() ? rig->engine()->omittedThru() : uint64_t(0); };
    d.rungs = [this](std::vector<plain::SummarizerRung>& out) {
      plain::SummarizerRung r;
      r.dialect = plain::Dialect::AnthropicMessages;
      r.host = "anthropic";
      r.model = o_.summarizerModel ? o_.summarizerModel() : std::string("claude-haiku-5-5");
      r.endpoint.host = "api.anthropic.com";
      r.endpoint.key = o_.key;
      out.assign(1, r);
    };
    d.tell = [this](const std::string& chat, const std::string& text) {
      if (o_.deliver) o_.deliver(chat, text);
    };
    d.spent = [this](const plain::SummarizerRung& r, const nimbus::orch::TokenUsage& u) {
      if (!o_.spend) return;
      nimbus::orch::TurnSpend s;
      s.add(r.host.c_str(), r.model.c_str(), u);
      o_.spend(s, "compact");
    };
    d.ctl = hostedTextIO(o_.dataDir + "/mem/compact_ctl.txt");
    driver_.reset(new plain::CompactDriver(d));
    return plain::installCompaction(rig_->hooks(), *driver_);
  }

  Engine2Options o_;
  CgroupMemory mem_;
  agent::Platform platform_;
  DaemonHttpTransport http_;
  PosixTxFiles txFiles_;
  HostedNotes notes_;
  HostedFileTools fileTools_;
  PosixTxFiles routineFiles_;   // <data>/mem/routines: routine state files and the outbox (lane E2-SCHED)
  plain::HeldStore held_;
  plain::HistoryTools history_;   // after txFiles_ and notes_, which it reads
  std::unique_ptr<plain::CompactDriver> driver_;
  std::unique_ptr<PlainRig> rig_;
  std::string curChat_;
  std::string bootModel_;
  uint64_t lastTurnEnd_ = 0;
};

}  // namespace nimbusd
