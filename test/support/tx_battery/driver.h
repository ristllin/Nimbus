#pragma once
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../scripted_provider/scripted_provider.h"
#include "items.h"
#include "mem_files.h"
#include "nimbus/harness/channel.h"
#include "nimbus/harness/plain/compact.h"
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/rails.h"
#include "nimbus/harness/plain/resume.h"
#include "nimbus/harness/platform.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"
#include "props.h"
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
  }

  plain::Dialect dialect;
  std::string chatKey = "1001";
  sp::ScriptedProvider model;
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

  // Process state: gone at a power cut, rebuilt by boot().
  agent::Platform platform;
  agent::Channel channel;
  std::unique_ptr<tx::TxArena> arena;
  std::unique_ptr<tx::TxLog> log;
  std::unique_ptr<plain::PlainHooks> hooks;
  std::unique_ptr<plain::PlainEngine> engine;
  tx::HeadState head;
  uint32_t turnNo = 0;
  size_t sends = 0;

  void advance(uint32_t seconds) {
    epoch += seconds;
    ms += seconds * 1000u;
  }

  // Start (or restart) the process over the same storage, world and model.
  tx::Status boot(bool afterCrash = false) {
    killed = false;
    arena.reset(new tx::TxArena());
    tx::Status st = arena->init(tx::kActiveChatArenaBytes, platform.allocLarge, platform.freeLarge);
    if (!st.ok()) return st;
    log.reset(new tx::TxLog(files, chatKey));
    if (!(st = guarded([&] { return log->open(head); })).ok()) return st;
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
    plain::TurnInput in;
    in.ctx = turnCtx(++turnNo, plain::Trigger::Owner);
    in.text = text;
    in.parts = std::move(parts);
    in.src = {9000u + turnNo};
    const size_t before = model.requests().size();
    tx::Status st = guarded([&] { return engine->runTurn(in, out); });
    for (size_t i = before; i < model.requests().size(); ++i) requestTurn.push_back(in.ctx.turn);
    return st;
  }

  // A power cut, then the boot that follows it: the resume seam runs there.
  tx::Status restartAfterCut() {
    engine.reset();
    hooks.reset();
    log.reset();
    arena.reset();
    files.clearFaults();
    killAtSend = killBeforeSend = -1;
    const size_t before = model.requests().size();
    tx::Status st = boot(true);
    for (size_t i = before; i < model.requests().size(); ++i) requestTurn.push_back(turnNo);
    return st;
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
    for (const std::string& seg : const_cast<MemFiles&>(files).segments(dir))
      if (!(st = decodeAll(files.files.at(seg), out)).ok()) return st;
    std::sort(out.begin(), out.end(), [](const tx::Item& a, const tx::Item& b) { return a.q < b.q; });
    return tx::Status::success();
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
    c.host = hostFor(dialect);
    c.model = dialect == plain::Dialect::AnthropicMessages ? "claude-sonnet-5-5" : "battery-model";
    c.profile = tx::Profile::Standard;
    c.startedAt = epoch;
    c.startedMs = ms;
    return c;
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
    d.dispatch = [this](const plain::ToolCall& c, const plain::TurnCtx& t) { return world.dispatch(c, t); };
    return d;
  }

  // The modules the properties need; one whose lane has not landed answers NotImplemented
  // and is simply not there yet. The truth probe is not installed: it would spend scripted
  // steps (test_truth covers it).
  void install() {
    (void)plain::installSuppression(*hooks);
    (void)plain::installResume(*hooks, plain::ResumeDeps{log.get()});
    (void)plain::installCompaction(*hooks, plain::CompactDeps{&platform, &model, log.get(), arena.get()});
    (void)plain::installRails(*hooks, plain::RailsDeps{});
  }

  bool onSend(const std::string& chat, const std::string& text) {
    if ((long)sends++ == killBeforeSend) throw Killed{"before send " + std::to_string(sends - 1)};
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

  static tx::Status decodeAll(const std::string& seg, std::vector<tx::Item>& out) {
    size_t at = 0;
    while (at < seg.size()) {
      const size_t nl = seg.find('\n', at);
      if (nl == std::string::npos) break;   // a torn tail: no end, dropped
      tx::Item it;
      tx::Status st = tx::decodeLine(seg.data() + at, nl - at, it);
      if (st.notImplemented()) return st;
      if (st.ok()) out.push_back(it);
      at = nl + 1;
    }
    return tx::Status::success();
  }
};

}  // namespace txb
