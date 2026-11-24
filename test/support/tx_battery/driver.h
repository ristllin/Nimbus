#pragma once
#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../scripted_provider/scripted_provider.h"
#include "items.h"
#include "mem_files.h"
#include "nimbus/harness/channel.h"
#include "nimbus/harness/plain/compact.h"
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/exchange.h"   // providerSlugOf: the budget key (RB-T4)
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/rails.h"
#include "nimbus/harness/plain/resume.h"
#include "nimbus/harness/platform.h"
#include "nimbus/net/tg_inbox.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/log.h"
#include "props.h"
#include "rows.h"
#include "stub_wire.h"
#include "world.h"

// tx_battery/driver.h - one chat run through the plain engine for the battery (lane RB-B0b):
// a scripted model at the transport, storage that can be cut, a fake tool registry, a
// recording channel and a virtual clock. The properties never reach past this class.
//
// PLUMBING, completed by the implementing lanes (RB-E1a first, then RB-E1b, RB-X1, RB-C2):
// how a turn is started (intake of the owner message), which modules are installed, and
// what EngineDeps carries. They edit this file (a declared cross-lane edit) and never a
// property in the suites. Every engine call is wrapped so a power cut (Killed) ends it the
// way a real cut would: nothing after it runs, the process state is gone, storage stays.
namespace txb {

namespace plain = agent::plain;

// What the battery's summarizer answers unless a case says otherwise: changes to the cumulative
// summary (lane RB-C3, tx::parseSummaryEdits).
inline constexpr const char* kSummaryAnswer =
    "+F The owner asked about the boat, read files and searched the web.\n"
    "+F He prefers short answers.\n"
    "+Q The boat Nereid II is 9.5 m long.";

class Battery {
 public:
  explicit Battery(plain::Dialect d = plain::Dialect::AnthropicMessages) : dialect(d) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return epoch; };
    platform.delayMs = [this](uint32_t d2) { ms += d2; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    channel.send = [this](const std::string& chat, const std::string& text) { return onSend(chat, text); };
    channel.isAllowed = [](const std::string&) { return true; };
    channel.firstAllowedChat = [this] { return chatKey; };
    model.clock = &ms;
    summarizer.clock = &ms;
    summarizer.respondWith([](const sp::Recorded&, size_t) { return sp::say(kSummaryAnswer); });
    registerTools();
  }
  // A battery on one provider table row (lane E2-R3a): its dialect, endpoint, model, route, and the
  // scripted provider answering in that provider's own chat shape.
  explicit Battery(const Row& r) : Battery(r.dialect) {
    hostOverride = r.host;
    modelOverride = r.model;
    routed = r.routed;
    model.chatShape = r.slot == caps::Slot::Mistral ? sp::ChatShape::Mistral
                      : r.slot == caps::Slot::Zai   ? sp::ChatShape::Zai
                                                    : sp::ChatShape::OpenAI;
  }

  plain::Dialect dialect;
  std::string chatKey = "1001";
  sp::ScriptedProvider model;
  // The summarizer's own provider (lane RB-C3): compaction runs at turn open and inside a turn too, so its
  // requests never take a step the head's script holds. A case that answers summaries sets its responder.
  sp::ScriptedProvider summarizer;
  MemFiles files;
  World world;
  std::vector<SentMessage> sent;
  uint64_t epoch = kT0;
  uint32_t ms = 0;
  long killAtSend = -1;            // the send with this index is cut after it went out
  long killBeforeSend = -1;        // the send with this index is cut before it goes out
  bool killed = false;
  std::string killedAt;
  std::vector<uint32_t> requestTurn;   // the turn of every recorded request
  // What composition reads (RB-E1a): the world's tools as the registry declares them, the
  // owner's Principal, and the wire (the dialect's real one once it lands, else txb::StubWire).
  nimbus::orch::ToolRegistry reg;
  // The live file list the engine's recent-files block reads (null: none), set before boot.
  tx::RecentFilesBlock::Source recentFiles;
  // The device's day start the turn setup hands the engine (RB-REC5's setting).
  uint16_t dayStartMin = plain::kDayStartDefaultMin;
  nimbus::orch::Principal who = nimbus::orch::principalForRole("1001", nimbus::orch::Role::Admin);
  // The head as the binding gives it (RB-E1b): host and model overrides (empty: the
  // dialect's), a head through the router, and a delivery of its own (null: deliverTurn).
  std::string hostOverride, modelOverride;
  bool routed = false;
  bool noHead = false;             // the setup names no model (a binding fault; ruling 89 item 3)
  // Compaction installed (default). A case that measures the arena or the ladder alone turns it off: since
  // lane RB-C3 the engine compacts first at turn open and before every rung, so the ladder would never act.
  bool compaction = true;
  // Compaction answers the engine's own asks at turn open and inside a turn (default). A case written for
  // the idle step alone turns this off: the driver then runs only from idle steps, as before lane RB-C3.
  bool compactFirst = true;
  std::unique_ptr<plain::CompactDriver> idleDriver;
  void installIdleOnlyCompaction() {
    idleDriver.reset(new plain::CompactDriver(compactDeps()));
    plain::CompactDriver* d = idleDriver.get();
    (void)hooks->idleStep(plain::HookSlot::Compaction, [d](const plain::IdleCtx& ctx) {
      plain::IdleOutcome o;
      if (ctx.urgency != plain::Urgency::None) return o;
      plain::CompactOutcome out;
      o.worked = d->step(ctx, out).ok() && out.ran;
      return o;
    });
  }
  // The profile every turn asks for: the default, full (ruling 91, blockers/F8). A case that tests an
  // owner's smaller choice names it here.
  tx::Profile profile = tx::kDefaultProfile;
  bool noHost = false;             // the setup names no host either way (a binding fault; ruling 91, F2)
  plain::DeliverFn deliverFn;
  plain::HeadCost cost;            // what the guards scale from (TurnSetup::cost)
  int storedRounds = 0;            // the stored orchLoopRnds (TurnSetup::storedRounds)
  std::function<void(const plain::ProgressEvent&)> progressFn;   // EngineDeps::progress (RB-X3)
  plain::TurnCeiling routineCeiling;   // TurnSetup::ceiling for a Routine turn (RB-U1a); none by default
  uint32_t bodyCapLimit = 0;       // TurnSetup::bodyCapLimit (lane E2-FIX9): a smaller body cap; 0: the head's

  // Process state: gone at a power cut, rebuilt by boot().
  agent::Platform platform;
  agent::Channel channel;
  std::unique_ptr<tx::TxArena> arena;
  std::unique_ptr<tx::TxLog> log;
  std::unique_ptr<plain::PlainHooks> hooks;
  std::unique_ptr<plain::PlainEngine> engine;
  std::string compactCtl, chatsum;     // RB-C2's small files (they survive a power cut)
  uint32_t turnNo = 0;
  size_t sends = 0;
  size_t tornCuts = 0;                 // torn cuts so far: each may leave one undecodable line
  plain::TurnInput lastInput;          // the owner message in flight (redelivered after a cut)
  std::vector<plain::ResumeReport> resumeReports;   // what the resume seam did at each boot (RB-X1)
  long killAtDispatch = -1;            // the dispatch with this index is cut before the tool runs (RB-X1)
  bool refuseSends = false;            // the channel refuses every send (no network at boot; RB-X1)
  size_t arenaBytes = tx::kActiveChatArenaBytes;   // the chat's arena at each boot (RB-X1: a small one)
  std::function<void(plain::PlainHooks&)> moreHooks;   // a test's own handlers, installed at each boot
  bool registryDispatch = false;   // E2-P-OUT2: calls run on `reg` as the device binding runs them (dispatchCall)
  size_t dispatches = 0;
  bool inboxRefuses = false;       // no card for the inbox: a long owner text passes whole (RB-TG1)
  plain::RailsDeps railsDeps;      // the rails' approval store (RB-RL); none: a pending change is refused
  // The binding's photo hook (TurnSetup::photo; lane E2-FIX4): the stored bytes of a photo by ref, as the
  // device reads them back from the card. Empty: no hook (every photo renders as its line).
  std::map<std::string, std::string> photoStore;

  // The head a turn of this battery names (host and model), for a test that builds its own TurnInput.
  plain::TurnCtx headCtx() const { return turnCtx(0, plain::Trigger::Owner); }
  // The budget key of that head as the engine forms it (its provider slug, not the endpoint host).
  tx::HeadKey headKey() const {
    const plain::TurnCtx c = headCtx();
    plain::HeadEndpoint e;
    e.host = c.host;
    e.routed = routed;
    return tx::HeadKey{plain::providerSlugOf(c, e), c.model, routed};
  }

  void advance(uint32_t seconds) {
    epoch += seconds;
    ms += seconds * 1000u;
  }

  // Start (or restart) the process over the same storage, world and model.
  tx::Status boot(bool afterCrash = false) {
    killed = false;
    arena.reset(new tx::TxArena());
    tx::Status st = arena->init(arenaBytes, platform.allocLarge, platform.freeLarge);
    if (!st.ok()) return st;
    log.reset(new tx::TxLog(files, chatKey));   // the engine opens and fills it at boot
    hooks.reset(new plain::PlainHooks());
    install();
    engine.reset(new plain::PlainEngine(deps()));
    return guarded([&] { return engine->boot(plain::BootCtx{epoch, afterCrash}); });
  }

  // One owner message, as intake hands it to the engine.
  tx::Status owner(const std::string& text, plain::TurnOutcome& out, std::vector<tx::Part> parts = {}) {
    if (!engine) {
      tx::Status st = boot();
      if (!st.ok()) return st;
    }
    lastInput = plain::TurnInput{};
    lastInput.ctx = turnCtx(++turnNo, plain::Trigger::Owner);
    lastInput.src = {9000u + turnNo};
    lastInput.text = intake(lastInput.src.front(), text);
    lastInput.parts = std::move(parts);
    return deliverInput(lastInput, out);
  }

  // One owner message with the Telegram update ids it came in (RB-TG1: the seen index keys on
  // them), as the Telegram intake hands it to the engine.
  tx::Status ownerFrom(std::vector<uint64_t> src, const std::string& text, plain::TurnOutcome& out,
                       std::vector<tx::Part> parts = {}) {
    if (!engine) {
      tx::Status st = boot();
      if (!st.ok()) return st;
    }
    lastInput = plain::TurnInput{};
    lastInput.ctx = turnCtx(++turnNo, plain::Trigger::Owner);
    lastInput.text = intake(src.empty() ? 0 : src.front(), text);
    lastInput.parts = std::move(parts);
    lastInput.src = std::move(src);
    return deliverInput(lastInput, out);
  }

  // A power cycle and the boot after it, with nothing redelivered by the battery itself (the
  // caller's intake decides what Telegram serves again).
  tx::Status powerCycle() {
    engine.reset();
    hooks.reset();
    log.reset();
    arena.reset();
    files.clearFaults();
    killAtSend = killBeforeSend = killAtDispatch = -1;
    return boot(true);
  }

  // A turn no person started (a routine fire, an agent report): its text is the turn's device
  // note. Nothing is redelivered after a cut (no Telegram update carries it).
  tx::Status unattended(plain::Trigger trigger, const std::string& text, plain::TurnOutcome& out) {
    if (!engine) {
      tx::Status st = boot();
      if (!st.ok()) return st;
    }
    lastInput = plain::TurnInput{};
    lastInput.ctx = turnCtx(++turnNo, trigger);
    lastInput.text = text;
    return deliverInput(lastInput, out);
  }

  // A power cut, then the boot that follows it: the resume seam runs there. Telegram then
  // redelivers the update the cut left unconfirmed (the offset lives in RAM); when its u item
  // was stored the seen index drops it, so the battery redelivers only an unstored message.
  tx::Status restartAfterCut() {
    tornCuts += files.tornKill && killed;
    engine.reset();
    hooks.reset();
    log.reset();
    arena.reset();
    files.clearFaults();
    killAtSend = killBeforeSend = killAtDispatch = -1;
    const size_t before = model.requests().size();
    tx::Status st = boot(true);
    for (size_t i = before; i < model.requests().size(); ++i) requestTurn.push_back(turnNo);
    if (!st.ok() || lastInput.src.empty()) return st;
    bool seen = false;
    if (!(st = ownerStored(lastInput.src.front(), seen)).ok() || seen) return st;
    plain::TurnOutcome out;
    return deliverInput(lastInput, out);
  }

  tx::Status idle(plain::IdleOutcome& out, uint32_t quietForS = 600) {
    if (!engine) return tx::Status::fail(tx::Err::InvalidArgument, "txb::Battery::idle before boot");
    plain::IdleCtx ctx;
    ctx.now = epoch;
    ctx.nowMs = ms;
    ctx.quietForS = quietForS;
    ctx.clockSet = true;
    const size_t before = model.requests().size();
    tx::Status st = guarded([&] { return engine->idle(ctx, out); });
    for (size_t i = before; i < model.requests().size(); ++i) requestTurn.push_back(0);
    return st;
  }

  // Every stored item of the chat, decoded from the segments in q order (torn tails left out).
  tx::Status stored(std::vector<tx::Item>& out) const {
    out.clear();
    std::string dir;
    tx::Status st = tx::txDir(chatKey, dir);
    if (!st.ok()) return st;
    size_t bad = 0;
    for (const std::string& seg : const_cast<MemFiles&>(files).segments(dir))
      if (!(st = decodeAll(files.files.at(seg), out, bad)).ok()) return st;
    if (bad > tornCuts)   // only a torn cut may leave a line that does not decode
      return tx::Status::fail(tx::Err::Torn, "txb::Battery::stored: a stored line does not decode");
    std::sort(out.begin(), out.end(), [](const tx::Item& a, const tx::Item& b) { return a.q < b.q; });
    return tx::Status::success();
  }

  // The stored items the last request was rendered from: the last turn's final text and
  // what follows it are stored after that request.
  tx::Status storedBeforeLastFinal(std::vector<tx::Item>& out) const {
    tx::Status st = stored(out);
    for (size_t i = 0; st.ok() && i < out.size(); ++i)
      if (out[i].turn == turnNo && out[i].kind == tx::ItemKind::Assistant && out[i].fin) {
        out.resize(i);
        break;
      }
    return st;
  }

  // What the coverage and overhead properties read for `items`: the newest stored pointer,
  // the open turn, bulk payloads read back through a TxLog over the same storage.
  CoverInput coverOf(const std::vector<tx::Item>& items) {
    CoverInput in;
    in.items = &items;
    in.openTurn = turnNo;
    in.bound = plain::boundTools(&reg);   // E2-FIX5: as the engine reads it
    for (const tx::Item& it : items)
      if (it.kind == tx::ItemKind::PointerMove) in.pointer = std::max(in.pointer, it.stub);
    in.readBulk = [this](const tx::Item& r, std::string& o) {
      tx::TxLog reader(files, chatKey);
      return reader.readBulk(r.handle, 0, r.bytes, o).ok();
    };
    return in;
  }

  // The e items' events, by turn.
  EventsByTurn events(const std::vector<tx::Item>& items) const {
    EventsByTurn ev;
    for (const tx::Item& it : items)
      if (it.kind == tx::ItemKind::TurnEnd) ev[it.turn] = it.end.events;
    return ev;
  }

  std::vector<SentRequest> sentRequests() const {
    std::vector<SentRequest> out;
    for (size_t i = 0; i < model.requests().size(); ++i)
      out.push_back(SentRequest{&model.requests()[i].view, i < requestTurn.size() ? requestTurn[i] : 0});
    return out;
  }

 private:
  plain::TurnCtx turnCtx(uint32_t turn, plain::Trigger trigger) const {
    plain::TurnCtx c;
    c.chatKey = chatKey;
    c.chatId = chatKey;
    c.turn = turn;
    c.trigger = trigger;
    c.dialect = dialect;
    c.host = hostOverride.empty() ? hostFor(dialect) : hostOverride;
    c.model = !modelOverride.empty()                              ? modelOverride
              : dialect == plain::Dialect::AnthropicMessages ? "claude-sonnet-5-5"
                                                             : "battery-model";
    if (noHead) c.model.clear();
    if (noHost) c.host.clear();
    c.profile = profile;
    c.startedAt = epoch;
    c.startedMs = ms;
    c.who = &who;
    return c;
  }

  // The world's tools: the always-on ones by their declarations, the battery's own with the
  // class the properties need (big_read is an internal pure read, device_control an effect).
  void registerTools() {
    namespace orch = nimbus::orch;
    auto handler = [](ArduinoJson::JsonObjectConst, const orch::Principal&) { return orch::ToolResult::ok(""); };
    for (const char* n : {"read_file", "write_file", "append_file", "web_search"}) {
      const auto* decl = plain::toolDecl(n);
      if (decl) reg.add(decl->name, decl->description, handler, plain::declSchema(*decl, false));
    }
    // The property suites' record tools, classed here so the class table's rows can change
    // under them (ruling 41) without changing what the properties test.
    reg.add("ledger_day", "", handler);
    reg.setClass("ledger_day", orch::ToolClass{"", orch::Taint::Internal, orch::EffectClass::Pure});
    reg.add("ledger_write", "", handler);
    reg.setClass("ledger_write", orch::ToolClass{"", orch::Taint::Internal, orch::EffectClass::Effect});
    reg.add("fetch_url", "", handler);
    reg.add("big_read", "", handler);
    reg.setClass("big_read", orch::ToolClass{"", orch::Taint::Internal, orch::EffectClass::Pure});
    reg.add("device_control", "", handler);
    reg.add("clock_now", "", handler);
    reg.setClass("clock_now", orch::ToolClass{"", orch::Taint::Internal, orch::EffectClass::Read});
  }

  tx::Status setup(const plain::TurnCtx& t, plain::TurnSetup& out) {
    out.facts.reg = &reg;
    out.facts.admin = true;
    out.facts.timeZone = "Europe/London";
    out.dayStartMin = dayStartMin;
    out.head.host = t.host;
    out.head.routed = routed;
    out.cost = cost;
    out.storedRounds = storedRounds;
    out.bodyCapLimit = bodyCapLimit;
    if (t.trigger == plain::Trigger::Routine) out.ceiling = routineCeiling;
    out.head.key = [] { return std::string("sk-fake-battery"); };
    out.wire = batteryWire(dialect);
    if (!photoStore.empty())
      out.photo = [this](const std::string& ref, plain::PhotoBytes& o) {
        o = plain::PhotoBytes{};
        auto it = photoStore.find(ref);
        if (it == photoStore.end()) return tx::Status::fail(tx::Err::NotFound, "txb photo");
        o.bytes = reinterpret_cast<const uint8_t*>(it->second.data());
        o.len = it->second.size();
        o.mime = "image/jpeg";
        return tx::Status::success();
      };
    return tx::Status::success();
  }
  static const char* hostFor(plain::Dialect d) {
    switch (d) {
      case plain::Dialect::AnthropicMessages: return "api.anthropic.com";
      case plain::Dialect::OpenAIResponses: return "api.openai.com";
      case plain::Dialect::ChatCompletions: return "api.z.ai";
      case plain::Dialect::Flat: return "custom.example.test";
    }
    return "api.anthropic.com";
  }

  plain::EngineDeps deps() {
    plain::EngineDeps d;
    d.platform = &platform;
    d.transport = &model;
    d.channel = &channel;
    d.hooks = hooks.get();
    d.log = log.get();
    d.arena = arena.get();
    d.dispatch = [this](const plain::ToolCall& c, const plain::TurnCtx& t) { return dispatch(c, t); };
    d.recentFiles = recentFiles;
    d.files = &files;
    d.chatKey = chatKey;
    d.setup = [this](const plain::TurnCtx& t, plain::TurnSetup& out) { return setup(t, out); };
    // Compaction is installed (install()); with no head named it cannot be (RB-T4's refusal), and the
    // battery names that case rather than let boot refuse it (ruling 91 blockers/F6, RB-H81).
    if (noHead) d.noCompaction = "battery: a turn setup that names no head";
    if (!compaction) d.noCompaction = "battery: a case that measures the engine with no compaction (RB-C3)";
    d.progress = [this](const plain::ProgressEvent& e) {
      if (progressFn) progressFn(e);
    };
    d.deliver = [this](const plain::DeliverInput& in, plain::DeliverOutcome& out) {
      return deliverFn ? deliverFn(in, out) : plain::deliverTurn(plain::DeliverDeps{&channel, hooks.get(), log.get()}, in, out);
    };
    return d;
  }

  // The modules the properties need; one whose lane has not landed answers NotImplemented
  // and is simply not there yet. The truth probe is not installed: it would spend scripted
  // steps (test_truth covers it).
  void install() {
    (void)plain::installSuppression(*hooks);
    (void)plain::installResume(*hooks, resumeDeps());
    if (compaction && compactFirst) (void)plain::installCompaction(*hooks, compactDeps());
    if (compaction && !compactFirst) installIdleOnlyCompaction();
    (void)plain::installRails(*hooks, railsDeps);
    if (moreHooks) moreHooks(*hooks);
  }

  // The resume seam (RB-X1): the engine's head, runTurn and delivery, read when it runs (the
  // engine exists by then: boot runs the seam), and the chat's context as an owner turn's.
  plain::ResumeDeps resumeDeps() {
    plain::ResumeDeps d;
    d.log = log.get();
    d.arena = arena.get();
    d.head = [this] { return engine ? engine->head() : nullptr; };
    d.runTurn = [this](const plain::TurnInput& in, plain::TurnOutcome& out) {
      return engine ? engine->runTurn(in, out) : tx::Status::fail(tx::Err::InvalidArgument, "txb::resume");
    };
    d.deliver = deps().deliver;
    d.context = [this](plain::TurnCtx& out) {
      out = turnCtx(turnNo, plain::Trigger::Resume);
      return tx::Status::success();
    };
    d.now = [this] { return epoch; };
    d.done = [this](const plain::ResumeReport& r) { resumeReports.push_back(r); };
    return d;
  }

  // The world's tools as the registry would run them: a `more` call runs the tool it names
  // with its arguments (the registry's router, without its listings), delete_file removes a
  // world file, and throw_tool throws (a handler's own exception).
  plain::ToolOutput dispatch(const plain::ToolCall& c, const plain::TurnCtx& t) {
    if ((long)dispatches++ == killAtDispatch) throw Killed{"dispatch " + std::to_string(dispatches - 1) + " " + c.name};
    if (registryDispatch) {
      JsonDocument a;
      (void)deserializeJson(a, c.argsJson);
      const nimbus::orch::ToolResult r = reg.dispatch(c.name, a.as<JsonObjectConst>(), t.who ? *t.who : who);
      plain::ToolOutput o;
      o.id = c.id;
      o.name = c.name;
      o.isError = !r.success;
      o.text = r.success ? r.output : r.error;
      return o;
    }
    if (c.name == "more") {
      JsonDocument a;
      if (deserializeJson(a, c.argsJson) || !a["name"].is<const char*>()) return world.dispatch(c, t);
      plain::ToolCall inner = c;
      inner.name = a["name"].as<const char*>();
      inner.argsJson.clear();
      if (a["args"].is<JsonObjectConst>()) serializeJson(a["args"], inner.argsJson);
      else inner.argsJson = "{}";
      plain::ToolOutput o = dispatch(inner, t);
      o.name = c.name;
      return o;
    }
    if (c.name == "throw_tool") throw std::out_of_range("a tool handler that throws");
    if (c.name == "delete_file") {
      JsonDocument a;
      deserializeJson(a, c.argsJson);
      world.dispatched.push_back(c.name);
      ++world.effectRuns[c.name + " " + c.argsJson];
      plain::ToolOutput o;
      o.id = c.id;
      o.name = c.name;
      o.isError = world.files.erase(a["path"] | "") == 0;
      o.text = std::string(o.isError ? "no such file: " : "deleted ") + (a["path"] | "");
      return o;
    }
    return world.dispatch(c, t);
  }

  // The compaction driver (RB-C2): the engine's head, digests and restart omission, read when it
  // runs; one rung, the dialect's wire on the head host; its control record and the chatsum
  // mirror in RAM (they outlive a power cut like storage does).
  plain::CompactDeps compactDeps() {
    plain::CompactDeps d{&platform, &summarizer, log.get(), arena.get()};
    d.files = &files;
    d.chatKey = chatKey;
    d.chatId = chatKey;
    d.headKey = headKey();   // the head the marks are read for (RB-T4)
    d.head = [this] { return engine ? engine->head() : nullptr; };
    d.digests = [this] { return engine ? engine->digests() : nullptr; };
    d.omittedThru = [this] { return engine ? engine->omittedThru() : uint64_t(0); };
    d.rungs = [this](std::vector<plain::SummarizerRung>& out) {
      plain::SummarizerRung r;
      r.wire = batteryWire(dialect);
      r.dialect = dialect;
      r.host = dialect == plain::Dialect::AnthropicMessages ? "anthropic" : "battery";
      r.model = dialect == plain::Dialect::AnthropicMessages ? "claude-haiku-5-5" : "battery-model";
      r.endpoint.host = hostFor(dialect);
      r.endpoint.key = [] { return std::string("sk-fake-battery"); };
      out.assign(1, r);
    };
    d.ctl.load = [this] { return compactCtl; };
    d.ctl.save = [this](const std::string& t) {
      compactCtl = t;
      return true;
    };
    d.chatsum.load = [this] { return chatsum; };
    d.chatsum.save = [this](const std::string& t) {
      chatsum = t;
      return true;
    };
    return d;
  }

  bool onSend(const std::string& chat, const std::string& text) {
    if ((long)sends++ == killBeforeSend) throw Killed{"before send " + std::to_string(sends - 1)};
    if (refuseSends) return false;
    sent.push_back(SentMessage{chat, text});
    if ((long)sent.size() - 1 == killAtSend) throw Killed{"inside send " + std::to_string(sent.size() - 1)};
    return true;
  }

  template <typename F>
  tx::Status guarded(F f) {
    try {
      return f();
    } catch (const Killed& k) {
      killed = true;
      killedAt = k.where;
      return tx::Status::fail(tx::Err::Io, "txb: power cut");
    }
  }

  static tx::Status decodeAll(const std::string& seg, std::vector<tx::Item>& out, size_t& bad) {
    size_t at = 0;
    while (at < seg.size()) {
      const size_t nl = seg.find('\n', at);
      if (nl == std::string::npos) break;   // a torn tail: no end, dropped
      tx::Item it;
      tx::Status st = tx::decodeLine(seg.data() + at, nl - at, it);
      if (st.notImplemented()) return st;
      if (st.ok()) out.push_back(it);
      else ++bad;
      at = nl + 1;
    }
    return tx::Status::success();
  }

  // The Telegram intake of an owner text (RB-TG1, nimbus/net/tg_inbox.h): over the inline cap it
  // is saved whole as inbox/<update id>.txt in the file store and the u text points at it.
  std::string intake(uint64_t updateId, const std::string& text) {
    const nimbus::net::InboxSave save = [this](const std::string& name, const std::string& whole) {
      if (inboxRefuses) return false;
      world.files["inbox/" + name] = whole;
      return true;
    };
    return nimbus::net::ownerTextIntake(text, updateId, tx::kInlineMessageCap, save).text;
  }

  tx::Status deliverInput(const plain::TurnInput& in, plain::TurnOutcome& out) {
    const size_t before = model.requests().size();
    tx::Status st = guarded([&] { return engine->runTurn(in, out); });
    for (size_t i = before; i < model.requests().size(); ++i) requestTurn.push_back(in.ctx.turn);
    return st;
  }

  tx::Status ownerStored(uint64_t src, bool& seen) const {
    std::vector<tx::Item> items;
    tx::Status st = stored(items);
    seen = false;
    for (const tx::Item& it : items)
      if (it.kind == tx::ItemKind::User)
        for (uint64_t s2 : it.src) seen |= s2 == src;
    return st;
  }
};

}  // namespace txb
