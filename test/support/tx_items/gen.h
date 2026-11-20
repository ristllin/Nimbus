#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "nimbus/orch/tx/item.h"

// tx_items/gen.h - items of every kind with every field set (lane RB-T1), for the codec
// and arena suites. fullItem fills ALL fields of tx::Item whatever the kind, so a round
// trip proves both halves of the rule: the kind's own fields come back exactly, and the
// fields it does not use are not written (they come back at their defaults). project()
// states, independently of the codec, which fields each kind keeps (tx/item.h).
namespace txgen {

namespace tx = nimbus::orch::tx;

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint64_t below(uint64_t n) { return n ? next() % n : 0; }
  uint64_t nonzero(uint64_t n) { return 1 + below(n); }
};

// Bytes of every kind: JSON specials, control bytes, NUL, DEL, UTF-8 and broken UTF-8.
inline std::string anyBytes(Rng& r, size_t maxLen) {
  static const char* pieces[] = {"\"", "\\", "\n", "\r", "\t", "", "\x7f", "\x01",
                                 "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x8d\x93", "\xc3", "\xff", "/", "}", "]",
                                 ",\"z\":\"", "kcal", " ", "\\u0041"};
  std::string s;
  const size_t len = 1 + r.below(maxLen);
  while (s.size() < len) {
    if (r.below(3) == 0) {
      const size_t i = r.below(sizeof pieces / sizeof pieces[0]);
      s += i == 5 ? std::string(1, '\0') : std::string(pieces[i]);   // pieces[5] stands for NUL
    } else {
      s.push_back(static_cast<char>(r.below(256)));
    }
  }
  return s;
}

inline std::string word(Rng& r, size_t maxLen) {
  std::string s;
  const size_t len = 1 + r.below(maxLen);
  for (size_t i = 0; i < len; ++i) s.push_back(static_cast<char>('a' + r.below(26)));
  return s;
}

// Every field of the item set to a non-default value.
inline tx::Item fullItem(tx::ItemKind kind, uint64_t q, Rng& r, size_t textMax = 200) {
  tx::Item it;
  it.q = q;
  it.kind = kind;
  it.turn = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  it.t = r.nonzero(UINT64_MAX - 1);
  it.x = anyBytes(r, textMax);
  const int tz = static_cast<int>(r.below(65536)) - 32768;   // the whole int16 range
  it.tzMinutes = static_cast<int16_t>(tz ? tz : 1);
  it.ch = word(r, 8);
  it.src = {r.nonzero(UINT64_MAX - 1), r.nonzero(1000)};
  it.m = r.nonzero(UINT64_MAX - 1);
  it.parts = {tx::Part{tx::PartKind::Image, "p" + word(r, 8), "image/jpeg", static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu))},
              tx::Part{tx::PartKind::File, anyBytes(r, 20), "", 0},
              tx::Part{tx::PartKind::Voice, "", anyBytes(r, 10), 7}};
  it.trigger = word(r, 10);
  it.head = anyBytes(r, 40);
  it.profile = word(r, 8);
  it.attempt = static_cast<uint8_t>(r.nonzero(255));
  it.photos = {"p" + word(r, 8), anyBytes(r, 12)};
  it.rd = static_cast<uint8_t>(r.nonzero(255));
  it.fin = true;
  it.id = anyBytes(r, 30);
  it.n = word(r, 16);
  it.args = anyBytes(r, textMax);
  it.eff = true;
  it.ok = false;
  it.cls = r.below(2) ? tx::ResultClass::Bulk : tx::ResultClass::Outside;
  it.handle = anyBytes(r, 30);
  it.bytes = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  it.lines = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  it.st = anyBytes(r, 120);
  it.note = static_cast<tx::NoteClass>(r.below(7));
  it.day = anyBytes(r, 10);
  it.thru = r.nonzero(UINT64_MAX - 1);
  it.ext = true;
  it.stub = r.nonzero(UINT64_MAX - 1);
  it.why = word(r, 12);
  it.sup = {r.nonzero(UINT64_MAX - 1), 3};
  it.msgIds = {-static_cast<int64_t>(r.nonzero(INT64_MAX)), static_cast<int64_t>(r.nonzero(INT64_MAX)), INT64_MIN};
  tx::TurnEndFacts& e = it.end;
  e.cause = anyBytes(r, 20);
  e.rounds = static_cast<uint16_t>(r.nonzero(0xFFFF));
  e.tools = static_cast<uint16_t>(r.nonzero(0xFFFF));
  e.headRequests = static_cast<uint16_t>(r.nonzero(0xFFFF));
  e.probeRequests = static_cast<uint16_t>(r.nonzero(0xFFFF));
  e.promptTokens = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  e.outputTokens = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  e.cacheReadTokens = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  e.cacheWriteTokens = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  e.microUsd = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  e.ms = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  e.exchanges = {tx::ExchangeFacts{1, 2, 3}, tx::ExchangeFacts{static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu)), 0, 429}};
  tx::ExchangeFacts ref{static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu)), 7, 200};   // a request by reference (RB-E3)
  ref.fromQ = r.nonzero(UINT64_MAX), ref.thruQ = r.nonzero(UINT64_MAX), ref.stub = r.nonzero(UINT64_MAX);
  ref.items = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  ref.closedThrough = r.nonzero(UINT64_MAX), ref.stubThrough = r.nonzero(UINT64_MAX), ref.omitThrough = r.nonzero(UINT64_MAX);
  ref.maxTokens = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  ref.toolChoiceNone = true;
  ref.bodyHash = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  ref.effort = anyBytes(r, 8);
  ref.omission = anyBytes(r, 40);
  e.exchanges.push_back(ref);
  e.events = {"change:pointer", anyBytes(r, 16)};
  e.promptHash = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  e.manifestHash = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));
  e.lastPromptTokens = static_cast<uint32_t>(r.nonzero(0xFFFFFFFFu));   // RB-H81
  return it;
}

// The item as a round trip must give it back: the common fields and the kind's own.
inline tx::Item project(const tx::Item& in) {
  tx::Item o;
  o.q = in.q;
  o.kind = in.kind;
  o.turn = in.turn;
  o.t = in.t;
  switch (in.kind) {
    case tx::ItemKind::User:
      o.x = in.x, o.tzMinutes = in.tzMinutes, o.ch = in.ch, o.src = in.src, o.m = in.m, o.parts = in.parts;
      break;
    case tx::ItemKind::TurnOpen:
      o.trigger = in.trigger, o.head = in.head, o.profile = in.profile, o.attempt = in.attempt, o.photos = in.photos;
      break;
    case tx::ItemKind::Assistant: o.x = in.x, o.rd = in.rd, o.fin = in.fin, o.handle = in.handle; break;
    case tx::ItemKind::Call: o.id = in.id, o.n = in.n, o.args = in.args, o.eff = in.eff, o.rd = in.rd, o.st = in.st; break;
    case tx::ItemKind::Intent: o.id = in.id, o.n = in.n; break;
    case tx::ItemKind::Result:
      o.id = in.id, o.n = in.n, o.ok = in.ok, o.cls = in.cls, o.x = in.x, o.handle = in.handle, o.bytes = in.bytes,
      o.lines = in.lines, o.st = in.st;
      break;
    case tx::ItemKind::RoundMeta: o.x = in.x, o.rd = in.rd; break;
    case tx::ItemKind::DeviceNote: o.x = in.x, o.note = in.note; break;
    case tx::ItemKind::Digest: o.x = in.x, o.day = in.day, o.thru = in.thru, o.ext = in.ext; break;
    case tx::ItemKind::PointerMove: o.stub = in.stub, o.why = in.why, o.sup = in.sup; break;
    case tx::ItemKind::Delivery: o.msgIds = in.msgIds; break;
    case tx::ItemKind::TurnEnd: o.end = in.end; break;
  }
  return o;
}

inline bool sameExchange(const tx::ExchangeFacts& p, const tx::ExchangeFacts& o) {
  return std::tie(p.requestBytes, p.ms, p.httpStatus, p.fromQ, p.thruQ, p.items, p.stub, p.closedThrough, p.stubThrough,
                  p.omitThrough, p.maxTokens, p.toolChoiceNone, p.bodyHash, p.effort, p.omission) ==
         std::tie(o.requestBytes, o.ms, o.httpStatus, o.fromQ, o.thruQ, o.items, o.stub, o.closedThrough, o.stubThrough,
                  o.omitThrough, o.maxTokens, o.toolChoiceNone, o.bodyHash, o.effort, o.omission);
}

// "" when every field of a and b is equal; else the first field that differs.
inline std::string firstDifference(const tx::Item& a, const tx::Item& b) {
#define TXGEN_SAME(f) \
  if (!(a.f == b.f)) return #f;
  TXGEN_SAME(q) TXGEN_SAME(kind) TXGEN_SAME(turn) TXGEN_SAME(t) TXGEN_SAME(x) TXGEN_SAME(tzMinutes)
  TXGEN_SAME(ch) TXGEN_SAME(src) TXGEN_SAME(m) TXGEN_SAME(trigger) TXGEN_SAME(head) TXGEN_SAME(profile)
  TXGEN_SAME(attempt) TXGEN_SAME(photos) TXGEN_SAME(rd) TXGEN_SAME(fin) TXGEN_SAME(id) TXGEN_SAME(n)
  TXGEN_SAME(args) TXGEN_SAME(eff) TXGEN_SAME(ok) TXGEN_SAME(cls) TXGEN_SAME(handle) TXGEN_SAME(bytes)
  TXGEN_SAME(lines) TXGEN_SAME(st) TXGEN_SAME(note) TXGEN_SAME(day) TXGEN_SAME(thru) TXGEN_SAME(ext)
  TXGEN_SAME(stub) TXGEN_SAME(why) TXGEN_SAME(sup) TXGEN_SAME(msgIds) TXGEN_SAME(end.cause)
  TXGEN_SAME(end.rounds) TXGEN_SAME(end.tools) TXGEN_SAME(end.headRequests) TXGEN_SAME(end.probeRequests)
  TXGEN_SAME(end.promptTokens) TXGEN_SAME(end.outputTokens) TXGEN_SAME(end.cacheReadTokens)
  TXGEN_SAME(end.cacheWriteTokens) TXGEN_SAME(end.microUsd) TXGEN_SAME(end.ms) TXGEN_SAME(end.events)
  TXGEN_SAME(end.promptHash) TXGEN_SAME(end.manifestHash) TXGEN_SAME(end.lastPromptTokens)
#undef TXGEN_SAME
  if (a.parts.size() != b.parts.size()) return "parts";
  for (size_t i = 0; i < a.parts.size(); ++i) {
    const tx::Part &p = a.parts[i], &o = b.parts[i];
    if (p.kind != o.kind || p.ref != o.ref || p.mime != o.mime || p.bytes != o.bytes) return "parts[" + std::to_string(i) + "]";
  }
  if (a.end.exchanges.size() != b.end.exchanges.size()) return "end.exchanges";
  for (size_t i = 0; i < a.end.exchanges.size(); ++i) {
    const tx::ExchangeFacts &p = a.end.exchanges[i], &o = b.end.exchanges[i];
    if (!sameExchange(p, o)) return "end.exchanges[" + std::to_string(i) + "]";
  }
  return "";
}

// A plausible item of `kind` for arena volume tests: realistic sizes, ASCII text.
inline tx::Item typicalItem(tx::ItemKind kind, uint64_t q, uint32_t turn, Rng& r) {
  tx::Item it;
  it.q = q;
  it.kind = kind;
  it.turn = turn;
  it.t = 1794920520ULL + q * 7;
  switch (kind) {
    case tx::ItemKind::User: it.x = word(r, 300), it.ch = "tg", it.src = {7000 + q}; break;
    case tx::ItemKind::Assistant: it.x = word(r, 400), it.fin = true; break;
    case tx::ItemKind::Call: it.id = "toolu_" + std::to_string(q), it.n = "read_file", it.args = "{\"path\":\"notes/" + word(r, 20) + ".md\"}"; break;
    case tx::ItemKind::Result:
      it.id = "toolu_" + std::to_string(q - 1), it.n = "read_file", it.x = word(r, 300), it.bytes = 300, it.lines = 4;
      it.st = "[stub read_file r" + std::to_string(q) + " 300B 4L 13:02] " + word(r, 60);
      break;
    case tx::ItemKind::TurnEnd: it.end.cause = "reply", it.end.rounds = 2, it.end.events = {"change:pointer"}; break;
    default: it.x = word(r, 60); break;
  }
  return it;
}

}  // namespace txgen
