// plain_wire_anthropic.cpp - the Anthropic Messages wire of engine 2 (PRD section 1,
// "Request on each wire"). Render half: lane RB-W1a. Parse half: lane RB-W1b.
//
// render() is a pure function of RenderInput: the same transcript and facts give the
// same bytes on every call (the exchange renders two or more times per request). It
// writes straight into the request body through the checked body writer (plain_body.h):
//
//   {"model", "max_tokens", "output_config":{"effort"} (when set),
//    "system":[core][skills index + pinned notes][digests + recent files],
//    "tools":[{"name","description","input_schema"}], "tool_choice":{"type":"auto"|"none"},
//    "messages":[...]}
//
// Messages, from the stored items after thru (the digests cover the rest), in q order:
//   u  a user message of its own: one text block '[Tue 17 Nov 2026 13:02] <text>', then
//      each photo: an image by file id or inline base64 when the open turn's t item lists
//      it as visible (tx::visiblePhotos decided that at turn open), else its stub line
//      (tx::photoStub) as a text block;
//   a  an assistant text block (whitespace-only text is not sent: the API refuses it);
//   k  the open turn's signed thinking, written as stored (raw); closed turns' is dropped;
//   c  a tool_use block; arguments that are not one JSON object (a cut or broken call, a
//      long-argument stub) are sent as {};
//   r  a tool_result block: whole, or its stored stub (tx::showResult decides; the stub
//      text is the one computed at capture, never rewritten here), is_error when not ok;
//   h  a device note: a user text block as stored.
// t, i, x, s, d and e items are not message content. A block joins the message before it
// only when the role and the turn are the same (an owner message always opens its own
// message), so a later item never rewrites a message an earlier request sent.
//
// Pairing, whatever the stored state: a call is sent only with its one result, a result
// only after its one call, each id once (a dangling call, an orphan or second result and
// a repeated id are left out); the results of a round open the next user message, before
// any other block (an item stored between a call and its result waits until the round's
// results are sent).
//
// Cache points (four, the API's maximum): the end of the notes block (the core when there
// are no notes), the end of the system (the digests block), the last block of the
// previous turn and the moving last block. Thinking blocks never carry one. A point is a
// marker only: the bytes before it do not change when a later item is appended.
//
// The ladder (RenderInput::ladder, the engine's LadderState): rung 1 closedThrough raises the
// pointer (with stubPointer and the newest x item's), rung 2 stubThrough is
// tx::ShowFacts::ladderStubThrough, rung 3 leaves out every whole turn after thru whose items
// are all at or below omitThrough (never the open turn) and sends omissionLine, the engine's
// tx::omissionLine text, in their place.
//
// Every model-facing text this file sends is stored text or a rules-lane text
// (orch_tx_rules.cpp): the wire adds no prose of its own.
#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/rules.h"
#include "nimbus/utf8.h"
#include "plain_body.h"

namespace agent {
namespace plain {

namespace {

namespace tx = nimbus::orch::tx;
using body::BodyWriter;
using body::Over;

constexpr const char* kDefaultHost = "api.anthropic.com";
constexpr const char* kVersion = "2023-06-01";
constexpr const char* kFilesBeta = "files-api-2025-04-14";
constexpr const char* kCacheMark = ",\"cache_control\":{\"type\":\"ephemeral\"}";

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
// The API refuses a text block that is empty or white space only.
bool visible(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const size_t b = blankAt(s, i);
    if (!b) return true;
    i += b;
  }
  return false;
}

// The API's id pattern, ^[a-zA-Z0-9_-]+$.
bool idChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
// A stored call id the API cannot take (another dialect's id, an empty one) is sent as
// 'h' and its bytes in hex: one to one, so two ids never collide, and the result's
// tool_use_id is mapped the same way.
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

bool imageMime(const std::string& m) {
  return m == "image/jpeg" || m == "image/png" || m == "image/gif" || m == "image/webp";
}

// A stored tool name the API cannot take (another engine's dotted name, an empty one) is
// sent with each other byte as '_', at most 64 bytes.
std::string wireName(const std::string& n) {
  std::string out = n.substr(0, 64);
  for (char& c : out)
    if (!idChar(c)) c = '_';
  return out.empty() ? std::string("_") : out;
}

// One JSON object the body writer accepts as it is (call arguments, signed thinking).
bool oneObject(const std::string& json) {
  size_t i = 0;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
  if (i >= json.size() || json[i] != '{') return false;
  std::string scratch;
  BodyWriter probe(scratch);
  probe.raw(json);
  return probe.status().ok();
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
};

struct Facts {
  std::vector<Row> rows;
  uint64_t thru = 0;                  // the newest s item's thru
  uint64_t pointer = 0;               // max(input pointer, newest x item's)
  std::vector<uint64_t> superseded;   // every x item's sup, sorted, unique
  std::vector<std::string> photos;    // the open turn's visible photo refs (its t item)
  size_t estimate = 0;                // body bytes to reserve once
  std::vector<uint32_t> omitted;      // turns the omission line stands for, oldest first
};

// Whether a stored item writes a block at all (a blank text or a broken meta item does not).
bool emitsBlock(const tx::Item& it, uint16_t dayStartMin) {
  switch (it.kind) {
    case tx::ItemKind::User: {
      char stamp[tx::kStampMaxBytes];
      for (const tx::Part& p : it.parts)
        if (p.kind == tx::PartKind::Image) return true;
      return visible(it.x) || tx::messageStamp(it.t, it.tzMinutes, dayStartMin, stamp, sizeof stamp).ok();
    }
    case tx::ItemKind::Assistant:
    case tx::ItemKind::DeviceNote:
      return visible(it.x);
    case tx::ItemKind::RoundMeta:
      return oneObject(it.x);
    default:
      return true;
  }
}

// What one item tells the request beyond its own blocks.
void noteFacts(const RenderInput& in, const tx::Item& it, Facts& f) {
  switch (it.kind) {
    case tx::ItemKind::PointerMove:
      f.pointer = std::max(f.pointer, it.stub);
      f.superseded.insert(f.superseded.end(), it.sup.begin(), it.sup.end());
      break;
    case tx::ItemKind::Digest:
      f.thru = std::max(f.thru, it.thru);
      break;
    case tx::ItemKind::TurnOpen:
      if (it.turn == in.turn->turn) f.photos = it.photos;
      break;
    default:
      break;
  }
  f.estimate += it.x.size() + it.args.size() + it.st.size() + 96;
  if (it.kind == tx::ItemKind::Result && !it.handle.empty()) f.estimate += it.bytes;
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
    row.emits = emitsBlock(it, in.turn->dayStartMin);
    noteFacts(in, it, f);
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

  bool used(const std::string& wid) const { return std::find(sent.begin(), sent.end(), wid) != sent.end(); }
  // Every call gets a wire id of its own: a stored id used again (a dialect that numbers its
  // calls per answer, an empty id) is told apart by the call's q.
  void call(Row& r, size_t row) {
    r.wid = wireId(r.id);
    while (used(r.wid)) r.wid += "-" + std::to_string(r.q);
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

// What is rendered, and the pairing: a call with its one result. Assistant items before the
// first user item after thru (a turn cut by compaction) are not sent: the request opens with
// the user's turn.
void chooseRows(const RenderInput& in, Facts& f) {
  Pairs pairs;
  bool userSeen = omissionNamed(in);   // the omission line opens the request
  for (size_t i = 0; i < f.rows.size(); ++i) {
    Row& r = f.rows[i];
    if (r.q == 0 || r.q <= f.thru || omittedTurn(f, r.turn) || !r.emits) continue;
    if (assistantKind(r.kind) && !userSeen) continue;
    if (userKind(r.kind)) r.keep = userSeen = true;
    else if (r.kind == tx::ItemKind::Assistant) r.keep = true;
    else if (r.kind == tx::ItemKind::RoundMeta) r.keep = r.turn == in.turn->turn && sendOpenTurnThinking(in);
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
  // The request ends with the user's turn (the API takes no assistant prefill): assistant
  // text or meta left last by a round whose calls were all left out is not sent.
  while (!order.empty() && assistantKind(f.rows[order.back()].kind)) order.pop_back();
  return order;
}

// ---- pass 2: the body -------------------------------------------------------------------

struct Mark {
  size_t closeAt = 0;   // the offset of the block's closing brace
  uint32_t turn = 0;
  bool system = false;
};

enum class Role : uint8_t { None, User, Assistant };

class Messages {
 public:
  Messages(BodyWriter& w, const std::string& body) : w_(w), body_(body) {}

  // Open the next block's message: a new one unless `join` and the last message has this
  // role and turn.
  void open(Role role, uint32_t turn, bool join) {
    if (join && role == role_ && turn == turn_ && role_ != Role::None) return;
    if (role_ != Role::None) w_.endArray().endObject();
    w_.beginObject().key("role").str(role == Role::User ? "user" : "assistant").key("content").beginArray();
    role_ = role;
    turn_ = turn;
    results_ = false;
  }
  BodyWriter& block(Role role, uint32_t turn, bool join) {
    open(role, turn, join);
    return w_.beginObject();
  }
  // Close the block; `cacheable` blocks can carry a point.
  void close(uint32_t turn, bool cacheable) {
    if (cacheable) marks_.push_back(Mark{body_.size(), turn, false});
    w_.endObject();
  }
  void finish() {
    if (role_ != Role::None) w_.endArray().endObject();
  }
  bool any() const { return !marks_.empty(); }   // a request with no message block is refused
  bool resultsOpen() const { return results_ && role_ == Role::User; }
  void setResults(bool on) { results_ = on; }
  const std::vector<Mark>& marks() const { return marks_; }

 private:
  BodyWriter& w_;
  const std::string& body_;
  Role role_ = Role::None;
  uint32_t turn_ = 0;
  bool results_ = false;
  std::vector<Mark> marks_;
};

struct Render {
  const RenderInput& in;
  const Facts& f;
  BodyWriter& w;
  std::string& body;
  BodySplices& splices;
  Messages& m;
  bool fileRef = false;   // a block names a provider file (the Files beta header)
};


// A text block; `join`: it may join the message before it.
void textBlock(Render& r, Role role, uint32_t turn, bool join, const std::string& text, Over over) {
  r.m.block(role, turn, join).key("type").str("text").key("text").str(text, over);
  r.m.close(turn, true);
  r.m.setResults(false);
}

// A photo of an owner message: pixels when the open turn shows it (by file id, else inline
// base64 spliced at send time), else its stub line. False when nothing was written (a
// part whose ref is not a photo ref has nothing to name).
bool photoBlock(Render& r, const tx::Item& u, const tx::Part& p, bool join) {
  const bool shown = std::find(r.f.photos.begin(), r.f.photos.end(), p.ref) != r.f.photos.end();
  PhotoBytes px;
  if (shown && r.in.photo && r.in.photo(p.ref, px).ok()) {
    if (!px.fileId.empty()) {
      r.m.block(Role::User, u.turn, join).key("type").str("image").key("source").beginObject();
      r.w.key("type").str("file").key("file_id").str(px.fileId).endObject();
      r.m.close(u.turn, true);
      r.fileRef = true;
      return true;
    }
    if (px.bytes && px.len && imageMime(px.mime)) {
      r.m.block(Role::User, u.turn, join).key("type").str("image").key("source").beginObject();
      r.w.key("type").str("base64").key("media_type").str(px.mime).key("data");
      r.w.beginStr().splice(px.bytes, px.len).endStr().endObject();
      r.m.close(u.turn, true);
      return true;
    }
  }
  if (p.ref.size() < 2 || p.ref[0] != 'p') return false;
  std::string when, stub;
  if (!tx::photoWhen(u.t, u.tzMinutes, when).ok()) when.clear();
  if (!tx::photoStub(p.ref.substr(1), when, p.note, stub).ok()) return false;   // its note kept (RB-H81)
  textBlock(r, Role::User, u.turn, join, stub, Over::Refuse);
  return true;
}

// An owner message opens a user message of its own: the stamped text, then its photos.
void ownerMessage(Render& r, const tx::Item& u) {
  char stamp[tx::kStampMaxBytes];
  const bool stamped = tx::messageStamp(u.t, u.tzMinutes, r.in.turn->dayStartMin, stamp, sizeof stamp).ok();
  bool first = true;
  if (stamped || visible(u.x)) {
    r.m.block(Role::User, u.turn, false).key("type").str("text").key("text").beginStr();
    if (stamped) r.w.part(stamp, std::strlen(stamp)).part(" ", 1);
    r.w.part(u.x).endStr();
    r.m.close(u.turn, true);
    r.m.setResults(false);
    first = false;
  }
  for (const tx::Part& p : u.parts)
    if (p.kind == tx::PartKind::Image && photoBlock(r, u, p, !first)) first = false;
}

// The whole text of a result: inline, or its bulk payload (the stub when it cannot be read).
std::string wholeText(const Render& r, const tx::Item& it) {
  if (it.handle.empty()) return it.x;
  std::string out;
  if (r.in.bulk && r.in.bulk(it.handle, out).ok()) return out;
  return it.st;
}

void resultBlock(Render& r, const tx::Item& it, const Row& row) {
  tx::ShowFacts sf;
  sf.pointer = r.f.pointer;
  sf.openTurn = r.in.turn->turn;
  sf.superseded = &r.f.superseded;
  sf.ladderStubThrough = r.in.ladder ? r.in.ladder->stubThrough : 0;
  tx::ShowAs as = tx::ShowAs::Stub;
  if (!tx::showResult(it, sf, as).ok()) as = tx::ShowAs::Stub;
  std::string text;
  if (as == tx::ShowAs::Whole) text = wholeText(r, it);
  else if (!it.st.empty()) text = it.st;
  else if (it.cls != tx::ResultClass::Outside) text = wholeText(r, it);   // no stub stored: never outside content
  r.m.block(Role::User, it.turn, r.m.resultsOpen()).key("type").str("tool_result").key("tool_use_id").str(row.wid);
  if (!text.empty()) r.w.key("content").str(text, Over::Clip);
  if (!it.ok && !text.empty()) r.w.key("is_error").boolean(true);   // the API refuses an empty error
  r.m.close(it.turn, true);
  r.m.setResults(true);
}

void callBlock(Render& r, const tx::Item& it, const Row& row) {
  r.m.block(Role::Assistant, it.turn, true).key("type").str("tool_use").key("id").str(row.wid);
  r.w.key("name").str(wireName(it.n)).key("input");
  const std::string& args = tx::shownArgs(it, r.in.turn->turn, r.in.ladder ? r.in.ladder->stubThrough : 0);   // RB-H81
  if (oneObject(args)) r.w.raw(args);
  else r.w.beginObject().endObject();   // a cut or broken call, a long-argument stub
  r.m.close(it.turn, true);
  r.m.setResults(false);
}

void itemBlocks(Render& r, const tx::Item& it, const Row& row) {
  switch (it.kind) {
    case tx::ItemKind::User:
      ownerMessage(r, it);
      return;
    case tx::ItemKind::Assistant:
      textBlock(r, Role::Assistant, it.turn, true, it.x, Over::Clip);   // blank text is not kept (emitsBlock)
      return;
    case tx::ItemKind::DeviceNote:
      textBlock(r, Role::User, it.turn, true, it.x, Over::Clip);
      return;
    case tx::ItemKind::RoundMeta:   // the open turn's signed thinking, as stored; never a point
      if (!oneObject(it.x)) return;
      r.m.open(Role::Assistant, it.turn, true);
      r.w.raw(it.x);
      r.m.setResults(false);
      return;
    case tx::ItemKind::Call:
      callBlock(r, it, row);
      return;
    case tx::ItemKind::Result:
      resultBlock(r, it, row);
      return;
    default:
      return;
  }
}

void omissionBlock(Render& r) {
  if (omissionNamed(r.in)) textBlock(r, Role::User, 0, false, r.in.ladder->omissionLine, Over::Refuse);
}

// The system blocks; the offsets of their two points.
void systemBlocks(BodyWriter& w, const std::string& body, const SystemBlocks& s, std::vector<size_t>& points) {
  const std::string* blocks[] = {&s.core, &s.notes, &s.digests};
  size_t closeAt[3] = {0, 0, 0};
  bool any = false;
  bool shown[3];
  for (int i = 0; i < 3; ++i) any = (shown[i] = visible(*blocks[i])) || any;
  if (!any) return;
  w.key("system").beginArray();
  for (int i = 0; i < 3; ++i) {
    if (!shown[i]) continue;
    w.beginObject().key("type").str("text").key("text").str(*blocks[i]);
    closeAt[i] = body.size();
    w.endObject();
  }
  w.endArray();
  const size_t stable = shown[1] ? closeAt[1] : closeAt[0];
  if (stable) points.push_back(stable);
  if (shown[2]) points.push_back(closeAt[2]);
}

Status toolsArray(BodyWriter& w, const RenderInput& in) {
  if (in.tools->empty()) return Status::success();
  w.key("tools").beginArray();
  for (const ToolSpec& t : *in.tools) {
    if (!oneObject(t.schemaJson)) return Status::fail(Err::InvalidArgument, "plain::anthropic.render.toolSchema");
    w.beginObject().key("name").str(t.name);
    if (!t.description.empty()) w.key("description").str(t.description);
    w.key("input_schema").raw(t.schemaJson).endObject();
  }
  w.endArray();
  w.key("tool_choice").beginObject().key("type").str(in.toolChoiceNone ? "none" : "auto").endObject();
  return Status::success();
}

// The two message points: the last block of the previous turn and the moving last block.
void messagePoints(const std::vector<Mark>& marks, uint32_t openTurn, std::vector<size_t>& points) {
  if (marks.empty()) return;
  for (size_t i = marks.size(); i-- > 0;)
    if (marks[i].turn < openTurn) {
      if (i + 1 != marks.size()) points.push_back(marks[i].closeAt);
      break;
    }
  points.push_back(marks.back().closeAt);
}

// Each point is the marker written before its block's closing brace; the photo splices
// after it move with the bytes.
void placePoints(std::vector<size_t> points, HttpRequest& out) {
  std::sort(points.begin(), points.end());
  points.erase(std::unique(points.begin(), points.end()), points.end());
  const size_t len = std::strlen(kCacheMark);
  for (size_t i = points.size(); i-- > 0;) {
    out.body.insert(points[i], kCacheMark, len);
    for (BodySplice& sp : out.splices)
      if (sp.at > points[i]) sp.at += len;
  }
}

// Where the request goes and its headers.
void coordinates(const RenderInput& in, bool fileRef, HttpRequest& out) {
  out.method = "POST";
  out.host = in.head->host.empty() ? kDefaultHost : in.head->host;
  out.port = in.head->port;
  out.tls = in.head->tls;
  out.path = (in.head->basePath.empty() ? std::string("/v1") : in.head->basePath) + "/messages";
  out.headers.emplace_back("content-type", "application/json");
  out.headers.emplace_back("anthropic-version", kVersion);
  if (fileRef) out.headers.emplace_back("anthropic-beta", kFilesBeta);
  const std::string key = in.head->key ? in.head->key() : std::string();
  if (key.empty() || !in.head->tls) return;   // a key never rides a cleartext socket
  if (in.head->routed) out.headers.emplace_back("authorization", std::string("Bearer") + ' ' + key);
  else out.headers.emplace_back("x-api-key", key);
}

size_t reserveFor(const RenderInput& in, const Facts& f) {
  size_t n = f.estimate + in.system->core.size() + in.system->notes.size() + in.system->digests.size() + 1024;
  for (const ToolSpec& t : *in.tools) n += t.name.size() + t.description.size() + t.schemaJson.size() + 64;
  return n;
}

Status renderBody(const RenderInput& in, HttpRequest& out) {
  Facts f;
  Status st = readFacts(in, *in.items, f);
  if (!st.ok()) return st;
  chooseOmitted(in, f);
  chooseRows(in, f);
  const std::vector<size_t> order = sendOrder(f);
  if (in.tools->empty())   // the API refuses tool blocks in a request without tools
    for (size_t idx : order)
      if (f.rows[idx].kind == tx::ItemKind::Call) return Status::fail(Err::InvalidArgument, "plain::anthropic.render.noTools");

  out.body.reserve(reserveFor(in, f));

  BodyWriter w(out.body, &out.splices);
  std::vector<size_t> points;
  w.beginObject().key("model").str(in.turn->model).key("max_tokens").unum(in.maxTokens);
  if (!in.head->effort.empty()) w.key("output_config").beginObject().key("effort").str(in.head->effort).endObject();
  systemBlocks(w, out.body, *in.system, points);
  if (!(st = toolsArray(w, in)).ok()) return st;

  w.key("messages").beginArray();
  Messages m(w, out.body);
  Render r{in, f, w, out.body, out.splices, m};
  omissionBlock(r);
  tx::Item it;
  for (size_t idx : order)
    if (in.items->get(idx, it).ok()) itemBlocks(r, it, f.rows[idx]);
  m.finish();
  w.endArray().endObject();
  if (!(st = w.status()).ok()) return st;
  if (!m.any()) return Status::fail(Err::InvalidArgument, "plain::anthropic.render.noMessages");

  messagePoints(m.marks(), in.turn->turn, points);
  placePoints(points, out);

  coordinates(in, r.fileRef, out);
  return Status::success();
}

// ---- the parse half (lane RB-W1b) ---------------------------------------------------------
//
// parse() reads one 2xx Messages answer with a strict JSON reader of its own (no JSON document:
// no 65,535 B string limit, no second copy of the body, every byte of a signed block kept as
// served) into a local ParsedStep that reaches `out` only once the whole body has been read and
// checked: a cut, malformed or error body fills nothing, so nothing half-read is acted on.
//
//   text              the text blocks, joined in order
//   tool_use          a call: id, name, its input object byte for byte (complete); an input that
//                     is not an object is a call that is never run (complete = false)
//   thinking          signed (a non-empty signature) or redacted (non-empty data): one opaque
//   redacted_thinking MetaPart, the block's bytes as served, before the call it preceded; a block
//                     a stored line cannot hold (tx::kMaxLineBytes) is not kept (wire.h MetaPart:
//                     the open turn's next request goes without it; the provider accepts that,
//                     evidence/LIVE-CONFORMANCE.md)
//   other block types are not ours to act on and are passed over.
// stop_reason: end_turn and stop_sequence end the turn, tool_use asks for the calls, max_tokens
// and model_context_window_exceeded are a cut (every call complete = false: its arguments may be
// cut), refusal keeps the text and drops every call (stop_details names why), anything else is
// Other. usage: input_tokens (Anthropic leaves cache reads and writes out of it), the cache split
// (read, write, the 1-hour part of the write), output_tokens, the served model.
constexpr uint8_t kParseDepth = 64;

class JsonReader {
 public:
  explicit JsonReader(const std::string& s) : s_(s) {}
  size_t at() const { return i_; }
  const std::string& body() const { return s_; }
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
  }
  bool peek(char c) {
    ws();
    return i_ < s_.size() && s_[i_] == c;
  }
  bool eat(char c) {
    if (!peek(c)) return false;
    ++i_;
    return true;
  }
  bool end() {
    ws();
    return i_ == s_.size();
  }

  // One string; its decoded text into `out` when given (escapes and surrogate pairs to UTF-8).
  bool str(std::string* out) {
    if (!eat('"')) return false;
    if (out) out->clear();
    for (;;) {
      if (i_ >= s_.size()) return false;
      const unsigned char c = (unsigned char)s_[i_];
      if (c == '"') {
        ++i_;
        return true;
      }
      if (c < 0x20) return false;   // a raw control byte: not JSON
      if (c != '\\') {
        const size_t from = i_;
        while (i_ < s_.size() && s_[i_] != '"' && s_[i_] != '\\' && (unsigned char)s_[i_] >= 0x20) ++i_;
        if (out) out->append(s_, from, i_ - from);
        continue;
      }
      if (++i_ >= s_.size()) return false;
      const char e = s_[i_++];
      if (e == 'u') {
        long code = 0;
        if (!hex4(code)) return false;
        if (code >= 0xD800 && code <= 0xDBFF) {   // a high surrogate needs its low half
          long low = 0;
          if (!(i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u')) return false;
          i_ += 2;
          if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
          return false;   // a lone low surrogate
        }
        if (out) putUtf8(*out, code);
        continue;
      }
      const char* kEsc = "\"\\/bfnrt";
      const char* kOut = "\"\\/\b\f\n\r\t";
      const char* hit = e ? std::strchr(kEsc, e) : nullptr;
      if (!hit) return false;
      if (out) out->push_back(kOut[hit - kEsc]);
    }
  }

  // A non-negative integer (saturating at UINT32_MAX) or null (0).
  bool count(uint32_t& out) {
    ws();
    out = 0;
    if (literal("null")) return true;
    const size_t from = i_;
    if (!skipNumber()) return false;
    uint64_t v = 0;
    for (size_t k = from; k < i_; ++k) {
      const char c = s_[k];
      if (c < '0' || c > '9') return false;   // negative, a fraction or an exponent: not a count
      v = v * 10 + (uint64_t)(c - '0');
      if (v > UINT32_MAX) v = UINT32_MAX;
    }
    out = (uint32_t)v;
    return true;
  }

  // Any one value, read without recursion (the device's stack never grows with the answer's
  // nesting): one bit per open container, object or array. peak() is the deepest nesting it met,
  // counted from where it started.
  bool skip() {
    uint64_t objects = 0;   // bit k: container k is an object
    uint8_t open = 0;
    peak_ = 0;
    for (;;) {
      ws();
      if (i_ >= s_.size()) return false;
      const char c = s_[i_];
      bool closed = false;   // a value just ended
      if (c == '{' || c == '[') {
        if (depth_ + open + 1 > kParseDepth) return false;
        ++i_;
        objects = (objects << 1) | (c == '{' ? 1u : 0u);
        if (++open > peak_) peak_ = open;
        if (eat(c == '{' ? '}' : ']')) {
          --open;
          objects >>= 1;
          closed = true;
        } else if (c == '{' && !(peek('"') && str(nullptr) && eat(':'))) {
          return false;
        }
      } else {
        const bool ok = c == '"' ? str(nullptr)
                        : (c == '-' || (c >= '0' && c <= '9')) ? skipNumber()
                                                                : (literal("true") || literal("false") || literal("null"));
        if (!ok) return false;
        closed = true;
      }
      while (closed) {   // after a value: the end, a comma and the next value, or a close
        if (!open) return true;
        const bool obj = objects & 1u;
        if (eat(',')) {
          if (obj && !(peek('"') && str(nullptr) && eat(':'))) return false;
          closed = false;
        } else if (eat(obj ? '}' : ']')) {
          --open;
          objects >>= 1;
        } else {
          return false;
        }
      }
    }
  }
  uint8_t peak() const { return peak_; }

  // An object: `onKey(key)` reads each value.
  template <typename F>
  bool object(F onKey) {
    if (!eat('{') || ++depth_ > kParseDepth) return false;
    std::string key;
    for (bool first = true; !eat('}'); first = false) {
      if (!first && !eat(',')) return false;
      if (!peek('"') || !str(&key) || !eat(':') || !onKey(key)) return false;
    }
    --depth_;
    return true;
  }
  // An array: `onItem()` reads each element.
  template <typename F>
  bool array(F onItem) {
    if (!eat('[') || ++depth_ > kParseDepth) return false;
    for (bool first = true; !eat(']'); first = false) {
      if (!first && !eat(',')) return false;
      if (!onItem()) return false;
    }
    --depth_;
    return true;
  }

 private:
  bool literal(const char* w) {
    const size_t n = std::strlen(w);
    if (s_.compare(i_, n, w) != 0) return false;
    i_ += n;
    return true;
  }
  bool digits() {
    const size_t d = i_;
    while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    return i_ > d;
  }
  bool skipNumber() {
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    const size_t intAt = i_;
    if (!digits()) return false;
    if (s_[intAt] == '0' && i_ - intAt > 1) return false;   // no leading zero
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      if (!digits()) return false;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (!digits()) return false;
    }
    return true;
  }
  bool hex4(long& code) {
    if (i_ + 4 > s_.size()) return false;
    code = 0;
    for (int k = 0; k < 4; ++k) {
      const char h = s_[i_++];
      const int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
      if (v < 0) return false;
      code = code * 16 + v;
    }
    return true;
  }
  static void putUtf8(std::string& o, long c) {
    if (c < 0x80) {
      o.push_back((char)c);
    } else if (c < 0x800) {
      o.push_back((char)(0xC0 | (c >> 6)));
      o.push_back((char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      o.push_back((char)(0xE0 | (c >> 12)));
      o.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
      o.push_back((char)(0x80 | (c & 0x3F)));
    } else {
      o.push_back((char)(0xF0 | (c >> 18)));
      o.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
      o.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
      o.push_back((char)(0x80 | (c & 0x3F)));
    }
  }

  const std::string& s_;
  size_t i_ = 0;
  uint8_t depth_ = 0;
  uint8_t peak_ = 0;
};

// One content block as read: its fields and where its bytes sit in the body.
struct Block {
  std::string type, text, id, name;
  size_t from = 0, to = 0;            // the block's bytes
  size_t inputFrom = 0, inputTo = 0;  // tool_use: the input's bytes when it is an object
  bool inputObject = false;
  bool inputRunnable = false;         // tool_use: an object the render can send back as it is
  bool thinkingText = false;          // thinking: its "thinking" is a string
  bool signature = false;             // a non-empty "signature" (thinking)
  bool data = false;                  // a non-empty "data" (redacted_thinking)
  uint32_t seen = 0;                  // the fields read (a field given twice is not one answer)
};

// A call's input is run only when the render can send it back as the model wrote it: one object,
// nested no deeper than the body writer takes (body::kMaxDepth), each of its own keys once.
bool runnableInput(const std::string& body, size_t from, size_t to, uint8_t peak) {
  if (peak > body::kMaxDepth) return false;
  const std::string input = body.substr(from, to - from);
  JsonReader j(input);
  std::vector<std::string> keys;
  return j.object([&j, &keys](const std::string& k) {
    if (std::find(keys.begin(), keys.end(), k) != keys.end()) return false;
    keys.push_back(k);
    return j.skip();
  });
}

// Each field of a block once; a known field of the wrong type fails the body.
bool blockField(JsonReader& j, const std::string& key, Block& b) {
  static const char* const kFields[] = {"type", "text", "id", "name", "input", "thinking", "signature", "data"};
  for (uint32_t k = 0; k < sizeof(kFields) / sizeof(kFields[0]); ++k) {
    if (key != kFields[k]) continue;
    if (b.seen & (1u << k)) return false;
    b.seen |= 1u << k;
    std::string scratch;
    switch (k) {
      case 0: return j.str(&b.type);
      case 1: return j.str(&b.text);
      case 2: return j.str(&b.id);
      case 3: return j.str(&b.name);
      case 4:
        b.inputObject = j.peek('{');
        b.inputFrom = j.at();
        if (!j.skip()) return false;
        b.inputTo = j.at();
        b.inputRunnable = b.inputObject && runnableInput(j.body(), b.inputFrom, b.inputTo, j.peak());
        return true;
      case 5:
        if (!j.peek('"')) return j.skip();   // read as not a thinking text below
        b.thinkingText = true;
        return j.str(nullptr);
      case 6:
        if (!j.str(&scratch)) return false;
        b.signature = !scratch.empty();
        return true;
      default:
        if (!j.str(&scratch)) return false;
        b.data = !scratch.empty();
        return true;
    }
  }
  return j.skip();
}

// Whether one opaque block fits the k item that stores it (the line limit, newline included).
bool fitsALine(const std::string& bytes) {
  struct Count final : tx::LineSink {
    void put(const char*, size_t) override {}
  } sink;
  tx::Item k;
  k.kind = tx::ItemKind::RoundMeta;
  k.q = 999999999999ULL;   // the widest q, turn, time and round a line holds
  k.turn = UINT32_MAX;
  k.t = 9999999999ULL;
  k.rd = 255;
  k.x = bytes;
  size_t n = 0;
  return tx::encodeLineTo(k, sink, n).ok();
}

struct Answer {
  std::vector<Block> blocks;
  std::string stop, refusal, model;
  nimbus::orch::TokenUsage usage;
  bool content = false, isMessage = false, notAssistant = false;
  uint32_t seen = 0;
};

bool usageField(JsonReader& j, const std::string& key, nimbus::orch::TokenUsage& u, uint32_t& seen) {
  static const char* const kFields[] = {"input_tokens", "output_tokens", "cache_read_input_tokens",
                                        "cache_creation_input_tokens", "cache_creation"};
  uint32_t* const into[] = {&u.promptTokens, &u.completionTokens, &u.cacheReadTokens, &u.cacheWriteTokens, nullptr};
  for (uint32_t k = 0; k < sizeof(kFields) / sizeof(kFields[0]); ++k) {
    if (key != kFields[k]) continue;
    if (seen & (1u << k)) return false;   // a count given twice is not one answer
    seen |= 1u << k;
    if (into[k]) return j.count(*into[k]);
    if (!j.peek('{')) return j.skip();
    bool once = false;
    return j.object([&j, &u, &once](const std::string& f) {
      if (f != "ephemeral_1h_input_tokens") return j.skip();
      if (once) return false;
      once = true;
      return j.count(u.cacheWrite1hTokens);
    });
  }
  return j.skip();
}

bool stopDetails(JsonReader& j, Answer& a) {
  if (!j.peek('{')) return j.skip();   // null on every stop but a refusal
  std::string category, explanation;
  const bool ok = j.object([&](const std::string& k) {
    if (k == "category" && j.peek('"')) return j.str(&category);
    if (k == "explanation" && j.peek('"')) return j.str(&explanation);
    return j.skip();
  });
  a.refusal = !explanation.empty() ? explanation : category;
  return ok;
}

bool topField(JsonReader& j, const std::string& key, Answer& a) {
  static const char* const kFields[] = {"type", "content", "stop_reason", "stop_details", "usage", "model", "role"};
  for (uint32_t k = 0; k < sizeof(kFields) / sizeof(kFields[0]); ++k) {
    if (key != kFields[k]) continue;
    if (a.seen & (1u << k)) return false;   // a field given twice is not one answer
    a.seen |= 1u << k;
    std::string type;
    switch (k) {
      case 0:
        if (!j.str(&type)) return false;
        a.isMessage = type == "message";
        return true;
      case 1:
        a.content = true;
        return j.array([&j, &a] {
          Block b;
          j.ws();
          b.from = j.at();
          if (!j.object([&j, &b](const std::string& f) { return blockField(j, f, b); })) return false;
          b.to = j.at();
          a.blocks.push_back(std::move(b));
          return true;
        });
      case 2: return j.peek('"') ? j.str(&a.stop) : j.skip();   // null: no stop named
      case 3: return stopDetails(j, a);
      case 4: {   // null: no usage given (every count 0)
        if (!j.peek('{')) return j.skip();
        uint32_t seen = 0;
        return j.object([&j, &a, &seen](const std::string& f) { return usageField(j, f, a.usage, seen); });
      }
      case 5: return j.peek('"') ? j.str(&a.model) : j.skip();
      default:
        if (!j.str(&type)) return false;
        a.notAssistant = type != "assistant";
        return true;
    }
  }
  return j.skip();
}

StopReason stopOf(const std::string& s) {
  if (s == "end_turn" || s == "stop_sequence") return StopReason::EndTurn;
  if (s == "tool_use") return StopReason::ToolUse;
  if (s == "max_tokens" || s == "model_context_window_exceeded") return StopReason::MaxTokens;
  if (s == "refusal") return StopReason::Refusal;
  return StopReason::Other;
}

// The step an answer that was read whole gives.
//
// Signed thinking is kept only while everything served before it is replayed byte for byte:
// the engine stores a round as [thinking before the first call] [one text] [call, thinking]...
// (plain_engine.cpp storeAnswer), and a signature binds every byte before its block (live,
// evidence/LIVE-CONFORMANCE.md). So the first block that would come back otherwise (a second
// text block, text after a call or after thinking that preceded it, text the engine stores as
// its stub, a block of a type not replayed, a thinking block not kept) ends the run: no later
// thinking block of the answer is kept. Leaving out a trailing run is accepted by the API.
void stepOf(const std::string& body, const Answer& a, ParsedStep& step) {
  step.stop = stopOf(a.stop);
  bool replayed = true;   // every block so far comes back byte for byte
  bool sawText = false;
  for (const Block& b : a.blocks) {
    if (b.type == "text") {
      replayed = replayed && !sawText && step.calls.empty();
      step.text += b.text;
      sawText = true;
      replayed = replayed && step.text.size() <= tx::kTier1AssistantText;
    } else if (b.type == "tool_use") {
      ToolCall c;
      c.id = b.id;
      c.name = b.name;
      // Only an answer that stops to call runs its calls: a cut one may hold cut arguments,
      // any other stop (none, a refusal, a pause, one not known) is not asking.
      c.complete = b.inputRunnable && step.stop == StopReason::ToolUse;
      c.argsJson = b.inputObject ? body.substr(b.inputFrom, b.inputTo - b.inputFrom) : std::string("{}");
      step.calls.push_back(std::move(c));
    } else if (b.type == "thinking" || b.type == "redacted_thinking") {
      const bool signedBlock = b.type == "thinking" ? b.thinkingText && b.signature : b.data;
      MetaPart m;
      m.beforeCall = (uint16_t)std::min<size_t>(step.calls.size(), UINT16_MAX);
      replayed = replayed && signedBlock && !(sawText && step.calls.empty());
      if (replayed) m.bytes = body.substr(b.from, b.to - b.from);
      replayed = replayed && fitsALine(m.bytes);
      if (replayed) step.meta.push_back(std::move(m));
    } else {
      replayed = false;   // a block the round does not store
    }
  }
  if (step.stop == StopReason::Refusal) {
    step.calls.clear();   // a refused answer runs nothing
    step.refusal = a.refusal.empty() ? a.stop : a.refusal;
  }
  step.usage = a.usage;
  step.usage.responses = 1;
  step.usage.servedModel = a.model;
  const uint64_t whole = (uint64_t)a.usage.promptTokens + a.usage.cacheReadTokens + a.usage.cacheWriteTokens;
  step.usage.maxRequestIn = (uint32_t)std::min<uint64_t>(whole, UINT32_MAX);
}

Status parseBody(const HttpResponse& in, ParsedStep& out) {
  if (in.status < 200 || in.status > 299) return Status::fail(Err::InvalidArgument, "plain::anthropic.parse.status");
  if (!nimbus::utf8Valid(in.body)) return Status::fail(Err::InvalidArgument, "plain::anthropic.parse.utf8");
  JsonReader j(in.body);
  Answer a;
  if (!j.object([&j, &a](const std::string& k) { return topField(j, k, a); }) || !j.end())
    return Status::fail(Err::InvalidArgument, "plain::anthropic.parse.json");
  if (!a.isMessage || !a.content || a.notAssistant)
    return Status::fail(Err::InvalidArgument, "plain::anthropic.parse.notAMessage");
  for (const Block& b : a.blocks)   // a call that cannot be answered is not an answer
    if (b.type == "tool_use" && (b.id.empty() || b.name.empty()))
      return Status::fail(Err::InvalidArgument, "plain::anthropic.parse.callWithoutIdOrName");
  ParsedStep step;
  stepOf(in.body, a, step);
  out = std::move(step);
  return Status::success();
}

class AnthropicWire final : public WireCodec {
 public:
  Dialect dialect() const override { return Dialect::AnthropicMessages; }

  Status render(const RenderInput& in, HttpRequest& out) const override {
    out = HttpRequest{};
    if (!in.turn || !in.head || !in.system || !in.tools || !in.items || in.turn->model.empty())
      return Status::fail(Err::InvalidArgument, "plain::anthropic.render");
    if (!in.maxTokens) return Status::fail(Err::InvalidArgument, "plain::anthropic.render.maxTokens");
    Status st;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
      st = renderBody(in, out);
    } catch (const std::bad_alloc&) {   // ruling 30: a refused allocation is a status
      st = Status::fail(Err::TooLarge, "plain::anthropic.render.memory");
    }
#else
    st = renderBody(in, out);
#endif
    if (!st.ok()) out = HttpRequest{};
    return st;
  }

  Status parse(const HttpResponse& in, ParsedStep& out) const override {
    out = ParsedStep{};
    Status st;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
      st = parseBody(in, out);
    } catch (const std::bad_alloc&) {   // ruling 30: a refused allocation is a status, nothing half-read
      st = Status::fail(Err::TooLarge, "plain::anthropic.parse.memory");
    }
#else
    st = parseBody(in, out);
#endif
    return st;   // parseBody fills `out` only once the whole body was read
  }
};

}  // namespace

const WireCodec& anthropicWire() {
  static const AnthropicWire wire;
  return wire;
}

}  // namespace plain
}  // namespace agent
