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
// The ladder (exchange.h): RenderInput::stubPointer and the newest x item's pointer
// (rung 1 and the stored moves), ladderStubThrough (rung 2), omitTurns (rung 3: the
// oldest whole turns after thru render as tx::omissionLine).
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

bool visible(const std::string& s) {
  for (char c : s)
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return true;
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
  std::string id;          // c and r: the stored call id
  std::string wid;         // c: the id as sent
};

struct Facts {
  std::vector<Row> rows;
  uint64_t thru = 0;                  // the newest s item's thru
  uint64_t pointer = 0;               // max(input pointer, newest x item's)
  std::vector<uint64_t> superseded;   // every x item's sup, sorted, unique
  std::vector<std::string> photos;    // the open turn's visible photo refs (its t item)
  int16_t tz = 0;                     // the newest owner message's zone (the omission dates)
  size_t estimate = 0;                // body bytes to reserve once
  std::vector<uint32_t> omitted;      // turns the omission line stands for, oldest first
  uint64_t omitFirstAt = 0, omitLastAt = 0;
};

Status readFacts(const RenderInput& in, const tx::TxArena& arena, Facts& f) {
  const size_t n = arena.count();
  f.rows.reserve(n);
  f.pointer = in.stubPointer;
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
    switch (it.kind) {
      case tx::ItemKind::Call:
      case tx::ItemKind::Result:
        row.id = it.id;
        break;
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
      case tx::ItemKind::User:
        f.tz = it.tzMinutes;
        break;
      default:
        break;
    }
    f.estimate += it.x.size() + it.args.size() + it.st.size() + 96;
    if (it.kind == tx::ItemKind::Result && !it.handle.empty()) f.estimate += it.bytes;
    f.rows.push_back(std::move(row));
  }
  std::sort(f.superseded.begin(), f.superseded.end());
  f.superseded.erase(std::unique(f.superseded.begin(), f.superseded.end()), f.superseded.end());
  return Status::success();
}

// Rung 3: the oldest `omitTurns` whole turns after thru, never the open turn.
void chooseOmitted(const RenderInput& in, Facts& f) {
  if (!in.omitTurns) return;
  for (const Row& r : f.rows) {
    if (r.q <= f.thru || r.q == 0) continue;
    if (r.turn == in.turn->turn) break;
    if (std::find(f.omitted.begin(), f.omitted.end(), r.turn) == f.omitted.end()) {
      if (f.omitted.size() == in.omitTurns) break;
      f.omitted.push_back(r.turn);
    }
    if (r.t && !f.omitFirstAt) f.omitFirstAt = r.t;
    if (r.t) f.omitLastAt = r.t;
  }
}

bool omittedTurn(const Facts& f, uint32_t turn) {
  return std::find(f.omitted.begin(), f.omitted.end(), turn) != f.omitted.end();
}

// The calls of the transcript by stored id, in q order: which row, answered yet.
struct Pairs {
  struct Open {
    std::string id;
    size_t row;
    bool answered;
  };
  std::vector<Open> calls;
  std::vector<std::string> sent;   // wire ids already used

  Open* find(const std::string& id) {
    for (Open& c : calls)
      if (c.id == id) return &c;
    return nullptr;
  }
  // A call is sent once per wire id.
  void call(Row& r, size_t row) {
    r.wid = wireId(r.id);
    if (std::find(sent.begin(), sent.end(), r.wid) != sent.end()) return;
    sent.push_back(r.wid);
    calls.push_back(Open{r.id, row, false});
  }
  // A result is sent when it is the first one after its call.
  void result(Row& r, std::vector<Row>& rows) {
    Open* c = find(r.id);
    if (!c || c->answered) return;
    c->answered = true;
    r.keep = true;
    r.wid = rows[c->row].wid;
  }
};

// What is rendered, and the pairing: a call with its first result, each id once.
void chooseRows(const RenderInput& in, Facts& f) {
  Pairs pairs;
  for (size_t i = 0; i < f.rows.size(); ++i) {
    Row& r = f.rows[i];
    if (r.q == 0 || r.q <= f.thru || omittedTurn(f, r.turn)) continue;
    if (r.kind == tx::ItemKind::User || r.kind == tx::ItemKind::Assistant || r.kind == tx::ItemKind::DeviceNote)
      r.keep = true;
    else if (r.kind == tx::ItemKind::RoundMeta) r.keep = r.turn == in.turn->turn;
    else if (r.kind == tx::ItemKind::Call) pairs.call(r, i);
    else if (r.kind == tx::ItemKind::Result) pairs.result(r, f.rows);
  }
  for (const Pairs::Open& c : pairs.calls) f.rows[c.row].keep = c.answered;
}

bool assistantKind(tx::ItemKind k) {
  return k == tx::ItemKind::Assistant || k == tx::ItemKind::RoundMeta || k == tx::ItemKind::Call;
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
    said_ = false;
  }
  BodyWriter& block(Role role, uint32_t turn, bool join) {
    open(role, turn, join);
    return w_.beginObject();
  }
  // Close the block; `cacheable` blocks can carry a point.
  void close(uint32_t turn, bool cacheable) {
    if (cacheable) marks_.push_back(Mark{body_.size(), turn, false});
    said_ = true;
    w_.endObject();
  }
  void finish() {
    if (role_ != Role::None) w_.endArray().endObject();
  }
  bool any() const { return !marks_.empty(); }   // a request with no message block is refused
  bool resultsOpen() const { return results_ && role_ == Role::User; }
  // The open message is the assistant's for `turn` and holds a block that is not thinking.
  bool assistantSaid(uint32_t turn) const { return role_ == Role::Assistant && turn_ == turn && said_; }
  void setResults(bool on) { results_ = on; }
  const std::vector<Mark>& marks() const { return marks_; }

 private:
  BodyWriter& w_;
  const std::string& body_;
  Role role_ = Role::None;
  uint32_t turn_ = 0;
  bool results_ = false;
  bool said_ = false;
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
  if (!tx::photoStub(p.ref.substr(1), when, stub).ok()) return false;
  textBlock(r, Role::User, u.turn, join, stub, Over::Refuse);
  return true;
}

// An owner message opens a user message of its own: the stamped text, then its photos.
void ownerMessage(Render& r, const tx::Item& u) {
  char stamp[tx::kStampMaxBytes];
  const bool stamped = tx::messageStamp(u.t, u.tzMinutes, stamp, sizeof stamp).ok();
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
  sf.ladderStubThrough = r.in.ladderStubThrough;
  tx::ShowAs as = tx::ShowAs::Stub;
  if (!tx::showResult(it, sf, as).ok()) as = tx::ShowAs::Stub;
  std::string text;
  if (as == tx::ShowAs::Whole) text = wholeText(r, it);
  else if (!it.st.empty()) text = it.st;
  else if (it.cls != tx::ResultClass::Outside) text = wholeText(r, it);   // no stub stored: never outside content
  r.m.block(Role::User, it.turn, r.m.resultsOpen()).key("type").str("tool_result").key("tool_use_id").str(row.wid);
  if (!text.empty()) r.w.key("content").str(text, Over::Clip);
  if (!it.ok) r.w.key("is_error").boolean(true);
  r.m.close(it.turn, true);
  r.m.setResults(true);
}

void callBlock(Render& r, const tx::Item& it, const Row& row) {
  r.m.block(Role::Assistant, it.turn, true).key("type").str("tool_use").key("id").str(row.wid);
  r.w.key("name").str(it.n).key("input");
  if (oneObject(it.args)) r.w.raw(it.args);
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
      if (visible(it.x)) textBlock(r, Role::Assistant, it.turn, true, it.x, Over::Clip);
      return;
    case tx::ItemKind::DeviceNote:
      if (visible(it.x)) textBlock(r, Role::User, it.turn, true, it.x, Over::Clip);
      return;
    case tx::ItemKind::RoundMeta:   // the open turn's signed thinking, as stored; never a point
      // thinking opens its round's message: after a round whose calls were all left out
      // (no result stored) it would follow that round's blocks, and is dropped
      if (!oneObject(it.x) || r.m.assistantSaid(it.turn)) return;
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
  if (r.f.omitted.empty()) return;
  std::string dates, line;
  if (!tx::omissionDates(r.f.omitFirstAt, r.f.omitLastAt, r.f.tz, dates).ok()) dates.clear();
  if (!tx::omissionLine((uint32_t)r.f.omitted.size(), dates, line).ok()) return;
  textBlock(r, Role::User, 0, false, line, Over::Refuse);
}

// The system blocks; the offsets of their two points.
void systemBlocks(BodyWriter& w, const std::string& body, const SystemBlocks& s, std::vector<size_t>& points) {
  const std::string* blocks[] = {&s.core, &s.notes, &s.digests};
  size_t closeAt[3] = {0, 0, 0};
  bool any = false;
  for (const std::string* b : blocks) any = any || !b->empty();
  if (!any) return;
  w.key("system").beginArray();
  for (int i = 0; i < 3; ++i) {
    if (blocks[i]->empty()) continue;
    w.beginObject().key("type").str("text").key("text").str(*blocks[i]);
    closeAt[i] = body.size();
    w.endObject();
  }
  w.endArray();
  const size_t stable = !s.notes.empty() ? closeAt[1] : closeAt[0];
  if (stable) points.push_back(stable);
  if (!s.digests.empty()) points.push_back(closeAt[2]);
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

  // Lane RB-W1b writes the parse half.
  Status parse(const HttpResponse&, ParsedStep& out) const override {
    out = ParsedStep{};
    return Status::pending("plain::anthropic.parse");
  }
};

}  // namespace

const WireCodec& anthropicWire() {
  static const AnthropicWire wire;
  return wire;
}

}  // namespace plain
}  // namespace agent
