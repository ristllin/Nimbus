#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/channel.h"
#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/chat_skills.h"
#include "nimbus/harness/plain/compact.h"
#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/unattended.h"   // OwnerSink, RoutineDeps (lane E2-SCHED)
#include "nimbus/harness/plain/rails.h"   // installRails - the safety rails (lane E2-FSAFE)
#include "nimbus/harness/platform.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/rbac.h"
#include "nimbus/orch/seen_index.h"
#include "nimbus/orch/spend_guard.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/digest.h"   // RecentFilesBlock::Source - the live file store's list
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/turn_spend.h"
#include "nimbus/orch/tx/migrate.h"

// plain/chat_binding.h - one chat on engine 2 on the device (lane E2-DEV). A copy of nimbusd's
// one-chat rig (nimbusd/src/plain_rig.h, PlainRig) with what a device chat needs on top:
//   - the arena sized for the head (tx::arenaBytesFor of its budget, about 1.1 MB for Sonnet),
//     not the 1,728 KB constant;
//   - compaction installed (plain::installCompaction): the summarizer is the head host's small
//     model on the same transport; the control file is the binding's TextIO (no chatsum mirror:
//     engine 2 never writes the earlier engine's fold record, lane E2-FIX2B finding P5);
//   - the suppression seam (installSuppression), the money guard (TurnSetup::cost from the head's
//     price row; no price row: boot refuses, fail closed) and one ChatBreaker;
//   - the seen index rebuilt from the filled arena after boot (RB-TG1);
//   - the safety rails (plain::installRails, RB-RL; lane E2-FSAFE): in a turn that read outside
//     content a note change goes to the binding's pending shelf (railPending) and never runs;
//     with no shelf every pending change is refused (fail closed);
//   - the shared tool registry, dispatched with the turn's Principal (plain_rig.h's dispatch).
// Lane E2-CHAT adds the owner's daily chat on top:
//   - photos: the message's image parts (ChatMessage::parts) and the photo hook (TurnSetup::photo);
//   - skills: the skills index and the context skills in the pinned block, each one whole or not
//     at all (skillRefused names one left out; chat_skills.h);
//   - commands that act on the engine: compactNow (a compaction now) and budgetView (/budget);
//   - the turn's usage handed to `usage` once per turn; the progress line (installProgress).
//   - lane E2-MIG: the earlier engine's conversation imported before the engine opens its log
//     (tx::migrate), when the device gives an oldEngine hook.
// Lane E2-SCHED: ownerSink and routineDeps, the parts a reminder or routine fire takes from the chat
// (nimbus/harness/plain/schedule_binding.h).
// Only lib headers are included, so a host test drives the same class with a fake platform, a
// scripted transport and a fake card (test/test_plain_chat_binding). The device seam
// (src/agent/orchestrator_plain.cpp) gives it the device's parts.
//
// It adds no behavior to the loop: a turn is PlainEngine::runTurn, and what the owner sees is
// what the engine sent through `sendPart` (one already-made part per call).
namespace agent {
namespace plain {

struct ChatBindingDeps {
  const Platform* platform = nullptr;            // nowMs, delayMs, nowEpoch, allocLarge, freeLarge
  HttpTransport* transport = nullptr;            // the head and the summarizer (one TLS slot)
  nimbus::orch::tx::TxFiles* files = nullptr;    // /mem/tx (false everywhere: RAM only)
  nimbus::orch::ToolRegistry* registry = nullptr;   // the device's shared registry
  // One part the deliver function already made; true when the channel took it.
  std::function<bool(const std::string& chatId, const std::string& part)> sendPart;
  std::function<std::string()> key;              // the head's key, read at render time, never stored
  std::function<std::string(const nimbus::orch::Principal&)> pinnedNotes;   // null: none
  TextIO compactCtl;                             // unset: kept in RAM
  nimbus::orch::SeenIndex* seen = nullptr;       // null: no rebuild (not a Telegram chat)
  // One line each; null: silent. A std::string, not a const char*: the tg_poll stack gate resolves
  // a std::function call to every handler of its exact signature, and the card listings' name
  // callbacks are void(const char*) (lane E2-FIX2B).
  std::function<void(const std::string& line)> log;
  // Lane E2-TOOLS: a tool whose result is outside content (web search); its outputs taint the turn
  // (ToolOutput::outside). Null: no tool reads outside content.
  std::function<bool(const std::string& tool)> outside;
  // Lane E2-TOOLS: EngineDeps::recentFiles for this chat's person (the device's file store); null: no list.
  std::function<nimbus::orch::tx::RecentFilesBlock::Source(const nimbus::orch::Principal&)> recentFiles;
  // Lane E2-CHAT.
  PhotoFn photo;                                 // the turn's photo bytes by ref; null: stubs only
  std::function<ChatSkills(const nimbus::orch::Principal&)> skills;   // null: no skills
  // A context skill that does not fit the pinned block whole: left out of it (listed instead).
  std::function<void(const std::string& id, size_t bytes)> skillRefused;
  // Once per turn, after runTurn: what the turn spent, for the owner's usage view.
  std::function<void(const TurnOutcome& out)> usage;
  // Lane E2-CMP: each paid summarizer request of this chat's compaction, for the same usage view and
  // monthly ceiling (provider slug, model, its tokens). Null: not counted.
  std::function<void(const std::string& provider, const std::string& model, const nimbus::orch::TokenUsage& u)>
      compactUsage;
  ProgressDeps progress;                         // send unset: no progress line
  // The earlier engine's conversation (lane E2-MIG): fills rows, fold, exportText and tzMinutes
  // of the migration's deps (the binding sets the rest); null, or false: no import. Called at
  // every boot, before the engine opens the log (tx/migrate.h's contract), and only for the chat's
  // own key: a key /clear started (<chat>_c<n>) is never filled.
  std::function<bool(nimbus::orch::tx::MigrateDeps&)> oldEngine;
  // True when no card is fitted at all (the chat runs from RAM by design); null: a card is
  // fitted, so a log that cannot open is an outage (EngineDeps::noCard, lane E2-STORE).
  std::function<bool()> noCard;
  // Lane E2-FSAFE: RailsDeps::pending, the approval shelf for a tainted turn's pending calls (the
  // device: the notes' pending shelf). Null: a pending call is refused, nothing kept, nothing run.
  RailPendingFn railPending;
};

// The owner's line on every turn while the stored conversation could not be loaded (lane
// E2-STORE, finding S5). Owner-facing only: sent to the owner's chat after the turn, never
// stored and never shown to a model.
inline constexpr char kNotLoadedLine[] =
    "The stored conversation could not be loaded; what you say now is kept in memory only until the card is back.";
// Lane E2-FIX3 (finding R1, ruling 116): the owner's line on each turn while the history import holds
// the chat (owner-facing only, as kNotLoadedLine). Stuck: it failed the same way twice in a row.
inline constexpr char kImportStuckLine[] =
    "The earlier conversation could not be imported, so what you say now is kept in memory only. Send /clear "
    "to keep it in a new conversation; the earlier one stays stored on the device.";
inline constexpr char kImportPendingLine[] =
    "The earlier conversation is not imported yet; what you say now is kept in memory only until it is.";

// The tags of the person's day-stream row for a message (the web chat's history, the import's
// rows). "e2" only when an engine-2 turn stores the message itself (it is then engine 2's: never
// imported). A message answered with a device line instead (the history wait, a refusal, a chat
// that could not start) is left untagged, so the import brings it into the conversation once
// at the chat's next boot (lane E2-FIX2B, finding P10).
inline std::string ownerRowTags(const std::string& fromName, bool turnStoresIt) {
  std::string t = fromName.empty() ? std::string() : "from:" + fromName;
  if (turnStoresIt) t += t.empty() ? "e2" : ",e2";
  return t;
}

struct ChatBindingConfig {
  std::string chatKey;                           // the transcript key (/mem/tx/<chatKey>)
  std::string chatId;                            // the routing chat; "" = chatKey
  std::string host = "api.anthropic.com";        // the head's endpoint host
  std::string provider = "anthropic";            // its provider slug (budget and price rows)
  std::string model;                             // the head model
  std::string summarizerModel;                   // the compaction rung (the provider's small model)
  nimbus::orch::Role role = nimbus::orch::Role::Admin;
  std::string timeZone;
  uint16_t dayStartMin = kDayStartDefaultMin;
  int storedRounds = 0;                          // the stored orchLoopRnds
  uint64_t monthlyCeilingMicro = 0;              // the owner's monthly ceiling (0: none set)
};

// One message in.
struct ChatMessage {
  std::string text;
  std::string ch = "tg";                         // u.ch
  std::vector<uint64_t> src;                     // Telegram update ids
  int16_t tzMinutes = 0;
  uint64_t m = 0;                                // the day-stream row it mirrors
  std::vector<nimbus::orch::tx::Part> parts;     // its photos (Image parts "p<ref>")
};

// A turn's spend as the owner's usage view files it (store::recordProviderSpend): one slot of the
// head's provider and model, in the provider's own counts (Anthropic's input leaves the cache out),
// one billed response per head request.
inline nimbus::orch::TurnSpend turnSpendOf(const TurnOutcome& out, const std::string& provider, const std::string& model) {
  nimbus::orch::TurnSpend s;
  nimbus::orch::TokenUsage u;
  u.promptTokens = out.end.promptTokens;
  u.completionTokens = out.end.outputTokens;
  u.cacheReadTokens = out.end.cacheReadTokens;
  u.cacheWriteTokens = out.end.cacheWriteTokens;
  u.cacheWrite1hTokens = out.end.cacheWrite1hTokens;   // the ledger prices it at 2x (R3; lane E2-CACHE)
  u.responses = out.headRequests;
  u.maxRequestIn = out.end.lastPromptTokens;
  s.add(provider.c_str(), model.c_str(), u);   // files nothing when nothing was billed
  return s;
}

class ChatBinding {
 public:
  // By reference: a caller that keeps both on the heap leaves no copy on its stack (E2-FIX2B).
  ChatBinding(const ChatBindingConfig& cfg, const ChatBindingDeps& d) : cfg_(cfg), d_(d) {
    if (cfg_.chatId.empty()) cfg_.chatId = cfg_.chatKey;
    who_ = nimbus::orch::principalForRole(cfg_.chatId, cfg_.role);
    channel_.send = [this](const std::string& chat, const std::string& part) {
      const bool ok = d_.sendPart && d_.sendPart(chat, part);
      ++(ok ? sent_ : refused_);
      return ok;
    };
    channel_.isAllowed = [this](const std::string& chat) { return chat == cfg_.chatId; };
    channel_.firstAllowedChat = [this] { return cfg_.chatId; };
  }
  ChatBinding(const ChatBinding&) = delete;
  ChatBinding& operator=(const ChatBinding&) = delete;

  const ChatBindingConfig& config() const { return cfg_; }
  PlainEngine* engine() { return engine_.get(); }
  const nimbus::orch::tx::TxArena* arena() const { return arena_.get(); }
  const HeadCost& cost() const { return cost_; }
  nimbus::orch::ChatBreaker& breaker() { return breaker_; }
  // Parts the channel took and refused in the last turn.
  uint32_t sentParts() const { return sent_; }
  uint32_t refusedParts() const { return refused_; }
  // The last boot's import of the earlier engine's conversation (E2-MIG): whether it ran, its
  // status and its outcome. A refused import does not stop the boot (the owner is answered); the
  // next boot runs it again, and the library resumes where it stopped.
  bool migrationRan() const { return migrationRan_; }
  const Status& migrationStatus() const { return migrationStatus_; }
  const nimbus::orch::tx::MigrationOutcome& migration() const { return migration_; }
  uint32_t migrationMs() const { return migrationMs_; }
  // Lane E2-FIX2B (finding P3, tx/migrate.h's contract): the import ran and has not answered Done,
  // so nothing is stored for this chat; it runs unloaded and the import is tried again before
  // each turn (reload). A refusal it can never get past (InvalidArgument) does not hold the chat.
  bool importPending() const { return importPending_; }
  // Lane E2-FIX3 (finding R1): the import failed the same way (error and place) on its last two
  // tries, so trying again before each turn is not likely to get past it (a marker over 8 KB, a
  // page that never decodes, a read the card always refuses). The tries go on (it ends by itself
  // when the cause is gone); the owner is told the import is the cause and that /clear gets out.
  // A card that does not answer is the card's cause (kNotLoadedLine), never the import's.
  bool importStuck() const { return importPending_ && movedTo_.empty() && cardAnswered_ && sameFailures_ >= 2; }
  // The key storeHeldAs is storing the held turns into ("" before it was asked).
  const std::string& movedTo() const { return movedTo_; }

  // Once: the head's budget and price, the arena, the log, the seams, the engine and its boot,
  // then the seen-index rebuild. A refused allocation is a status, never an abort.
  Status boot(bool afterCrash = false) {
    namespace tx = nimbus::orch::tx;
    if (engine_) return Status::fail(tx::Err::Busy, "plain::ChatBinding::boot.twice");
    if (!bound()) return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::boot");
    const Platform* p = d_.platform;
    Status st = headFigures();
    if (!st.ok()) return st;
    try {
      std::unique_ptr<tx::TxArena> arena(new tx::TxArena());
      st = arena->init(arenaBytes_, p->allocLarge, p->freeLarge);
      if (!st.ok()) return st;
      arena_ = std::move(arena);
      log_.reset(new tx::TxLog(*d_.files, cfg_.chatKey));
      hooks_.reset(new PlainHooks());
      (void)installSuppression(*hooks_);
      RailsDeps rails;
      rails.pending = d_.railPending;
      st = installRails(*hooks_, rails);
      if (st.ok()) {
        driver_.reset(new CompactDriver(compactDeps()));
        st = installCompaction(*hooks_, *driver_);
      }
      if (!st.ok()) return fail(st);
      if (d_.progress.send) {
        reporter_.reset(new ProgressReporter(d_.progress));
        const Platform* clock = p;
        st = installProgress(*hooks_, *reporter_, clock->nowMs);   // a copy: no lambda of the same signature wrapping it
        if (!st.ok()) return fail(st);
      }
      migrateOld();
      engine_.reset(new PlainEngine(deps()));
      st = engine_->boot(BootCtx{p->nowEpoch(), afterCrash});
    } catch (const std::bad_alloc&) {
      st = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::boot.memory");
    }
    if (!st.ok()) return fail(st);
    if (engine_->unloaded()) say("e2: the stored conversation could not be loaded: held in RAM, retried before each turn");
    rebuildSeen();
    return st;
  }

  // The stored conversation could not be loaded and is not yet (the owner's line is due).
  bool unloaded() const { return engine_ && engine_->unloaded(); }
  // While unloaded: open the log again and store the RAM turns after the conversation
  // (PlainEngine::reopen), then rebuild the seen index. True when the chat is loaded after it.
  bool reload() {
    if (!engine_ || moved_) return !!engine_;
    if (!movedTo_.empty()) return storeHeldAs(movedTo_).ok();   // E2-FIX3: only into the new conversation
    if (!engine_->unloaded()) return true;
    if (importPending_) {   // P3: the import first; the RAM turns follow what it stored, once, in order
      migrateOld();
      if (importPending_) {
        say("e2: the earlier engine's conversation is still not imported: this turn is held in RAM");
        return false;
      }
      say("e2: the earlier engine's conversation is imported now");
    }
    const Status r = engine_->reopen();
    if (engine_->unloaded()) {
      if (!r.ok()) say("e2: the stored conversation still cannot be loaded: this turn is held in RAM");
      return false;
    }
    say("e2: the stored conversation is loaded again; the turns held in RAM are stored after it");
    rebuildSeen();
    return true;
  }

  // Lane E2-FIX3 (finding R1): /clear on a chat the history import holds. The turns held in RAM are
  // stored, in order, each once, as the start of the transcript `key` (the new conversation), and
  // nothing is written under this chat's key: its earlier history stays on the card as it is, for a
  // later import. Success: this binding is done (moved), the caller boots one on `key`. Not ok:
  // still held, and every later reload() stores into `key` only (resuming after what landed).
  Status storeHeldAs(const std::string& key) {
    namespace tx = nimbus::orch::tx;
    if (moved_ || !engine_ || !engine_->unloaded() || (!importPending_ && movedTo_.empty()) ||
        (!movedTo_.empty() && key != movedTo_))
      return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::storeHeldAs");
    std::unique_ptr<tx::TxLog> log(new (std::nothrow) tx::TxLog(*d_.files, key));
    if (!log) return Status::fail(tx::Err::TooLarge, "plain::ChatBinding::storeHeldAs.memory");
    movedTo_ = key;
    importPending_ = false;   // the import of this key is left to a later boot; nothing more runs here
    const Status st = engine_->reopenAs(*log, key);
    if (!st.ok()) {
      say("e2: the turns held in RAM are not stored in the new conversation yet: tried again before the next turn");
      return st;
    }
    log->close();
    moved_ = true;
    say("e2: the turns held in RAM are stored in the new conversation");
    return st;
  }
  // storeHeldAs succeeded: nothing more runs on this binding.
  bool moved() const { return moved_; }

  // One person's message through the loop. The breaker admits every person's message (it holds
  // back only turns no person started); the spend is recorded after.
  Status turn(const ChatMessage& msg, TurnOutcome& out) {
    namespace tx = nimbus::orch::tx;
    if (!engine_ || moved_) return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::turn.before-boot");
    if (busy_) return Status::fail(tx::Err::Busy, "plain::ChatBinding::turn.busy");
    busy_ = true;
    struct Idle {
      bool& busy;
      ~Idle() { busy = false; }
    } idle{busy_};
    sent_ = refused_ = 0;
    const uint32_t now = d_.platform->nowMs();
    (void)breaker_.admit(/*person=*/true, now);
    if (reporter_) reporter_->open(turnCtx(), now);
    TurnInput in;
    in.ctx = turnCtx();
    in.text = msg.text;
    in.ch = msg.ch;
    in.src = msg.src;
    in.tzMinutes = msg.tzMinutes;
    in.m = msg.m;
    in.parts = msg.parts;
    // Lane E2-STORE: a chat whose stored conversation could not be loaded tries again before
    // the turn, so the turn sees the whole conversation as soon as the card answers.
    if (movedTo_.empty()) reload();   // E2-FIX3: held for the new conversation, the caller stores it first
    Status st;
    try {
      st = engine_->runTurn(in, out);
    } catch (const std::bad_alloc&) {
      st = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::turn.memory");
    }
    if (engine_->unloaded() && d_.sendPart) (void)d_.sendPart(cfg_.chatId, unloadedLine());
    breaker_.spent(/*person=*/true, out.end.microUsd, d_.platform->nowMs());
    if (reporter_) reporter_->close(d_.platform->nowMs());
    if (d_.usage) d_.usage(out);
    return st;
  }

  // A compaction of this chat now (/compact): the driver's manual run, stepped here until it has
  // answered (the driver tells the chat its one line through sendPart). Busy while one is queued.
  Status compactNow() {
    namespace tx = nimbus::orch::tx;
    if (!engine_ || !driver_) return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::compactNow");
    if (busy_ || !driver_->requestManual()) return Status::fail(tx::Err::Busy, "plain::ChatBinding::compactNow");
    Status st;
    for (uint32_t i = 0; i < kCompactManualRequests + 2 && driver_->manualPending(); ++i) {
      IdleCtx ctx;
      ctx.now = d_.platform->nowEpoch();
      ctx.nowMs = d_.platform->nowMs();
      ctx.clockSet = ctx.now != 0;
      IdleOutcome o;
      try {
        st = engine_->idle(ctx, o);
      } catch (const std::bad_alloc&) {
        st = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::compactNow.memory");
      }
      if (!st.ok()) break;
    }
    return st;
  }

  // What /budget shows (rulings 105 and 106): the fixed part as this chat composes it now, the
  // summary and the transcript after it, the marks and the settings, in tokens.
  Status budgetView(BudgetView& out) {
    namespace tx = nimbus::orch::tx;
    if (!engine_ || !engine_->head() || !engine_->digests())
      return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::budgetView");
    tx::HeadBudget b;
    Status st = tx::budgetFor(headKey(), tx::kDefaultProfile, b);
    if (!st.ok()) return st;
    const TurnCtx t = turnCtx();
    TurnSetup ts;
    if (!(st = setup(t, ts)).ok()) return st;
    ts.facts.turn = &t;
    SystemBlocks sys;
    std::vector<ToolSpec> tools;
    if (!(st = composeSystem(ts.facts, sys)).ok() || !(st = toolManifest(ts.facts, tools)).ok()) return st;
    BudgetViewIn in;
    in.budget = &b;
    in.system = &sys;
    in.tools = &tools;
    in.live = engine_->digests();
    in.arena = arena_.get();
    in.thru = engine_->head()->thru;
    return plain::budgetView(in, out);
  }

  // Lane E2-SCHED: this chat as the sink of a reminder or routine fire (plain::deliverHeld): its
  // channel's delivery and a routine device note appended to its transcript between its turns.
  OwnerSink ownerSink() {
    OwnerSink s;
    s.chatKey = cfg_.chatKey;
    s.chatId = cfg_.chatId;
    s.deliver = [this](const DeliverInput& in, DeliverOutcome& out) {
      return deliverTurn(DeliverDeps{&channel_, nullptr, nullptr}, in, out);
    };
    s.append = [this](const std::string& note, uint64_t at) {
      if (!engine_ || busy_) return Status::fail(nimbus::orch::tx::Err::Busy, "plain::ChatBinding::ownerSink.append");
      return appendOwnerNote(*engine_, note, at);
    };
    return s;
  }

  // Lane E2-SCHED: one routine fire over this chat's parts (plain::runRoutine): the platform, the
  // transport, the card, the shared registry (runRoutine cuts it to the routine's tools), this
  // chat's dispatch and setup (pinned notes, the money guard's cost), the suppression seam, and
  // the head `model` on this chat's provider ("" = this chat's head). The caller sets held,
  // quiet, tzMinutes and head.tainted.
  RoutineDeps routineDeps(const std::string& model) {
    RoutineDeps r;
    r.platform = d_.platform;
    r.transport = d_.transport;
    r.files = d_.files;
    r.reg = d_.registry;
    r.dispatch = [this](const ToolCall& c, const TurnCtx& t) { return dispatchCall(c, t); };
    r.setup = [this](const TurnCtx& t, TurnSetup& out) { return setup(t, out); };
    r.head = turnCtx();
    r.head.trigger = Trigger::Routine;
    if (!model.empty()) r.head.model = model;
    r.moreHooks = [](PlainHooks& h) { (void)installSuppression(h); };
    r.owner = ownerSink();
    return r;
  }

  // The head of a message: true when every update id of `src` was already taken into this chat
  // (a re-served update after a restart: acked, never answered again).
  bool allSeen(const std::vector<uint64_t>& src) const {
    if (!d_.seen || !d_.seen->ready() || src.empty()) return false;
    for (uint64_t id : src)
      if (!d_.seen->seen(cfg_.chatId, id)) return false;
    return true;
  }

 private:
  void say(const char* line) const {
    if (d_.log) d_.log(std::string(line));
  }
  // The owner's line for an unloaded turn, naming the true cause (E2-FIX3, ruling 116).
  const char* unloadedLine() const {
    if (importStuck()) return kImportStuckLine;
    return importPending_ && cardAnswered_ ? kImportPendingLine : kNotLoadedLine;
  }
  // Every binding boot needs is there.
  bool bound() const {
    const Platform* p = d_.platform;
    const bool clock = p && p->nowMs && p->delayMs && p->nowEpoch;
    const bool memory = p && p->allocLarge && p->freeLarge;
    return clock && memory && d_.transport && d_.files && d_.registry && !cfg_.model.empty();
  }
  // E2-MIG: tx::migrate for this chat before the engine opens its log. Digest 0 is asked of the
  // compaction rung (the summarizer) on the same transport.
  void migrateOld() {
    namespace tx = nimbus::orch::tx;
    const Status before = migrationStatus_;
    migrationRan_ = false;
    importPending_ = false;
    migrationStatus_ = Status::success();
    migration_ = tx::MigrationOutcome{};
    // Only the chat's own key (generation 0) takes the earlier engine's history: a conversation
    // that /clear started (<chat>_c<n>, E2-CHAT) begins empty, and the import never fills it.
    if (!d_.oldEngine || cfg_.chatKey != cfg_.chatId) return;
    // On the heap: the import runs deep on the device's poll task (the tg_poll stack gate, E2-FIX2B).
    std::unique_ptr<tx::MigrateDeps> mp(new (std::nothrow) tx::MigrateDeps());
    if (!mp) {
      migrationRan_ = true;
      migrationStatus_ = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::migrateOld.memory");
      importPending_ = !(d_.noCard && d_.noCard());
      return;
    }
    tx::MigrateDeps& m = *mp;
    m.files = d_.files;
    m.chatKey = cfg_.chatKey;
    m.head = headKey();
    m.arena = arena_.get();
    m.ramFollows = true;   // RAM turns are stored after the import (PlainEngine::reopen renumbers them)
    m.now = d_.platform->nowEpoch();
    m.dayStartMin = cfg_.dayStartMin;
    m.digest0 = digest0Asker(compactDeps());
    if (!d_.oldEngine(m)) return;
    migrationRan_ = true;
    const uint32_t t0 = d_.platform->nowMs();
    migrationStatus_ = tx::migrate(m, migration_);
    migrationMs_ = d_.platform->nowMs() - t0;
    if (migrationStatus_.ok()) {
      sameFailures_ = 0;
      return;
    }
    say("e2: the earlier engine's conversation was not imported this time");
    const bool noCard = d_.noCard && d_.noCard();   // no card at all: RAM by design, nothing to hold for
    importPending_ = !noCard && migrationStatus_.err != tx::Err::InvalidArgument;
    const bool same = !before.ok() && before.err == migrationStatus_.err &&
                      std::string(before.where ? before.where : "") == (migrationStatus_.where ? migrationStatus_.where : "");
    sameFailures_ = same ? sameFailures_ + 1 : 1;
    std::vector<std::string> names;
    cardAnswered_ = d_.files->list(tx::kTxRoot, names);   // the card answers: the cause is the import
    if (importStuck()) say("e2: the earlier engine's conversation fails to import the same way: the owner is offered /clear");
  }
  // RB-TG1: the chat's taken updates from the filled arena and head.json's high water.
  void rebuildSeen() {
    if (!d_.seen || !d_.seen->ready() || !engine_->head()) return;
    if (!nimbus::orch::rebuildSeen(*d_.seen, cfg_.chatId, *engine_->head(), *arena_).ok())
      say("e2: seen-index rebuild refused");
  }
  Status fail(const Status& st) {
    engine_.reset();   // a failed boot leaves the binding unbooted, never half-booted
    return st;
  }

  // Does `block` reach the request whole as the last lines of the pinned notes in `facts`?
  static bool keptWhole(const RequestFacts& facts, const std::string& block) {
    SystemBlocks sys;
    if (!composeSystem(facts, sys).ok()) return false;
    const std::string tail = "\n" + block;
    return sys.notes.size() >= tail.size() && sys.notes.compare(sys.notes.size() - tail.size(), tail.size(), tail) == 0;
  }

  // The skills index and the context skills: each context skill rides the pinned block whole,
  // after the notes, or is listed in the index and named through skillRefused. Measured against
  // the index with every context skill listed, so a later one cannot squeeze an earlier one out.
  void skillFacts(const TurnCtx& t, RequestFacts& facts) {
    if (!d_.skills) return;
    const ChatSkills s = d_.skills(who_);
    std::string worst = s.index;
    for (const ContextSkill& c : s.context) worst += (worst.empty() ? "" : "\n") + c.indexLine;
    RequestFacts probe = facts;
    probe.turn = &t;
    probe.skillsIndex = worst;
    std::string index = s.index;
    for (const ContextSkill& c : s.context) {
      const std::string before = probe.pinnedNotes;
      probe.pinnedNotes = before.empty() ? c.block : before + "\n" + c.block;
      if (keptWhole(probe, c.block)) continue;
      probe.pinnedNotes = before;
      index += (index.empty() ? "" : "\n") + c.indexLine;
      if (d_.skillRefused) d_.skillRefused(c.id, c.block.size());
    }
    facts.pinnedNotes = probe.pinnedNotes;
    facts.skillsIndex = index;
  }

  // The head's marks, arena size and guard cost. No price row: refused (money fails closed).
  Status headFigures() {
    namespace tx = nimbus::orch::tx;
    tx::HeadBudget b;
    Status st = tx::budgetFor(headKey(), tx::kDefaultProfile, b);
    if (!st.ok()) return st;
    arenaBytes_ = tx::arenaBytesFor(b);
    const nimbus::orch::BuiltinPrice* row = nimbus::orch::builtinModelPrice(cfg_.provider.c_str(), cfg_.model.c_str());
    if (!row) row = nimbus::orch::builtinDefaultPrice(cfg_.provider.c_str(), cfg_.model.c_str());
    if (!row || !row->microIn || !row->microOut)
      return Status::fail(tx::Err::NotFound, "plain::ChatBinding::boot.price");
    HeadRates r;
    r.in = row->microIn;
    r.out = row->microOut;
    r.cacheRead = row->microCacheR ? row->microCacheR : row->microIn / 10;
    r.cacheWrite = row->microCacheW ? row->microCacheW : row->microIn + row->microIn / 4;
    cost_ = guardCostFor(b, r);
    breaker_.setSpendCap(nimbus::orch::spendRateCapMicro(
        cfg_.monthlyCeilingMicro, nimbus::orch::blendedTurnMicro(cost_.warmRequestMicroUsd, cost_.coldRequestMicroUsd)));
    return Status::success();
  }

  nimbus::orch::tx::HeadKey headKey() const { return nimbus::orch::tx::HeadKey{cfg_.provider, cfg_.model, false}; }

  TurnCtx turnCtx() const {
    TurnCtx c;
    c.chatKey = cfg_.chatKey;
    c.chatId = cfg_.chatId;
    c.turn = 0;   // the engine numbers the turn
    c.trigger = Trigger::Owner;
    c.host = cfg_.host;
    c.model = cfg_.model;
    c.dialect = Dialect::AnthropicMessages;
    c.dayStartMin = cfg_.dayStartMin;
    c.who = &who_;
    return c;
  }

  HeadEndpoint endpoint() const {
    HeadEndpoint e;
    e.host = cfg_.host;
    e.key = d_.key ? d_.key : [] { return std::string(); };
    return e;
  }

  Status setup(const TurnCtx& t, TurnSetup& out) {
    out.facts.reg = d_.registry;
    out.facts.admin = who_.owner;
    out.facts.timeZone = cfg_.timeZone;
    if (d_.pinnedNotes) out.facts.pinnedNotes = d_.pinnedNotes(who_);
    skillFacts(t, out.facts);
    out.photo = d_.photo;
    out.dayStartMin = cfg_.dayStartMin;
    out.head = endpoint();
    out.wire = nullptr;   // wireFor(AnthropicMessages)
    out.storedRounds = cfg_.storedRounds;
    out.cost = cost_;
    return Status::success();
  }

  // plain_rig.h's dispatch: the registry's answer as the model sees it. Arguments that are not a
  // JSON object reach the handler as an empty object.
  ToolOutput dispatchCall(const ToolCall& c, const TurnCtx& turn) {
    JsonDocument args;
    (void)deserializeJson(args, c.argsJson, DeserializationOption::NestingLimit(64));
    const nimbus::orch::ToolResult r = d_.registry->dispatch(c.name, args.as<JsonObjectConst>(), turn.who ? *turn.who : who_);
    ToolOutput o;
    o.id = c.id;
    o.name = c.name;
    o.isError = !r.success;
    o.text = r.success ? r.output : r.error;
    o.outside = d_.outside && d_.outside(c.name);
    return o;
  }

  CompactDeps compactDeps() {
    CompactDeps c;
    c.platform = d_.platform;
    c.transport = d_.transport;
    c.log = log_.get();
    c.arena = arena_.get();
    c.files = d_.files;
    c.chatKey = cfg_.chatKey;
    c.chatId = cfg_.chatId;
    c.head = [this] { return engine_ ? engine_->head() : nullptr; };
    c.digests = [this] { return engine_ ? engine_->digests() : nullptr; };
    c.omittedThru = [this] { return engine_ ? engine_->omittedThru() : uint64_t(0); };
    c.rungs = [this](std::vector<SummarizerRung>& out) {
      out.clear();
      SummarizerRung r;
      r.endpoint = endpoint();
      r.host = cfg_.provider;
      r.model = cfg_.summarizerModel.empty() ? cfg_.model : cfg_.summarizerModel;
      r.dialect = Dialect::AnthropicMessages;
      out.push_back(r);
    };
    c.jobRunning = [](uint32_t) { return false; };   // no agent jobs on engine 2 yet
    c.park = [this](const SummarizerRung&) { say("e2: compaction rung refused its key or credit"); };
    c.spent = [this](const SummarizerRung& r, const nimbus::orch::TokenUsage& u) {
      if (d_.compactUsage) d_.compactUsage(r.host, r.model, u);
    };
    c.tell = [this](const std::string& chat, const std::string& text) {
      if (d_.sendPart) (void)d_.sendPart(chat, text);
    };
    c.ctl = d_.compactCtl;
    c.headKey = headKey();
    c.dayStartMin = cfg_.dayStartMin;
    return c;
  }

  EngineDeps deps() {
    EngineDeps e;
    e.platform = d_.platform;
    e.transport = d_.transport;
    e.channel = &channel_;
    e.hooks = hooks_.get();
    e.log = log_.get();
    e.arena = arena_.get();
    e.files = d_.files;
    e.chatKey = cfg_.chatKey;
    e.dispatch = [this](const ToolCall& c, const TurnCtx& t) { return dispatchCall(c, t); };
    e.setup = [this](const TurnCtx& t, TurnSetup& out) { return setup(t, out); };
    if (d_.recentFiles) e.recentFiles = d_.recentFiles(who_);
    e.stablePrefix = true;   // lane E2-CACHE: a person's chat keeps its system part across switches
    if (d_.progress.send)
      e.progress = [this](const ProgressEvent& ev) {
        if (reporter_) (void)reporter_->tick(ev);
      };
    e.noCard = d_.noCard;
    e.holdUnloaded = [this] { return importPending_; };
    return e;
  }

  ChatBindingConfig cfg_;
  ChatBindingDeps d_;
  nimbus::orch::Principal who_;
  Channel channel_;
  HeadCost cost_;
  size_t arenaBytes_ = 0;
  nimbus::orch::ChatBreaker breaker_;
  uint32_t sent_ = 0, refused_ = 0;
  bool busy_ = false;
  bool migrationRan_ = false;
  bool importPending_ = false;
  uint32_t sameFailures_ = 0;   // the import's failures in a row with the same error and place (E2-FIX3)
  bool cardAnswered_ = false;   // the card listed /mem/tx after the import's last failure
  std::string movedTo_;         // storeHeldAs's key: the held turns go there only
  bool moved_ = false;          // storeHeldAs succeeded: the binding is done
  uint32_t migrationMs_ = 0;
  Status migrationStatus_;
  nimbus::orch::tx::MigrationOutcome migration_;
  std::unique_ptr<nimbus::orch::tx::TxArena> arena_;
  std::unique_ptr<nimbus::orch::tx::TxLog> log_;
  std::unique_ptr<PlainHooks> hooks_;
  std::unique_ptr<CompactDriver> driver_;        // the hooks call it: it outlives no hook
  std::unique_ptr<ProgressReporter> reporter_;
  std::unique_ptr<PlainEngine> engine_;
};

}  // namespace plain
}  // namespace agent
