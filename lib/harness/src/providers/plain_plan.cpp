// plain_plan.cpp - the shared planner of engine 2's wires (plain_plan.h). Moved out of
// plain_wire_anthropic.cpp by lane E2-R2b; the rules are the ones that file's header names
// (messages after thru, pairing, send order, the stub rule, photos, the ladder, the cache marks).
// Lane E2-P-CONN2 adds providerTools at the end: the provider-side connectors and sandbox.
#include "plain_plan.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nimbus/harness/log.h"
#include "nimbus/harness/plain/exchange.h"   // providerSlugOf (lane E2-P-CONN2)
#include "nimbus/harness/plain/history.h"   // viewedPhoto (lane E2-SRCH)
#include "nimbus/harness/plain/route.h"     // the request's row (lane E2-R4b)
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/connector_grant.h"    // whose connectors ride (CUM-828, lane E2-P-CONN2)
#include "nimbus/orch/connectors_wire.h"    // engine 1's attach builders (lane E2-P-CONN2)
#include "nimbus/orch/psram_alloc.h"   // workingAlloc: the plan state off the stack
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/rules.h"

namespace agent {
namespace plain {
namespace plan {

namespace {

namespace tx = nimbus::orch::tx;
namespace caps = nimbus::orch::caps;

// ---- small rules ---------------------------------------------------------------------

// The bytes of a blank character at s[i] (ASCII and Unicode white space), 0 when not blank.
size_t blankAt(const std::string& s, size_t i) {
  const unsigned char c = (unsigned char)s[i];
  if (c == ' ' || (c >= '\t' && c <= '\r')) return 1;
  static const char* const kBlanks[] = {
      "\xC2\x85",     "\xC2\xA0",     "\xE1\x9A\x80", "\xE2\x80\x80", "\xE2\x80\x81", "\xE2\x80\x82",
      "\xE2\x80\x83", "\xE2\x80\x84", "\xE2\x80\x85", "\xE2\x80\x86", "\xE2\x80\x87", "\xE2\x80\x88",
      "\xE2\x80\x89", "\xE2\x80\x8A", "\xE2\x80\xA8", "\xE2\x80\xA9", "\xE2\x80\xAF", "\xE2\x81\x9F",
      "\xE3\x80\x80"};
  for (const char* b : kBlanks) {
    const size_t n = std::strlen(b);
    if (s.compare(i, n, b) == 0) return n;
  }
  return 0;
}
// A provider refuses a text block that is empty or white space only.
bool visible(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const size_t b = blankAt(s, i);
    if (!b) return true;
    i += b;
  }
  return false;
}

// The id pattern a wire takes, ^[a-zA-Z0-9_-]+$.
bool idChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
// A stored call id a wire cannot take (another dialect's id, an empty one) is sent as
// 'h' and its bytes in hex: one to one, so two ids never collide, and the result's
// call id is mapped the same way.
std::string wireId(const std::string& id) {
  bool ok = !id.empty();
  for (char c : id) ok = ok && idChar(c);
  if (ok) return id;
  static const char kHex[] = "0123456789abcdef";
  std::string out = "h";
  for (unsigned char c : id) {
    out += kHex[c >> 4];
    out += kHex[c & 15];
  }
  return out;
}

// Mistral's rule: exactly 9 alphanumerics; another id is a deterministic 9-character digest
// (engine 1's mistralCallId, the same digest).
std::string nineId(const std::string& id) {
  bool ok = id.size() == 9;
  for (char c : id) ok = ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
  if (ok) return id;
  static const char kAl[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  uint32_t h = 2166136261u;   // FNV-1a
  for (char c : id) {
    h ^= (uint8_t)c;
    h *= 16777619u;
  }
  std::string out(9, 'a');
  for (int i = 0; i < 9; i++) {
    out[i] = kAl[h % 36];
    h /= 36;
    h = h * 31 + 7;
  }
  return out;
}

// A stored call id as the request's row takes it (lane E2-R4b, router design section 5 item 5): mapped at
// render time, identically for the call and its result; storage keeps the provider's own id.
std::string mappedId(caps::IdRule rule, const std::string& id) {
  switch (rule) {
    case caps::IdRule::Passthrough: return id.empty() ? wireId(id) : id;
    case caps::IdRule::Mistral9: return nineId(id);
    case caps::IdRule::AnthropicPattern: break;
  }
  return wireId(id);
}

// What the request's row says about the planner's choices (lane E2-R4b). No route, or a host no table
// slot names: the Anthropic Messages facts every request had before the router.
struct RowRules {
  caps::IdRule idRule = caps::IdRule::AnthropicPattern;
  caps::ReplayScope replayScope = caps::ReplayScope::OpenTurn;
  bool dropUnderRung = true;
  bool tools = true;
  uint8_t order = caps::order::kEndsOnUser | caps::order::kResultsFirstInUser | caps::order::kNoEmptyText |
                  caps::order::kMergeUsers | caps::order::kNotOnlySystem;
  uint32_t realBytesMax = 0;   // the route's real-byte limit (0 = none)
};

RowRules rowRules(const RenderInput& in) {
  RowRules r;
  if (!in.route) return r;
  r.realBytesMax = in.route->realBytesMax;
  if (!in.route->caps.provider) return r;
  const caps::ModelRow& row = in.route->caps.row;
  r.idRule = row.idRule;
  r.replayScope = row.replayScope;
  r.dropUnderRung = row.flags & caps::flag::kDropUnderRung;
  // Text-mode history only for a request that offers no tools: a row without tools whose codec or pre-flight
  // sends none (a custom Messages head keeps its tools array until the verify step, lane R8).
  r.tools = (row.flags & caps::flag::kTools) || (in.tools && !in.tools->empty());
  r.order = row.orderRules;
  return r;
}

bool imageMime(const std::string& m) {
  return m == "image/jpeg" || m == "image/png" || m == "image/gif" || m == "image/webp";
}

// ---- pass 1: the facts of the transcript ---------------------------------------------

struct Row {
  uint64_t q = 0;
  uint64_t t = 0;
  uint32_t turn = 0;
  tx::ItemKind kind = tx::ItemKind::User;
  bool keep = false;       // rendered
  bool emits = true;       // writes a block (a blank text or a broken meta item does not)
  std::string id;          // c and r: the stored call id
  std::string wid;         // c: the id as sent
  std::string tag;         // k: its replay tag ("" = stored before the tag)
  size_t est = 0;          // the body bytes it may take
};

struct Facts {
  std::vector<Row> rows;
  uint64_t thru = 0;                  // the newest s item's thru
  uint64_t pointer = 0;               // max(input pointer, newest x item's)
  std::vector<uint64_t> superseded;   // the stub lists (tx::stubList x items' sup), sorted, unique
  std::vector<std::string> photos;    // the open turn's visible photo refs (its t item)
  size_t estimate = 0;                // body bytes the rows after thru not left out may take
  FragmentOk sendable = nullptr;      // the codec's test of a stored fragment
  std::vector<uint32_t> omitted;      // turns the omission line stands for, oldest first
  RowRules rules;                     // the request's row
  std::string tag;                    // the request's replay tag
  std::vector<std::pair<uint32_t, std::string>> turnModel;   // each resident t item's model
};

// Whether a stored item writes a block at all (a blank text or a broken meta item does not).
bool emitsBlock(const tx::Item& it, uint16_t dayStartMin, FragmentOk sendable, bool blankOut) {
  switch (it.kind) {
    case tx::ItemKind::User: {
      char stamp[tx::kStampMaxBytes];
      for (const tx::Part& p : it.parts)
        if (p.kind == tx::PartKind::Image) return true;
      return visible(it.x) || tx::messageStamp(it.t, it.tzMinutes, dayStartMin, stamp, sizeof stamp).ok();
    }
    case tx::ItemKind::Assistant:
    case tx::ItemKind::DeviceNote:
      return !blankOut || visible(it.x);
    case tx::ItemKind::RoundMeta:
      return sendable(it.x);
    default:
      return true;
  }
}

// What one item tells the request beyond its own blocks.
// Returns the body bytes it may take.
size_t noteFacts(const RenderInput& in, const tx::Item& it, Facts& f) {
  switch (it.kind) {
    case tx::ItemKind::PointerMove:
      f.pointer = std::max(f.pointer, it.stub);
      if (tx::stubList(it)) f.superseded.insert(f.superseded.end(), it.sup.begin(), it.sup.end());
      break;
    case tx::ItemKind::Digest:
      f.thru = std::max(f.thru, it.thru);
      break;
    case tx::ItemKind::TurnOpen: {
      if (it.turn == in.turn->turn) f.photos = it.photos;
      const size_t slash = it.head.find('/');   // "host/model"
      if (slash != std::string::npos) f.turnModel.push_back({it.turn, it.head.substr(slash + 1)});
      break;
    }
    default:
      break;
  }
  size_t est = it.x.size() + it.args.size() + it.st.size() + 96;
  if (it.kind == tx::ItemKind::Result && !it.handle.empty()) est += it.bytes;
  return est;
}

Status readFacts(const RenderInput& in, const tx::TxArena& arena, Facts& f) {
  const size_t n = arena.count();
  f.rows.reserve(n);
  f.pointer = std::max(in.stubPointer, in.ladder ? in.ladder->closedThrough : 0);
  tx::Item it;
  for (size_t i = 0; i < n; ++i) {
    Row row;
    if (!arena.get(i, it).ok()) {   // a line kept by the restart fill that cannot be read
      row.kind = tx::ItemKind::Delivery;
      f.rows.push_back(row);
      continue;
    }
    row.q = it.q;
    row.t = it.t;
    row.turn = it.turn;
    row.kind = it.kind;
    if (it.kind == tx::ItemKind::Call || it.kind == tx::ItemKind::Result) row.id = it.id;
    if (it.kind == tx::ItemKind::RoundMeta) row.tag = it.wr;
    row.emits = emitsBlock(it, in.turn->dayStartMin, f.sendable, f.rules.order & caps::order::kNoEmptyText);
    row.est = noteFacts(in, it, f);
    f.rows.push_back(std::move(row));
  }
  std::sort(f.superseded.begin(), f.superseded.end());
  f.superseded.erase(std::unique(f.superseded.begin(), f.superseded.end()), f.superseded.end());
  return Status::success();
}

// Rung 3: every whole turn after thru whose items are all at or below omitThrough, never the
// open turn; only when the engine wrote the line that stands in their place.
void chooseOmitted(const RenderInput& in, Facts& f) {
  if (!in.ladder || !in.ladder->omitThrough || in.ladder->omissionLine.empty()) return;
  std::vector<std::pair<uint32_t, uint64_t>> last;   // turn -> its newest q, in first-seen order
  for (const Row& r : f.rows) {
    if (r.q <= f.thru || r.q == 0 || r.turn == 0) continue;   // turn 0 is never a whole turn to omit
    auto it = std::find_if(last.begin(), last.end(), [&r](const std::pair<uint32_t, uint64_t>& t) { return t.first == r.turn; });
    if (it == last.end()) last.push_back({r.turn, r.q});
    else it->second = std::max(it->second, r.q);
  }
  for (const auto& t : last)
    if (t.first != in.turn->turn && t.second <= in.ladder->omitThrough) f.omitted.push_back(t.first);
}

// The engine's omission line stands at the head of the request whenever its ladder names one,
// even when no resident turn is left out by it (turns a full arena already dropped from RAM).
bool omissionNamed(const RenderInput& in) {
  return in.ladder && in.ladder->omitThrough && !in.ladder->omissionLine.empty();
}

bool omittedTurn(const Facts& f, uint32_t turn) {
  return std::find(f.omitted.begin(), f.omitted.end(), turn) != f.omitted.end();
}

// The calls of the transcript, in q order: which row, answered yet.
struct Pairs {
  struct Open {
    std::string id;
    uint32_t turn;
    size_t row;
    bool answered;
  };
  std::vector<Open> calls;
  std::vector<std::string> sent;   // wire ids already used
  caps::IdRule rule = caps::IdRule::AnthropicPattern;

  bool used(const std::string& wid) const { return std::find(sent.begin(), sent.end(), wid) != sent.end(); }
  // Every call gets a wire id of its own: a stored id used again (a dialect that numbers its
  // calls per answer, an empty id) is told apart by the call's q.
  void call(Row& r, size_t row) {
    r.wid = mappedId(rule, r.id);
    for (std::string again = r.id; used(r.wid);) {   // Mistral's 9 characters: a digest of the id and its q
      again += "-" + std::to_string(r.q);
      r.wid = rule == caps::IdRule::Mistral9 ? nineId(again) : r.wid + "-" + std::to_string(r.q);
    }
    sent.push_back(r.wid);
    calls.push_back(Open{r.id, r.turn, row, false});
  }
  // A result answers the earliest unanswered call of its id in its own turn (two calls of one
  // round sharing an id get their results in order); any other is left out (an orphan, a
  // second result).
  void result(Row& r, std::vector<Row>& rows) {
    for (size_t i = 0; i < calls.size(); ++i) {
      Open& c = calls[i];
      if (c.answered || c.id != r.id || c.turn != r.turn) continue;
      c.answered = true;
      r.keep = true;
      r.wid = rows[c.row].wid;
      return;
    }
  }
};

bool userKind(tx::ItemKind k) { return k == tx::ItemKind::User || k == tx::ItemKind::DeviceNote; }
bool assistantKind(tx::ItemKind k) {
  return k == tx::ItemKind::Assistant || k == tx::ItemKind::RoundMeta || k == tx::ItemKind::Call;
}

// Whether the open turn's signed thinking is sent: not once a ladder rung is applied. Settled live
// by RB-W1b (evidence/LIVE-CONFORMANCE.md, claude-haiku-5-5, 19 Nov 2026): a signature binds the
// block to every byte before it, a rung changes bytes before it (rungs 1 and 2 stub earlier
// results, rung 3 replaces earlier turns), and where the API enforces that check (accounts made on
// or after 31 Aug 2026, or a request that sets prefix_mismatch_behavior) the request is a 400
// ("bound to a different conversation"); the same request with the open turn's thinking left out
// is accepted, enforced or not, with no block reported dropped. The engine's ladder only climbs
// inside a turn, so from the first rung on every request of the turn goes without it (leaving out
// all of them keeps nothing that a later rung could invalidate).
bool sendOpenTurnThinking(const RenderInput& in) {
  const LadderState* l = in.ladder;
  return !l || (!l->closedThrough && !l->stubThrough && !omissionNamed(in));
}

// The tag a k item was written under: its own, else (stored before the tag) Anthropic, direct, its
// turn's model; a turn whose t item is not resident reads as the request's model.
std::string storedTag(const Facts& f, const Row& r, const std::string& requestModel) {
  if (!r.tag.empty()) return r.tag;
  std::string model = requestModel;
  for (const auto& tm : f.turnModel)
    if (tm.first == r.turn) model = tm.second;
  return replayTag(Dialect::AnthropicMessages, false, model);
}

// Lane E2-R4b (router design section 5 item 4): a stored k item goes back only to the request whose tag
// equals the one it was written under, in the turns the row's replay scope covers, and under a ladder rung
// only on a row that keeps it there. Anything else is left out at render time; storage is never rewritten.
bool replays(const RenderInput& in, const Facts& f, const Row& r) {
  if (f.rules.replayScope == caps::ReplayScope::OpenTurn && r.turn != in.turn->turn) return false;
  if (f.rules.dropUnderRung && !sendOpenTurnThinking(in)) return false;
  return storedTag(f, r, in.turn->model) == f.tag;
}

// What is rendered, and the pairing: a call with its one result. Assistant items before the
// first user item after thru (a turn cut by compaction) are not sent: the request opens with
// the user's turn.
void chooseRows(const RenderInput& in, Facts& f) {
  Pairs pairs;
  pairs.rule = f.rules.idRule;
  bool userSeen = omissionNamed(in);   // the omission line opens the request
  for (size_t i = 0; i < f.rows.size(); ++i) {
    Row& r = f.rows[i];
    if (r.q == 0 || r.q <= f.thru || omittedTurn(f, r.turn)) continue;
    f.estimate += r.est;
    if (!r.emits) continue;
    if (assistantKind(r.kind) && !userSeen) continue;
    if (userKind(r.kind)) r.keep = userSeen = true;
    else if (r.kind == tx::ItemKind::Assistant) r.keep = true;
    else if (r.kind == tx::ItemKind::RoundMeta) r.keep = replays(in, f, r);
    else if (r.kind == tx::ItemKind::Call) pairs.call(r, i);
    else if (r.kind == tx::ItemKind::Result) pairs.result(r, f.rows);
  }
  for (const Pairs::Open& c : pairs.calls) f.rows[c.row].keep = c.answered;
}

bool waitingFor(const std::vector<std::string>& waiting, const std::string& id) {
  return std::find(waiting.begin(), waiting.end(), id) != waiting.end();
}

// While calls wait: the round's next assistant item stored before its results (until the
// first result is sent), else the first waiting result.
size_t nextWhileWaiting(const Facts& f, const std::vector<size_t>& queue, const std::vector<bool>& done, size_t first,
                        const std::vector<std::string>& waiting, bool answering) {
  size_t join = queue.size();
  for (size_t k = first; k < queue.size(); ++k) {
    if (done[k]) continue;
    const Row& r = f.rows[queue[k]];
    if (r.kind == tx::ItemKind::Result && waitingFor(waiting, r.id)) return join < k ? join : k;
    if (r.kind == tx::ItemKind::Result) answering = true;   // a later round's result: the round is over
    else if (!answering && join == queue.size() && assistantKind(r.kind)) join = k;
  }
  return first;   // unreachable: every kept call has its result later
}

// Signed thinking goes back exactly where it was stored (before the text, between calls, after
// the last call: wire.h MetaPart), in q order; only a run of one turn's assistant items made of
// thinking alone (its round's calls all left out) is not sent.
void dropThinkingAlone(const Facts& f, std::vector<size_t>& order) {
  std::vector<size_t> out;
  out.reserve(order.size());
  for (size_t i = 0; i < order.size();) {
    const Row& r = f.rows[order[i]];
    size_t j = i;
    while (j < order.size() && assistantKind(f.rows[order[j]].kind) && f.rows[order[j]].turn == r.turn) ++j;
    if (j == i) {
      out.push_back(order[i++]);
      continue;
    }
    bool said = false;
    for (size_t k = i; k < j; ++k) said = said || f.rows[order[k]].kind != tx::ItemKind::RoundMeta;
    if (said) out.insert(out.end(), order.begin() + (long)i, order.begin() + (long)j);
    i = j;
  }
  order.swap(out);
}

// The send order: q order, except while a round's calls wait for their results. Until the
// first of those results is sent, the round's further assistant items (more calls, text,
// meta) stored before it join the round; then only the waiting results are sent. Anything
// else stored between a call and its result (a device note, a late owner message) waits.
std::vector<size_t> sendOrder(const Facts& f) {
  std::vector<size_t> queue;
  for (size_t i = 0; i < f.rows.size(); ++i)
    if (f.rows[i].keep) queue.push_back(i);
  std::vector<size_t> order;
  order.reserve(queue.size());
  std::vector<bool> done(queue.size(), false);
  std::vector<std::string> waiting;   // stored ids of calls sent, results not yet
  bool answering = false;             // a result of the waiting round was sent
  size_t first = 0;
  while (order.size() < queue.size()) {
    while (first < queue.size() && done[first]) ++first;
    size_t pick = first;
    if (!waiting.empty()) pick = nextWhileWaiting(f, queue, done, first, waiting, answering);
    const Row& r = f.rows[queue[pick]];
    if (r.kind == tx::ItemKind::Call) waiting.push_back(r.id);
    if (r.kind == tx::ItemKind::Result) {
      auto w = std::find(waiting.begin(), waiting.end(), r.id);
      if (w != waiting.end()) waiting.erase(w);
      answering = !waiting.empty();
    }
    done[pick] = true;
    order.push_back(queue[pick]);
  }
  dropThinkingAlone(f, order);
  // The request ends with the user's turn (a provider takes no assistant prefill): assistant
  // text or meta left last by a round whose calls were all left out is not sent.
  if (f.rules.order & caps::order::kEndsOnUser)
    while (!order.empty() && assistantKind(f.rows[order.back()].kind)) order.pop_back();
  return order;
}

// ---- pass 2: the pieces -------------------------------------------------------------------

struct Builder {
  const RenderInput& in;
  const Facts& f;
  Plan& out;
  // Photos attached to this request so far and their stored bytes (E2-FIX4's caps also bound the
  // photos a view result brings back, lane E2-SRCH).
  size_t photosAttached = 0;
  size_t photoBytesAttached = 0;
  std::vector<std::string> refs;   // the ref of each photo attached, one to one with out.photos

  Piece& add(PieceKind kind, uint32_t item, uint32_t turn) {
    Piece p;
    p.kind = kind;
    p.item = item;
    p.turn = turn;
    out.pieces.push_back(p);
    return out.pieces.back();
  }
  uint32_t line(std::string s) {
    out.lines.push_back(std::move(s));
    return (uint32_t)(out.lines.size() - 1);
  }
  uint32_t id(const std::string& wid) {
    out.ids.push_back(wid);
    return (uint32_t)(out.ids.size() - 1);
  }
  uint16_t photo(const PhotoBytes& px, const std::string& ref) {
    ++photosAttached;
    refs.push_back(ref);
    Photo ph;
    if (px.fileId.empty()) {
      photoBytesAttached += px.len;
      ph.bytes = px.bytes;
      ph.len = px.len;
    }
    ph.text = line(px.fileId.empty() ? px.mime : px.fileId);
    out.photos.push_back(ph);
    return (uint16_t)(out.photos.size() - 1);
  }
};

// A user text line of the planner's own (a photo's line, the omission line).
void linePiece(Builder& b, uint32_t item, uint32_t turn, bool opens, std::string text) {
  const uint32_t at = b.line(std::move(text));
  Piece& p = b.add(PieceKind::Line, item, turn);
  p.src = Src::Line;
  p.line = at;
  if (opens) p.flags |= kOpens;
}

// Lane E2-P-PHOTO: why this request may not carry `ref` as pixels at all (a head without image
// input, a photo the provider refused in this chat); false when it may.
bool photoAbsent(const RenderInput& in, const std::string& ref, tx::PhotoAbsent& why) {
  const std::vector<std::string>* refused = in.refusedPhotos;
  if (refused && std::find(refused->begin(), refused->end(), ref) != refused->end()) {
    why = tx::PhotoAbsent::Refused;
    return true;
  }
  why = tx::PhotoAbsent::Blind;
  return in.turn && in.turn->photosBlind;
}

// The line of an owner message's photo sent without its pixels. Its note kept (RB-H81), else its stored
// caption (lane E2-R4b, design section 7). E2-FIX5: names view only when the turn's registry binds it.
bool photoLine(const Builder& b, const tx::Item& u, const tx::Part& p, bool absent, tx::PhotoAbsent why, std::string& stub) {
  std::string when;
  if (!tx::photoWhen(u.t, u.tzMinutes, when).ok()) when.clear();
  const std::string& note = p.note.empty() ? p.cap : p.note;
  return absent ? tx::photoAbsentLine(p.ref.substr(1), when, note, why, stub).ok()
                : tx::photoStub(p.ref.substr(1), when, note, stub, b.in.bound.view).ok();
}

// A photo of an owner message: pixels when the open turn shows it (by file id, else inline bytes
// spliced at send time), else its stub line. False when nothing is sent (a part whose ref is not a
// photo ref has nothing to name).
bool photoPiece(Builder& b, const tx::Item& u, uint32_t idx, const tx::Part& p, bool opens) {
  tx::PhotoAbsent why = tx::PhotoAbsent::Blind;
  const bool absent = photoAbsent(b.in, p.ref, why);
  const bool shown = !absent && std::find(b.f.photos.begin(), b.f.photos.end(), p.ref) != b.f.photos.end();
  PhotoBytes px;
  if (shown && b.in.photo && b.in.photo(p.ref, px).ok() &&
      (!px.fileId.empty() || (px.bytes && px.len && imageMime(px.mime)))) {
    const uint16_t at = b.photo(px, p.ref);
    Piece& pc = b.add(PieceKind::Photo, idx, u.turn);
    pc.photo = at;
    if (opens) pc.flags |= kOpens;
    return true;
  }
  if (p.ref.size() < 2 || p.ref[0] != 'p') return false;
  std::string stub;
  if (!photoLine(b, u, p, absent, why, stub)) return false;
  linePiece(b, idx, u.turn, opens, std::move(stub));
  return true;
}

// An owner message opens a user message of its own: the stamped text, then its photos.
void ownerPieces(Builder& b, const tx::Item& u, uint32_t idx) {
  char stamp[tx::kStampMaxBytes];
  const bool stamped = tx::messageStamp(u.t, u.tzMinutes, b.in.turn->dayStartMin, stamp, sizeof stamp).ok();
  bool first = true;
  if (stamped || visible(u.x)) {
    const uint32_t at = stamped ? b.line(stamp) : 0;
    Piece& p = b.add(PieceKind::OwnerText, idx, u.turn);
    p.flags = kOpens | (stamped ? kStamp : 0);
    p.line = at;
    first = false;
  }
  for (const tx::Part& p : u.parts)
    if (p.kind == tx::PartKind::Image && photoPiece(b, u, idx, p, first)) first = false;
}

// A bulk file that could not be read, in the device log once per turn (the request is rendered
// again each round and on each ladder rung).
void logUnread(const RenderInput& in, const tx::Item& it) {
  static std::string s_logged;
  const std::string key = in.turn->chatKey + "#" + std::to_string(in.turn->turn);
  if (key == s_logged) return;
  s_logged = key;
  hlog::logf("wire(%s): the bulk result %s could not be read; its place says so", in.turn->chatKey.c_str(),
             it.handle.c_str());
}

// The whole text of a result: inline (its x), or its bulk payload (into `own`). A bulk file that is
// missing or short is never sent as less without a word (finding P11): the marker stands in its
// place, then the stub.
Src wholeText(const RenderInput& in, const tx::Item& it, std::string& own) {
  if (it.handle.empty()) return Src::X;
  if (!in.bulk) return Src::St;   // a render with no bulk reader (stubs by design)
  if (in.bulk(it.handle, own).ok()) return Src::Line;
  logUnread(in, it);
  own = texts::stubs::kResultUnread;
  if (!it.st.empty()) own += "\n" + it.st;
  return Src::Line;
}

// The stub rule's facts for this request: the pointer (with ladder rung 1), the stub lists, rung 2.
tx::ShowFacts showFacts(const RenderInput& in, const Facts& f) {
  tx::ShowFacts sf;
  sf.pointer = f.pointer;
  sf.openTurn = in.turn->turn;
  sf.superseded = &f.superseded;
  sf.ladderStubThrough = in.ladder ? in.ladder->stubThrough : 0;
  return sf;
}

// Lane E2-SRCH: a view result that names a photo of this conversation, while its turn is open,
// carries the photo's pixels after its line (by file id, else inline bytes), as an owner message's
// photo does, inside E2-FIX4's caps (kInlinePhotosInViewMax photos and kInlinePhotoBytesInViewMax
// stored bytes for the whole request, the photos already attached counted).
bool attachViewed(Builder& b, const std::string& ref, Piece& p) {
  PhotoBytes px;
  tx::PhotoAbsent why = tx::PhotoAbsent::Blind;
  if (photoAbsent(b.in, ref, why) || !b.in.photo || !b.in.photo(ref, px).ok()) return false;   // E2-P-PHOTO
  const bool file = !px.fileId.empty(), inline64 = px.bytes && px.len && imageMime(px.mime);
  const size_t bytes = file ? 0 : px.len;
  if ((!file && !inline64) || b.photosAttached + 1 > tx::kInlinePhotosInViewMax ||
      b.photoBytesAttached + bytes > tx::kInlinePhotoBytesInViewMax)
    return false;
  p.photo = b.photo(px, ref);
  p.flags |= kViewed;
  return true;
}

// Otherwise (a closed turn, the caps, a photo the hook cannot give) its content is the photo's
// line (tx::photoStub), so it never says "shown" when nothing is attached. True when that line
// replaces the content; false when the result is not a viewed photo or keeps its text.
bool viewedPhotoContent(Builder& b, const tx::Item& it, const std::string& text, Piece& p) {
  const std::string ref = it.ok ? viewedPhoto(it.n, text) : std::string();
  if (ref.empty()) return false;
  if (it.turn == b.in.turn->turn && attachViewed(b, ref, p)) return false;
  // "photo p<ref> <when>: shown here": the line with the same time, the photo not attached.
  const size_t from = std::min(text.size(), sizeof(texts::tool_results::kViewPhoto) - 1 + ref.size() + 1);
  const size_t to = text.size() - (sizeof(texts::tool_results::kViewPhotoShown) - 1);
  std::string line;
  const std::string when = from < to ? text.substr(from, to - from) : std::string();
  tx::PhotoAbsent why = tx::PhotoAbsent::Blind;
  if (photoAbsent(b.in, ref, why) ? !tx::photoAbsentLine(ref.substr(1), when, std::string(), why, line).ok()
                                  : !tx::photoStub(ref.substr(1), when, line, b.in.bound.view).ok())
    return false;
  p.src = Src::Line;
  p.line = b.line(std::move(line));
  return true;
}

// A result: whole or its stored stub (tx::showResult decides; the stub text is the one computed at
// capture, never rewritten here); not ok is an error only with content (an empty one is refused).
void resultPiece(Builder& b, const tx::Item& it, uint32_t idx, const Row& row) {
  tx::ShowAs as = tx::ShowAs::Stub;
  if (!tx::showResult(it, showFacts(b.in, b.f), as).ok()) as = tx::ShowAs::Stub;
  std::string own;
  Src src = Src::None;
  if (as == tx::ShowAs::Whole) src = wholeText(b.in, it, own);
  else if (!it.st.empty()) src = Src::St;
  else if (it.cls != tx::ResultClass::Outside) src = wholeText(b.in, it, own);   // no stub stored: never outside content
  const std::string& text = src == Src::X ? it.x : src == Src::St ? it.st : own;
  if (!b.f.rules.tools) {   // a row without tools: the call and its result as one line in their place
    linePiece(b, idx, it.turn, true, texts::stubs::kEarlierRan + it.n + texts::device_notes::kReasonSep + text);
    return;
  }
  const uint32_t wid = b.id(row.wid);
  Piece p;
  p.kind = PieceKind::Result;
  p.item = idx;
  p.turn = it.turn;
  p.id = wid;
  if (!text.empty()) {
    p.src = src;
    if (!it.ok) p.flags |= kError;
    if (!viewedPhotoContent(b, it, text, p) && src == Src::Line) p.line = b.line(std::move(own));
  }
  b.out.pieces.push_back(p);
}

// A call: its arguments whole until the overflow path (E2-VERB); arguments that are not one JSON
// object (a cut or broken call, a long-argument stub) are sent as an empty object.
void callPiece(Builder& b, const tx::Item& it, uint32_t idx, const Row& row) {
  const std::string& args = tx::shownArgs(it, showFacts(b.in, b.f));
  const uint32_t wid = b.id(row.wid);
  Piece& p = b.add(PieceKind::Call, idx, it.turn);
  p.id = wid;
  if (b.f.sendable(args)) p.src = &args == &it.st ? Src::St : Src::Args;
}

void itemPieces(Builder& b, const tx::Item& it, uint32_t idx, const Row& row) {
  switch (it.kind) {
    case tx::ItemKind::User:
      ownerPieces(b, it, idx);
      return;
    case tx::ItemKind::Assistant:   // blank text is not kept (emitsBlock)
      b.add(PieceKind::AssistantText, idx, it.turn).src = Src::X;
      return;
    case tx::ItemKind::DeviceNote:
      b.add(PieceKind::DeviceNote, idx, it.turn).src = Src::X;
      return;
    case tx::ItemKind::RoundMeta:   // the open turn's signed thinking, as stored; never a mark
      if (b.f.sendable(it.x)) b.add(PieceKind::Meta, idx, it.turn).src = Src::X;
      return;
    case tx::ItemKind::Call:   // a row without tools: its result's line says it (resultPiece)
      if (b.f.rules.tools) callPiece(b, it, idx, row);
      return;
    case tx::ItemKind::Result:
      resultPiece(b, it, idx, row);
      return;
    default:
      return;
  }
}

size_t base64Of(size_t n) { return (n + 2) / 3 * 4; }

// Lane E2-R4b (router design section 7): a route's real-byte limit (the router's 4,194,304 B, base64
// included), kept apart from the body cap, which counts a photo as kPhotoCapBytes and climbs the ladder.
// The text of the body is at most its body cap; while text and photos pass the limit, an owner message's
// photo is sent as its line instead of its pixels, oldest first, never one of the open turn (the photo
// that just arrived). The bytes stay stored, and the ladder never climbs for this.
void fitRealBytes(Builder& b) {
  const uint32_t limit = b.f.rules.realBytesMax;
  if (!limit) return;
  const SystemBlocks& sys = *b.in.system;
  const size_t text = b.in.bodyCap ? b.in.bodyCap : b.out.estimate + sys.core.size() + sys.notes.size() + sys.digests.size();
  size_t photos = 0;
  for (const Piece& p : b.out.pieces)
    if (p.kind == PieceKind::Photo || (p.flags & kViewed)) photos += base64Of(b.out.photos[p.photo].len);
  tx::Item u;
  for (Piece& p : b.out.pieces) {
    if (text + photos <= limit) return;
    if (p.kind != PieceKind::Photo || p.turn == b.in.turn->turn || !b.in.items->get(p.item, u).ok()) continue;
    const std::string& ref = b.refs[p.photo];
    auto part = std::find_if(u.parts.begin(), u.parts.end(), [&ref](const tx::Part& x) { return x.ref == ref; });
    std::string stub;
    if (part == u.parts.end() || !photoLine(b, u, *part, false, tx::PhotoAbsent::Blind, stub)) continue;
    photos -= base64Of(b.out.photos[p.photo].len);
    p.kind = PieceKind::Line;
    p.src = Src::Line;
    p.line = b.line(std::move(stub));
    p.flags &= kOpens;
  }
}

// The system blocks sent (a blank one is not) and their two marks: the end of the notes block (the
// core when there are no notes) and the end of the digests block.
void systemPlan(const SystemBlocks& s, SystemPlan& sp) {
  sp.shown[0] = visible(s.core);
  sp.shown[1] = visible(s.notes);
  sp.shown[2] = visible(s.digests);
  sp.stableAfter = sp.shown[1] ? 1 : sp.shown[0] ? 0 : -1;
  sp.digestsMark = sp.shown[2];
}

// The two message marks: the last block of the previous turn (when it is not the last block) and the
// moving last block. Reasoning is never a mark.
void messageMarks(Plan& out, uint32_t openTurn) {
  size_t last = out.pieces.size();
  for (size_t i = out.pieces.size(); i-- > 0;)
    if (out.pieces[i].kind != PieceKind::Meta) {
      last = i;
      break;
    }
  if (last == out.pieces.size()) return;
  out.anyBlock = true;
  for (size_t i = last + 1; i-- > 0;) {
    const Piece& p = out.pieces[i];
    if (p.kind == PieceKind::Meta || p.turn >= openTurn) continue;
    if (i != last) out.pieces[i].flags |= kMarkStable;
    break;
  }
  out.pieces[last].flags |= kMarkMoving;
}

}  // namespace

namespace {
// The plan's working state (the facts and one stored item) lives in the working set (PSRAM on the
// device), not on the stack: a request is planned at the bottom of the Telegram task's deepest
// chain (lane E2-STACK2). Freed on every way out.
struct PlanState {
  Facts f;
  tx::Item it;
};
struct OwnedPlanState {
  PlanState* p;
  ~OwnedPlanState() {
    if (!p) return;
    p->~PlanState();
    nimbus::orch::workingFree(p);
  }
};
}  // namespace

Status plan(const RenderInput& in, FragmentOk sendable, Plan& out) {
  out = Plan{};
  if (!in.turn || !in.system || !in.items || !sendable) return Status::fail(Err::InvalidArgument, "plain::plan");
  void* mem = nimbus::orch::workingAlloc(sizeof(PlanState));
  if (!mem) return Status::fail(Err::TooLarge, "plain::plan: no memory for the plan state");
  const OwnedPlanState owned{new (mem) PlanState()};
  Facts& f = owned.p->f;
  f.sendable = sendable;
  f.rules = rowRules(in);
  f.tag = replayTag(in.turn->dialect, in.head && in.head->routed, in.turn->model);
  Status st = readFacts(in, *in.items, f);
  if (!st.ok()) return st;
  chooseOmitted(in, f);
  chooseRows(in, f);
  const std::vector<size_t> order = sendOrder(f);
  out.estimate = f.estimate;
  systemPlan(*in.system, out.system);
  out.pieces.reserve(order.size() + 1);
  Builder b{in, f, out};
  if (omissionNamed(in)) linePiece(b, 0, 0, true, in.ladder->omissionLine);   // it opens the request
  tx::Item& it = owned.p->it;
  for (size_t idx : order)
    if (in.items->get(idx, it).ok()) itemPieces(b, it, (uint32_t)idx, f.rows[idx]);
  fitRealBytes(b);
  messageMarks(out, in.turn->turn);
  return Status::success();
}

// ---- provider-side connectors and the sandbox (lane E2-P-CONN2) -----------------------------------

namespace {

namespace orch = nimbus::orch;

// One JSON object onto the list.
void joinOne(JsonVariantConst v, std::vector<std::string>& out) {
  out.emplace_back();
  serializeJson(v, out.back());
}

// The grant of the turn's person, never looser than the calling task's (a scope a routine set). A turn
// with no person is no owner's: built-ins only.
orch::ConnectorGrant turnGrant(const TurnCtx& turn) {
  const orch::ConnectorGrant own = turn.who ? orch::connectorGrantFor(*turn.who) : orch::ConnectorGrant::BuiltinsOnly;
  return std::max(own, orch::currentConnectorGrant());
}

void anthropicTools(const std::vector<orch::ConnectorInfo>& cs, const orch::BearerFn& bearer, ProviderTools& out) {
  JsonDocument d;
  orch::attachAnthropicWire(d, cs, bearer);
  JsonDocument t;
  for (JsonObjectConst s : d["mcp_servers"].as<JsonArrayConst>()) {
    joinOne(s, out.servers);
    t.clear();
    t["type"] = "mcp_toolset";
    t["mcp_server_name"] = s["name"];
    joinOne(t.as<JsonVariantConst>(), out.tools);
  }
}

void openaiTools(const std::vector<orch::ConnectorInfo>& cs, const orch::BearerFn& bearer, ProviderTools& out) {
  JsonDocument d;
  orch::attachOpenAIWire(d, cs, bearer);
  for (JsonVariantConst t : d["tools"].as<JsonArrayConst>()) joinOne(t, out.tools);
}

// The head the request reaches, when it is one whose own connectors engine 1 attached: "anthropic" on
// the Messages wire, "openai" on Responses; "" for every other head (Mistral, Z.ai, a custom endpoint).
std::string attachingHead(const RenderInput& in) {
  const Dialect d = in.route ? in.route->dialect : in.turn->dialect;
  const std::string provider = in.route ? in.route->provider : providerSlugOf(*in.turn, *in.head);
  if (d == Dialect::AnthropicMessages && provider == "anthropic") return provider;
  return d == Dialect::OpenAIResponses && provider == "openai" ? provider : std::string();
}

}  // namespace

void providerTools(const RenderInput& in, ProviderTools& out) {
  out = ProviderTools{};
  if (!in.turn || !in.head || !in.head->connectors || !in.tools || in.tools->empty()) return;
  if (in.route ? in.route->routed : in.head->routed) return;   // engine 1's router route: no connector, no sandbox
  const std::string head = attachingHead(in);
  if (head.empty()) return;
  const bool messages = head == "anthropic";
  const orch::ConnectorGrantScope grant(turnGrant(*in.turn));
  const std::vector<orch::ConnectorInfo> cs = in.head->connectors();
  const orch::BearerFn bearer = in.head->bearer ? orch::BearerFn(in.head->bearer) : orch::BearerFn();
  if (messages) anthropicTools(cs, bearer, out);
  else openaiTools(cs, bearer, out);
}

}  // namespace plan
}  // namespace plain
}  // namespace agent
