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

// plain/chat_binding.h - one chat on engine 2 on the device (lane E2-DEV). A copy of nimbusd's
// one-chat rig (nimbusd/src/plain_rig.h, PlainRig) with what a device chat needs on top:
//   - the arena sized for the head (tx::arenaBytesFor of its budget, about 1.1 MB for Sonnet),
//     not the 1,728 KB constant;
//   - compaction installed (plain::installCompaction): the summarizer is the head host's small
//     model on the same transport; the control and chatsum files are the binding's TextIO;
//   - the suppression seam (installSuppression), the money guard (TurnSetup::cost from the head's
//     price row; no price row: boot refuses, fail closed) and one ChatBreaker;
//   - the seen index rebuilt from the filled arena after boot (RB-TG1);
//   - the shared tool registry, dispatched with the turn's Principal (plain_rig.h's dispatch).
// Lane E2-CHAT adds the owner's daily chat on top:
//   - photos: the message's image parts (ChatMessage::parts) and the photo hook (TurnSetup::photo);
//   - skills: the skills index and the context skills in the pinned block, each one whole or not
//     at all (skillRefused names one left out; chat_skills.h);
//   - commands that act on the engine: compactNow (a compaction now) and budgetView (/budget);
//   - the turn's usage handed to `usage` once per turn; the progress line (installProgress).
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
  TextIO chatsum;                                // unset: no mirror
  nimbus::orch::SeenIndex* seen = nullptr;       // null: no rebuild (not a Telegram chat)
  std::function<void(const char* line)> log;     // one line each; null: silent
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
  ProgressDeps progress;                         // send unset: no progress line
};

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
  u.responses = out.headRequests;
  u.maxRequestIn = out.end.lastPromptTokens;
  s.add(provider.c_str(), model.c_str(), u);   // files nothing when nothing was billed
  return s;
}

class ChatBinding {
 public:
  ChatBinding(ChatBindingConfig cfg, ChatBindingDeps d) : cfg_(std::move(cfg)), d_(std::move(d)) {
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
      driver_.reset(new CompactDriver(compactDeps()));
      st = installCompaction(*hooks_, *driver_);
      if (!st.ok()) return fail(st);
      if (d_.progress.send) {
        reporter_.reset(new ProgressReporter(d_.progress));
        const Platform* clock = p;
        st = installProgress(*hooks_, *reporter_, [clock] { return clock->nowMs(); });
        if (!st.ok()) return fail(st);
      }
      engine_.reset(new PlainEngine(deps()));
      st = engine_->boot(BootCtx{p->nowEpoch(), afterCrash});
    } catch (const std::bad_alloc&) {
      st = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::boot.memory");
    }
    if (!st.ok()) return fail(st);
    rebuildSeen();
    return st;
  }

  // One person's message through the loop. The breaker admits every person's message (it holds
  // back only turns no person started); the spend is recorded after.
  Status turn(const ChatMessage& msg, TurnOutcome& out) {
    namespace tx = nimbus::orch::tx;
    if (!engine_) return Status::fail(tx::Err::InvalidArgument, "plain::ChatBinding::turn.before-boot");
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
    Status st;
    try {
      st = engine_->runTurn(in, out);
    } catch (const std::bad_alloc&) {
      st = Status::fail(tx::Err::TooLarge, "plain::ChatBinding::turn.memory");
    }
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
    if (d_.log) d_.log(line);
  }
  // Every binding boot needs is there.
  bool bound() const {
    const Platform* p = d_.platform;
    const bool clock = p && p->nowMs && p->delayMs && p->nowEpoch;
    const bool memory = p && p->allocLarge && p->freeLarge;
    return clock && memory && d_.transport && d_.files && d_.registry && !cfg_.model.empty();
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
    c.tell = [this](const std::string& chat, const std::string& text) {
      if (d_.sendPart) (void)d_.sendPart(chat, text);
    };
    c.ctl = d_.compactCtl;
    c.chatsum = d_.chatsum;
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
    if (d_.progress.send)
      e.progress = [this](const ProgressEvent& ev) {
        if (reporter_) (void)reporter_->tick(ev);
      };
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
  std::unique_ptr<nimbus::orch::tx::TxArena> arena_;
  std::unique_ptr<nimbus::orch::tx::TxLog> log_;
  std::unique_ptr<PlainHooks> hooks_;
  std::unique_ptr<CompactDriver> driver_;        // the hooks call it: it outlives no hook
  std::unique_ptr<ProgressReporter> reporter_;
  std::unique_ptr<PlainEngine> engine_;
};

}  // namespace plain
}  // namespace agent
