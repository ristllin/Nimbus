// plain_wire_openai.cpp - the OpenAI Responses codec of engine 2 (router design sections 4 and 5,
// lane E2-R7). It spells the shared planner's Plan (plain_plan.h) as one Responses request and
// reads one Responses answer, direct (api.openai.com/v1/responses) and through the Cumulo router
// (/router/openai/v1/responses). Everything model-specific comes from the route's table rows
// (RenderInput::route: path, auth, the model row's replay, cache, image and structured facts; the
// output cap the pre-flight left); the codec holds no host, window, cap or price. With no route
// (a property suite rendering a stored transcript) it reads the same rows by the turn's model.
//
// Request (key order fixed; the same RenderInput gives the same bytes):
//   {"model", "store":false, "include":["reasoning.encrypted_content"] (a row that replays encrypted
//    reasoning), "prompt_cache_key" (a provider row with the key field: "nb-" and the chat key's
//    FNV-1a 64), "reasoning":{"effort"} (when the head has one; pre-flight clears a value the row
//    refuses), "max_output_tokens" (only when the pre-flight left a cap: none direct, the routed
//    ceiling), "text":{"format":{json_schema}} (RenderInput::answerSchema), "instructions" (the core
//    system block), "tools":[flat functions, strict false], "tool_choice":"auto"|"none",
//    "input":[...]}
// Input items, in the plan's order:
//   developer messages   the notes and the digests system blocks (input_text)
//   user messages        owner text with its stamp, photo parts (input_image, detail high), lines,
//                        device notes; a block joins the user message before it in the same turn
//                        unless it opens its own (an owner message)
//   assistant messages   output_text parts
//   reasoning            a stored reasoning item as stored ({"type":"reasoning","id":"rs_..",
//                        "summary":[..],"encrypted_content":..}), only on a row that replays
//                        encrypted items; any other stored round meta is left out
//   function_call        call_id (the stored call id), name, arguments as a JSON string; the item
//                        id (fc_...) is never stored and never sent
//   function_call_output call_id and the result text; a photo a result shows goes inside the
//                        output where the row allows images in tool results, else in one user
//                        message right after the round's outputs (engine-1 parity O13)
// Lane E2-P-CONN2: on an openai head, tools[] also holds what plain_plan.h providerTools gives after the
// functions: the owner's MCP servers and first-party connectors ({"type":"mcp","require_approval":"never",
// "server_label",...}) and the Code sandbox ({"type":"code_interpreter","container":{"type":"auto"}}),
// engine 1's attach entries as they are (https://platform.openai.com/docs/guides/tools-remote-mcp and
// .../tools-code-interpreter, as engine 1's grid recorded them; checked 24 Nov 2026).
// Cache breakpoints ({"mode":"explicit"} on an input_text part) only on an openaiBreakpoints row:
// the notes block, the digests block and the last input_text at or before the previous turn's
// end (the plan's three stable marks; the core rides `instructions`, which takes none).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nimbus/harness/plain/route.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/model_caps.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/utf8.h"
#include "plain_body.h"
#include "plain_json_reader.h"
#include "plain_plan.h"

// One out-of-line copy of a helper the body writer would otherwise inline at every call site
// (the codec's image budget, design section 14).
#define R7_NOINLINE __attribute__((noinline))

namespace agent {
namespace plain {

namespace {

namespace tx = nimbus::orch::tx;
namespace caps = nimbus::orch::caps;
using body::BodyWriter;
using body::Over;

constexpr const char* kBreakpoint = ",\"prompt_cache_breakpoint\":{\"mode\":\"explicit\"}";
constexpr const char* kInclude = "reasoning.encrypted_content";
constexpr uint32_t kMinOutputTokens = 16;   // the Responses reference's minimum for max_output_tokens

// ---- one reader for every object and list this codec reads ------------------------------------------

struct Answer;
struct Item;

// What one read fills: the answer and its item being read, or three loose strings.
struct Ctx {
  JsonReader& j;
  Answer* a = nullptr;
  Item* it = nullptr;
  std::string s[3];
  std::vector<std::string> keys;
};
using OnKey = bool (*)(Ctx&, const std::string&);
using OnItem = bool (*)(Ctx&);

struct KeyCall {
  Ctx* c;
  OnKey f;
  bool operator()(const std::string& k) const { return f(*c, k); }
};
struct ItemCall {
  Ctx* c;
  OnItem f;
  bool operator()() const { return f(*c); }
};

R7_NOINLINE bool readObject(Ctx& c, OnKey f) { return c.j.object(KeyCall{&c, f}); }
R7_NOINLINE bool readList(Ctx& c, OnItem f) { return c.j.array(ItemCall{&c, f}); }

// A string field into `out`; a null or a value of another type reads as absent.
R7_NOINLINE bool optStr(JsonReader& j, std::string& out) { return j.peek('"') ? j.str(&out) : j.skip(); }

// ---- small rules -------------------------------------------------------------------------------

// A stored tool name the API cannot take is sent with each byte outside [a-zA-Z0-9_-] as '_',
// at most 64 bytes (the same rule as the Messages codec).
R7_NOINLINE std::string wireName(const std::string& n) {
  std::string out = n.substr(0, 64);
  for (char& c : out)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
  return out.empty() ? std::string("_") : out;
}

// One JSON object the body writer accepts as it is (the planner's test of call arguments and
// stored round meta).
bool oneObject(const std::string& json) {
  size_t i = 0;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
  if (i >= json.size() || json[i] != '{') return false;
  std::string scratch;
  BodyWriter probe(scratch);
  probe.raw(json);
  return probe.status().ok();
}

// A stored round meta this wire replays: a reasoning item as this codec stores it (reasoningOf
// below: type, then an rs_ id, ..., a non-empty encrypted_content); the planner checked it is one
// object. Anything else (another dialect's thinking, a foreign shape) is left out.
bool reasoningItem(const std::string& json) {
  static const char kHead[] = "{\"type\":\"reasoning\",\"id\":\"rs_";
  static const char kEnc[] = "\"encrypted_content\":\"";
  if (json.compare(0, sizeof kHead - 1, kHead) != 0) return false;
  const size_t at = json.find(kEnc);
  return at != std::string::npos && at + sizeof kEnc - 1 < json.size() && json[at + sizeof kEnc - 1] != '"';
}

// The rows the request reads: the route's; with no route, the turn's model on the OpenAI slot.
R7_NOINLINE caps::Caps capsOf(const RenderInput& in) {
  if (in.route && in.route->caps.provider) return in.route->caps;
  return caps::capsFor(caps::Slot::OpenAI, in.turn->model.c_str(), in.head->routed);
}

R7_NOINLINE std::string cacheKeyOf(const std::string& chatKey) {
  uint64_t h = 1469598103934665603ull;   // FNV-1a 64 (engine 1's prompt_layout cacheKey)
  for (char c : chatKey) {
    h ^= (uint8_t)c;
    h *= 1099511628211ull;
  }
  static const char kHex[] = "0123456789abcdef";
  std::string out = "nb-";
  for (int i = 15; i >= 0; --i) out += kHex[(h >> (i * 4)) & 0xF];
  return out;
}

// "key":"literal" and "key":"text", each one call.
R7_NOINLINE BodyWriter& put(BodyWriter& w, const char* k, const char* v) { return w.key(k).str(v, std::strlen(v)); }
R7_NOINLINE BodyWriter& putText(BodyWriter& w, const char* k, const std::string& v, Over over) {
  return w.key(k).str(v.data(), v.size(), over);
}
// {"type":"<type>" opened.
R7_NOINLINE BodyWriter& typed(BodyWriter& w, const char* type) { return put(w.beginObject(), "type", type); }

// ---- the body: a Plan as Responses input items ---------------------------------------------------

enum class Role : uint8_t { None, User, Assistant };

struct Render {
  const RenderInput& in;
  const plan::Plan& p;
  const caps::ModelRow& row;
  BodyWriter& w;
  std::string& body;
  Role role = Role::None;            // the open message item's role
  uint32_t turn = 0;
  size_t lastText = 0;               // where the last input_text part closes (0: none yet)
  std::vector<size_t> points;        // breakpoint offsets, ascending
  std::vector<uint16_t> viewed;      // photos the round's outputs show, for the message after them
};

R7_NOINLINE void closeMessage(Render& r) {
  if (r.role != Role::None) r.w.endArray().endObject();
  r.role = Role::None;
}

// Open the next part's message: a new one unless `join` and the open one has this role and turn.
R7_NOINLINE void openMessage(Render& r, Role role, uint32_t turn, bool join) {
  if (join && role == r.role && turn == r.turn) return;
  closeMessage(r);
  put(typed(r.w, "message"), "role", role == Role::User ? "user" : "assistant").key("content").beginArray();
  r.role = role;
  r.turn = turn;
}

// A stable mark becomes a breakpoint on the last input_text part written (once per part).
R7_NOINLINE void markStable(Render& r) {
  if (r.row.cacheStyle != caps::CacheStyle::OpenaiBreakpoints || !r.lastText) return;
  if (r.points.empty() || r.points.back() < r.lastText) r.points.push_back(r.lastText);
}

// An input_text part; its closing brace is where a breakpoint would go.
R7_NOINLINE void inputText(Render& r, const std::string& text, Over over) {
  putText(typed(r.w, "input_text"), "text", text, over);
  r.lastText = r.body.size();
  r.w.endObject();
}

R7_NOINLINE void imagePart(Render& r, const plan::Photo& ph) {
  typed(r.w, "input_image");
  if (!ph.bytes) {
    putText(r.w, "file_id", r.p.lines[ph.text], Over::Refuse);
  } else {
    r.w.key("image_url").beginStr().part("data:", 5).part(r.p.lines[ph.text]).part(";base64,", 8);
    r.w.splice(ph.bytes, ph.len).endStr();
  }
  put(r.w, "detail", "high").endObject();
}

R7_NOINLINE const std::string& textOf(const Render& r, const plan::Piece& pc, const tx::Item& it) {
  static const std::string kNone;
  switch (pc.src) {
    case plan::Src::X: return it.x;
    case plan::Src::St: return it.st;
    case plan::Src::Args: return it.args;
    case plan::Src::Line: return r.p.lines[pc.line];
    default: return kNone;
  }
}

// The photos the round's outputs showed, in one user message after the last output.
R7_NOINLINE void flushViewed(Render& r, uint32_t turn) {
  if (r.viewed.empty()) return;
  openMessage(r, Role::User, turn, false);
  for (uint16_t ph : r.viewed) imagePart(r, r.p.photos[ph]);
  r.viewed.clear();
}

R7_NOINLINE void ownerText(Render& r, const plan::Piece& pc, const tx::Item& u) {
  openMessage(r, Role::User, pc.turn, false);
  typed(r.w, "input_text").key("text").beginStr();
  if (pc.flags & plan::kStamp) r.w.part(r.p.lines[pc.line]).part(" ", 1);
  r.w.part(u.x).endStr();
  r.lastText = r.body.size();
  r.w.endObject();
}

R7_NOINLINE void reasoning(Render& r, const tx::Item& it) {
  if (r.row.replay != caps::Replay::EncryptedItems || !reasoningItem(it.x)) return;   // not this wire's
  closeMessage(r);
  r.w.raw(it.x);
}

R7_NOINLINE void callItem(Render& r, const plan::Piece& pc, const tx::Item& it) {
  closeMessage(r);
  putText(typed(r.w, "function_call"), "call_id", r.p.ids[pc.id], Over::Refuse);
  putText(r.w, "name", wireName(it.n), Over::Refuse);
  const std::string& args = textOf(r, pc, it);
  if (args.empty() || args.size() > nimbus::orch::kJsonStrGuard) put(r.w, "arguments", "{}");   // never a clipped object
  else putText(r.w, "arguments", args, Over::Refuse);
  r.w.endObject();
}

R7_NOINLINE void resultItem(Render& r, const plan::Piece& pc, const tx::Item& it) {
  closeMessage(r);
  putText(typed(r.w, "function_call_output"), "call_id", r.p.ids[pc.id], Over::Refuse);
  const std::string& text = textOf(r, pc, it);
  const bool viewed = (pc.flags & plan::kViewed) != 0;
  if (viewed && (r.row.flags & caps::flag::kImageInToolResult)) {
    r.w.key("output").beginArray();
    if (!text.empty()) inputText(r, text, Over::Clip);
    imagePart(r, r.p.photos[pc.photo]);
    r.w.endArray();
  } else {
    putText(r.w, "output", text, Over::Clip);
    if (viewed) r.viewed.push_back(pc.photo);
  }
  r.w.endObject();
}

R7_NOINLINE void piece(Render& r, const plan::Piece& pc, const tx::Item& it) {
  const bool join = !(pc.flags & plan::kOpens);
  switch (pc.kind) {
    case plan::PieceKind::OwnerText: return ownerText(r, pc, it);
    case plan::PieceKind::Photo:
      openMessage(r, Role::User, pc.turn, join);
      return imagePart(r, r.p.photos[pc.photo]);
    case plan::PieceKind::Omission:
    case plan::PieceKind::Line:
      openMessage(r, Role::User, pc.turn, join);
      return inputText(r, r.p.lines[pc.line], Over::Refuse);
    case plan::PieceKind::AssistantText:
      openMessage(r, Role::Assistant, pc.turn, true);
      putText(typed(r.w, "output_text"), "text", it.x, Over::Clip).endObject();
      return;
    case plan::PieceKind::DeviceNote:
      openMessage(r, Role::User, pc.turn, true);
      return inputText(r, it.x, Over::Clip);
    case plan::PieceKind::Meta: return reasoning(r, it);
    case plan::PieceKind::Call: return callItem(r, pc, it);
    case plan::PieceKind::Result: return resultItem(r, pc, it);
  }
}

// The notes and digests system blocks as developer messages (the core rides `instructions`).
R7_NOINLINE void developerItems(Render& r) {
  const SystemBlocks& s = *r.in.system;
  const plan::SystemPlan& sp = r.p.system;
  const std::string* blocks[] = {&s.core, &s.notes, &s.digests};
  for (int i = 1; i < 3; ++i) {
    if (!sp.shown[i]) continue;
    put(typed(r.w, "message"), "role", "developer").key("content").beginArray();
    inputText(r, *blocks[i], Over::Refuse);
    r.w.endArray().endObject();
    if (i == sp.stableAfter || (i == 2 && sp.digestsMark)) markStable(r);
  }
}

R7_NOINLINE void inputArray(Render& r) {
  r.w.key("input").beginArray();
  developerItems(r);
  tx::Item it;
  size_t fetched = SIZE_MAX;
  for (const plan::Piece& pc : r.p.pieces) {
    const bool ownText = pc.kind == plan::PieceKind::Line || pc.kind == plan::PieceKind::Photo ||
                         pc.kind == plan::PieceKind::Omission;
    if (!ownText && pc.item != fetched) {
      if (!r.in.items->get(pc.item, it).ok()) continue;
      fetched = pc.item;
    }
    if (pc.kind != plan::PieceKind::Result) flushViewed(r, pc.turn);
    piece(r, pc, it);
    if (pc.flags & plan::kMarkStable) markStable(r);
  }
  flushViewed(r, r.turn);
  closeMessage(r);
  r.w.endArray();
}

R7_NOINLINE Status toolsArray(BodyWriter& w, const RenderInput& in, const plan::ProviderTools& pt) {
  if (in.tools->empty()) return Status::success();
  w.key("tools").beginArray();
  for (const ToolSpec& t : *in.tools) {
    if (!oneObject(t.schemaJson)) return Status::fail(Err::InvalidArgument, "plain::openai.render.toolSchema");
    putText(typed(w, "function"), "name", t.name, Over::Refuse);
    if (!t.description.empty()) putText(w, "description", t.description, Over::Refuse);
    // strict false: the registry's schemas are not strict-shaped (engine-1 O8; Responses
    // defaults strict to true).
    w.key("parameters").raw(t.schemaJson).key("strict").boolean(false).endObject();
  }
  for (const std::string& t : pt.tools) w.raw(t);   // provider-side connectors and sandbox (E2-P-CONN2)
  w.endArray();
  put(w, "tool_choice", in.toolChoiceNone ? "none" : "auto");
  return Status::success();
}

// The fields before the input: store, replay, cache key, effort, cap, structured form, instructions.
R7_NOINLINE void envelope(BodyWriter& w, const RenderInput& in, const caps::Caps& c, const plan::Plan& p) {
  putText(w.beginObject(), "model", in.route ? in.route->model : in.turn->model, Over::Refuse).key("store").boolean(false);
  if (c.row.replay == caps::Replay::EncryptedItems) w.key("include").beginArray().str(kInclude, std::strlen(kInclude)).endArray();
  if (c.provider && c.provider->cacheKey == caps::CacheKey::PromptCacheKey && !in.turn->chatKey.empty())
    putText(w, "prompt_cache_key", cacheKeyOf(in.turn->chatKey), Over::Refuse);
  if (!in.head->effort.empty()) putText(w.key("reasoning").beginObject(), "effort", in.head->effort, Over::Refuse).endObject();
  if (in.maxTokens) w.key("max_output_tokens").unum(std::max(in.maxTokens, kMinOutputTokens));
  if (in.answerSchema && *in.answerSchema) {
    put(put(w.key("text").beginObject().key("format").beginObject(), "type", "json_schema"), "name", "answer");
    w.key("schema").raw(in.answerSchema).endObject().endObject();
  }
  if (p.system.shown[0] && !in.system->core.empty()) putText(w, "instructions", in.system->core, Over::Refuse);
}

// The breakpoints, last first so the earlier offsets hold; the photo splices after each move with it.
R7_NOINLINE void placePoints(const std::vector<size_t>& points, HttpRequest& out) {
  const size_t len = std::strlen(kBreakpoint);
  for (size_t i = points.size(); i-- > 0;) {
    out.body.insert(points[i], kBreakpoint, len);
    for (BodySplice& sp : out.splices)
      if (sp.at > points[i]) sp.at += len;
  }
}

// Where the request goes and its headers: the route's path and auth style; with no route, the
// endpoint's base path (or /v1) and a Bearer key.
R7_NOINLINE void coordinates(const RenderInput& in, HttpRequest& out) {
  const Route* r = in.route;
  out.method = "POST";
  out.host = in.head->host;
  out.port = in.head->port;
  out.tls = in.head->tls;
  out.path = r ? r->path : (in.head->basePath.empty() ? std::string("/v1") : in.head->basePath) + "/responses";
  out.headers.emplace_back("content-type", "application/json");
  if (!in.head->key || !in.head->tls || (r && r->auth == caps::Auth::None)) return;   // never on a cleartext socket
  std::string bearer = std::string("Bearer") + ' ';   // one word: not model text (the catalog gate)
  bearer += in.head->key();
  if (bearer.size() > 7) out.headers.emplace_back("authorization", std::move(bearer));
}

R7_NOINLINE size_t reserveFor(const RenderInput& in, const plan::Plan& p, const plan::ProviderTools& pt) {
  size_t n = p.estimate + in.system->core.size() + in.system->notes.size() + in.system->digests.size() + 1024 + pt.bytes();
  for (const ToolSpec& t : *in.tools) n += t.name.size() + t.description.size() + t.schemaJson.size() + 96;
  return n;
}

R7_NOINLINE Status renderBody(const RenderInput& in, HttpRequest& out) {
  plan::Plan p;
  Status st = plan::plan(in, oneObject, p);
  if (!st.ok()) return st;
  if (in.tools->empty())   // the API refuses function_call items in a request without tools
    for (const plan::Piece& pc : p.pieces)
      if (pc.kind == plan::PieceKind::Call) return Status::fail(Err::InvalidArgument, "plain::openai.render.noTools");
  if (!p.anyBlock) return Status::fail(Err::InvalidArgument, "plain::openai.render.noMessages");
  plan::ProviderTools pt;
  plan::providerTools(in, pt);
  out.body.reserve(reserveFor(in, p, pt));
  const caps::Caps c = capsOf(in);
  BodyWriter w(out.body, &out.splices);
  envelope(w, in, c, p);
  if (!(st = toolsArray(w, in, pt)).ok()) return st;
  Render r{in, p, c.row, w, out.body};
  inputArray(r);
  w.endObject();
  if (!(st = w.status()).ok()) return st;
  placePoints(r.points, out);
  coordinates(in, out);
  return Status::success();
}

// ---- the parse half -------------------------------------------------------------------------------
//
// parse() reads one 2xx Responses answer with the strict JSON reader into a local step that reaches
// `out` only once the whole body was read: a cut, malformed or error body fills nothing.
//   message           its output_text parts joined into the text; a refusal part is a refusal
//   function_call     a call: its call_id as the call's id (function_call_output must name it; the
//                     item id is not kept), the name, the arguments string when it holds one object
//   reasoning         one MetaPart: {"type","id","summary","encrypted_content"} as served, before
//                     the call it preceded; one without encrypted content, or one a stored line
//                     cannot hold, is not kept
//   mcp_call          a provider-side connector ran (lane E2-P-CONN2): the step is outside content
//   other items       not ours to act on, passed over (mcp_list_tools, code_interpreter_call, ...); a
//                     reasoning item followed by one is not kept, since it is never sent without the item
//                     after it
// The stop table (design section 5 item 9): completed is EndTurn, or ToolUse when it holds calls,
// or MaxTokens when a call's arguments do not parse; incomplete with max_output_tokens is MaxTokens
// (also with no visible output, billed reasoning only), with content_filter a Refusal, with any
// other reason Other; a refusal part is a Refusal; failed is not an answer (the exchange's 2xx
// check calls it Transport before a parse). usage: input_tokens includes the cache on this wire, so it
// is maxRequestIn (the whole prompt); promptTokens is the uncached part, cached_tokens the reads and
// cache_write_tokens the writes (uncached + read + write = whole, the rule every codec keeps).

struct Item {
  std::string type, id, callId, name, args, enc, text, refusal;
  size_t summaryFrom = 0, summaryTo = 0;   // the summary list's bytes
  bool argsString = false, summaryList = false;
  uint32_t seen = 0;
};

struct Answer {
  std::vector<Item> items;
  std::string object, status, reason, model;
  nimbus::orch::TokenUsage usage;
  bool output = false, error = false;
  uint32_t seen = 0;
};

// A content part: type, text, refusal into s[0..2], then onto the item.
bool partKey(Ctx& c, const std::string& k) {
  if (k == "type") return optStr(c.j, c.s[0]);
  if (k == "text") return optStr(c.j, c.s[1]);
  if (k == "refusal") return optStr(c.j, c.s[2]);
  return c.j.skip();
}

bool contentPart(Ctx& c) {
  for (std::string& s : c.s) s.clear();
  if (!readObject(c, partKey)) return false;
  if (c.s[0] == "output_text") c.it->text += c.s[1];
  if (c.s[0] == "refusal") c.it->refusal += c.s[2].empty() ? std::string("refusal") : c.s[2];
  return true;
}

bool itemKey(Ctx& c, const std::string& key) {
  static const char* const kFields[] = {"type", "id", "call_id", "name", "arguments", "encrypted_content", "content", "summary"};
  Item& it = *c.it;
  uint32_t k = 0;
  if (!fieldOnce(key, kFields, 8, it.seen, k)) return false;
  std::string* const into[] = {&it.type, &it.id, &it.callId, &it.name, &it.args, &it.enc};
  if (k == 4) it.argsString = c.j.peek('"');
  if (k < 6) return optStr(c.j, *into[k]);
  if (k == 6) return c.j.peek('[') ? readList(c, contentPart) : c.j.skip();
  if (k != 7) return c.j.skip();
  it.summaryList = c.j.peek('[');
  it.summaryFrom = c.j.at();
  const bool ok = c.j.skip();
  it.summaryTo = c.j.at();
  return ok;
}

bool outputItem(Ctx& c) {
  c.a->items.emplace_back();
  c.it = &c.a->items.back();
  return readObject(c, itemKey);
}

bool detailsKey(Ctx& c, const std::string& k) {
  if (k == "cached_tokens") return c.j.count(c.a->usage.cacheReadTokens);
  if (k == "cache_write_tokens") return c.j.count(c.a->usage.cacheWriteTokens);
  return c.j.skip();
}

bool usageKey(Ctx& c, const std::string& k) {
  if (k == "input_tokens") return c.j.count(c.a->usage.promptTokens);
  if (k == "output_tokens") return c.j.count(c.a->usage.completionTokens);
  if (k == "input_tokens_details" && c.j.peek('{')) return readObject(c, detailsKey);
  return c.j.skip();
}

bool reasonKey(Ctx& c, const std::string& k) { return k == "reason" ? optStr(c.j, c.a->reason) : c.j.skip(); }

bool topKey(Ctx& c, const std::string& key) {
  static const char* const kFields[] = {"object", "status", "incomplete_details", "output", "usage", "model", "error"};
  Answer& a = *c.a;
  uint32_t k = 0;
  if (!fieldOnce(key, kFields, 7, a.seen, k)) return false;
  switch (k) {
    case 0: return optStr(c.j, a.object);
    case 1: return optStr(c.j, a.status);
    case 2: return c.j.peek('{') ? readObject(c, reasonKey) : c.j.skip();   // null on a completed answer
    case 3:
      a.output = c.j.peek('[');
      return a.output ? readList(c, outputItem) : c.j.skip();
    case 4: return c.j.peek('{') ? readObject(c, usageKey) : c.j.skip();
    case 5: return optStr(c.j, a.model);
    case 6:
      a.error = !c.j.peek('n');   // null on every answer that is one
      return c.j.skip();
    default: return c.j.skip();
  }
}

bool argKey(Ctx& c, const std::string& k) {
  if (std::find(c.keys.begin(), c.keys.end(), k) != c.keys.end()) return false;
  c.keys.push_back(k);
  return c.j.skip();
}

// Arguments the engine can run and store as written: one object, its keys once each.
R7_NOINLINE bool runnableArgs(const std::string& args) {
  JsonReader j(args);
  Ctx c{j};
  return readObject(c, argKey) && j.end() && j.peak() <= body::kMaxDepth;
}

// Whether one opaque item fits the k item that stores it (tx::kMaxLineBytes, newline included),
// counted from above: every byte as the line codec could escape it at most ('"' and '\\' 2, a
// control or non-ASCII byte 6), plus the widest q, turn, time, round and tag keys (256 B).
bool fitsALine(const std::string& bytes) {
  size_t n = 256;
  for (unsigned char ch : bytes) n += ch == '"' || ch == '\\' ? 2 : ch < 0x20 || ch >= 0x80 ? 6 : 1;
  return n <= tx::kMaxLineBytes;
}

// The reasoning item as it is stored and replayed: its id, its summary as served, its content.
R7_NOINLINE void reasoningOf(const std::string& body, const Item& it, ParsedStep& step) {
  if (it.enc.empty() || it.id.empty()) return;
  std::string bytes;
  BodyWriter w(bytes);
  putText(typed(w, "reasoning"), "id", it.id, Over::Refuse).key("summary");
  if (it.summaryList) w.raw(body.substr(it.summaryFrom, it.summaryTo - it.summaryFrom));
  else w.beginArray().endArray();
  putText(w, "encrypted_content", it.enc, Over::Refuse).endObject();
  if (!w.status().ok() || !fitsALine(bytes)) return;
  MetaPart m;
  m.beforeCall = (uint16_t)std::min<size_t>(step.calls.size(), UINT16_MAX);
  m.bytes = std::move(bytes);
  step.meta.push_back(std::move(m));
}

R7_NOINLINE void callOf(const Item& it, ParsedStep& step) {
  ToolCall c;
  c.id = it.callId;
  c.name = it.name;
  c.complete = it.argsString && runnableArgs(it.args);
  if (c.complete) c.argsJson = it.args;
  else c.argsJson.assign("{}", 2);
  step.calls.push_back(std::move(c));
}

R7_NOINLINE StopReason stopOf(const Answer& a, const ParsedStep& step, const std::string& refusal) {
  if (!refusal.empty()) return StopReason::Refusal;
  if (a.status == "incomplete") {
    if (a.reason == "max_output_tokens") return StopReason::MaxTokens;
    return a.reason == "content_filter" ? StopReason::Refusal : StopReason::Other;
  }
  if (a.status != "completed") return StopReason::Other;
  if (step.calls.empty()) return StopReason::EndTurn;
  for (const ToolCall& c : step.calls)
    if (!c.complete) return StopReason::MaxTokens;   // arguments that do not parse are a cut
  return StopReason::ToolUse;
}

// Whether item i is followed by an item this wire never sends back (a provider-side tool's item).
bool followedByUnsent(const Answer& a, size_t i) {
  if (i + 1 >= a.items.size()) return false;
  const std::string& next = a.items[i + 1].type;
  return next != "message" && next != "function_call" && next != "reasoning";
}

R7_NOINLINE void stepOf(const std::string& body, const Answer& a, ParsedStep& step) {
  std::string refusal;
  for (size_t i = 0; i < a.items.size(); ++i) {
    const Item& it = a.items[i];
    if (it.type == "message") {
      step.text += it.text;
      refusal += it.refusal;
    } else if (it.type == "function_call") {
      callOf(it, step);
    } else if (it.type == "reasoning") {
      if (!followedByUnsent(a, i)) reasoningOf(body, it, step);
    } else if (it.type == "mcp_call") {
      step.outside = true;
    }
  }
  step.stop = stopOf(a, step, refusal);
  if (step.stop != StopReason::ToolUse)
    for (ToolCall& c : step.calls) c.complete = false;   // only an answer that stops to call runs its calls
  if (step.stop == StopReason::Refusal) {
    step.calls.clear();   // a refused answer runs nothing
    step.refusal = refusal.empty() ? a.reason : refusal;
  }
  step.usage = a.usage;
  step.usage.responses = 1;
  step.usage.servedModel = a.model;
  // input_tokens holds the cache on this wire: it is the whole prompt, and promptTokens keeps the
  // uncached part (every codec's rule since lane E2-R6), never below 0.
  step.usage.maxRequestIn = a.usage.promptTokens;
  const uint64_t cached = (uint64_t)a.usage.cacheReadTokens + a.usage.cacheWriteTokens;
  step.usage.promptTokens = cached < a.usage.promptTokens ? (uint32_t)(a.usage.promptTokens - cached) : 0;
}

R7_NOINLINE Status parseBody(const HttpResponse& in, ParsedStep& out) {
  if (in.status < 200 || in.status > 299) return Status::fail(Err::InvalidArgument, "plain::openai.parse.status");
  if (!nimbus::utf8Valid(in.body)) return Status::fail(Err::InvalidArgument, "plain::openai.parse.utf8");
  JsonReader j(in.body);
  Answer a;
  Ctx c{j};
  c.a = &a;
  if (!readObject(c, topKey) || !j.end()) return Status::fail(Err::InvalidArgument, "plain::openai.parse.json");
  if (a.object != "response" || !a.output) return Status::fail(Err::InvalidArgument, "plain::openai.parse.notAResponse");
  if (a.status == "failed" || a.error) return Status::fail(Err::InvalidArgument, "plain::openai.parse.failed");
  for (const Item& it : a.items)   // a call that cannot be answered is not an answer
    if (it.type == "function_call" && (it.callId.empty() || it.name.empty()))
      return Status::fail(Err::InvalidArgument, "plain::openai.parse.callWithoutIdOrName");
  ParsedStep step;
  stepOf(in.body, a, step);
  out = std::move(step);
  return Status::success();
}

class ResponsesWire final : public WireCodec {
 public:
  Dialect dialect() const override { return Dialect::OpenAIResponses; }

  Status render(const RenderInput& in, HttpRequest& out) const override {
    out = HttpRequest{};
    if (!in.turn || !in.head || !in.system || !in.tools || !in.items || in.turn->model.empty())
      return Status::fail(Err::InvalidArgument, "plain::openai.render");
    Status st;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
      st = renderBody(in, out);
    } catch (const std::bad_alloc&) {   // ruling 30: a refused allocation is a status
      st = Status::fail(Err::TooLarge, "plain::openai.render.memory");
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
    } catch (const std::bad_alloc&) {   // ruling 30: nothing half-read
      st = Status::fail(Err::TooLarge, "plain::openai.parse.memory");
    }
#else
    st = parseBody(in, out);
#endif
    return st;
  }
};

}  // namespace

const WireCodec& responsesWire() {
  static const ResponsesWire wire;
  return wire;
}

}  // namespace plain
}  // namespace agent
