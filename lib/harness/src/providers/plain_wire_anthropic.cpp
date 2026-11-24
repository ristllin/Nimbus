// plain_wire_anthropic.cpp - the Anthropic Messages wire of engine 2 (PRD section 1,
// "Request on each wire"). Render half: lane RB-W1a. Parse half: lane RB-W1b.
// Lane E2-R2b: what a request carries (the items, pairing, send order, stubs, photos, the
// ladder, the cache marks below) is decided by the shared planner (plain_plan.h); this file
// spells its Plan as Messages JSON and reads Messages answers (plain_json_reader.h).
//
// render() is a pure function of RenderInput: the same transcript and facts give the
// same bytes on every call (the exchange renders two or more times per request). It
// writes straight into the request body through the checked body writer (plain_body.h):
//
//   {"model", "max_tokens", "output_config":{"effort", "format"} (each when set; the format is
//    RenderInput::answerSchema as a json_schema, lane E2-CMP),
//    "system":[core][skills index + pinned notes][digests + recent files],
//    "tools":[{"name","description","input_schema"}], "tool_choice":{"type":"auto"|"none"},
//    "messages":[...]}
// Lane E2-P-CONN2: on the owner's turn with an MCP server enabled for Anthropic (plain_plan.h
// providerTools), tools[] also holds one {"type":"mcp_toolset","mcp_server_name"} per server and
// "mcp_servers":[...] follows tool_choice, with the "anthropic-beta: mcp-client-2025-11-20" header
// (https://platform.claude.com/docs/en/agents-and-tools/mcp-connector, read 24 Nov 2026). With no
// such server nothing of it is written: the bytes are the ones before.
//
// Messages, from the stored items after thru (the digests cover the rest), in q order:
//   u  a user message of its own: one text block '[Tue 17 Nov 2026 13:02] <text>', then
//      each photo: an image by file id or inline base64 when the open turn's t item lists
//      it as visible (tx::visiblePhotos decided that at turn open), else its line
//      (tx::photoStub: shown earlier, not attached to this request) as a text block;
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
// RenderInput::longCache (lane E2-CACHE): the first three carry "ttl":"1h" and the moving
// last block keeps the 5-minute lifetime. The API wants a longer lifetime before a shorter
// one in request order (tools, system, messages): the moving point is always the last.
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
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nimbus/harness/plain/route.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/utf8.h"
#include "plain_body.h"
#include "plain_json_reader.h"
#include "plain_plan.h"

namespace agent {
namespace plain {

namespace {

namespace tx = nimbus::orch::tx;
using body::BodyWriter;
using body::Over;

constexpr const char* kDefaultHost = "api.anthropic.com";
constexpr const char* kVersion = "2023-06-01";
constexpr const char* kFilesBeta = "files-api-2025-04-14";
constexpr const char* kMcpBeta = "mcp-client-2025-11-20";   // lane E2-P-CONN2
constexpr const char* kCacheMark = ",\"cache_control\":{\"type\":\"ephemeral\"}";
constexpr const char* kCacheMarkLong = ",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}";

// A cache point: the offset its marker goes in front of, and whether it is a stable one (the
// 1-hour lifetime under RenderInput::longCache).
struct Point {
  size_t at = 0;
  bool stable = false;
};

// ---- small rules ---------------------------------------------------------------------

// A stored tool name the API cannot take (another engine's dotted name, an empty one) is
// sent with each byte outside ^[a-zA-Z0-9_-]$ as '_', at most 64 bytes.
std::string wireName(const std::string& n) {
  std::string out = n.substr(0, 64);
  for (char& c : out)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
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

// ---- the body: a Plan in Messages JSON ------------------------------------------------------

enum class Role : uint8_t { None, User, Assistant };

class Messages {
 public:
  explicit Messages(BodyWriter& w) : w_(w) {}

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
  void finish() {
    if (role_ != Role::None) w_.endArray().endObject();
  }
  bool resultsOpen() const { return results_ && role_ == Role::User; }
  void setResults(bool on) { results_ = on; }

 private:
  BodyWriter& w_;
  Role role_ = Role::None;
  uint32_t turn_ = 0;
  bool results_ = false;
};

struct Render {
  const RenderInput& in;
  const plan::Plan& p;
  BodyWriter& w;
  std::string& body;
  Messages& m;
  std::vector<Point>& points;
  bool fileRef = false;   // a block names a provider file (the Files beta header)
};

// The text a piece names (its item's field or the plan's line).
const std::string& textOf(const Render& r, const plan::Piece& pc, const tx::Item& it) {
  switch (pc.src) {
    case plan::Src::X: return it.x;
    case plan::Src::St: return it.st;
    case plan::Src::Args: return it.args;
    default: return r.p.lines[pc.line];
  }
}

// Close the block; the plan's marks become cache points before its closing brace.
void close(Render& r, const plan::Piece& pc) {
  if (pc.flags & plan::kMarkStable) r.points.push_back(Point{r.body.size(), true});
  if (pc.flags & plan::kMarkMoving) r.points.push_back(Point{r.body.size(), false});
  r.w.endObject();
}

// A text block; `join`: it may join the message before it.
void textBlock(Render& r, const plan::Piece& pc, Role role, bool join, const std::string& text, Over over) {
  r.m.block(role, pc.turn, join).key("type").str("text").key("text").str(text, over);
  close(r, pc);
  r.m.setResults(false);
}

// An image source: by file id, else inline base64 spliced at send time.
void imageSource(Render& r, const plan::Photo& ph) {
  r.w.key("type").str("image").key("source").beginObject();
  if (!ph.bytes) {
    r.w.key("type").str("file").key("file_id").str(r.p.lines[ph.text]).endObject();
    r.fileRef = true;
    return;
  }
  r.w.key("type").str("base64").key("media_type").str(r.p.lines[ph.text]).key("data");
  r.w.beginStr().splice(ph.bytes, ph.len).endStr().endObject();
}

void ownerText(Render& r, const plan::Piece& pc, const tx::Item& u) {
  r.m.block(Role::User, pc.turn, false).key("type").str("text").key("text").beginStr();
  if (pc.flags & plan::kStamp) r.w.part(r.p.lines[pc.line]).part(" ", 1);
  r.w.part(u.x).endStr();
  close(r, pc);
  r.m.setResults(false);
}

void photoBlock(Render& r, const plan::Piece& pc) {
  r.m.block(Role::User, pc.turn, !(pc.flags & plan::kOpens));
  imageSource(r, r.p.photos[pc.photo]);
  close(r, pc);
}

void resultBlock(Render& r, const plan::Piece& pc, const tx::Item& it) {
  r.m.block(Role::User, pc.turn, r.m.resultsOpen()).key("type").str("tool_result").key("tool_use_id").str(r.p.ids[pc.id]);
  if (pc.flags & plan::kViewed) {   // its line, then the photo it views
    r.w.key("content").beginArray().beginObject().key("type").str("text").key("text").str(textOf(r, pc, it), Over::Clip);
    r.w.endObject().beginObject();
    imageSource(r, r.p.photos[pc.photo]);
    r.w.endObject().endArray();
  } else if (pc.src != plan::Src::None) {
    r.w.key("content").str(textOf(r, pc, it), Over::Clip);
  }
  if (pc.flags & plan::kError) r.w.key("is_error").boolean(true);
  close(r, pc);
  r.m.setResults(true);
}

void callBlock(Render& r, const plan::Piece& pc, const tx::Item& it) {
  r.m.block(Role::Assistant, pc.turn, true).key("type").str("tool_use").key("id").str(r.p.ids[pc.id]);
  r.w.key("name").str(wireName(it.n)).key("input");
  if (pc.src != plan::Src::None) r.w.raw(textOf(r, pc, it));
  else r.w.beginObject().endObject();
  close(r, pc);
  r.m.setResults(false);
}

void pieceBlocks(Render& r, const plan::Piece& pc, const tx::Item& it) {
  switch (pc.kind) {
    case plan::PieceKind::OwnerText:
      return ownerText(r, pc, it);
    case plan::PieceKind::Photo:
      return photoBlock(r, pc);
    case plan::PieceKind::Line:
      return textBlock(r, pc, Role::User, !(pc.flags & plan::kOpens), r.p.lines[pc.line], Over::Refuse);
    case plan::PieceKind::AssistantText:
      return textBlock(r, pc, Role::Assistant, true, it.x, Over::Clip);
    case plan::PieceKind::DeviceNote:
      return textBlock(r, pc, Role::User, true, it.x, Over::Clip);
    case plan::PieceKind::Meta:
      r.m.open(Role::Assistant, pc.turn, true);
      r.w.raw(it.x);
      r.m.setResults(false);
      return;
    case plan::PieceKind::Call:
      return callBlock(r, pc, it);
    case plan::PieceKind::Result:
      return resultBlock(r, pc, it);
  }
}

// The system blocks the plan sends; their marks as cache points.
void systemBlocks(BodyWriter& w, const std::string& body, const SystemBlocks& s, const plan::SystemPlan& sp,
                  std::vector<Point>& points) {
  if (!sp.shown[0] && !sp.shown[1] && !sp.shown[2]) return;
  const std::string* blocks[] = {&s.core, &s.notes, &s.digests};
  w.key("system").beginArray();
  for (int i = 0; i < 3; ++i) {
    if (!sp.shown[i]) continue;
    w.beginObject().key("type").str("text").key("text").str(*blocks[i]);
    if (i == sp.stableAfter || (i == 2 && sp.digestsMark)) points.push_back(Point{body.size(), true});
    w.endObject();
  }
  w.endArray();
}

// The effort and the answer's schema, each when set (lane E2-CMP: the summarizer's changes).
void outputConfig(BodyWriter& w, const RenderInput& in) {
  const bool effort = !in.head->effort.empty(), schema = in.answerSchema && *in.answerSchema;
  if (!effort && !schema) return;
  w.key("output_config").beginObject();
  if (effort) w.key("effort").str(in.head->effort);
  if (schema) w.key("format").beginObject().key("type").str("json_schema").key("schema").raw(in.answerSchema).endObject();
  w.endObject();
}

Status toolsArray(BodyWriter& w, const RenderInput& in, const plan::ProviderTools& pt) {
  if (in.tools->empty()) return Status::success();
  w.key("tools").beginArray();
  for (const ToolSpec& t : *in.tools) {
    if (!oneObject(t.schemaJson)) return Status::fail(Err::InvalidArgument, "plain::anthropic.render.toolSchema");
    w.beginObject().key("name").str(t.name);
    if (!t.description.empty()) w.key("description").str(t.description);
    w.key("input_schema").raw(t.schemaJson).endObject();
  }
  for (const std::string& t : pt.tools) w.raw(t);   // the toolsets (lane E2-P-CONN2)
  w.endArray();
  w.key("tool_choice").beginObject().key("type").str(in.toolChoiceNone ? "none" : "auto").endObject();
  if (!pt.servers.empty()) {
    w.key("mcp_servers").beginArray();
    for (const std::string& sv : pt.servers) w.raw(sv);
    w.endArray();
  }
  return Status::success();
}

// Each point is the marker written before its block's closing brace; the photo splices
// after it move with the bytes. Two points on one block are one (the stable lifetime wins).
void placePoints(std::vector<Point> points, bool longCache, HttpRequest& out) {
  std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
    return a.at != b.at ? a.at < b.at : a.stable > b.stable;
  });
  points.erase(std::unique(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.at == b.at; }),
               points.end());
  for (size_t i = points.size(); i-- > 0;) {
    const char* mark = longCache && points[i].stable ? kCacheMarkLong : kCacheMark;
    const size_t len = std::strlen(mark);
    out.body.insert(points[i].at, mark, len);
    for (BodySplice& sp : out.splices)
      if (sp.at > points[i].at) sp.at += len;
  }
}

// Where the request goes and its headers: the route's path, version and auth style (lane E2-R4a);
// with no route, a direct head's defaults as before.
// The betas a request needs, comma-separated in one header: the Files API for a file source, the MCP
// connector for mcp_servers (lane E2-P-CONN2). Neither: no header.
void betaHeader(bool fileRef, bool mcp, HttpRequest& out) {
  if (!fileRef && !mcp) return;
  out.headers.emplace_back("anthropic-beta", fileRef && mcp ? std::string(kFilesBeta) + "," + kMcpBeta
                                                            : std::string(fileRef ? kFilesBeta : kMcpBeta));
}

void coordinates(const RenderInput& in, bool fileRef, bool mcp, HttpRequest& out) {
  const Route* r = in.route;
  out.method = "POST";
  out.host = in.head->host.empty() ? kDefaultHost : in.head->host;
  out.port = in.head->port;
  out.tls = in.head->tls;
  out.path = r ? r->path : (in.head->basePath.empty() ? std::string("/v1") : in.head->basePath) + "/messages";
  out.headers.emplace_back("content-type", "application/json");
  out.headers.emplace_back("anthropic-version", r && *r->version ? r->version : kVersion);
  betaHeader(fileRef, mcp, out);
  const std::string key = in.head->key ? in.head->key() : std::string();
  if (key.empty() || !in.head->tls) return;   // a key never rides a cleartext socket
  const bool bearer = r ? r->auth == nimbus::orch::caps::Auth::Bearer : in.head->routed;
  if (bearer) out.headers.emplace_back("authorization", std::string("Bearer") + ' ' + key);
  else if (!r || r->auth == nimbus::orch::caps::Auth::XApiKey) out.headers.emplace_back("x-api-key", key);
}

size_t reserveFor(const RenderInput& in, const plan::Plan& p, const plan::ProviderTools& pt) {
  size_t n = p.estimate + in.system->core.size() + in.system->notes.size() + in.system->digests.size() + 1024 + pt.bytes();
  for (const ToolSpec& t : *in.tools) n += t.name.size() + t.description.size() + t.schemaJson.size() + 64;
  return n;
}

// The messages, piece by piece in the plan's order (a piece's item fetched once).
void messagesArray(Render& r) {
  r.w.key("messages").beginArray();
  tx::Item it;
  size_t fetched = SIZE_MAX;
  for (const plan::Piece& pc : r.p.pieces) {
    const bool ownText = pc.kind == plan::PieceKind::Line || pc.kind == plan::PieceKind::Photo;
    if (!ownText && pc.item != fetched) {
      if (!r.in.items->get(pc.item, it).ok()) continue;
      fetched = pc.item;
    }
    pieceBlocks(r, pc, it);
  }
  r.m.finish();
  r.w.endArray();
}

Status renderBody(const RenderInput& in, HttpRequest& out) {
  plan::Plan p;
  Status st = plan::plan(in, oneObject, p);
  if (!st.ok()) return st;
  if (in.tools->empty())   // the API refuses tool blocks in a request without tools
    for (const plan::Piece& pc : p.pieces)
      if (pc.kind == plan::PieceKind::Call) return Status::fail(Err::InvalidArgument, "plain::anthropic.render.noTools");

  plan::ProviderTools pt;
  plan::providerTools(in, pt);
  out.body.reserve(reserveFor(in, p, pt));

  BodyWriter w(out.body, &out.splices);
  std::vector<Point> points;
  // The route's model: a routed selector's bare id (the router prices it); else the turn's.
  w.beginObject().key("model").str(in.route ? in.route->model : in.turn->model).key("max_tokens").unum(in.maxTokens);
  outputConfig(w, in);
  systemBlocks(w, out.body, *in.system, p.system, points);
  if (!(st = toolsArray(w, in, pt)).ok()) return st;

  Messages m(w);
  Render r{in, p, w, out.body, m, points};
  messagesArray(r);
  w.endObject();
  if (!(st = w.status()).ok()) return st;
  if (!p.anyBlock) return Status::fail(Err::InvalidArgument, "plain::anthropic.render.noMessages");

  placePoints(points, in.longCache, out);
  coordinates(in, r.fileRef, !pt.servers.empty(), out);
  return Status::success();
}

// ---- the parse half (lane RB-W1b) ---------------------------------------------------------
//
// parse() reads one 2xx Messages answer with the strict JSON reader (plain_json_reader.h; no JSON document:
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
//   mcp_tool_use      a provider-side connector call and its answer (lane E2-P-CONN2): not stored
//   mcp_tool_result   (the model's text after them is); a result marks the step outside content
//   other block types are not ours to act on and are passed over.
// stop_reason: end_turn and stop_sequence end the turn, tool_use asks for the calls, max_tokens is a
// cut and model_context_window_exceeded a full window (lane E2-R4a: StopReason::Context; every call
// complete = false either way: its arguments may be cut), refusal keeps the text and drops every call (stop_details names why), anything else is
// Other. usage: input_tokens (Anthropic leaves cache reads and writes out of it), the cache split
// (read, write, the 1-hour part of the write), output_tokens, the served model.
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

// Each field of a block once (fieldOnce, plain_json_reader.h); a known field of the wrong type
// fails the body.
bool inputField(JsonReader& j, Block& b) {
  b.inputObject = j.peek('{');
  b.inputFrom = j.at();
  if (!j.skip()) return false;
  b.inputTo = j.at();
  b.inputRunnable = b.inputObject && runnableInput(j.body(), b.inputFrom, b.inputTo, j.peak());
  return true;
}

// A string field whose only fact is whether it is empty.
bool nonEmpty(JsonReader& j, bool& out) {
  std::string scratch;
  if (!j.str(&scratch)) return false;
  out = !scratch.empty();
  return true;
}

bool blockField(JsonReader& j, const std::string& key, Block& b) {
  static const char* const kFields[] = {"type", "text", "id", "name", "input", "thinking", "signature", "data"};
  uint32_t k = 0;
  if (!fieldOnce(key, kFields, 8, b.seen, k)) return false;
  switch (k) {
    case 0: return j.str(&b.type);
    case 1: return j.str(&b.text);
    case 2: return j.str(&b.id);
    case 3: return j.str(&b.name);
    case 4: return inputField(j, b);
    case 5:
      if (!j.peek('"')) return j.skip();   // read as not a thinking text below
      b.thinkingText = true;
      return j.str(nullptr);
    case 6: return nonEmpty(j, b.signature);
    case 7: return nonEmpty(j, b.data);
    default: return j.skip();
  }
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

bool contentField(JsonReader& j, Answer& a) {
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
}

bool usageObject(JsonReader& j, Answer& a) {
  if (!j.peek('{')) return j.skip();   // null: no usage given (every count 0)
  uint32_t seen = 0;
  return j.object([&j, &a, &seen](const std::string& f) { return usageField(j, f, a.usage, seen); });
}

// A string field compared with the one value it must hold; `same` says whether it did.
bool expect(JsonReader& j, const char* want, bool& same) {
  std::string v;
  if (!j.str(&v)) return false;
  same = v == want;
  return true;
}

bool topField(JsonReader& j, const std::string& key, Answer& a) {
  static const char* const kFields[] = {"type", "content", "stop_reason", "stop_details", "usage", "model", "role"};
  uint32_t k = 0;
  if (!fieldOnce(key, kFields, 7, a.seen, k)) return false;
  bool assistant = true;
  switch (k) {
    case 0: return expect(j, "message", a.isMessage);
    case 1: return contentField(j, a);
    case 2: return j.peek('"') ? j.str(&a.stop) : j.skip();   // null: no stop named
    case 3: return stopDetails(j, a);
    case 4: return usageObject(j, a);
    case 5: return j.peek('"') ? j.str(&a.model) : j.skip();
    case 6:
      if (!expect(j, "assistant", assistant)) return false;
      a.notAssistant = !assistant;
      return true;
    default: return j.skip();
  }
}

StopReason stopOf(const std::string& s) {
  if (s == "end_turn" || s == "stop_sequence") return StopReason::EndTurn;
  if (s == "tool_use") return StopReason::ToolUse;
  if (s == "max_tokens") return StopReason::MaxTokens;
  if (s == "model_context_window_exceeded") return StopReason::Context;
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
struct Replay {
  bool replayed = true;   // every block so far comes back byte for byte
  bool sawText = false;
};

void textOf(const Block& b, Replay& r, ParsedStep& step) {
  r.replayed = r.replayed && !r.sawText && step.calls.empty();
  step.text += b.text;
  r.sawText = true;
  r.replayed = r.replayed && step.text.size() <= tx::kTier1AssistantText;
}

void callOf(const std::string& body, const Block& b, ParsedStep& step) {
  ToolCall c;
  c.id = b.id;
  c.name = b.name;
  // Only an answer that stops to call runs its calls: a cut one may hold cut arguments, any
  // other stop (none, a refusal, a pause, one not known) is not asking.
  c.complete = b.inputRunnable && step.stop == StopReason::ToolUse;
  c.argsJson = b.inputObject ? body.substr(b.inputFrom, b.inputTo - b.inputFrom) : std::string("{}");
  step.calls.push_back(std::move(c));
}

void thinkingOf(const std::string& body, const Block& b, Replay& r, ParsedStep& step) {
  const bool signedBlock = b.type == "thinking" ? b.thinkingText && b.signature : b.data;
  MetaPart m;
  m.beforeCall = (uint16_t)std::min<size_t>(step.calls.size(), UINT16_MAX);
  r.replayed = r.replayed && signedBlock && !(r.sawText && step.calls.empty());
  if (r.replayed) m.bytes = body.substr(b.from, b.to - b.from);
  r.replayed = r.replayed && fitsALine(m.bytes);
  if (r.replayed) step.meta.push_back(std::move(m));
}

void usageOf(const Answer& a, ParsedStep& step) {
  step.usage = a.usage;
  step.usage.responses = 1;
  step.usage.servedModel = a.model;
  const uint64_t whole = (uint64_t)a.usage.promptTokens + a.usage.cacheReadTokens + a.usage.cacheWriteTokens;
  step.usage.maxRequestIn = (uint32_t)std::min<uint64_t>(whole, UINT32_MAX);
}

void stepOf(const std::string& body, const Answer& a, ParsedStep& step) {
  step.stop = stopOf(a.stop);
  Replay r;
  for (const Block& b : a.blocks) {
    if (b.type == "text") textOf(b, r, step);
    else if (b.type == "tool_use") callOf(body, b, step);
    else if (b.type == "thinking" || b.type == "redacted_thinking") thinkingOf(body, b, r, step);
    else r.replayed = false;   // a block the round does not store
    // Lane E2-P-CONN2: a tool server the provider called for the owner answered inside this answer.
    if (b.type == "mcp_tool_result") step.outside = true;
  }
  if (step.stop == StopReason::Refusal) {
    step.calls.clear();   // a refused answer runs nothing
    step.refusal = a.refusal.empty() ? a.stop : a.refusal;
  }
  usageOf(a, step);
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
