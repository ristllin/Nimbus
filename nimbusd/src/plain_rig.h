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
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/platform.h"
#include "nimbus/orch/rbac.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"

// plain_rig.h - engine 2 (the plain loop, plain/engine.h) on a host: nimbusd and the
// harness lab (lane RB-I1a). One PlainRig is one chat: it owns the chat's arena, log,
// hooks, tool registry, recording channel and engine, and wires the loop core's seams to
// what the host hands it (PlainRigBindings). It adds no behavior to the loop: a turn is
// PlainEngine::runTurn, and what the owner sees is what the engine sent.
//
// Only lib headers are included, so the lab includes this file too (one rig, both hosts).
// Nothing here enters a device image: device bindings are src/agent/*_device.cpp.
//
//   nimbusd::PlainRig rig(cfg, PlainRigBindings{&platform, &transport, &files, wire});
//   rig.registry().add(...);                   // a tool binding (hosted_<name>.h), before boot
//   (void)plain::installRails(rig.hooks(), ...);   // a seam module, before boot
//   rig.boot();
//   nimbusd::PlainTurn t = rig.say("hello");   // t.stated is never empty
//
// The rig has no engine choice: which engine answers a turn is RB-I1's (the engine.json
// sidecar and head qualification); the old engine's paths do not include this file.
namespace nimbusd {

namespace plain = agent::plain;

// What the host binds. platform, transport and files are required (boot says
// InvalidArgument without them); they must outlive the rig.
struct PlainRigBindings {
  const agent::Platform* platform = nullptr;   // nowMs, delayMs, nowEpoch, allocLarge, freeLarge
  agent::HttpTransport* transport = nullptr;   // the head (a real transport or a scripted model)
  nimbus::orch::tx::TxFiles* files = nullptr;  // where the transcript lives (/mem/tx/<chatKey>/)
  const plain::WireCodec* wire = nullptr;      // null: plain::wireFor(dialect)
  // The host's own channel, after the rig has recorded the message; null: record only.
  std::function<bool(const std::string& chatId, const std::string& text)> send;
  // A tool call the registry does not answer the way the host wants (a binding that marks
  // outside content); null: every call goes to registry().dispatch with the chat's Principal.
  plain::ToolDispatchFn dispatch;
};

struct PlainRigConfig {
  std::string chatKey = "lab";                 // the transcript key (/mem/tx/<chatKey>)
  std::string chatId;                          // the routing chat; "" = chatKey
  plain::Dialect dialect = plain::Dialect::AnthropicMessages;
  std::string host = "api.anthropic.com";      // the head's host (HeadEndpoint::host)
  std::string model = "claude-sonnet-5-5";
  std::function<std::string()> key;            // read at render time, never stored
  std::string effort;                          // "" = the provider default
  bool routed = false;                         // through the Cumulo router
  nimbus::orch::Role role = nimbus::orch::Role::Admin;   // the person talking (owner by default)
  std::string timeZone = "Europe/London";
  int16_t tzMinutes = 0;                       // local offset from UTC
  bool qualified = false;                      // the head is qualified on engine 2 (tx::budgetFor)
  int storedRounds = 0;                        // the stored orchLoopRnds (guardLimitsFor)
};

// How a turn ended, from the host's side. The engine decides every owner-facing byte; the
// rig only names what happened, so a host never reads a turn as silent by accident.
enum class PlainEnding : uint8_t {
  Reply = 0,    // the model's text went out (a status line may follow it)
  Status = 1,   // no model text went out; the engine's one status line did
  Silent = 2,   // the engine returned ok and sent nothing (a loop-core gap, see `stated`)
  Failed = 3,   // nothing went out: runTurn failed, the host's channel refused, or the rig was busy
};
inline const char* plainEndingSlug(PlainEnding e) {
  switch (e) {
    case PlainEnding::Reply: return "reply";
    case PlainEnding::Status: return "status";
    case PlainEnding::Silent: return "silent";
    case PlainEnding::Failed: return "failed";
  }
  return "failed";
}

struct PlainTurn {
  plain::Status status;               // runTurn's (or boot's) status
  plain::TurnOutcome out;             // the engine's outcome, unchanged
  std::vector<std::string> sent;      // every message the host's channel took in this turn, in order
  std::vector<std::string> unsent;    // every message the host's channel refused
  PlainEnding ending = PlainEnding::Failed;
  // Never empty: the reply when one went out, else the status line, else the rig's one line
  // naming why nothing went out (for the host's log; the rig never sends it).
  std::string stated;
};

class PlainRig {
 public:
  PlainRig(PlainRigConfig cfg, PlainRigBindings b) : cfg_(std::move(cfg)), b_(std::move(b)) {
    if (cfg_.chatId.empty()) cfg_.chatId = cfg_.chatKey;
    who_ = nimbus::orch::principalForRole(cfg_.chatId, cfg_.role);
    channel_.send = [this](const std::string& chat, const std::string& text) {
      const bool ok = b_.send ? b_.send(chat, text) : true;
      if (ok) sent_.push_back(text);
      else unsent_.push_back(text);
      return ok;
    };
    channel_.isAllowed = [this](const std::string& chat) { return chat == cfg_.chatId; };
    channel_.firstAllowedChat = [this] { return cfg_.chatId; };
  }
  PlainRig(const PlainRig&) = delete;
  PlainRig& operator=(const PlainRig&) = delete;

  // A tool binding registers here and a seam module installs on hooks(), both before boot.
  nimbus::orch::ToolRegistry& registry() { return reg_; }
  plain::PlainHooks& hooks() { return hooks_; }
  const nimbus::orch::Principal& who() const { return who_; }
  const PlainRigConfig& config() const { return cfg_; }
  plain::PlainEngine* engine() { return engine_.get(); }
  nimbus::orch::tx::TxLog* log() { return log_.get(); }
  nimbus::orch::tx::TxArena* arena() { return arena_.get(); }

  // Once: the arena, the log over the host's files, the engine and its boot (open the log,
  // fill the arena, the resume seam). InvalidArgument without a required binding.
  // A refused allocation is a status (ruling 30), never an abort.
  plain::Status boot(bool afterCrash = false) {
    namespace tx = nimbus::orch::tx;
    if (engine_) return plain::Status::fail(tx::Err::Busy, "nimbusd::PlainRig::boot.twice");
    const agent::Platform* p = b_.platform;
    if (!p || !p->nowMs || !p->delayMs || !p->nowEpoch || !p->allocLarge || !p->freeLarge || !b_.transport ||
        !b_.files)
      return plain::Status::fail(tx::Err::InvalidArgument, "nimbusd::PlainRig::boot");
    plain::Status st;
    try {
      std::unique_ptr<tx::TxArena> arena(new tx::TxArena());
      st = arena->init(tx::kActiveChatArenaBytes, p->allocLarge, p->freeLarge);
      if (!st.ok()) return st;
      arena_ = std::move(arena);
      log_.reset(new tx::TxLog(*b_.files, cfg_.chatKey));
      engine_.reset(new plain::PlainEngine(deps()));
      st = engine_->boot(plain::BootCtx{p->nowEpoch(), afterCrash});
    } catch (const std::bad_alloc&) {
      st = plain::Status::fail(tx::Err::TooLarge, "nimbusd::PlainRig::boot.memory");
    }
    if (!st.ok()) engine_.reset();   // a failed boot leaves the rig unbooted, never half-booted
    return st;
  }

  // One owner message through the loop: from the u item to the e item.
  // One turn at a time: a say from inside a turn (a host send that answers back) is refused.
  PlainTurn say(const std::string& text) {
    PlainTurn t;
    if (busy_) {
      t.status = plain::Status::fail(nimbus::orch::tx::Err::Busy, "nimbusd::PlainRig::say.busy");
      t.stated = "engine 2: a turn is already running on this chat";
      return t;
    }
    if (!engine_) {
      t.status = plain::Status::fail(nimbus::orch::tx::Err::InvalidArgument, "nimbusd::PlainRig::say.before-boot");
      t.stated = "engine 2: the rig is not booted";
      return t;
    }
    busy_ = true;
    struct Idle {
      bool& busy;
      ~Idle() { busy = false; }
    } idle{busy_};
    sent_.clear();
    unsent_.clear();
    plain::TurnInput in;
    in.ctx = turnCtx();
    in.text = text;
    in.ch = "rig";
    in.tzMinutes = cfg_.tzMinutes;
    t.status = engine_->runTurn(in, t.out);
    t.sent.swap(sent_);
    t.unsent.swap(unsent_);
    state(t);
    return t;
  }

 private:
  plain::TurnCtx turnCtx() const {
    plain::TurnCtx c;
    c.chatKey = cfg_.chatKey;
    c.chatId = cfg_.chatId;
    c.turn = 0;   // the engine numbers the turn
    c.trigger = plain::Trigger::Owner;
    c.host = cfg_.host;
    c.model = cfg_.model;
    c.dialect = cfg_.dialect;
    c.who = &who_;
    return c;
  }

  plain::Status setup(const plain::TurnCtx&, plain::TurnSetup& out) {
    out.facts.reg = &reg_;
    out.facts.admin = who_.owner;
    out.facts.timeZone = cfg_.timeZone;
    out.head.host = cfg_.host;
    out.head.effort = cfg_.effort;
    out.head.routed = cfg_.routed;
    out.head.key = cfg_.key ? cfg_.key : [] { return std::string(); };
    out.wire = b_.wire;
    out.storedRounds = cfg_.storedRounds;
    out.qualified = cfg_.qualified;
    return plain::Status::success();
  }

  // The registry's answer as the model sees it. A call's arguments that are not a JSON
  // object reach the handler as an empty object (the engine never dispatches a cut call).
  plain::ToolOutput dispatchCall(const plain::ToolCall& c, const plain::TurnCtx& turn) {
    if (b_.dispatch) return b_.dispatch(c, turn);
    JsonDocument args;
    (void)deserializeJson(args, c.argsJson, DeserializationOption::NestingLimit(64));   // not an object: empty
    const nimbus::orch::ToolResult r = reg_.dispatch(c.name, args.as<JsonObjectConst>(), turn.who ? *turn.who : who_);
    plain::ToolOutput o;
    o.id = c.id;
    o.name = c.name;
    o.isError = !r.success;
    o.text = r.success ? r.output : r.error;
    return o;
  }

  plain::EngineDeps deps() {
    plain::EngineDeps d;
    d.platform = b_.platform;
    d.transport = b_.transport;
    d.channel = &channel_;
    d.hooks = &hooks_;
    d.log = log_.get();
    d.arena = arena_.get();
    d.files = b_.files;
    d.chatKey = cfg_.chatKey;
    d.dispatch = [this](const plain::ToolCall& c, const plain::TurnCtx& t) { return dispatchCall(c, t); };
    d.setup = [this](const plain::TurnCtx& t, plain::TurnSetup& out) { return setup(t, out); };
    return d;
  }

  void state(PlainTurn& t) const {
    const bool replied = t.out.delivered && !t.out.text.empty();
    if (replied) {
      t.ending = PlainEnding::Reply;
      t.stated = t.out.text;
    } else if (!t.sent.empty()) {
      t.ending = PlainEnding::Status;
      t.stated = t.sent.back();
    } else if (!t.unsent.empty()) {
      t.ending = PlainEnding::Failed;
      t.stated = "engine 2: the host's channel refused " + std::to_string(t.unsent.size()) + " message(s)";
    } else if (t.status.ok()) {
      t.ending = PlainEnding::Silent;
      t.stated = "engine 2: the turn sent nothing (cause " + t.out.end.cause + ")";
    } else {
      t.ending = PlainEnding::Failed;
      t.stated = std::string("engine 2: the turn failed (") + nimbus::orch::tx::errSlug(t.status.err) + " at " +
                 t.status.where + ")";
    }
  }

  PlainRigConfig cfg_;
  PlainRigBindings b_;
  nimbus::orch::Principal who_;
  nimbus::orch::ToolRegistry reg_;
  plain::PlainHooks hooks_;
  agent::Channel channel_;
  std::vector<std::string> sent_, unsent_;
  bool busy_ = false;
  std::unique_ptr<nimbus::orch::tx::TxArena> arena_;
  std::unique_ptr<nimbus::orch::tx::TxLog> log_;
  std::unique_ptr<plain::PlainEngine> engine_;
};

}  // namespace nimbusd
