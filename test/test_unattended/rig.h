#pragma once
#include <ArduinoJson.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../support/scripted_provider/scripted_provider.h"
#include "../support/tx_battery/mem_files.h"
#include "../support/tx_battery/stub_wire.h"
#include "nimbus/harness/channel.h"
#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/unattended.h"
#include "nimbus/harness/platform.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"

// test_unattended/rig.h - one device for the routine tests (lane RB-U1a): one card (MemFiles)
// shared by the owner's chat and every routine's own transcript, one registry with the plain
// tools, the owner's engine booted with the unattended seam installed, a scripted model for the
// owner and another for the routines (so each side's requests are read apart), a recording
// channel, a virtual clock, and the files the tools act on.
namespace ut {

namespace plain = agent::plain;
namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;
namespace sp = harness_test::scripted;

// 2026-11-17 12:00:00 UTC, a Tuesday: outside quiet hours (22:00 to 09:00) at offset 0.
inline constexpr uint64_t kNoon = 1794916800ull;
inline constexpr uint64_t kDay = 86400ull;

struct Sent {
  std::string chat, text;
};

// The files the tools act on, and the tools, by their plain names.
struct Files {
  std::map<std::string, std::string> files;
  std::vector<std::string> ran;   // tool names, in order

  plain::ToolOutput dispatch(const plain::ToolCall& c) {
    if (c.name == "more") {   // the device's router: it runs the tool it names against the whole registry
      JsonDocument a;
      deserializeJson(a, c.argsJson);
      ran.push_back(c.name);
      plain::ToolCall inner = c;
      inner.name = a["name"] | "";
      inner.argsJson = "{}";
      if (a["args"].is<JsonObjectConst>()) {
        inner.argsJson.clear();
        serializeJson(a["args"], inner.argsJson);
      }
      return inner.name.empty() ? plain::ToolOutput{c.id, c.name, "a listing"} : dispatch(inner);
    }
    ran.push_back(c.name);
    JsonDocument a;
    deserializeJson(a, c.argsJson);
    const std::string path = a["path"] | "";
    plain::ToolOutput o;
    o.id = c.id;
    o.name = c.name;
    if (c.name == "read_file") {
      auto it = files.find(path);
      o.isError = it == files.end();
      o.text = o.isError ? "no such file" : it->second;
    } else if (c.name == "write_file") {
      files[path] = a["content"] | (a["text"] | "");
      o.text = "wrote " + path;
    } else if (c.name == "append_file") {
      files[path] += a["text"] | "";
      o.text = "appended " + path;
    } else if (c.name == "edit_file") {
      auto it = files.find(path);
      const std::string from = a["old"] | "", to = a["new"] | "";
      const size_t at = it == files.end() || from.empty() ? std::string::npos : it->second.find(from);
      o.isError = at == std::string::npos;
      if (!o.isError) it->second.replace(at, from.size(), to);
      o.text = o.isError ? "no match" : "edited " + path;
    } else if (c.name == "delete_file") {
      o.isError = files.erase(path) == 0;
      o.text = o.isError ? "no such file" : "deleted " + path;
    } else if (c.name == "web_search") {
      o.outside = true;
      o.text = "a search result about " + std::string(a["q"] | "it");
    } else {
      o.text = "done";
    }
    return o;
  }
};

class Rig {
 public:
  Rig() {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return epoch; };
    platform.delayMs = [this](uint32_t d) { ms += d; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    channel.send = [this](const std::string& chat, const std::string& text) {
      if (refuseSends || (refuseIf && refuseIf(text))) return false;
      sent.push_back(Sent{chat, text});
      return true;
    };
    channel.isAllowed = [](const std::string&) { return true; };
    channel.firstAllowedChat = [] { return std::string("1001"); };
    ownerModel.clock = &ms;
    routineModel.clock = &ms;
    registerTools();
  }

  uint32_t ms = 0;
  uint64_t epoch = kNoon;
  agent::Platform platform;
  agent::Channel channel;
  std::vector<Sent> sent;
  bool refuseSends = false;
  std::function<bool(const std::string&)> refuseIf;   // the channel refuses a message it matches
  txb::MemFiles card;
  Files world;
  orch::ToolRegistry reg;
  orch::Principal who = orch::principalForRole("1001", orch::Role::Admin);
  sp::ScriptedProvider ownerModel, routineModel;
  plain::HeadCost cost;   // the routines' head cost (the spend guard)
  std::string heldFile;   // the outbox on the card
  std::unique_ptr<plain::HeldStore> held;
  plain::QuietHours quiet;
  std::vector<plain::FireReport> fires;
  std::function<bool(uint64_t, plain::RoutineFire&)> due;

  std::unique_ptr<tx::TxArena> arena;
  std::unique_ptr<tx::TxLog> log;
  std::unique_ptr<plain::PlainHooks> hooks;
  std::unique_ptr<plain::PlainEngine> owner;
  uint32_t turnNo = 0;

  void advance(uint64_t s) {
    epoch += s;
    ms += (uint32_t)(s * 1000u);
  }

  // The owner's engine and the outbox, booted (again, after a restart) over the same card.
  tx::Status boot() {
    owner.reset();
    held.reset(new plain::HeldStore(plain::HeldFile{[this] { return heldFile; },
                                                     [this](const std::string& t) {
                                                       heldFile = t;
                                                       return true;
                                                     }}));
    arena.reset(new tx::TxArena());
    tx::Status st = arena->init(tx::kActiveChatArenaBytes, platform.allocLarge, platform.freeLarge);
    if (!st.ok()) return st;
    log.reset(new tx::TxLog(card, "1001"));
    hooks.reset(new plain::PlainHooks());
    st = plain::installUnattended(*hooks, unattendedDeps());
    if (!st.ok()) return st;
    owner.reset(new plain::PlainEngine(ownerDeps()));
    return owner->boot(plain::BootCtx{epoch, false});
  }

  // One owner message on his chat.
  tx::Status ownerTurn(const std::string& text, plain::TurnOutcome& out) {
    plain::TurnInput in;
    in.ctx = headCtx("1001", plain::Trigger::Owner);
    in.ctx.turn = ++turnNo;
    in.text = text;
    in.src = {7000u + turnNo};
    return owner->runTurn(in, out);
  }

  tx::Status idle(plain::IdleOutcome& out) {
    plain::IdleCtx c;
    c.now = epoch;
    c.nowMs = ms;
    c.clockSet = true;
    c.quietForS = 600;
    return owner->idle(c, out);
  }

  tx::Status fire(const plain::RoutineFire& f, plain::FireReport& out) { return plain::runRoutine(routineDeps(), f, out); }

  plain::OwnerSink sink() {
    plain::OwnerSink s;
    s.chatKey = "1001";
    s.chatId = "1001";
    s.deliver = [this](const plain::DeliverInput& in, plain::DeliverOutcome& out) {
      return plain::deliverTurn(plain::DeliverDeps{&channel, nullptr, nullptr}, in, out);
    };
    s.append = [this](const std::string& note, uint64_t at) {
      return owner ? plain::appendOwnerNote(*owner, note, at) : tx::Status::fail(tx::Err::InvalidArgument, "rig");
    };
    return s;
  }

  plain::RoutineDeps routineDeps() {
    plain::RoutineDeps d;
    d.platform = &platform;
    d.transport = &routineModel;
    d.files = &card;
    d.reg = &reg;
    d.dispatch = [this](const plain::ToolCall& c, const plain::TurnCtx&) { return world.dispatch(c); };
    d.setup = [this](const plain::TurnCtx& t, plain::TurnSetup& out) { return setup(t, out, true); };
    d.head = headCtx("", plain::Trigger::Routine);
    d.held = held.get();
    d.owner = sink();
    d.quiet = quiet;
    return d;
  }

  // The owner chat's stored items, from its arena (every item stays resident in these tests).
  std::vector<tx::Item> ownerItems() const {
    std::vector<tx::Item> out;
    for (size_t i = 0; i < arena->count(); ++i) {
      tx::Item it;
      if (arena->get(i, it).ok()) out.push_back(it);
    }
    return out;
  }
  // The routine notes in the owner's chat.
  std::vector<std::string> ownerNotes() const {
    std::vector<std::string> out;
    for (const tx::Item& it : ownerItems())
      if (it.kind == tx::ItemKind::DeviceNote && it.note == tx::NoteClass::Routine) out.push_back(it.x);
    return out;
  }

  plain::TurnCtx headCtx(const std::string& chat, plain::Trigger t) const {
    plain::TurnCtx c;
    c.chatKey = chat;
    c.chatId = chat;
    c.trigger = t;
    c.host = "api.anthropic.com";
    c.model = "claude-sonnet-5-5";
    c.dialect = plain::Dialect::AnthropicMessages;
    c.who = &who;
    return c;
  }

 private:
  void registerTools() {
    auto handler = [](ArduinoJson::JsonObjectConst, const orch::Principal&) { return orch::ToolResult::ok(""); };
    for (const char* n : {"read_file", "write_file", "append_file", "edit_file", "list_files", "note", "search",
                          "web_search", "view", "send", "schedule", "calc"}) {
      const auto* d = plain::toolDecl(n);
      if (d) reg.add(d->name, d->description, handler, plain::declSchema(*d, false));
    }
    reg.add("delete_file", "Delete a file by path.", handler);
    orch::registerMore(reg);
  }

  tx::Status setup(const plain::TurnCtx& t, plain::TurnSetup& out, bool routine) {
    out.facts.reg = &reg;
    out.facts.admin = true;
    out.facts.timeZone = "Europe/London";
    out.facts.pinnedNotes = "home: the person's pinned note";
    out.head.host = t.host;
    out.head.key = [] { return std::string("sk-fake-unattended"); };
    out.wire = txb::batteryWire(t.dialect);
    if (routine) out.cost = cost;
    return tx::Status::success();
  }

  plain::UnattendedDeps unattendedDeps() {
    plain::UnattendedDeps d;
    d.held = held.get();
    d.owner = sink();
    d.quiet = quiet;
    d.due = [this](uint64_t now, plain::RoutineFire& f) { return due && due(now, f); };
    d.fireDeps = [this] { return routineDeps(); };
    d.fired = [this](const plain::FireReport& r) { fires.push_back(r); };
    return d;
  }

  plain::EngineDeps ownerDeps() {
    plain::EngineDeps d;
    d.platform = &platform;
    d.transport = &ownerModel;
    d.channel = &channel;
    d.hooks = hooks.get();
    d.log = log.get();
    d.arena = arena.get();
    d.files = &card;
    d.chatKey = "1001";
    d.dispatch = [this](const plain::ToolCall& c, const plain::TurnCtx&) { return world.dispatch(c); };
    d.setup = [this](const plain::TurnCtx& t, plain::TurnSetup& out) { return setup(t, out, false); };
    d.noCompaction = "test-unattended-rig";
    return d;
  }
};

// Deterministic generators.
inline std::string hexOf(std::mt19937& r, size_t n) {
  static const char k[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) s += k[r() % 16];
  return s;
}
inline std::string wordsOf(std::mt19937& r, size_t n) {
  static const char* const k[] = {"check", "the", "weather", "summary", "list", "today", "plan", "notes", "brief", "train"};
  std::string s;
  for (size_t i = 0; i < n; ++i) s += (i ? " " : "") + std::string(k[r() % 10]);
  return s;
}
// Text with ASCII and multibyte UTF-8, no markup and no control bytes.
inline std::string textOf(std::mt19937& r, size_t bytes) {
  static const char* const k[] = {"a", "b", "z", " ", "1", ".", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x8c\xa7", "Q"};
  std::string s;
  while (s.size() < bytes) s += k[r() % 10];
  return s;
}

}  // namespace ut
