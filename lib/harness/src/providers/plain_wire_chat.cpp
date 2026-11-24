// plain_wire_chat.cpp - the chat-completions wire of engine 2 (router design section 4, codec C10;
// lane E2-R6). One codec for Mistral (direct and routed), Z.ai (both hosts and routed), an
// OpenAI-compatible custom endpoint and text mode (Dialect::Flat, or a route whose row has the tools
// bit off). What a request carries is the shared planner's (plain_plan.h); this file spells its Plan
// as chat-completions JSON and reads chat-completions answers (plain_json_reader.h). Everything that
// differs between those providers is a field of the route (plain/route.h): the path, the auth, the
// model, the cap mode (applied by the pre-flight), the cache key field, the replay kind, the tools
// bit and the empty-text placeholder. The codec compares no host and holds no window, cap or price.
//
//   {"model", "max_tokens" (when the pre-flight left a cap), "reasoning_effort" (when set),
//    "response_format":{"type":"json_schema",...} (RenderInput::answerSchema), "prompt_cache_key" (a
//    row with that cache key field), "tools":[{"type":"function","function":{...}}],
//    "tool_choice":"auto"|"none" (no tools and no tool_choice in text mode), "messages":[...]}
//
// Messages: one system message (the three system blocks joined by a fixed separator, so the bytes
// stay stable); user messages (a single text as a string, else a list of text and image_url parts;
// an owner message opens its own); assistant messages (text, the row's replayed reasoning, tool_calls
// with arguments as a JSON string); one tool message per result. A photo a view result brings back
// goes in one user message right after the round's tool messages (no row of this dialect takes an
// image inside a tool result). Reasoning replay is the one switch: Mistral thinking chunks inside the
// content list (sent only beside text: an assistant content of thinking alone is refused), Z.ai
// reasoning_content beside the content. A k item replays only when its stored form is the route's
// kind, so another dialect's reasoning is never sent here. The planner's cache marks write nothing.
//
// Every model-facing text this file sends is stored text, a rules-lane text or the route's
// placeholder: the wire adds no prose of its own.
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
namespace caps = nimbus::orch::caps;
using body::BodyWriter;
using body::Over;

constexpr const char* kSystemJoin = "\n\n";

// ---- small rules -------------------------------------------------------------------------------

// A stored tool name the providers cannot take is sent with each byte outside [a-zA-Z0-9_.-] as
// '_', at most 64 bytes (Mistral and Z.ai take dotted names: engine-1 quirks M2, Z5).
std::string wireName(const std::string& n) {
  std::string out = n.substr(0, 64);
  for (char& c : out)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
      c = '_';
  return out.empty() ? std::string("_") : out;
}

// One JSON object the body writer accepts as it is (call arguments, a stored reasoning item).
bool oneObject(const std::string& json) {
  size_t i = 0;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
  if (i >= json.size() || json[i] != '{') return false;
  std::string scratch;
  BodyWriter probe(scratch);
  probe.raw(json);
  return probe.status().ok();
}

// What a stored k item holds, by its form: a Mistral thinking chunk ({"type":"thinking","thinking":
// [...]}), a Z.ai reasoning text ({"reasoning_content":"..."}), or something else (another dialect's).
enum class MetaKind : uint8_t { Other, Chunk, Reasoning };

// The fields of a stored k item that tell its kind.
struct MetaFields {
  std::string type;
  bool list = false, text = false, other = false;
};

bool metaField(JsonReader& j, const std::string& k, MetaFields& f, std::string* reasoning) {
  if (k == "type" && j.peek('"')) return j.str(&f.type);
  if (k == "reasoning_content" && j.peek('"')) return (f.text = true) && j.str(reasoning);
  if (k == "thinking") f.list = j.peek('[');
  else f.other = f.other || k != "signature";
  return j.skip();
}

MetaKind metaKind(const std::string& stored, std::string* reasoning) {
  JsonReader j(stored);
  MetaFields f;
  if (!j.object([&j, &f, reasoning](const std::string& k) { return metaField(j, k, f, reasoning); }) || !j.end())
    return MetaKind::Other;
  if (f.type == "thinking" && f.list && !f.text) return MetaKind::Chunk;
  if (f.text && f.type.empty() && !f.list && !f.other) return MetaKind::Reasoning;
  return MetaKind::Other;
}

// The FNV-1a hash of the chat key: the stable prompt_cache_key of one conversation.
std::string cacheKeyOf(const std::string& chatKey) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : chatKey) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  static const char kHex[] = "0123456789abcdef";
  std::string out = "nimbus-";
  for (int s = 60; s >= 0; s -= 4) out += kHex[(h >> s) & 15];
  return out;
}

// ---- grouping: the Plan's pieces as chat messages -------------------------------------------------

enum class Role : uint8_t { User, Assistant, Tool };

struct Msg {
  Role role = Role::User;
  uint32_t turn = 0;
  std::vector<uint32_t> pieces;   // indices into Plan::pieces
  std::vector<uint16_t> viewed;   // photos a round's view results brought back (a user message)
};

Role roleOf(plan::PieceKind k) {
  switch (k) {
    case plan::PieceKind::AssistantText:
    case plan::PieceKind::Meta:
    case plan::PieceKind::Call: return Role::Assistant;
    case plan::PieceKind::Result: return Role::Tool;
    default: return Role::User;
  }
}

struct Grouper {
  std::vector<Msg> msgs;
  std::vector<uint16_t> viewed;   // photos waiting for the end of the round's tool messages
  uint32_t viewedTurn = 0;

  // The photos of a round's view results: one user message after its tool messages.
  void flushViewed() {
    if (viewed.empty()) return;
    Msg m;
    m.role = Role::User;
    m.turn = viewedTurn;
    m.viewed.swap(viewed);
    msgs.push_back(std::move(m));
  }
  void add(const plan::Piece& pc, uint32_t at) {
    const Role role = roleOf(pc.kind);
    if (role == Role::Tool) {
      Msg m;
      m.role = Role::Tool;
      m.turn = pc.turn;
      m.pieces.push_back(at);
      msgs.push_back(std::move(m));
      if (pc.flags & plan::kViewed) {
        viewed.push_back(pc.photo);
        viewedTurn = pc.turn;
      }
      return;
    }
    flushViewed();
    const bool join = !msgs.empty() && msgs.back().role == role && msgs.back().turn == pc.turn &&
                      !(role == Role::User && (pc.flags & plan::kOpens));
    if (!join) {
      Msg m;
      m.role = role;
      m.turn = pc.turn;
      msgs.push_back(std::move(m));
    }
    msgs.back().pieces.push_back(at);
  }
};

// ---- the body: a Plan in chat-completions JSON ---------------------------------------------------

struct Render {
  const RenderInput& in;
  const plan::Plan& p;
  BodyWriter& w;
  caps::Replay replay = caps::Replay::None;
  std::string emptyText;   // the route's placeholder for an empty text ("" = none)
  tx::Item it;             // the item of the piece being written (one, read again where needed)
};

// The item of a piece into r.it (Line and Photo pieces name none of their own); an arena read gives
// the same item every time.
bool itemOf(Render& r, const plan::Piece& pc) {
  if (pc.kind == plan::PieceKind::Line || pc.kind == plan::PieceKind::Photo) return true;
  return r.in.items->get(pc.item, r.it).ok();
}

const std::string& textOf(const Render& r, const plan::Piece& pc, const tx::Item& it) {
  switch (pc.src) {
    case plan::Src::X: return it.x;
    case plan::Src::St: return it.st;
    case plan::Src::Args: return it.args;
    default: return r.p.lines[pc.line];
  }
}

// One text value of a user or assistant piece (an owner message's stamp in front of its text).
void textValue(Render& r, const plan::Piece& pc, const tx::Item& it) {
  if (pc.kind == plan::PieceKind::OwnerText) {
    r.w.beginStr();
    if (pc.flags & plan::kStamp) r.w.part(r.p.lines[pc.line]).part(" ", 1);
    r.w.part(it.x).endStr();
    return;
  }
  const bool line = pc.kind == plan::PieceKind::Line;
  r.w.str(line ? r.p.lines[pc.line] : it.x, line ? Over::Refuse : Over::Clip);
}

void imagePart(Render& r, const plan::Photo& ph) {
  r.w.beginObject().key("type").str("image_url").key("image_url").beginObject().key("url");
  r.w.beginStr().part("data:", 5).part(r.p.lines[ph.text]).part(";base64,", 8).splice(ph.bytes, ph.len).endStr();
  r.w.endObject().endObject();
}

void textPart(Render& r, const plan::Piece& pc, const tx::Item& it) {
  r.w.beginObject().key("type").str("text").key("text");
  textValue(r, pc, it);
  r.w.endObject();
}

// A user message: a single text as a string, else its parts in order.
Status userMessage(Render& r, const Msg& m) {
  r.w.beginObject().key("role").str("user").key("content");
  const tx::Item& it = r.it;
  const bool single = m.viewed.empty() && m.pieces.size() == 1 && r.p.pieces[m.pieces[0]].kind != plan::PieceKind::Photo;
  if (single) {
    const plan::Piece& pc = r.p.pieces[m.pieces[0]];
    if (!itemOf(r, pc)) return Status::fail(Err::NotFound, "plain::chat.render.item");
    textValue(r, pc, it);
    r.w.endObject();
    return Status::success();
  }
  r.w.beginArray();
  for (uint32_t at : m.pieces) {
    const plan::Piece& pc = r.p.pieces[at];
    if (pc.kind == plan::PieceKind::Photo) {
      if (!r.p.photos[pc.photo].bytes) return Status::fail(Err::InvalidArgument, "plain::chat.render.fileRef");
      imagePart(r, r.p.photos[pc.photo]);
      continue;
    }
    if (!itemOf(r, pc)) return Status::fail(Err::NotFound, "plain::chat.render.item");
    textPart(r, pc, it);
  }
  for (uint16_t ph : m.viewed) {
    if (!r.p.photos[ph].bytes) return Status::fail(Err::InvalidArgument, "plain::chat.render.fileRef");
    imagePart(r, r.p.photos[ph]);
  }
  r.w.endArray().endObject();
  return Status::success();
}

// An assistant message read into its parts: texts and calls (piece indices; each item is read again
// where it is written), the replayable reasoning.
struct AssistantParts {
  std::vector<uint32_t> texts, calls;
  std::vector<std::string> chunks;   // Mistral thinking chunks, as stored
  std::string reasoning;             // Z.ai reasoning_content, the stored texts joined
  bool anyReasoning = false;
};

void noteMeta(const Render& r, AssistantParts& a) {
  std::string text;
  const MetaKind k = metaKind(r.it.x, &text);
  if (k == MetaKind::Chunk && r.replay == caps::Replay::ThinkChunks) a.chunks.push_back(r.it.x);
  if (k == MetaKind::Reasoning && r.replay == caps::Replay::ReasoningContent) {
    a.reasoning += text;
    a.anyReasoning = true;
  }
}

Status readAssistant(Render& r, const Msg& m, AssistantParts& a) {
  for (uint32_t at : m.pieces) {
    const plan::Piece& pc = r.p.pieces[at];
    if (!itemOf(r, pc)) return Status::fail(Err::NotFound, "plain::chat.render.item");
    if (pc.kind == plan::PieceKind::Meta) noteMeta(r, a);
    else (pc.kind == plan::PieceKind::Call ? a.calls : a.texts).push_back(at);
  }
  return Status::success();
}

void assistantContent(Render& r, const AssistantParts& a) {
  if (a.texts.empty()) return;   // a call-only message carries no content
  r.w.key("content");
  const bool one = a.chunks.empty() && a.texts.size() == 1;
  if (!one) r.w.beginArray();
  for (const std::string& c : a.chunks) r.w.raw(c);
  for (uint32_t at : a.texts) {
    const plan::Piece& pc = r.p.pieces[at];
    (void)itemOf(r, pc);   // read once already by readAssistant
    if (one) textValue(r, pc, r.it);
    else textPart(r, pc, r.it);
  }
  if (!one) r.w.endArray();
}

void toolCalls(Render& r, const AssistantParts& a) {
  if (a.calls.empty()) return;
  r.w.key("tool_calls").beginArray();
  for (uint32_t at : a.calls) {
    const plan::Piece& pc = r.p.pieces[at];
    (void)itemOf(r, pc);   // read once already by readAssistant
    r.w.beginObject().key("id").str(r.p.ids[pc.id]).key("type").str("function").key("function").beginObject();
    r.w.key("name").str(wireName(r.it.n)).key("arguments");
    if (pc.src != plan::Src::None) r.w.str(textOf(r, pc, r.it), Over::Clip);
    else r.w.str("{}");
    r.w.endObject().endObject();
  }
  r.w.endArray();
}

Status assistantMessage(Render& r, const Msg& m) {
  AssistantParts a;
  Status st = readAssistant(r, m, a);
  if (!st.ok()) return st;
  if (a.texts.empty() && a.calls.empty()) return Status::success();   // reasoning of another kind alone
  r.w.beginObject().key("role").str("assistant");
  assistantContent(r, a);
  if (a.anyReasoning) r.w.key("reasoning_content").str(a.reasoning, Over::Clip);
  toolCalls(r, a);
  r.w.endObject();
  return Status::success();
}

Status toolMessage(Render& r, const Msg& m) {
  const plan::Piece& pc = r.p.pieces[m.pieces[0]];
  if (!itemOf(r, pc)) return Status::fail(Err::NotFound, "plain::chat.render.item");
  r.w.beginObject().key("role").str("tool").key("tool_call_id").str(r.p.ids[pc.id]).key("content");
  if (pc.src != plan::Src::None) r.w.str(textOf(r, pc, r.it), Over::Clip);
  else r.w.str(r.emptyText);   // a result with no content: the route's placeholder
  r.w.endObject();
  return Status::success();
}

// The one system message: the blocks the plan sends, joined by the fixed separator.
void systemMessage(BodyWriter& w, const SystemBlocks& s, const plan::SystemPlan& sp) {
  const std::string* blocks[] = {&s.core, &s.notes, &s.digests};
  bool any = false;
  for (int i = 0; i < 3; ++i) any = any || sp.shown[i];
  if (!any) return;
  w.beginObject().key("role").str("system").key("content").beginStr();
  bool first = true;
  for (int i = 0; i < 3; ++i) {
    if (!sp.shown[i]) continue;
    if (!first) w.part(kSystemJoin);
    w.part(*blocks[i]);
    first = false;
  }
  w.endStr().endObject();
}

Status messagesArray(Render& r, const std::vector<Msg>& msgs) {
  r.w.key("messages").beginArray();
  systemMessage(r.w, *r.in.system, r.p.system);
  for (const Msg& m : msgs) {
    const Status st = m.role == Role::User ? userMessage(r, m) : m.role == Role::Tool ? toolMessage(r, m) : assistantMessage(r, m);
    if (!st.ok()) return st;
  }
  r.w.endArray();
  return Status::success();
}

Status toolsArray(BodyWriter& w, const RenderInput& in) {
  if (in.tools->empty()) return Status::success();
  w.key("tools").beginArray();
  for (const ToolSpec& t : *in.tools) {
    if (!oneObject(t.schemaJson)) return Status::fail(Err::InvalidArgument, "plain::chat.render.toolSchema");
    w.beginObject().key("type").str("function").key("function").beginObject().key("name").str(t.name);
    if (!t.description.empty()) w.key("description").str(t.description);
    w.key("parameters").raw(t.schemaJson).endObject().endObject();
  }
  w.endArray();
  w.key("tool_choice").str(in.toolChoiceNone ? "none" : "auto");
  return Status::success();
}

// The settings before the messages: the cap, the effort, the answer's schema, the cache key.
void settings(BodyWriter& w, const RenderInput& in) {
  if (in.maxTokens) w.key("max_tokens").unum(in.maxTokens);
  if (!in.head->effort.empty()) w.key("reasoning_effort").str(in.head->effort);
  if (in.answerSchema && *in.answerSchema) {
    w.key("response_format").beginObject().key("type").str("json_schema").key("json_schema").beginObject();
    w.key("name").str("answer").key("schema").raw(in.answerSchema).key("strict").boolean(true).endObject().endObject();
  }
  const caps::ProviderRow* p = in.route ? in.route->caps.provider : nullptr;
  if (p && p->cacheKey == caps::CacheKey::PromptCacheKey && !in.turn->chatKey.empty())
    w.key("prompt_cache_key").str(cacheKeyOf(in.turn->chatKey));
}

// Text mode: Dialect::Flat, or a route whose row has the tools bit off.
bool textMode(const RenderInput& in) {
  if (in.turn->dialect == Dialect::Flat) return true;
  return in.route && in.route->caps.provider && !(in.route->caps.row.flags & caps::flag::kTools);
}

// Where the request goes and its headers: the route's path and auth style; with no route, an
// OpenAI-compatible endpoint's defaults (bearer, /v1/chat/completions).
Status coordinates(const RenderInput& in, HttpRequest& out) {
  const Route* r = in.route;
  const caps::CapsTable& t = caps::builtinCaps();
  out.method = "POST";
  out.host = in.head->host;
  if (out.host.empty() && r && r->caps.provider) out.host = caps::poolStr(t, r->caps.provider->host);
  if (out.host.empty()) return Status::fail(Err::InvalidArgument, "plain::chat.render.host");
  out.port = in.head->port;
  out.tls = in.head->tls;
  out.path = r ? r->path : (in.head->basePath.empty() ? std::string("/v1") : in.head->basePath) + "/chat/completions";
  out.headers.emplace_back("content-type", "application/json");
  const std::string key = in.head->key ? in.head->key() : std::string();
  if (key.empty() || !in.head->tls) return Status::success();   // a key never rides a cleartext socket
  if (!r || r->auth != caps::Auth::None) out.headers.emplace_back("authorization", std::string("Bearer") + ' ' + key);
  return Status::success();
}

size_t reserveFor(const RenderInput& in, const plan::Plan& p) {
  size_t n = p.estimate + in.system->core.size() + in.system->notes.size() + in.system->digests.size() + 1024;
  for (const ToolSpec& t : *in.tools) n += t.name.size() + t.description.size() + t.schemaJson.size() + 64;
  return n;
}

Status groupPieces(const plan::Plan& p, bool text, std::vector<Msg>& out) {
  Grouper g;
  for (uint32_t i = 0; i < p.pieces.size(); ++i) {
    const plan::PieceKind k = p.pieces[i].kind;
    // Text-mode history (calls and results as text) is the planner's (lane E2-R4b): until it lands a
    // text-mode request over a stored call is refused, never sent with the round left out.
    if (text && (k == plan::PieceKind::Call || k == plan::PieceKind::Result))
      return Status::fail(Err::InvalidArgument, "plain::chat.render.textModeCalls");
    g.add(p.pieces[i], i);
  }
  g.flushViewed();
  out.swap(g.msgs);
  return Status::success();
}

Status renderBody(const RenderInput& in, HttpRequest& out) {
  plan::Plan p;
  Status st = plan::plan(in, oneObject, p);
  if (!st.ok()) return st;
  if (!p.anyBlock) return Status::fail(Err::InvalidArgument, "plain::chat.render.noMessages");
  const bool text = textMode(in);
  std::vector<Msg> msgs;
  if (!(st = groupPieces(p, text, msgs)).ok()) return st;
  if (!text && in.tools->empty())
    for (const Msg& m : msgs)
      if (m.role == Role::Tool) return Status::fail(Err::InvalidArgument, "plain::chat.render.noTools");

  out.body.reserve(reserveFor(in, p));
  BodyWriter w(out.body, &out.splices);
  w.beginObject().key("model").str(in.route ? in.route->model : in.turn->model);
  settings(w, in);
  if (!text && !(st = toolsArray(w, in)).ok()) return st;
  Render r{in, p, w};
  if (in.route && in.route->caps.provider) {
    r.replay = in.route->caps.row.replay;
    r.emptyText = caps::poolStr(caps::builtinCaps(), in.route->caps.provider->emptyText);
  }
  if (!(st = messagesArray(r, msgs)).ok()) return st;
  w.endObject();
  if (!(st = w.status()).ok()) return st;
  return coordinates(in, out);
}

// ---- the parse half ----------------------------------------------------------------------------
//
// parse() reads one 2xx chat-completions answer with the strict JSON reader into a local ParsedStep
// that reaches `out` only once the whole body was read and checked. choices[0].message gives:
//   content            a string, null, or a list of chunks: text chunks joined in order; a Mistral
//                      thinking chunk is one MetaPart (its bytes as served); other chunks are passed over
//   reasoning_content  Z.ai's reasoning: one MetaPart {"reasoning_content":<the string as served>}
//   tool_calls         a call each: id, function.name, function.arguments as a JSON string holding an
//                      object or (Z.ai) an object; arguments that do not parse as one object are a call
//                      that never runs (complete = false)
//   refusal            an OpenAI-compatible server's refusal text (a refusal)
// finish_reason (router design section 5 item 9): stop or tool_calls end the answer (ToolUse when it
// holds calls, whatever the label); length is a cut; model_length and model_context_window_exceeded a
// full window; content_filter and sensitive a refusal; anything else Other. usage: prompt_tokens (it
// includes the cached tokens on every row of this dialect: usageMode inputIncludesCache) less
// prompt_tokens_details.cached_tokens, the cached tokens, completion_tokens, the whole prompt as
// maxRequestIn, the served model.

struct Call {
  std::string id, name, args;
  bool runnable = false;    // that object can be sent back as it is
  uint32_t seen = 0;
};

struct Message {
  std::string text, refusal;
  std::vector<MetaPart> meta;
  std::vector<Call> calls;
  bool assistant = true;
  uint32_t seen = 0;
};

struct Answer {
  Message msg;
  std::string finish, model;
  nimbus::orch::TokenUsage usage;
  bool choice = false;
  uint32_t seen = 0;
};

// Whether one opaque piece fits the k item that stores it (the line limit, newline included).
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

void keepMeta(Message& m, std::string bytes) {
  if (!fitsALine(bytes)) return;   // a line cannot hold it: the next request goes without it
  MetaPart p;
  p.bytes = std::move(bytes);
  m.meta.push_back(std::move(p));
}

// Arguments that are one object, each of its own keys once (they go back as a string, so the body
// writer's nesting limit does not bind them).
bool runnableArgs(const std::string& args) {
  JsonReader j(args);
  std::vector<std::string> keys;
  const bool ok = j.object([&j, &keys](const std::string& k) {
    if (std::find(keys.begin(), keys.end(), k) != keys.end()) return false;
    keys.push_back(k);
    return j.skip();
  });
  return ok && j.end();
}

bool argsField(JsonReader& j, Call& c) {
  if (j.peek('"')) {   // the documented form: a JSON string holding the object
    if (!j.str(&c.args)) return false;
  } else {             // Z.ai's response doc shows an object
    if (!j.peek('{')) return j.skip();   // anything else: arguments that never run
    const size_t from = j.at();
    if (!j.skip()) return false;
    c.args = j.body().substr(from, j.at() - from);
  }
  c.runnable = runnableArgs(c.args);
  return true;
}

bool functionField(JsonReader& j, Call& c) {
  uint32_t seen = 0;
  return j.object([&j, &c, &seen](const std::string& k) {
    static const char* const kFields[] = {"name", "arguments"};
    uint32_t f = 0;
    if (!fieldOnce(k, kFields, 2, seen, f)) return false;
    if (f == 0) return j.str(&c.name);
    if (f == 1) return argsField(j, c);
    return j.skip();
  });
}

bool callsField(JsonReader& j, Message& m) {
  if (!j.peek('[')) return j.skip();   // null: no calls
  return j.array([&j, &m] {
    Call c;
    const bool ok = j.object([&j, &c](const std::string& k) {
      static const char* const kFields[] = {"id", "function"};
      uint32_t f = 0;
      if (!fieldOnce(k, kFields, 2, c.seen, f)) return false;
      if (f == 0) return j.peek('"') ? j.str(&c.id) : j.skip();
      if (f == 1) return functionField(j, c);
      return j.skip();
    });
    m.calls.push_back(std::move(c));
    return ok;
  });
}

// One chunk of a content list: text joins the text, a thinking chunk is kept as served.
bool chunk(JsonReader& j, Message& m) {
  j.ws();
  const size_t from = j.at();
  std::string type, text;
  const bool ok = j.object([&j, &type, &text](const std::string& k) {
    if (k == "type" && j.peek('"')) return j.str(&type);
    if (k == "text" && j.peek('"')) return j.str(&text);
    return j.skip();
  });
  if (!ok) return false;
  if (type == "text") m.text += text;
  if (type == "thinking") keepMeta(m, j.body().substr(from, j.at() - from));
  return true;
}

bool contentField(JsonReader& j, Message& m) {
  if (j.peek('"')) return j.str(&m.text);
  if (j.peek('[')) return j.array([&j, &m] { return chunk(j, m); });
  return j.skip();   // null: no text
}

bool reasoningField(JsonReader& j, Message& m) {
  if (!j.peek('"')) return j.skip();
  const size_t from = j.at();
  std::string text;
  if (!j.str(&text)) return false;
  if (!text.empty()) keepMeta(m, "{\"reasoning_content\":" + j.body().substr(from, j.at() - from) + "}");
  return true;
}

bool messageField(JsonReader& j, const std::string& key, Message& m) {
  static const char* const kFields[] = {"role", "content", "reasoning_content", "tool_calls", "refusal"};
  uint32_t k = 0;
  if (!fieldOnce(key, kFields, 5, m.seen, k)) return false;
  std::string role;
  switch (k) {
    case 0:
      if (!j.str(&role)) return false;
      m.assistant = role == "assistant";
      return true;
    case 1: return contentField(j, m);
    case 2: return reasoningField(j, m);
    case 3: return callsField(j, m);
    case 4: return j.peek('"') ? j.str(&m.refusal) : j.skip();
    default: return j.skip();
  }
}

bool choiceObject(JsonReader& j, Answer& a) {
  uint32_t seen = 0;
  return j.object([&j, &a, &seen](const std::string& k) {
    static const char* const kFields[] = {"message", "finish_reason"};
    uint32_t f = 0;
    if (!fieldOnce(k, kFields, 2, seen, f)) return false;
    if (f == 0) {
      a.choice = true;
      return j.object([&j, &a](const std::string& mk) { return messageField(j, mk, a.msg); });
    }
    if (f == 1) return j.peek('"') ? j.str(&a.finish) : j.skip();
    return j.skip();
  });
}

bool choicesField(JsonReader& j, Answer& a) {
  bool first = true;
  return j.array([&j, &a, &first] {
    if (!first) return j.skip();   // only the first choice is the answer (n is never asked)
    first = false;
    return choiceObject(j, a);
  });
}

bool usageObject(JsonReader& j, nimbus::orch::TokenUsage& u) {
  if (!j.peek('{')) return j.skip();   // null: no usage given
  uint32_t seen = 0;
  return j.object([&j, &u, &seen](const std::string& k) {
    static const char* const kFields[] = {"prompt_tokens", "completion_tokens", "prompt_tokens_details"};
    uint32_t f = 0;
    if (!fieldOnce(k, kFields, 3, seen, f)) return false;
    if (f == 0) return j.count(u.promptTokens);
    if (f == 1) return j.count(u.completionTokens);
    if (f != 2 || !j.peek('{')) return j.skip();
    return j.object([&j, &u](const std::string& d) { return d == "cached_tokens" ? j.count(u.cacheReadTokens) : j.skip(); });
  });
}

bool topField(JsonReader& j, const std::string& key, Answer& a) {
  static const char* const kFields[] = {"choices", "usage", "model"};
  uint32_t k = 0;
  if (!fieldOnce(key, kFields, 3, a.seen, k)) return false;
  switch (k) {
    case 0: return choicesField(j, a);
    case 1: return usageObject(j, a.usage);
    case 2: return j.peek('"') ? j.str(&a.model) : j.skip();
    default: return j.skip();
  }
}

// The stop-value table (router design section 5 item 9).
StopReason stopOf(const std::string& s, bool calls) {
  if (s == "stop" || s == "tool_calls" || s == "function_call") return calls ? StopReason::ToolUse : StopReason::EndTurn;
  if (s == "length") return StopReason::MaxTokens;
  if (s == "model_length" || s == "model_context_window_exceeded") return StopReason::Context;
  if (s == "content_filter" || s == "sensitive") return StopReason::Refusal;
  return StopReason::Other;
}

void stepOf(Answer& a, ParsedStep& step) {
  Message& m = a.msg;
  step.stop = stopOf(a.finish, !m.calls.empty());
  if (!m.refusal.empty()) step.stop = StopReason::Refusal;
  step.text = std::move(m.text);
  step.meta = std::move(m.meta);
  for (Call& c : m.calls) {
    ToolCall t;
    t.id = std::move(c.id);
    t.name = std::move(c.name);
    t.complete = c.runnable && step.stop == StopReason::ToolUse;   // only an answer that stops to call runs them
    t.argsJson = c.args.empty() ? std::string("{}") : std::move(c.args);   // as written: the engine stubs what cannot run
    step.calls.push_back(std::move(t));
  }
  if (step.stop == StopReason::Refusal) {
    step.calls.clear();   // a refused answer runs nothing
    step.refusal = !m.refusal.empty() ? m.refusal : a.finish;
  }
  // prompt_tokens includes the cached tokens: the engine's promptTokens is the uncached part (it adds
  // the cache reads back for its size guard and prices them at the cache rate), maxRequestIn the whole.
  step.usage = a.usage;
  const uint32_t whole = a.usage.promptTokens;
  step.usage.promptTokens = whole >= a.usage.cacheReadTokens ? whole - a.usage.cacheReadTokens : whole;
  step.usage.responses = 1;
  step.usage.servedModel = a.model;
  step.usage.maxRequestIn = whole;
}

Status parseBody(const HttpResponse& in, ParsedStep& out) {
  if (in.status < 200 || in.status > 299) return Status::fail(Err::InvalidArgument, "plain::chat.parse.status");
  if (!nimbus::utf8Valid(in.body)) return Status::fail(Err::InvalidArgument, "plain::chat.parse.utf8");
  JsonReader j(in.body);
  Answer a;
  if (!j.object([&j, &a](const std::string& k) { return topField(j, k, a); }) || !j.end())
    return Status::fail(Err::InvalidArgument, "plain::chat.parse.json");
  if (!a.choice || !a.msg.assistant) return Status::fail(Err::InvalidArgument, "plain::chat.parse.notAnAnswer");
  for (const Call& c : a.msg.calls)   // a call that cannot be answered is not an answer
    if (c.id.empty() || c.name.empty()) return Status::fail(Err::InvalidArgument, "plain::chat.parse.callWithoutIdOrName");
  ParsedStep step;
  stepOf(a, step);
  out = std::move(step);
  return Status::success();
}

class ChatWire final : public WireCodec {
 public:
  Dialect dialect() const override { return Dialect::ChatCompletions; }

  Status render(const RenderInput& in, HttpRequest& out) const override {
    out = HttpRequest{};
    if (!in.turn || !in.head || !in.system || !in.tools || !in.items || in.turn->model.empty())
      return Status::fail(Err::InvalidArgument, "plain::chat.render");
    Status st;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
      st = renderBody(in, out);
    } catch (const std::bad_alloc&) {   // ruling 30: a refused allocation is a status
      st = Status::fail(Err::TooLarge, "plain::chat.render.memory");
    }
#else
    st = renderBody(in, out);
#endif
    if (!st.ok()) out = HttpRequest{};
    return st;
  }

  Status parse(const HttpResponse& in, ParsedStep& out) const override {
    out = ParsedStep{};
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
      return parseBody(in, out);
    } catch (const std::bad_alloc&) {   // ruling 30: nothing half-read
      return Status::fail(Err::TooLarge, "plain::chat.parse.memory");
    }
#else
    return parseBody(in, out);
#endif
  }
};

}  // namespace

const WireCodec& chatWire() {
  static const ChatWire wire;
  return wire;
}

}  // namespace plain
}  // namespace agent
