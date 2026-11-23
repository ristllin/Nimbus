#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/tx/item.h"

// tx_battery/items.h - a stored transcript built by hand (lane RB-B0b): the fixtures the
// wire-level properties render, and the good and bad inputs the checkers' counter-tests
// read. Items are what the engine would store (PRD section 2); stubs are written in the
// PRD's shape here, once, as the engine computes them at capture.
namespace txb {

namespace tx = nimbus::orch::tx;

// 2026-11-17 13:02:00 UTC, a Tuesday.
inline constexpr uint64_t kT0 = 1794920520ULL;

struct Convo {
  std::vector<tx::Item> items;
  std::map<std::string, std::string> bulk;   // handle -> payload (b/<turn>.txb)
  uint64_t q = 0;
  uint32_t turn = 0;
  uint64_t t = kT0;
  uint8_t round = 0;
  int callNo = 0;
  std::string callPrefix = "toolu_";

  tx::Item& add(tx::ItemKind k) {
    tx::Item it;
    it.q = ++q;
    it.kind = k;
    it.turn = turn;
    it.t = t;
    items.push_back(it);
    return items.back();
  }
  uint64_t openTurn(const char* trigger = "owner") {
    ++turn;
    round = 0;
    tx::Item& it = add(tx::ItemKind::TurnOpen);
    it.trigger = trigger;
    it.head = "api.anthropic.com/claude-sonnet-5-5";
    it.profile = "standard";
    return it.q;
  }
  uint64_t owner(const std::string& text, const std::vector<std::string>& photos = {}) {
    tx::Item& it = add(tx::ItemKind::User);
    it.x = text;
    it.ch = "tg";
    it.src = {7000 + q};
    for (const std::string& ref : photos) it.parts.push_back(tx::Part{tx::PartKind::Image, ref, "image/jpeg", 41000});
    return it.q;
  }
  uint64_t say(const std::string& text, bool fin = true) {
    tx::Item& it = add(tx::ItemKind::Assistant);
    it.x = text;
    it.rd = round;
    it.fin = fin;
    return it.q;
  }
  // A call; returns its id.
  std::string call(const std::string& name, const std::string& args, bool effect = false) {
    tx::Item& it = add(tx::ItemKind::Call);
    char id[32];
    std::snprintf(id, sizeof id, "%s%04d", callPrefix.c_str(), ++callNo);
    it.id = id;
    it.n = name;
    it.args = args;
    it.eff = effect;
    return it.id;
  }
  uint64_t intent(const std::string& id, const std::string& name) {
    tx::Item& it = add(tx::ItemKind::Intent);
    it.id = id;
    it.n = name;
    return it.q;
  }
  // A receipt kept whole (tier 1).
  uint64_t receipt(const std::string& id, const std::string& name, const std::string& text, bool ok = true) {
    tx::Item& it = add(tx::ItemKind::Result);
    it.id = id;
    it.n = name;
    it.x = text;
    it.ok = ok;
    it.cls = tx::ResultClass::Receipt;
    it.bytes = (uint32_t)text.size();
    it.lines = lineCount(text);
    it.st = internalStub(name, it.q, text);
    return it.q;
  }
  // A tier-2 result: payload in the bulk file, the stub on the line.
  uint64_t bulkResult(const std::string& id, const std::string& name, const std::string& payload) {
    tx::Item& it = add(tx::ItemKind::Result);
    it.id = id;
    it.n = name;
    it.cls = tx::ResultClass::Bulk;
    it.handle = "b/" + std::to_string(turn) + ".txb@" + std::to_string(it.q);
    it.bytes = (uint32_t)payload.size();
    it.lines = lineCount(payload);
    it.st = internalStub(name, it.q, payload);
    bulk[it.handle] = payload;
    return it.q;
  }
  // Outside content (web, fetch, connectors): whole only inside its own turn.
  uint64_t outside(const std::string& id, const std::string& name, const std::string& args, const std::string& payload) {
    tx::Item& it = add(tx::ItemKind::Result);
    it.id = id;
    it.n = name;
    it.cls = tx::ResultClass::Outside;
    it.handle = "b/" + std::to_string(turn) + ".txb@" + std::to_string(it.q);
    it.bytes = (uint32_t)payload.size();
    it.lines = lineCount(payload);
    it.st = "[stub " + name + "(" + args.substr(0, 120) + ") r" + std::to_string(it.q) + " " +
            std::to_string(payload.size()) + "B outside content, not shown: view id=r" + std::to_string(it.q) + "]";
    bulk[it.handle] = payload;
    return it.q;
  }
  uint64_t note(const std::string& text, tx::NoteClass cls = tx::NoteClass::Notice) {
    tx::Item& it = add(tx::ItemKind::DeviceNote);
    it.x = text;
    it.note = cls;
    return it.q;
  }
  uint64_t move(uint64_t to, const char* why, std::vector<uint64_t> sup = {}) {
    tx::Item& it = add(tx::ItemKind::PointerMove);
    it.stub = to;
    it.why = why;
    it.sup = std::move(sup);
    return it.q;
  }
  void nextRound() { ++round; }
  void advance(uint64_t seconds) { t += seconds; }
  // d and e: the turn is closed.
  void close(int64_t msgId = 0) {
    tx::Item& d = add(tx::ItemKind::Delivery);
    d.msgIds = {msgId ? msgId : (int64_t)(500 + turn)};
    tx::Item& e = add(tx::ItemKind::TurnEnd);
    e.end.cause = "reply";
    e.end.rounds = (uint16_t)(round + 1);
  }
  // One whole owner turn: a question, optional tool rounds, a final answer.
  void ownerTurn(const std::string& ask, const std::string& answer) {
    openTurn();
    owner(ask);
    say(answer);
    close();
  }

  static uint32_t lineCount(const std::string& s) {
    uint32_t n = s.empty() ? 0 : 1;
    for (char c : s)
      if (c == '\n') ++n;
    return n;
  }
  // The PRD's internal stub, '[stub <tool> r<q> <bytes>B <lines>L <HH:MM>] <first 200 B> ... (shown earlier in this
  // conversation; not attached to this request; view id=r<q> shows it again)' (lane E2-FIX9).
  static std::string internalStub(const std::string& tool, uint64_t q, const std::string& payload) {
    std::string first = payload.substr(0, 200);
    for (char& c : first)
      if (c == '\n') c = ' ';
    return "[stub " + tool + " r" + std::to_string(q) + " " + std::to_string(payload.size()) + "B " +
           std::to_string(lineCount(payload)) + "L 13:02] " + first + " ... (shown earlier in this conversation; not attached to this request; view id=r" + std::to_string(q) +
           " shows it again)";
  }
};

// A payload of `bytes` bytes with a unique marker at the start, so a renderer that shows it
// (or leaks part of it) is caught by search.
inline std::string payload(const std::string& marker, size_t bytes) {
  std::string s = marker + "\n";
  int line = 0;
  while (s.size() < bytes) s += "row " + std::to_string(++line) + " of " + marker + " data\n";
  s.resize(bytes);
  return s;
}

}  // namespace txb
