// test_plain_wire_anthropic - the Anthropic Messages wire of engine 2, render half (lane
// RB-W1a; PRD section 1 "Request on each wire" and section 2). The class rules, over
// random transcripts and every item kind rather than one instance:
//   - every body passes a strict local checker of the API's documented request shape (no
//     network): keys, block types and roles, the id and name patterns, non-empty text,
//     tool results first after their calls, at most four cache points, strict JSON, valid
//     UTF-8 and no string over the 60,000 B guard; the checker is counter-tested first;
//   - render is pure: the same transcript and facts give the same bytes on every call and
//     after a restart (every item re-read from its stored line);
//   - pairing on wire bytes whatever was stored: every tool call has exactly one result
//     right after it and no result stands alone (dangling calls, orphan and second results,
//     repeated and foreign ids, a note stored between a call and its result);
//   - the four cache points sit where RB-E2 and the PRD say (end of the notes block, end of
//     the system, last block of the previous turn, the moving last block) and nothing before
//     a point changes when a later item is appended;
//   - stubs, photo stubs and the omission line are the rules lane's texts, byte for byte;
//     photos follow the open turn's visible set (tx::visiblePhotos);
//   - a cut or broken call renders as {}; arguments, schemas and thinking go through raw();
//   - a refused allocation answers a status (ruling 30);
//   - the measured framing per block, for test_tx_overhead (ruling 16).
#include <ArduinoJson.h>
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../../lib/harness/src/providers/image_parts.h"
#include "../../lib/harness/src/providers/plain_body.h"
#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"
#include "../support/tx_battery/render.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/rules.h"
#include "nimbus/orch/wire_limits.h"
#include "nimbus/utf8.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace td = agent::plain::texts::tool_descriptions;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonPairConst;
using ArduinoJson::JsonVariantConst;

// ---- refused allocations (ruling 30) -------------------------------------------------------
static bool gArmed = false;
static long gAllocs = 0;
static long gFailAt = -1;
static bool gHit = false;
static bool refuseNow() {
  if (!gArmed) return false;
  if (gAllocs++ == gFailAt) {
    gHit = true;
    return true;
  }
  return false;
}
void* operator new(std::size_t n) {
  if (refuseNow()) throw std::bad_alloc();
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void setUp() {}
void tearDown() {}

namespace {

constexpr const char* kMark = ",\"cache_control\":{\"type\":\"ephemeral\"}";

// ---- the rig -----------------------------------------------------------------------------------
struct Opts {
  uint64_t pointer = 0;
  plain::LadderState ladder;   // the engine's overflow ladder (RenderInput::ladder)
  bool none = false;
  uint32_t maxTokens = 4096;
  bool inlinePhotos = false;
  std::string mime = "image/jpeg";
  const char* answerSchema = nullptr;   // lane E2-CMP: the summarizer's structured form
};

struct Out {
  agent::HttpRequest req;
  std::string body;   // materialized (photos spliced in)
  sp::RequestView view;
};

struct Rig {
  txb::RenderFixture fx;
  std::map<std::string, std::string> photos;   // ref -> jpeg bytes
  Rig() {
    fx.head.effort = "medium";
    photos = fx.photoBytes;
  }
  void addPhoto(const std::string& ref) {
    photos[ref] = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(60 + ref.size(), (char)('a' + ref.size() % 20));
  }

  tx::Status render(const txb::Convo& c, Out& out, const Opts& o = Opts(), uint32_t openTurn = 0) const {
    tx::TxArena arena;
    tx::Status st = arena.init(1 << 22, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); });
    if (!st.ok()) return st;
    for (const tx::Item& it : c.items)
      if (!(st = arena.append(it)).ok()) return st;
    return renderArena(c, arena, out, o, openTurn);
  }

  plain::RenderInput input(const txb::Convo& c, const tx::TxArena& arena, const Opts& o, plain::TurnCtx& turn,
                           uint32_t openTurn) const {
    turn = fx.turn;
    turn.turn = openTurn ? openTurn : c.turn;
    plain::RenderInput in;
    in.turn = &turn;
    in.head = &fx.head;
    in.system = &fx.system;
    in.tools = &fx.tools;
    in.items = &arena;
    in.stubPointer = o.pointer;
    in.ladder = &o.ladder;
    in.toolChoiceNone = o.none;
    in.answerSchema = o.answerSchema;
    in.maxTokens = o.maxTokens;
    const auto* bulk = &c.bulk;
    in.bulk = [bulk](const std::string& handle, std::string& s) {
      auto it = bulk->find(handle);
      if (it == bulk->end()) return tx::Status::fail(tx::Err::NotFound, "rig bulk");
      s = it->second;
      return tx::Status::success();
    };
    const auto* ph = &photos;
    const bool inl = o.inlinePhotos;
    const std::string mime = o.mime;
    in.photo = [ph, inl, mime](const std::string& ref, plain::PhotoBytes& p) {
      auto it = ph->find(ref);
      if (it == ph->end()) return tx::Status::fail(tx::Err::NotFound, "rig photo");
      p.bytes = (const uint8_t*)it->second.data();
      p.len = it->second.size();
      p.mime = mime;
      p.fileId = inl ? "" : "file_" + ref;
      return tx::Status::success();
    };
    return in;
  }

  // `in.ladder` points into `o`: a temporary Opts would leave it dangling once the call returns.
  plain::RenderInput input(const txb::Convo&, const tx::TxArena&, const Opts&&, plain::TurnCtx&, uint32_t) const = delete;

  tx::Status renderArena(const txb::Convo& c, const tx::TxArena& arena, Out& out, const Opts& o,
                         uint32_t openTurn) const {
    const plain::WireCodec* wire = nullptr;
    tx::Status st = plain::renderWireFor(plain::Dialect::AnthropicMessages, wire);
    if (!st.ok()) return st;
    plain::TurnCtx turn;
    const plain::RenderInput in = input(c, arena, o, turn, openTurn);
    if (!(st = wire->render(in, out.req)).ok()) return st;
    out.body = agent::materializeBody(out.req);
    sp::parseRequest(sp::Dialect::Anthropic, out.body, out.view);
    return tx::Status::success();
  }
};

#define RENDER_OK(rig, c, out, ...) TXB_OK((rig).render((c), (out), ##__VA_ARGS__))

// ---- the strict checker: the API's documented request shape ------------------------------------
namespace chk {

struct C {
  std::string why;
  bool fail(const std::string& w) {
    if (why.empty()) why = w;
    return false;
  }
};

bool pattern(const char* s, size_t max) {
  const size_t n = s ? std::strlen(s) : 0;
  if (!n || n > max) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  return true;
}
// White space as the API reads it: ASCII and the Unicode space characters.
bool visibleText(const char* s) {
  static const char* const kSpaces[] = {"\xC2\x85", "\xC2\xA0", "\xE1\x9A\x80", "\xE2\x80\xA8", "\xE2\x80\xA9",
                                        "\xE2\x80\xAF", "\xE2\x81\x9F", "\xE3\x80\x80"};
  while (s && *s) {
    if (std::isspace((unsigned char)*s)) {
      ++s;
      continue;
    }
    const unsigned char* u = (const unsigned char*)s;
    if (u[0] == 0xE2 && u[1] == 0x80 && u[2] >= 0x80 && u[2] <= 0x8A) {   // U+2000 to U+200A
      s += 3;
      continue;
    }
    size_t n = 0;
    for (const char* sp : kSpaces)
      if (std::strncmp(s, sp, std::strlen(sp)) == 0) n = std::strlen(sp);
    if (!n) return true;
    s += n;
  }
  return false;
}
bool onlyKeys(JsonObjectConst o, std::initializer_list<const char*> keys, C& c, const std::string& where) {
  for (JsonPairConst kv : o) {
    bool known = false;
    for (const char* k : keys) known = known || kv.key() == k;
    if (!known) return c.fail(where + ": unknown key '" + kv.key().c_str() + "'");
  }
  return true;
}
// A cache point: {"type":"ephemeral"} (ttl 5m or 1h allowed). Counts it.
bool cacheOk(JsonObjectConst o, C& c, const std::string& where, int& marks) {
  JsonVariantConst cc = o["cache_control"];
  if (cc.isNull()) return true;
  ++marks;
  JsonObjectConst m = cc.as<JsonObjectConst>();
  if (m.isNull() || std::string(m["type"] | "") != "ephemeral") return c.fail(where + ": cache_control is not ephemeral");
  if (!m["ttl"].isNull() && std::string(m["ttl"] | "") != "5m" && std::string(m["ttl"] | "") != "1h")
    return c.fail(where + ": bad ttl");
  return onlyKeys(m, {"type", "ttl"}, c, where + ".cache_control");
}
bool base64Ok(const char* s) {
  const size_t n = s ? std::strlen(s) : 0;
  if (!n || n % 4) return false;
  for (size_t i = 0; i < n; ++i) {
    const char ch = s[i];
    if (!(std::isalnum((unsigned char)ch) || ch == '+' || ch == '/' || (ch == '=' && i + 2 >= n))) return false;
  }
  return true;
}

struct Seen {
  int marks = 0;
  bool fileSource = false;
  bool anyToolBlock = false;
};

bool textBlock(JsonObjectConst b, C& c, const std::string& w, Seen& s) {
  if (!b["text"].is<const char*>() || !visibleText(b["text"])) return c.fail(w + ": text empty or whitespace only");
  return onlyKeys(b, {"type", "text", "cache_control"}, c, w) && cacheOk(b, c, w, s.marks);
}
bool imageBlock(JsonObjectConst b, C& c, const std::string& w, Seen& s) {
  JsonObjectConst src = b["source"];
  const std::string type = src["type"] | "";
  if (type == "base64") {
    const std::string mt = src["media_type"] | "";
    if (mt != "image/jpeg" && mt != "image/png" && mt != "image/gif" && mt != "image/webp")
      return c.fail(w + ": media_type " + mt);
    if (!base64Ok(src["data"] | "")) return c.fail(w + ": data is not base64");
    if (!onlyKeys(src, {"type", "media_type", "data"}, c, w + ".source")) return false;
  } else if (type == "file") {
    if (!*(src["file_id"] | "")) return c.fail(w + ": empty file_id");
    s.fileSource = true;
    if (!onlyKeys(src, {"type", "file_id"}, c, w + ".source")) return false;
  } else {
    return c.fail(w + ": image source '" + type + "'");
  }
  return onlyKeys(b, {"type", "source", "cache_control"}, c, w) && cacheOk(b, c, w, s.marks);
}
bool toolUse(JsonObjectConst b, C& c, const std::string& w, Seen& s, std::set<std::string>& ids) {
  if (!pattern(b["id"] | "", 256)) return c.fail(w + ": tool_use id pattern");
  if (!pattern(b["name"] | "", 64)) return c.fail(w + ": tool_use name pattern");
  if (!b["input"].is<JsonObjectConst>()) return c.fail(w + ": tool_use input is not an object");
  if (!ids.insert(b["id"] | "").second) return c.fail(w + ": tool_use id repeated");
  s.anyToolBlock = true;
  return onlyKeys(b, {"type", "id", "name", "input", "cache_control"}, c, w) && cacheOk(b, c, w, s.marks);
}
bool toolResult(JsonObjectConst b, C& c, const std::string& w, Seen& s) {
  if (!pattern(b["tool_use_id"] | "", 256)) return c.fail(w + ": tool_use_id pattern");
  JsonVariantConst content = b["content"];
  if (!content.isNull() && !content.is<const char*>()) {
    if (!content.is<JsonArrayConst>()) return c.fail(w + ": content is neither a string nor blocks");
    for (JsonObjectConst p : content.as<JsonArrayConst>())
      if (std::string(p["type"] | "") != "text" || !textBlock(p, c, w + ".content", s)) return c.fail(w + ": content block");
  }
  if (!b["is_error"].isNull() && !b["is_error"].is<bool>()) return c.fail(w + ": is_error is not a bool");
  if ((b["is_error"] | false) && (content.isNull() || std::string(content | "x").empty()))
    return c.fail(w + ": an error result with no content");
  s.anyToolBlock = true;
  return onlyKeys(b, {"type", "tool_use_id", "content", "is_error", "cache_control"}, c, w) && cacheOk(b, c, w, s.marks);
}

bool thinkingBlock(JsonObjectConst b, const std::string& type, C& c, const std::string& w) {
  if (!b["cache_control"].isNull()) return c.fail(w + ": cache_control on a thinking block");
  if (type == "redacted_thinking")
    return b["data"].is<const char*>() ? onlyKeys(b, {"type", "data"}, c, w) : c.fail(w + ": redacted_thinking without data");
  return b["thinking"].is<const char*>() && b["signature"].is<const char*>()
             ? onlyKeys(b, {"type", "thinking", "signature"}, c, w)
             : c.fail(w + ": thinking without its text or signature");
}

bool block(JsonObjectConst b, const std::string& role, C& c, const std::string& w, Seen& s, std::set<std::string>& ids) {
  const std::string type = b["type"] | "";
  const bool user = role == "user";
  if (type == "text") return textBlock(b, c, w, s);
  if (type == "image") return user ? imageBlock(b, c, w, s) : c.fail(w + ": image in an assistant message");
  if (type == "tool_use") return !user ? toolUse(b, c, w, s, ids) : c.fail(w + ": tool_use in a user message");
  if (type == "tool_result") return user ? toolResult(b, c, w, s) : c.fail(w + ": tool_result from the assistant");
  if (type == "thinking" || type == "redacted_thinking")
    return !user ? thinkingBlock(b, type, c, w) : c.fail(w + ": thinking in a user message");
  return c.fail(w + ": unknown block type '" + type + "'");
}

struct Turn {
  std::string role;
  std::vector<JsonObjectConst> blocks;
};

std::string typeOf(JsonObjectConst b) { return b["type"] | ""; }

// An assistant turn: not thinking alone (interleaved thinking keeps its place); its call ids.
bool assistantTurn(const Turn& t, size_t i, C& c, std::vector<std::string>& calls) {
  bool other = false;
  for (JsonObjectConst b : t.blocks) {
    const bool th = typeOf(b) == "thinking" || typeOf(b) == "redacted_thinking";
    other = other || !th;
    if (typeOf(b) == "tool_use") calls.push_back(b["id"] | "");
  }
  return other ? true : c.fail("turn " + std::to_string(i) + ": an assistant turn of thinking alone");
}

bool calledBefore(const std::vector<Turn>& turns, size_t i, const std::string& id) {
  if (i == 0) return false;
  for (JsonObjectConst p : turns[i - 1].blocks)
    if (typeOf(p) == "tool_use" && std::string(p["id"] | "") == id) return true;
  return false;
}

// A user turn: results first, each answering a call of the turn before it.
bool userTurn(const std::vector<Turn>& turns, size_t i, C& c) {
  bool other = false;
  for (JsonObjectConst b : turns[i].blocks) {
    if (typeOf(b) != "tool_result") {
      other = true;
      continue;
    }
    if (other) return c.fail("turn " + std::to_string(i) + ": a tool_result after another block");
    if (!calledBefore(turns, i, b["tool_use_id"] | ""))
      return c.fail(std::string("tool_result ") + (b["tool_use_id"] | "") + " answers no call of the turn before it");
  }
  return true;
}

// The calls of turn i are exactly the results that open turn i + 1.
bool answered(const std::vector<Turn>& turns, size_t i, std::vector<std::string> calls, C& c) {
  if (i + 1 >= turns.size() || turns[i + 1].role != "user")
    return c.fail("calls of turn " + std::to_string(i) + " have no results after them");
  std::vector<std::string> results;
  for (JsonObjectConst b : turns[i + 1].blocks) {
    if (typeOf(b) != "tool_result") break;
    results.push_back(b["tool_use_id"] | "");
  }
  std::sort(calls.begin(), calls.end());
  std::sort(results.begin(), results.end());
  return calls == results ? true : c.fail("the results opening turn " + std::to_string(i + 1) + " are not exactly the calls before them");
}

// The API combines consecutive messages of one role: check pairing on the combined turns.
bool pairing(const std::vector<Turn>& turns, C& c) {
  for (size_t i = 0; i < turns.size(); ++i) {
    std::vector<std::string> calls;
    if (turns[i].role == "user" ? !userTurn(turns, i, c) : !assistantTurn(turns[i], i, c, calls)) return false;
    if (!calls.empty() && !answered(turns, i, calls, c)) return false;
  }
  return true;
}

bool messages(JsonArrayConst ms, C& c, Seen& s) {
  if (ms.isNull() || ms.size() == 0) return c.fail("messages missing or empty");
  std::vector<Turn> turns;
  std::set<std::string> ids;
  size_t i = 0;
  for (JsonObjectConst m : ms) {
    const std::string w = "messages[" + std::to_string(i++) + "]";
    const std::string role = m["role"] | "";
    if (role != "user" && role != "assistant") return c.fail(w + ": role '" + role + "'");
    if (!onlyKeys(m, {"role", "content"}, c, w)) return false;
    JsonArrayConst content = m["content"];
    if (content.isNull() || content.size() == 0) return c.fail(w + ": content missing or empty");
    if (turns.empty() || turns.back().role != role) turns.push_back(Turn{role, {}});
    size_t k = 0;
    for (JsonObjectConst b : content) {
      if (!block(b, role, c, w + ".content[" + std::to_string(k++) + "]", s, ids)) return false;
      turns.back().blocks.push_back(b);
    }
  }
  if (turns.front().role != "user") return c.fail("the first message is not the user's");
  if (turns.back().role != "user") return c.fail("the last message is not the user's (no prefill)");
  return pairing(turns, c);
}

bool system(JsonVariantConst sys, C& c, Seen& s) {
  if (sys.isNull()) return true;
  if (!sys.is<JsonArrayConst>()) return c.fail("system is not an array of blocks");
  size_t i = 0;
  for (JsonObjectConst b : sys.as<JsonArrayConst>()) {
    const std::string w = "system[" + std::to_string(i++) + "]";
    if (std::string(b["type"] | "") != "text" || !textBlock(b, c, w, s)) return c.fail(w + ": not a text block");
  }
  return true;
}

bool toolEntry(JsonObjectConst t, const std::string& w, C& c, Seen& s, std::set<std::string>& names) {
  if (!pattern(t["name"] | "", 64)) return c.fail(w + ": name pattern");
  if (!names.insert(t["name"] | "").second) return c.fail(w + ": name repeated");
  if (!t["description"].isNull() && !t["description"].is<const char*>()) return c.fail(w + ": description");
  if (std::string(t["input_schema"]["type"] | "") != "object") return c.fail(w + ": input_schema is not an object schema");
  return onlyKeys(t, {"name", "description", "input_schema", "cache_control"}, c, w) && cacheOk(t, c, w, s.marks);
}

bool toolChoice(JsonObjectConst tc, const std::set<std::string>& names, C& c) {
  if (tc.isNull()) return true;
  const std::string type = tc["type"] | "";
  if (type != "auto" && type != "any" && type != "none" && type != "tool") return c.fail("tool_choice type " + type);
  if (type == "tool" && !names.count(tc["name"] | "")) return c.fail("tool_choice names no tool");
  return onlyKeys(tc, {"type", "name", "disable_parallel_tool_use"}, c, "tool_choice");
}

bool tools(JsonObjectConst root, C& c, Seen& s) {
  JsonVariantConst ts = root["tools"];
  if (ts.isNull()) return root["tool_choice"].isNull() ? true : c.fail("tool_choice without tools");
  std::set<std::string> names;
  size_t i = 0;
  for (JsonObjectConst t : ts.as<JsonArrayConst>())
    if (!toolEntry(t, "tools[" + std::to_string(i++) + "]", c, s, names)) return false;
  return toolChoice(root["tool_choice"], names, c);
}

bool headerMap(const agent::HttpRequest& r, std::map<std::string, std::string>& h, C& c) {
  for (const auto& kv : r.headers) {
    std::string k = kv.first;
    for (char& ch : k) ch = (char)std::tolower((unsigned char)ch);
    if (!h.emplace(k, kv.second).second) return c.fail("header repeated: " + k);
  }
  return true;
}

bool headers(const agent::HttpRequest& r, bool fileSource, C& c) {
  if (r.method != "POST") return c.fail("method " + r.method);
  if (r.path.size() < 9 || r.path.compare(r.path.size() - 9, 9, "/messages") != 0) return c.fail("path " + r.path);
  std::map<std::string, std::string> h;
  if (!headerMap(r, h, c)) return false;
  if (h["content-type"] != "application/json" || h["anthropic-version"] != "2023-06-01")
    return c.fail("content-type or anthropic-version");
  const bool beta = h["anthropic-beta"].find("files-api-2025-04-14") != std::string::npos;
  if (fileSource != beta) return c.fail(fileSource ? "a file source without the files beta" : "the files beta without a file source");
  const size_t keys = h.count("x-api-key") + h.count("authorization");
  if (r.tls ? keys != 1 : keys != 0) return c.fail(r.tls ? "not exactly one key header" : "a key on a cleartext socket");
  return true;
}

// The bytes: no raw control byte, valid UTF-8, every string inside the guard.
bool bytesOk(const agent::HttpRequest& r, const std::string& body, C& c) {
  for (unsigned char ch : body)
    if (ch < 0x20) return c.fail("a raw control byte in the body");
  if (!nimbus::utf8Valid(body)) return c.fail("the body is not valid UTF-8");
  if (!plain::body::detail::stringsWithinGuard(r.body, nimbus::orch::kJsonStrGuard))
    return c.fail("a string over the 60,000 B guard");
  return true;
}

// output_config: an effort the API knows and/or (lane E2-CMP) a structured form
// {"type":"json_schema","schema":{...}}, nothing else.
bool outputConfigOk(JsonObjectConst oc, C& c) {
  if (oc["effort"].isNull() && oc["format"].isNull()) return c.fail("output_config holds neither effort nor format");
  const std::string e = oc["effort"] | "";
  if (!oc["effort"].isNull() && e != "low" && e != "medium" && e != "high" && e != "max") return c.fail("effort '" + e + "'");
  if (!oc["format"].isNull()) {
    if (std::string(oc["format"]["type"] | "") != "json_schema" || !oc["format"]["schema"].is<JsonObjectConst>())
      return c.fail("output_config.format is not a json_schema");
    if (!onlyKeys(oc["format"], {"type", "schema"}, c, "output_config.format")) return false;
  }
  return onlyKeys(oc, {"effort", "format"}, c, "output_config");
}

bool envelope(JsonObjectConst root, C& c) {
  if (root.isNull()) return c.fail("the body is not an object");
  if (!onlyKeys(root, {"model", "max_tokens", "system", "tools", "tool_choice", "messages", "output_config"}, c, "body"))
    return false;
  if (std::string(root["model"] | "").empty()) return c.fail("model missing");
  if (!root["max_tokens"].is<unsigned>() || (root["max_tokens"] | 0u) < 1) return c.fail("max_tokens missing or 0");
  return root["output_config"].isNull() || outputConfigOk(root["output_config"], c);
}

// The whole request. `body` is the materialized body (photos spliced in).
bool request(const agent::HttpRequest& r, const std::string& body, std::string& why) {
  C c;
  JsonDocument d;
  Seen s;
  bool ok = bytesOk(r, body, c);
  ok = ok && (!deserializeJson(d, body, ArduinoJson::DeserializationOption::NestingLimit(64)) || c.fail("not JSON"));
  JsonObjectConst root = d.as<JsonObjectConst>();
  ok = ok && envelope(root, c) && system(root["system"], c, s) && tools(root, c, s) && messages(root["messages"], c, s);
  ok = ok && (s.marks <= 4 || c.fail(std::to_string(s.marks) + " cache points (at most 4)"));
  ok = ok && (!s.anyToolBlock || !root["tools"].isNull() || c.fail("tool blocks without tools"));
  ok = ok && headers(r, s.fileSource, c);
  why = c.why;
  return ok;
}

}  // namespace chk

#define EXPECT_API_OK(out, ctx)                                                         \
  do {                                                                                  \
    std::string why_;                                                                   \
    const bool ok_ = chk::request((out).req, (out).body, why_);                         \
    TEST_ASSERT_TRUE_MESSAGE(ok_, (std::string(ctx) + ": " + why_).c_str());            \
  } while (0)

// ---- random transcripts ------------------------------------------------------------------------
struct GenOpts {
  bool broken = false;    // inject stored-state faults (pairing, ids)
  bool dirty = false;     // invalid UTF-8 and control bytes in stored text
  bool outside = true;    // outside results
  bool thinking = true;   // k items
  bool photos = true;
  int closedTurns = -1;   // -1: random 0..5
};

struct Gen {
  std::mt19937 rng;
  GenOpts o;
  Rig* rig;
  txb::Convo c;
  explicit Gen(unsigned seed, GenOpts opts, Rig* r) : rng(seed), o(opts), rig(r) {}
  int pick(int n) { return (int)(rng() % (unsigned)n); }
  bool chance(int pct) { return pick(100) < pct; }

  std::string text(const std::string& head) {
    static const char* bits[] = {"plain words", "caf\xC3\xA9 au lait", "quote \" and \\ back", "line\nbreak",
                                 "tab\tstop", "emoji \xF0\x9F\x9A\xB2", "[brackets]", "</tag>"};
    std::string s = head;
    for (int i = pick(4); i >= 0; --i) s += std::string(" ") + bits[pick(8)];
    if (o.dirty && chance(30)) s += chance(50) ? std::string("\xC3", 1) : std::string("\x01\x7F", 2);
    return s;
  }
  std::string args() {
    if (o.broken && chance(10)) return chance(50) ? R"({"path":"notes/a.md)" : "[1,2]";
    return "{\"path\":\"notes/" + std::to_string(pick(9)) + ".md\",\"n\":" + std::to_string(pick(1000)) + "}";
  }
  void result(const std::string& id, const std::string& name) {
    const int k = o.outside ? pick(10) : pick(7);
    if (k < 4) c.receipt(id, name, text("receipt of " + id), !chance(15));
    else if (k < 7) c.bulkResult(id, name, txb::payload("BULK-" + id, 300 + pick(3000)));
    else c.outside(id, "web_search", R"({"q":"x"})", txb::payload("OUT-" + id, 200 + pick(2000)));
    if (o.dirty && chance(20)) c.items.back().x += std::string("\xE2\x82", 2);
  }
  void meta() {
    tx::Item& k = c.add(tx::ItemKind::RoundMeta);
    k.x = "{\"type\":\"thinking\",\"thinking\":\"plan " + std::to_string(c.q) + "\",\"signature\":\"sig" +
          std::to_string(c.q) + "\"}";
    k.rd = c.round;
  }
  std::vector<std::string> calls() {
    std::vector<std::string> ids;
    for (int i = 1 + pick(3); i > 0; --i) {
      c.call(chance(50) ? "read_file" : "list_files", args(), chance(30));
      if (o.broken && chance(8)) c.items.back().id = chance(50) ? "call.1:x/" + std::to_string(c.q) : "";
      if (o.broken && chance(8)) c.items.back().id = "call_" + std::to_string(i);   // numbered per answer
      if (o.broken && chance(5)) c.items.back().n = chance(50) ? "files.read" : "";
      ids.push_back(c.items.back().id);
    }
    return ids;
  }
  // Broken stored states: a dangling call, a second result, an orphan, a repeated call id.
  void results(const std::vector<std::string>& ids) {
    for (const std::string& id : ids) {
      if (o.broken && chance(8)) continue;
      result(id, "read_file");
      if (o.broken && chance(6)) c.receipt(id, "read_file", "second");
    }
    if (!o.broken) return;
    if (chance(6)) c.receipt("toolu_orphan" + std::to_string(c.q), "read_file", "orphan");
    if (chance(5)) {
      c.call("read_file", "{}");
      c.items.back().id = ids[0];
      c.receipt(ids[0], "read_file", "repeat");
    }
  }
  void round() {
    if (o.thinking && chance(50)) meta();
    if (chance(40)) c.say(o.broken && chance(15) ? std::string("  \n") : text("working"), false);
    const std::vector<std::string> ids = calls();
    if (o.broken && chance(15)) c.note("[device] a note between a call and its result");
    results(ids);
    if (chance(15)) c.note("[device] " + text("note"));
    c.nextRound();
  }
  void turn(bool open) {
    c.advance(30 + pick(600));
    c.openTurn();
    std::vector<std::string> refs;
    if (o.photos && chance(30)) {
      for (int i = 1 + pick(2); i > 0; --i) {
        char ref[16];
        std::snprintf(ref, sizeof ref, "p%08x", (unsigned)rng());
        refs.push_back(ref);
        rig->addPhoto(ref);
      }
    }
    tx::Item& t = c.items.back();
    t.photos = refs;
    if (o.photos && chance(30)) t.photos.push_back("p0ld0001");   // an older photo still in view
    rig->addPhoto("p0ld0001");
    c.owner(text("ask " + std::to_string(c.turn) + ":"), refs);
    if (chance(10)) c.items.back().parts.push_back(tx::Part{tx::PartKind::Image, "p0ld0001", "image/jpeg", 9000});
    const int rounds = pick(4);
    for (int r = 0; r < rounds; ++r) round();
    if (!open) {
      c.say(text("answer"));
      c.close();
    }
  }
  txb::Convo build() {
    const int closed = o.closedTurns >= 0 ? o.closedTurns : pick(6);
    for (int i = 0; i < closed; ++i) turn(false);
    turn(true);
    return c;
  }
};

txb::Convo randomConvo(unsigned seed, Rig& rig, GenOpts o = GenOpts()) { return Gen(seed, o, &rig).build(); }

// ---- body helpers ------------------------------------------------------------------------------
std::string stripMarks(const std::string& body) {
  std::string out = body;
  size_t at;
  while ((at = out.find(kMark)) != std::string::npos) out.erase(at, std::strlen(kMark));
  return out;
}
// The offsets (in the body with every mark removed) where each mark sat.
std::vector<size_t> markOffsets(const std::string& body) {
  std::vector<size_t> out;
  size_t at = 0, removed = 0;
  while ((at = body.find(kMark, at)) != std::string::npos) {
    out.push_back(at - removed);
    removed += std::strlen(kMark);
    at += std::strlen(kMark);
  }
  return out;
}

struct Where {
  bool system = false;
  size_t msg = 0, block = 0;
};
std::vector<Where> marksOf(const std::string& body) {
  JsonDocument d;
  deserializeJson(d, body);
  std::vector<Where> out;
  size_t i = 0;
  for (JsonObjectConst b : d["system"].as<JsonArrayConst>()) {
    if (!b["cache_control"].isNull()) out.push_back(Where{true, 0, i});
    ++i;
  }
  size_t m = 0;
  for (JsonObjectConst msg : d["messages"].as<JsonArrayConst>()) {
    size_t k = 0;
    for (JsonObjectConst b : msg["content"].as<JsonArrayConst>()) {
      if (!b["cache_control"].isNull()) out.push_back(Where{false, m, k});
      ++k;
    }
    ++m;
  }
  return out;
}

bool cacheable(const sp::Block& b) { return b.kind != sp::BlockKind::Thinking; }
// The last cacheable block of messages [0, end).
bool lastCacheable(const sp::RequestView& v, size_t end, Where& w) {
  for (size_t m = end; m-- > 0;)
    for (size_t k = v.messages[m].blocks.size(); k-- > 0;)
      if (cacheable(v.messages[m].blocks[k])) {
        w = Where{false, m, k};
        return true;
      }
  return false;
}
size_t messageWith(const sp::RequestView& v, const std::string& text) {
  for (size_t m = 0; m < v.messages.size(); ++m)
    for (const sp::Block& b : v.messages[m].blocks)
      if (b.kind == sp::BlockKind::Text && b.text.find(text) != std::string::npos) return m;
  return v.messages.size();
}

// Every stored owner text is valid when the generator is not dirty: covers() reads them.
txb::CoverInput coverOf(const txb::Convo& c, uint64_t pointer, uint32_t openTurn) {
  txb::CoverInput in;
  in.items = &c.items;
  in.bulk = &c.bulk;
  in.pointer = pointer;
  in.openTurn = openTurn ? openTurn : c.turn;
  return in;
}

// The message stamp as tx::messageStamp writes it (RB-REC5: with the device's day start).
std::string stampOf(uint64_t t, int16_t tz, uint16_t dayStart) {
  char stamp[tx::kStampMaxBytes];
  TXB_OK(tx::messageStamp(t, tz, dayStart, stamp, sizeof stamp));
  return stamp;
}

// ---- 1. the checker bites ------------------------------------------------------------------------
agent::HttpRequest reqOf(const std::string& body) {
  agent::HttpRequest r;
  r.method = "POST";
  r.path = "/v1/messages";
  r.body = body;
  r.headers = {{"content-type", "application/json"}, {"anthropic-version", "2023-06-01"}, {"x-api-key", "k"}};
  return r;
}

void test_the_checker_accepts_the_documented_shape_and_rejects_what_the_api_refuses() {
  const std::string good =
      R"({"model":"m","max_tokens":10,"system":[{"type":"text","text":"core","cache_control":{"type":"ephemeral"}}],)"
      R"("tools":[{"name":"read_file","description":"d","input_schema":{"type":"object"}}],"tool_choice":{"type":"auto"},)"
      R"("messages":[{"role":"user","content":[{"type":"text","text":"hi"}]},)"
      R"({"role":"assistant","content":[{"type":"thinking","thinking":"t","signature":"s"},{"type":"tool_use","id":"toolu_1","name":"read_file","input":{}}]},)"
      R"({"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_1","content":"x"},{"type":"text","text":"more"}]}]})";
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(chk::request(reqOf(good), good, why), why.c_str());
  struct Bad {
    const char* from;
    const char* to;
  };
  const Bad bad[] = {
      {R"("max_tokens":10,)", ""},                                                          // max_tokens missing
      {R"("model":"m")", R"("model":"")"},                                                  // no model
      {R"("text":"hi")", R"("text":"  ")"},                                                 // whitespace-only text
      {R"("text":"hi")", R"("text":"")"},                                                   // empty text
      {R"("tool_use_id":"toolu_1")", R"("tool_use_id":"toolu_2")"},                        // result answers nothing
      {R"({"type":"tool_result","tool_use_id":"toolu_1","content":"x"},{"type":"text","text":"more"})",
       R"({"type":"text","text":"more"},{"type":"tool_result","tool_use_id":"toolu_1","content":"x"})"},  // result after text
      {R"(,{"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_1","content":"x"},{"type":"text","text":"more"}]})",
       R"(,{"role":"user","content":[{"type":"text","text":"more"}]})"},                    // call without its result
      {R"("id":"toolu_1")", R"("id":"toolu.1")"},                                           // id pattern
      {R"("name":"read_file","input")", R"("name":"read file","input")"},                   // name pattern
      {R"("input":{})", R"("input":[])"},                                                   // input not an object
      {R"({"type":"thinking","thinking":"t","signature":"s"},)", ""},                       // fine: removing thinking
      {R"("type":"ephemeral")", R"("type":"forever")"},                                    // cache type
      {R"("tool_choice":{"type":"auto"})", R"("tool_choice":{"type":"sometimes"})"},       // tool_choice
      {R"("messages":[{"role":"user")", R"("messages":[{"role":"system")"},                 // role
      {R"({"role":"user","content":[{"type":"text","text":"hi"}]},)", ""},                 // first message assistant
      {R"({"type":"text","text":"more"}]}]})", R"({"type":"text","text":"more"}]},{"role":"assistant","content":[{"type":"text","text":"pre"}]}]})"},  // prefill
      {R"("input_schema":{"type":"object"})", R"("input_schema":{"type":"string"})"},     // schema
      {R"("system":[{"type":"text")", R"("system":[{"type":"image")"},                     // system block
      {R"("max_tokens":10,)", R"("max_tokens":10,"temperature_x":1,)"},                    // unknown key
      {R"({"type":"thinking","thinking":"t","signature":"s"})", R"({"type":"thinking","thinking":"t","signature":"s","cache_control":{"type":"ephemeral"}})"},
      {R"("tools":[{"name":"read_file","description":"d","input_schema":{"type":"object"}}],"tool_choice":{"type":"auto"},)", ""},  // tool blocks without tools
      {R"({"type":"text","text":"hi"})", R"({"type":"image","source":{"type":"base64","media_type":"image/tiff","data":"AAAA"}})"},
      {R"({"type":"text","text":"hi"})", R"({"type":"image","source":{"type":"base64","media_type":"image/jpeg","data":"A@A"}})"},
      {R"("text":"hi")", "\"text\":\"h\x01i\""},                                            // a raw control byte
      {R"("text":"hi")", "\"text\":\"h\xC3i\""},                                            // invalid UTF-8
      {R"("content":"x"})", R"("content":"","is_error":true})"},                          // an error with no content
      {R"({"type":"thinking","thinking":"t","signature":"s"},{"type":"tool_use","id":"toolu_1","name":"read_file","input":{}}]},)",
       R"({"type":"thinking","thinking":"t","signature":"s"}]},{"role":"user","content":[{"type":"text","text":"y"}]},{"role":"assistant","content":[{"type":"thinking","thinking":"t","signature":"s"},{"type":"tool_use","id":"toolu_1","name":"read_file","input":{}}]},)"},
  };
  int caught = 0;
  for (const Bad& b : bad) {
    std::string body = good;
    const size_t at = body.find(b.from);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, b.from);
    body.replace(at, std::strlen(b.from), b.to);
    why.clear();
    const bool ok = chk::request(reqOf(body), body, why);
    if (std::string(b.from) == R"({"type":"thinking","thinking":"t","signature":"s"},)" && !*b.to) {
      TEST_ASSERT_TRUE_MESSAGE(ok, why.c_str());   // dropping thinking is allowed
      continue;
    }
    TEST_ASSERT_FALSE_MESSAGE(ok, (std::string("accepted: ") + b.from + " -> " + b.to).c_str());
    ++caught;
  }
  TEST_ASSERT_EQUAL((int)(sizeof bad / sizeof bad[0]) - 1, caught);
  // Five cache points, a key on a cleartext socket, the files beta missing.
  std::string five = good;
  for (int i = 0; i < 4; ++i) five.replace(five.find(R"({"type":"text","text":"hi"})"), 27, R"({"type":"text","text":"hi","cache_control":{"type":"ephemeral"}},{"type":"text","text":"hi"})");
  TEST_ASSERT_FALSE(chk::request(reqOf(five), five, why));
  agent::HttpRequest clear = reqOf(good);
  clear.tls = false;
  TEST_ASSERT_FALSE(chk::request(clear, good, why));
  std::string file = good;
  file.replace(file.find(R"({"type":"text","text":"hi"})"), 27, R"({"type":"image","source":{"type":"file","file_id":"f1"}})");
  TEST_ASSERT_FALSE(chk::request(reqOf(file), file, why));
  agent::HttpRequest beta = reqOf(file);
  beta.headers.push_back({"anthropic-beta", "files-api-2025-04-14"});
  TEST_ASSERT_TRUE_MESSAGE(chk::request(beta, file, why), why.c_str());
}

// ---- 2. the battery's day, every pointer ---------------------------------------------------------
void test_the_day_renders_bodies_the_api_accepts_at_every_pointer() {
  Rig rig;
  for (const txb::Snapshot& s : txb::dayOfFourTurns(plain::Dialect::AnthropicMessages))
    for (uint64_t p = 0; p <= s.c.q; ++p) {
      Out out;
      Opts o;
      o.pointer = p;
      RENDER_OK(rig, s.c, out, o, s.openTurn);
      const std::string ctx = s.name + " pointer " + std::to_string(p);
      EXPECT_API_OK(out, ctx);
      std::string why;
      TEST_ASSERT_TRUE_MESSAGE(txb::covers(out.view, coverOf(s.c, p, s.openTurn), why), (ctx + ": " + why).c_str());
      TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(out.view, why), (ctx + ": " + why).c_str());
      TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(out.view, s.c.items, why), (ctx + ": " + why).c_str());
    }
}

// ---- 3. pure ----------------------------------------------------------------------------------------
void test_the_same_transcript_and_facts_give_the_same_bytes() {
  for (unsigned seed = 1; seed <= 300; ++seed) {
    Rig rig;
    GenOpts go;
    go.broken = seed % 3 == 0;
    go.dirty = seed % 5 == 0;
    const txb::Convo c = randomConvo(seed, rig, go);
    Opts o;
    o.pointer = seed % 4 ? 0 : c.q / 2;
    o.inlinePhotos = seed % 2;
    Out a, b, restarted;
    RENDER_OK(rig, c, a, o);
    RENDER_OK(rig, c, b, o);
    const std::string ctx = "seed " + std::to_string(seed);
    TEST_ASSERT_TRUE_MESSAGE(a.req.body == b.req.body && a.body == b.body, (ctx + ": render is not pure").c_str());
    TEST_ASSERT_EQUAL(a.req.splices.size(), b.req.splices.size());
    // After a restart every item is read back from its stored line.
    txb::Convo back = c;
    for (tx::Item& it : back.items) {
      std::string line;
      TXB_OK(tx::encodeLine(it, line));
      TXB_OK(tx::decodeLine(line.data(), line.size(), it));
    }
    RENDER_OK(rig, back, restarted, o);
    TEST_ASSERT_TRUE_MESSAGE(restarted.body == a.body, (ctx + ": a restart renders other bytes").c_str());
    // Nothing but the facts is read: round, attempt, clock, taint, trigger.
    tx::TxArena arena;
    TXB_OK(arena.init(1 << 22, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }));
    for (const tx::Item& it : c.items) TXB_OK(arena.append(it));
    plain::TurnCtx turn;
    plain::RenderInput in = rig.input(c, arena, o, turn, 0);
    turn.roundNo = (uint16_t)seed;
    turn.attempt = 3;
    turn.startedAt = 1234567;
    turn.startedMs = seed * 7;
    turn.tainted = true;
    turn.trigger = plain::Trigger::Routine;
    const plain::WireCodec* w = nullptr;
    TXB_OK(plain::renderWireFor(plain::Dialect::AnthropicMessages, w));
    agent::HttpRequest req;
    TXB_OK(w->render(in, req));
    TEST_ASSERT_TRUE_MESSAGE(agent::materializeBody(req) == a.body, (ctx + ": a non-fact moved the bytes").c_str());
  }
}

// ---- 4. pairing on wire bytes ------------------------------------------------------------------------
void test_every_call_has_exactly_one_result_after_it_whatever_was_stored() {
  int faulty = 0;
  for (unsigned seed = 1; seed <= 400; ++seed) {
    Rig rig;
    GenOpts go;
    go.broken = seed % 2 == 0;
    const txb::Convo c = randomConvo(seed, rig, go);
    std::string why;
    faulty += !txb::itemsPaired(c.items, why);
    for (uint64_t p : {(uint64_t)0, c.q / 3, c.q}) {
      Opts o;
      o.pointer = p;
      Out out;
      RENDER_OK(rig, c, out, o);
      const std::string ctx = "seed " + std::to_string(seed) + " pointer " + std::to_string(p);
      EXPECT_API_OK(out, ctx);
      why.clear();
      TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(out.view, why), (ctx + ": " + why).c_str());
      if (!go.broken) {
        why.clear();
        TEST_ASSERT_TRUE_MESSAGE(txb::covers(out.view, coverOf(c, p, 0), why), (ctx + ": " + why).c_str());
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(faulty > 50, "the generator stored too few pairing faults to prove anything");
}

// A note stored between a call and its result waits until the round's results are sent.
void test_a_block_stored_between_a_call_and_its_result_waits_for_the_result() {
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.owner("read two files");
  const std::string a = c.call("read_file", R"({"path":"a"})");
  const std::string b = c.call("read_file", R"({"path":"b"})");
  c.note("[device] typing");
  c.owner("and a third");   // a late message joined mid-round
  c.receipt(b, "read_file", "B");
  c.receipt(a, "read_file", "A");
  Out out;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "between");
  const auto& ms = out.view.messages;
  TEST_ASSERT_EQUAL(4, (int)ms.size());
  TEST_ASSERT_EQUAL(3, (int)ms[2].blocks.size());   // both results first, then the note
  TEST_ASSERT_TRUE(ms[2].blocks[0].kind == sp::BlockKind::ToolResult && ms[2].blocks[0].id == b);
  TEST_ASSERT_TRUE(ms[2].blocks[1].kind == sp::BlockKind::ToolResult && ms[2].blocks[1].id == a);
  TEST_ASSERT_EQUAL_STRING("[device] typing", ms[2].blocks[2].text.c_str());
  TEST_ASSERT_EQUAL_STRING("user", ms[3].role.c_str());
  TEST_ASSERT_TRUE(ms[3].blocks[0].text.find("and a third") != std::string::npos);
}

// ---- 5. cache points -------------------------------------------------------------------------------
void expectPoints(const Rig& rig, const txb::Convo& c, const std::string& openAsk, const std::string& ctx) {
  Out out;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, ctx);
  const std::vector<Where> marks = marksOf(out.body);
  std::vector<Where> want;
  const plain::SystemBlocks& s = rig.fx.system;
  std::vector<size_t> sysIdx;   // rendered index of each non-empty block
  size_t n = 0;
  long core = -1, notes = -1, digests = -1;
  if (!s.core.empty()) core = (long)n++;
  if (!s.notes.empty()) notes = (long)n++;
  if (!s.digests.empty()) digests = (long)n++;
  if (notes >= 0) want.push_back(Where{true, 0, (size_t)notes});
  else if (core >= 0) want.push_back(Where{true, 0, (size_t)core});
  if (digests >= 0) want.push_back(Where{true, 0, (size_t)digests});
  const size_t open = messageWith(out.view, openAsk);
  TEST_ASSERT_TRUE_MESSAGE(open < out.view.messages.size(), (ctx + ": no open-turn message").c_str());
  Where w;
  if (lastCacheable(out.view, open, w)) want.push_back(w);
  TEST_ASSERT_TRUE(lastCacheable(out.view, out.view.messages.size(), w));
  want.push_back(w);
  TEST_ASSERT_EQUAL_MESSAGE(want.size(), marks.size(), ctx.c_str());
  for (size_t i = 0; i < want.size(); ++i) {
    const bool same = want[i].system == marks[i].system && want[i].msg == marks[i].msg && want[i].block == marks[i].block;
    TEST_ASSERT_TRUE_MESSAGE(same, (ctx + ": cache point " + std::to_string(i) + " is not where stated").c_str());
  }
  TEST_ASSERT_TRUE(marks.size() <= 4);
}

void test_the_four_cache_points_sit_at_the_stated_places() {
  for (unsigned seed = 1; seed <= 300; ++seed) {
    Rig rig;
    switch (seed % 6) {   // every shape of the system: points 1 and 2 move with it
      case 1: rig.fx.system.notes.clear(); break;
      case 2: rig.fx.system.digests.clear(); break;
      case 3: rig.fx.system.notes.clear(); rig.fx.system.digests.clear(); break;
      case 4: rig.fx.system = plain::SystemBlocks{}; break;
      default: break;
    }
    GenOpts go;
    go.closedTurns = seed % 4 == 0 ? 0 : -1;   // a first turn has no previous-turn point
    const txb::Convo c = randomConvo(seed, rig, go);
    expectPoints(rig, c, "ask " + std::to_string(c.turn) + ":", "seed " + std::to_string(seed));
  }
  // The open turn ends in thinking? Never: the point goes on the last block that can take it.
  Rig rig;
  txb::Convo c;
  c.ownerTurn("ask 1: first", "fine");
  c.openTurn();
  c.owner("ask 2: second");
  Out out;
  RENDER_OK(rig, c, out);
  TEST_ASSERT_EQUAL(4, (int)marksOf(out.body).size());
}

// Request after request, nothing before a cache point changes: bytes up to every point of
// the earlier body are the start of the later one (the points themselves are markers). Turns
// open warm, without outside content or photos leaving view; dropped thinking is the one
// change point here and leaves only the points before its turn.
struct Growth {
  struct Snap {
    std::string body;
    bool thinking;   // the open turn holds thinking
    bool opened;     // rendered at a turn open
  };
  Rig rig;
  std::mt19937 rng;
  txb::Convo c;
  std::vector<Snap> snaps;
  bool thinking = false, opened = false;
  bool photo;
  explicit Growth(unsigned seed) : rng(seed), photo(seed % 2) {}

  void snap() {
    Out out;
    RENDER_OK(rig, c, out);
    EXPECT_API_OK(out, "growth");
    snaps.push_back({out.body, thinking, opened});
    opened = false;
  }
  void round() {
    if (rng() % 2) {
      c.add(tx::ItemKind::RoundMeta).x = R"({"type":"thinking","thinking":"hm","signature":"s"})";
      thinking = true;
    }
    if (rng() % 2) c.say("looking", false);
    const std::string id = c.call("read_file", R"({"path":"a"})");
    if (rng() % 2) c.receipt(id, "read_file", "A " + std::to_string(c.q));
    else c.bulkResult(id, "read_file", txb::payload("B" + std::to_string(c.q), 4000));
    if (rng() % 4 == 0) c.note("[device] round note " + std::to_string(c.q));
    c.nextRound();
    snap();
  }
  void turn(bool first) {
    c.advance(60);
    c.openTurn();
    thinking = false;
    opened = true;
    if (photo) c.items.back().photos = {"pab12cd34"};   // the photo stays in view
    c.owner("ask " + std::to_string(c.turn) + ": go", first && photo ? std::vector<std::string>{"pab12cd34"} : std::vector<std::string>{});
    snap();
    for (int r = (int)(rng() % 3); r > 0; --r) round();
    c.say("done " + std::to_string(c.turn));
    c.close();
  }
  // Bytes up to every point of request i - 1 start request i. A new turn drops the thinking
  // of the turn before it (a named change point): only the points before that turn hold.
  size_t check(unsigned seed) const {
    size_t checked = 0;
    for (size_t i = 1; i < snaps.size(); ++i) {
      const std::string a = stripMarks(snaps[i - 1].body), b = stripMarks(snaps[i].body);
      const std::vector<size_t> at = markOffsets(snaps[i - 1].body);
      const size_t limit = snaps[i].opened && snaps[i - 1].thinking ? at.size() - 1 : at.size();
      for (size_t k = 0; k < limit; ++k, ++checked) {
        const bool same = b.size() >= at[k] && b.compare(0, at[k], a, 0, at[k]) == 0;
        TEST_ASSERT_TRUE_MESSAGE(same, ("seed " + std::to_string(seed) + " request " + std::to_string(i) +
                                        ": bytes before cache point " + std::to_string(k) + " changed").c_str());
      }
    }
    return checked;
  }
};

// Request after request, nothing before a cache point changes (the points themselves are
// markers). Turns open warm, without outside content or a photo leaving view.
void test_nothing_before_a_cache_point_changes_when_items_are_appended() {
  size_t checked = 0;
  for (unsigned seed = 1; seed <= 120; ++seed) {
    Growth g(seed);
    const int turns = 2 + (int)(g.rng() % 4);
    for (int t = 0; t < turns; ++t) g.turn(t == 0);
    checked += g.check(seed);
  }
  TEST_ASSERT_TRUE(checked > 1000);
}

// ---- 6. the guard and UTF-8 -------------------------------------------------------------------------
void test_no_string_passes_the_guard_and_every_body_is_valid_utf8() {
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.owner(std::string("bad \xC3( text \x01\x7F") + std::string("\xF0\x9F", 2));
  c.say(std::string(59000, 'a') + "\xE2\x82", false);   // a stored line holds at most 60,000 B
  const std::string id = c.call("read_file", std::string("{\"path\":\"a\xC3\"}"));
  c.bulkResult(id, "read_file", std::string(90000, 'b') + "\xC3");   // bulk can pass the guard
  const std::string id2 = c.call("read_file", R"({"path":"b"})");
  c.receipt(id2, "read_file", std::string(9000, '\x01'));
  c.note("[device] " + std::string(59000, 'n'));
  c.nextRound();
  Out out;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "guard");
  TEST_ASSERT_TRUE(out.body.find("[cut: kept") != std::string::npos);
  TEST_ASSERT_TRUE(out.body.find(std::string(1000, 'b')) != std::string::npos);
  // System text is refused whole over the guard, never clipped; nothing is left to send.
  Rig sys;
  sys.fx.system.notes = std::string(60001, 's');
  txb::Convo small;
  small.ownerTurn("hi", "hello");
  TEST_ASSERT_EQUAL(tx::Err::TooLarge, sys.render(small, out).err);
  // 200 dirty random transcripts.
  for (unsigned seed = 1; seed <= 200; ++seed) {
    Rig r;
    GenOpts go;
    go.dirty = true;
    go.broken = seed % 2;
    const txb::Convo d = randomConvo(seed, r, go);
    Out o;
    RENDER_OK(r, d, o);
    EXPECT_API_OK(o, "dirty seed " + std::to_string(seed));
  }
}

// ---- 7. calls --------------------------------------------------------------------------------------
void test_a_cut_or_broken_call_renders_as_an_empty_object() {
  const char* args[] = {R"({"path":"notes/a.md)", "[1,2]", "", "nope", R"({"a":01})", "[stub 15204B sha:ab12cd34: view id=r9]",
                        R"({"a":"b"} trailing)", "null"};
  for (const char* a : args) {
    Rig rig;
    txb::Convo c;
    c.openTurn();
    c.owner("go");
    const std::string id = c.call("write_file", a, true);
    c.receipt(id, "write_file", "not run: the answer was cut", false);
    Out out;
    RENDER_OK(rig, c, out);
    EXPECT_API_OK(out, a);
    const sp::Block& b = out.view.messages[1].blocks[0];
    TEST_ASSERT_EQUAL_STRING_MESSAGE("{}", b.args.c_str(), a);
    TEST_ASSERT_TRUE(out.view.messages[2].blocks[0].isError);
  }
  // Valid arguments are sent as stored (compacted), whitespace and all keys kept.
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.owner("go");
  const std::string id = c.call("read_file", "{ \"path\" : \"a b\",\n \"n\": [1, 2.5e3, true, null] }");
  c.receipt(id, "read_file", "ok");
  Out out;
  RENDER_OK(rig, c, out);
  TEST_ASSERT_TRUE(out.req.body.find(R"("input":{"path":"a b","n":[1,2.5e3,true,null]})") != std::string::npos);
  // A foreign or empty id is sent in a form the API takes, the same on the call and the result.
  for (const char* foreign : {"call.1:x", "", "id with space"}) {
    txb::Convo f;
    f.openTurn();
    f.owner("go");
    f.call("read_file", "{}");
    f.items.back().id = foreign;
    f.receipt(foreign, "read_file", "ok");
    RENDER_OK(rig, f, out);
    EXPECT_API_OK(out, foreign);
    TEST_ASSERT_EQUAL_STRING(out.view.messages[1].blocks[0].id.c_str(), out.view.messages[2].blocks[0].id.c_str());
  }
}

// ---- 8. the rules lane's texts --------------------------------------------------------------------
void test_stubs_photo_stubs_and_the_omission_line_are_the_rules_texts() {
  Rig rig;
  txb::Convo c;
  for (int i = 0; i < 4; ++i) {
    c.advance(86400);
    c.openTurn();
    c.owner("ask " + std::to_string(c.turn) + ": old");
    const std::string id = c.call("read_file", R"({"path":"a"})");
    c.bulkResult(id, "read_file", txb::payload("OLD" + std::to_string(i), 5000));
    c.nextRound();
    c.say("ok");
    c.close();
  }
  c.advance(60);
  c.openTurn();
  c.owner("ask 5: now", {"pab12cd34"});   // not in the t item's visible set: its stub
  const std::string id = c.call("read_file", R"({"path":"b"})");
  c.bulkResult(id, "read_file", txb::payload("NEW", 5000));
  c.nextRound();
  const std::string id2 = c.call("read_file", R"({"path":"c"})");
  c.bulkResult(id2, "read_file", txb::payload("NEWER", 5000));
  c.nextRound();
  Out out;
  Opts o;
  // Rung 3 as the engine writes it: turns 1 and 2 (7 items each) and tx::omissionLine's text.
  std::string dates, line;
  TXB_OK(tx::omissionDates(c.items[0].t, c.items[13].t, 0, dates));
  TXB_OK(tx::omissionLine(2, dates, line));
  o.ladder.omitThrough = c.items[13].q;
  o.ladder.omissionLine = line;
  o.ladder.stubThrough = c.q - 1;   // rung 2: this turn's bulk up to here
  RENDER_OK(rig, c, out, o);
  EXPECT_API_OK(out, "rules texts");
  TEST_ASSERT_EQUAL_STRING(line.c_str(), out.view.messages[0].blocks[0].text.c_str());
  TEST_ASSERT_TRUE(out.body.find("ask 1:") == std::string::npos && out.body.find("ask 2:") == std::string::npos);
  TEST_ASSERT_TRUE(out.body.find("ask 3:") != std::string::npos);
  // The photo stub, as tx::photoStub writes it.
  std::string when, stub;
  TXB_OK(tx::photoWhen(c.items.back().t, 0, when));
  for (const tx::Item& it : c.items)
    if (it.kind == tx::ItemKind::User && !it.parts.empty()) TXB_OK(tx::photoWhen(it.t, 0, when));
  TXB_OK(tx::photoStub("ab12cd34", when, stub));
  TEST_ASSERT_TRUE_MESSAGE(txb::detail::textIn(out.view, "user", stub), stub.c_str());
  // Rung 2 stubs this turn's older bulk with its stored stub; the newest stays whole.
  for (const tx::Item& it : c.items) {
    if (it.kind != tx::ItemKind::Result || it.turn != c.turn) continue;
    const auto blocks = txb::detail::blocksOf(out.view, sp::BlockKind::ToolResult, it.id);
    TEST_ASSERT_EQUAL(1, (int)blocks.size());
    TEST_ASSERT_EQUAL_STRING((it.q <= o.ladder.stubThrough ? it.st : c.bulk[it.handle]).c_str(), blocks[0]->text.c_str());
  }
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(out.view, c.items, why), why.c_str());
  // The open turn is never omitted, however many turns the ladder asks for.
  TXB_OK(tx::omissionLine(4, dates, o.ladder.omissionLine));
  o.ladder.omitThrough = c.q;
  RENDER_OK(rig, c, out, o);
  EXPECT_API_OK(out, "omit all");
  TEST_ASSERT_TRUE(out.body.find("ask 5: now") != std::string::npos && out.body.find("ask 4:") == std::string::npos);
  TEST_ASSERT_TRUE(out.view.messages[0].blocks[0].text.rfind("[4 earlier turns, ", 0) == 0);
  // No line from the engine: nothing is left out (never silently).
  o.ladder.omissionLine.clear();
  RENDER_OK(rig, c, out, o);
  TEST_ASSERT_TRUE(out.body.find("ask 1:") != std::string::npos);
  // Over random transcripts: every text block is stored text or a rules text.
  for (unsigned seed = 1; seed <= 150; ++seed) {
    Rig r;
    const txb::Convo d = randomConvo(seed, r);
    Opts p;
    if (seed % 3) {
      p.ladder.omitThrough = d.q / 3;
      TXB_OK(tx::omissionLine(seed % 3, "", p.ladder.omissionLine));
    }
    p.ladder.closedThrough = seed % 5 ? 0 : d.q / 2;
    p.pointer = seed % 2 ? d.q / 2 : 0;
    RENDER_OK(r, d, out, p);
    EXPECT_API_OK(out, "texts seed " + std::to_string(seed));
    why.clear();
    TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(out.view, d.items, why), why.c_str());
  }
}

// ---- 8b. the overflow ladder (RenderInput::ladder, the engine's LadderState) --------------------
// Each rung changes the rendered bytes, makes the body smaller, and never leaves a tool_use
// without its tool_result (or a result without its call); calls are never stubbed.
void expectCallsWhole(const Out& out, const txb::Convo& c, const std::string& ctx) {
  for (const sp::Message& m : out.view.messages)
    for (const sp::Block& b : m.blocks)
      if (b.kind == sp::BlockKind::ToolCall)
        for (const tx::Item& it : c.items)
          if (it.kind == tx::ItemKind::Call && it.id == b.id)
            TEST_ASSERT_EQUAL_STRING_MESSAGE(it.args.c_str(), b.args.c_str(), ctx.c_str());
}

void test_each_rung_of_the_ladder_changes_the_bytes_and_keeps_every_pair() {
  Rig rig;
  txb::Convo c;
  std::vector<uint64_t> turnEnd;
  for (int t = 0; t < 3; ++t) {
    c.advance(3600);
    c.openTurn();
    c.owner("ask " + std::to_string(c.turn) + ": read");
    const std::string id = c.call("read_file", R"({"path":"a"})");
    c.bulkResult(id, "read_file", txb::payload("CLOSED" + std::to_string(t), 6000));
    c.nextRound();
    c.say("read it");
    c.close();
    turnEnd.push_back(c.q);
  }
  c.advance(60);
  c.openTurn();
  c.owner("ask 4: now");
  std::vector<uint64_t> roundResult;
  for (int r = 0; r < 3; ++r) {
    const std::string id = c.call("read_file", R"({"path":"b"})");
    roundResult.push_back(c.bulkResult(id, "read_file", txb::payload("OPEN" + std::to_string(r), 6000)));
    c.nextRound();
  }
  Out none, r1, r2, r3;
  RENDER_OK(rig, c, none);
  Opts o;
  o.ladder.closedThrough = turnEnd.back();   // rung 1: every closed turn stubbed
  RENDER_OK(rig, c, r1, o);
  o.ladder.stubThrough = roundResult[0];     // rung 2: this turn's bulk older than two rounds
  RENDER_OK(rig, c, r2, o);
  std::string line;
  TXB_OK(tx::omissionLine(2, "", line));
  o.ladder.omitThrough = turnEnd[1];         // rung 3: the two oldest turns
  o.ladder.omissionLine = line;
  RENDER_OK(rig, c, r3, o);
  const Out* steps[] = {&none, &r1, &r2, &r3};
  for (int i = 1; i < 4; ++i) {
    const std::string ctx = "rung " + std::to_string(i);
    EXPECT_API_OK(*steps[i], ctx);
    TEST_ASSERT_TRUE_MESSAGE(steps[i]->body.size() < steps[i - 1]->body.size(), (ctx + " did not shrink the body").c_str());
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(steps[i]->view, why), (ctx + ": " + why).c_str());
    expectCallsWhole(*steps[i], c, ctx);
  }
  TEST_ASSERT_TRUE(none.body.find("CLOSED0\\nrow") != std::string::npos && r1.body.find("CLOSED0\\nrow") == std::string::npos);
  TEST_ASSERT_TRUE(r1.body.find("OPEN0\\nrow") != std::string::npos && r2.body.find("OPEN0\\nrow") == std::string::npos);
  TEST_ASSERT_TRUE(r2.body.find("OPEN2\\nrow") != std::string::npos);   // the newest rounds stay whole
  TEST_ASSERT_TRUE(r3.body.find("ask 1:") == std::string::npos && r3.body.find("ask 2:") == std::string::npos);
  TEST_ASSERT_TRUE(r3.body.find("ask 3:") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING(line.c_str(), r3.view.messages[0].blocks[0].text.c_str());
  // The engine climbs again while over its cap: rung 2 advances round by round and rung 3 turn
  // by turn; every further step renders other, smaller bytes, still valid and paired.
  Opts step;
  std::string prev = none.body;
  for (uint64_t through : {roundResult[0], roundResult[1]}) {
    step.ladder.stubThrough = through;
    Out o2;
    RENDER_OK(rig, c, o2, step);
    TEST_ASSERT_TRUE_MESSAGE(o2.body.size() < prev.size(), "a further rung 2 step changed nothing");
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(o2.view, why), why.c_str());
    prev = o2.body;
  }
  for (uint32_t n = 1; n <= 3; ++n) {
    step.ladder.omitThrough = turnEnd[n - 1];
    TXB_OK(tx::omissionLine(n, "", step.ladder.omissionLine));
    Out o3;
    RENDER_OK(rig, c, o3, step);
    EXPECT_API_OK(o3, "rung 3 step " + std::to_string(n));
    TEST_ASSERT_TRUE_MESSAGE(o3.body.size() < prev.size(), "a further rung 3 step changed nothing");
    TEST_ASSERT_TRUE(o3.body.find("ask " + std::to_string(n) + ":") == std::string::npos);
    prev = o3.body;
  }
  // Turn 0 items (stored outside any turn) are never left out as a turn.
  txb::Convo zero;
  zero.note("[device] before any turn");
  for (const tx::Item& it0 : c.items) {
    tx::Item copy = it0;
    copy.q += 1;
    zero.items.push_back(copy);
  }
  zero.q = c.q + 1;
  zero.turn = c.turn;
  zero.bulk = c.bulk;
  Opts zo;
  zo.ladder.omitThrough = turnEnd[0] + 1;
  TXB_OK(tx::omissionLine(1, "", zo.ladder.omissionLine));
  Out zout;
  RENDER_OK(rig, zero, zout, zo);
  EXPECT_API_OK(zout, "turn 0");
  TEST_ASSERT_TRUE(zout.body.find("before any turn") != std::string::npos && zout.body.find("ask 1:") == std::string::npos);
  // Any rung at any point of random transcripts, broken ones too: valid and paired.
  for (unsigned seed = 1; seed <= 200; ++seed) {
    Rig r;
    GenOpts go;
    go.broken = seed % 2;
    const txb::Convo d = randomConvo(seed, r, go);
    std::mt19937 rng(seed);
    Opts p;
    p.ladder.closedThrough = rng() % (d.q + 1);
    p.ladder.stubThrough = rng() % (d.q + 1);
    if (rng() % 2) {
      p.ladder.omitThrough = rng() % (d.q + 1);
      TXB_OK(tx::omissionLine(1 + rng() % 5, "", p.ladder.omissionLine));
    }
    Out out;
    RENDER_OK(r, d, out, p);
    const std::string ctx = "ladder seed " + std::to_string(seed);
    EXPECT_API_OK(out, ctx);
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(out.view, why), (ctx + ": " + why).c_str());
  }
}

// The loop sizes rung 3 by a lower bound of what each item renders (plain_engine.cpp
// renderedBytes: the text each item shows, no framing). On this wire, leaving out a run of whole
// turns frees at least that bound, so one climb is enough. (Stored states this wire renders
// shorter than their text, so a second climb may follow: arguments that are not one compact JSON
// object, blank text, a superseded receipt, unpaired items, bulk past the 60,000 B guard.)
size_t loopLowerBound(const tx::Item& it, uint16_t dayStart) {
  char stamp[tx::kStampMaxBytes];
  switch (it.kind) {
    case tx::ItemKind::User:
      return it.x.size() + (tx::messageStamp(it.t, it.tzMinutes, dayStart, stamp, sizeof stamp).ok() ? std::strlen(stamp) : 0);
    case tx::ItemKind::Assistant:
    case tx::ItemKind::DeviceNote:
      return it.x.size();
    case tx::ItemKind::Call:
      return it.id.size() + it.n.size() + it.args.size();
    case tx::ItemKind::Result:
      if (it.cls == tx::ResultClass::Outside) return it.st.size();
      return it.cls == tx::ResultClass::Receipt || it.handle.empty() ? it.x.size() : it.bytes;
    default:
      return 0;
  }
}

void test_omitting_whole_turns_frees_at_least_the_loops_lower_bound() {
  size_t checked = 0;
  for (unsigned seed = 1; seed <= 150; ++seed) {
    Rig rig;
    GenOpts go;
    go.closedTurns = 2 + seed % 4;
    const txb::Convo c = randomConvo(seed, rig, go);
    Out full;
    RENDER_OK(rig, c, full);
    std::vector<uint32_t> turns;
    for (const tx::Item& it : c.items)
      if (it.turn != c.turn && (turns.empty() || turns.back() != it.turn)) turns.push_back(it.turn);
    for (size_t n = 1; n <= turns.size(); ++n) {
      uint64_t through = 0;
      size_t bound = 0;
      for (const tx::Item& it : c.items)
        if (it.turn <= turns[n - 1]) {
          through = std::max(through, it.q);
          bound += loopLowerBound(it, rig.fx.turn.dayStartMin);
        }
      Opts o;
      o.ladder.omitThrough = through;
      TXB_OK(tx::omissionLine((uint32_t)n, "", o.ladder.omissionLine));
      Out cut;
      RENDER_OK(rig, c, cut, o);
      const long freed = (long)full.body.size() - (long)cut.body.size() + (long)o.ladder.omissionLine.size();
      ++checked;
      TEST_ASSERT_TRUE_MESSAGE(freed >= (long)bound, ("seed " + std::to_string(seed) + " omitting " + std::to_string(n) +
                                                      " turns freed " + std::to_string(freed) + " B, under the loop's bound " +
                                                      std::to_string(bound)).c_str());
    }
  }
  TEST_ASSERT_TRUE(checked > 300);
}

// ---- 9. photos -------------------------------------------------------------------------------------
// Five owner turns, photos arriving on the first and the fourth; the visible set of each turn
// comes from tx::visiblePhotos at its open (the t item records it). Every photo part renders
// as pixels exactly when its ref is visible, else as its stub.
void photoTurns(Rig& rig, bool inl) {
  txb::Convo c;
  std::vector<std::string> candidates;   // newest first
  std::vector<uint32_t> since;
  rig.addPhoto("pcd56ef78");
  size_t hidden = 0;
  for (int t = 0; t < 5; ++t) {
    c.advance(120);
    c.openTurn();
    const std::string arrives = t == 0 ? "pab12cd34" : t == 3 ? "pcd56ef78" : "";
    if (!arrives.empty()) {
      candidates.insert(candidates.begin(), arrives);
      since.insert(since.begin(), 0);
    }
    tx::PhotoFacts pf;
    pf.mode = inl ? tx::PhotoMode::Inline : tx::PhotoMode::FileRef;
    pf.now = c.t;
    pf.lastOwnerTurnAt = c.t - 120;
    pf.candidates = candidates;
    pf.ownerTurnsSince = since;
    std::vector<std::string> vis;
    TXB_OK(tx::visiblePhotos(pf, vis));
    c.items.back().photos = vis;
    c.owner("ask " + std::to_string(c.turn) + ":", arrives.empty() ? std::vector<std::string>{} : std::vector<std::string>{arrives});
    Opts o;
    o.inlinePhotos = inl;
    Out out;
    RENDER_OK(rig, c, out, o);
    EXPECT_API_OK(out, inl ? "inline" : "file");
    for (const std::string& ref : candidates) {
      const bool shown = std::find(vis.begin(), vis.end(), ref) != vis.end();
      const bool pixels = inl ? out.req.splices.size() >= 1 && shown : out.body.find("\"file_id\":\"file_" + ref + "\"") != std::string::npos;
      TEST_ASSERT_TRUE_MESSAGE(pixels == shown, ("turn " + std::to_string(t) + " " + ref).c_str());
      TEST_ASSERT_TRUE_MESSAGE(txb::detail::textIn(out.view, "user", "[photo " + ref + " ") == !shown, ref.c_str());
      hidden += !shown;
    }
    TEST_ASSERT_EQUAL(vis.size(), txb::detail::countImages(out.view));
    if (inl) TEST_ASSERT_EQUAL(vis.size(), out.req.splices.size());
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::covers(out.view, coverOf(c, 0, 0), why), why.c_str());
    for (uint32_t& n : since) ++n;
    c.say("seen");
    c.close();
  }
  TEST_ASSERT_TRUE_MESSAGE(hidden > 0, "no photo ever left view: the run proves nothing");
}

void test_photos_follow_the_open_turns_visible_set() {
  Rig rig;
  photoTurns(rig, false);
  photoTurns(rig, true);
  // The inline bytes are the photo's, base64 at send time.
  txb::Convo c;
  c.openTurn();
  c.items.back().photos = {"pab12cd34"};
  c.owner("look", {"pab12cd34"});
  Opts inl;
  inl.inlinePhotos = true;
  Out out;
  RENDER_OK(rig, c, out, inl);
  TEST_ASSERT_TRUE(out.body.find("\"media_type\":\"image/jpeg\",\"data\":\"/9j/4G") != std::string::npos);
  // A type the API refuses is named, not sent.
  inl.mime = "image/tiff";
  RENDER_OK(rig, c, out, inl);
  EXPECT_API_OK(out, "tiff");
  TEST_ASSERT_EQUAL(0, (int)txb::detail::countImages(out.view));
  TEST_ASSERT_TRUE(txb::detail::textIn(out.view, "user", "[photo pab12cd34 "));
  // A photo whose bytes cannot be read, or of a type the API refuses, is named, not dropped.
  Rig bad;
  bad.photos.clear();
  RENDER_OK(bad, c, out);
  EXPECT_API_OK(out, "unreadable photo");
  TEST_ASSERT_EQUAL(0, (int)txb::detail::countImages(out.view));
  TEST_ASSERT_TRUE(txb::detail::textIn(out.view, "user", "[photo pab12cd34 "));
  // A photo with no text and no clock is still a message of its own: the stamp says the time
  // is unknown (RB-REC5), then the photo.
  txb::Convo only;
  only.openTurn();
  only.items.back().photos = {"pab12cd34"};
  only.t = 0;
  only.owner("", {"pab12cd34"});
  RENDER_OK(rig, only, out);
  EXPECT_API_OK(out, "photo only");
  TEST_ASSERT_EQUAL(1, (int)out.view.messages.size());
  TEST_ASSERT_EQUAL_STRING((stampOf(0, 0, 240) + " ").c_str(), out.view.messages[0].blocks[0].text.c_str());
  TEST_ASSERT_TRUE(out.view.messages[0].blocks[1].kind == sp::BlockKind::Image);
}

// ---- 10. thinking ------------------------------------------------------------------------------------
void test_signed_thinking_rides_the_open_turn_only() {
  Rig rig;
  txb::Convo c;
  const std::string th = R"({"type":"thinking","thinking":"plan","signature":"EqQBCkgIARABGAIiQ"})";
  c.openTurn();
  c.owner("ask 1:");
  c.add(tx::ItemKind::RoundMeta).x = th;
  const std::string id = c.call("read_file", "{}");
  c.receipt(id, "read_file", "A");
  c.nextRound();
  Out out;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "open thinking");
  TEST_ASSERT_TRUE(out.req.body.find(th) != std::string::npos);
  TEST_ASSERT_TRUE(out.view.messages[1].blocks[0].kind == sp::BlockKind::Thinking);
  // Interleaved thinking keeps its stored place: k(T0) c(A) k(T1) c(B) goes out as T0 A T1 B.
  txb::Convo il;
  il.openTurn();
  il.owner("ask 1:");
  il.add(tx::ItemKind::RoundMeta).x = R"({"type":"thinking","thinking":"T0","signature":"s0"})";
  const std::string ca = il.call("read_file", R"({"path":"A"})");
  il.add(tx::ItemKind::RoundMeta).x = R"({"type":"thinking","thinking":"T1","signature":"s1"})";
  const std::string cb = il.call("read_file", R"({"path":"B"})");
  il.add(tx::ItemKind::RoundMeta).x = R"({"type":"thinking","thinking":"T2","signature":"s2"})";
  il.receipt(ca, "read_file", "a");
  il.receipt(cb, "read_file", "b");
  il.nextRound();
  Out ilOut;
  RENDER_OK(rig, il, ilOut);
  EXPECT_API_OK(ilOut, "interleaved");
  const auto& blocks = ilOut.view.messages[1].blocks;
  TEST_ASSERT_EQUAL(5, (int)blocks.size());
  const char* want[] = {"T0", "A", "T1", "B", "T2"};
  for (int k = 0; k < 5; ++k)
    TEST_ASSERT_EQUAL_STRING(want[k], blocks[k].kind == sp::BlockKind::Thinking ? blocks[k].text.c_str()
                                                                                 : blocks[k].args.substr(9, 1).c_str());
  // Pretty-printed or with a broken signature block: written compact, or left out.
  c.add(tx::ItemKind::RoundMeta).x = "{ \"type\": \"thinking\",\n \"thinking\": \"x\", \"signature\": \"s\" }";
  c.add(tx::ItemKind::RoundMeta).x = "{\"type\":\"thinking\"";
  c.say("done");
  c.close();
  c.openTurn();
  c.owner("ask 2:");
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "closed thinking");
  TEST_ASSERT_TRUE(out.req.body.find("\"thinking\"") == std::string::npos);
}

// ---- 11. the envelope -------------------------------------------------------------------------------
void test_the_envelope_carries_the_facts_and_no_orch_turn() {
  Rig rig;
  // The real always-on tools (no domain pack): the array is exactly what was given.
  rig.fx.tools.clear();
  for (const td::ToolDecl& d : td::kTools) {
    const std::string n = d.name;
    if (n == "ledger_write" || n == "ledger_day") continue;
    rig.fx.tools.push_back({n, d.description, plain::declSchema(d, false)});
  }
  txb::Convo c;
  c.ownerTurn("hi", "hello");
  c.openTurn();
  c.owner("again");
  Out out;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "real tools");
  TEST_ASSERT_EQUAL(rig.fx.tools.size(), out.view.tools.size());
  for (size_t i = 0; i < rig.fx.tools.size(); ++i) TEST_ASSERT_EQUAL_STRING(rig.fx.tools[i].name.c_str(), out.view.tools[i].c_str());
  TEST_ASSERT_TRUE(out.body.find("orch_turn") == std::string::npos);
  const size_t at = out.req.body.find("\"tools\":") + 8;
  const size_t end = out.req.body.find(",\"tool_choice\":");
  TEST_ASSERT_EQUAL(plain::toolsBytes(rig.fx.tools), end - at);   // RB-E2's figure is the wire's
  TEST_ASSERT_EQUAL_STRING("auto", out.view.toolChoice.c_str());
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5-5", out.view.model.c_str());
  TEST_ASSERT_EQUAL(4096, (int)out.view.maxTokens);
  TEST_ASSERT_TRUE(out.body.find(R"("output_config":{"effort":"medium"})") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com", out.req.host.c_str());
  TEST_ASSERT_EQUAL_STRING("/v1/messages", out.req.path.c_str());
  // The guard's last round: tool choice none, the same tools (the cache prefix).
  Opts none;
  none.none = true;
  none.maxTokens = 777;
  Out last;
  RENDER_OK(rig, c, last, none);
  EXPECT_API_OK(last, "tool choice none");
  TEST_ASSERT_EQUAL_STRING("none", last.view.toolChoice.c_str());
  TEST_ASSERT_TRUE(last.view.toolsJson == out.view.toolsJson && last.view.systemBlocks == out.view.systemBlocks);
  TEST_ASSERT_EQUAL(777, (int)last.view.maxTokens);
  // Lane E2-CMP: the summarizer's structured form rides beside the effort, the same bytes at every render,
  // and nothing else of the request changes.
  Opts form;
  form.answerSchema = plain::texts::summarizer::kChangesSchema;
  Out a, b;
  RENDER_OK(rig, c, a, form);
  RENDER_OK(rig, c, b, form);
  EXPECT_API_OK(a, "the structured form");
  TEST_ASSERT_EQUAL_STRING(a.body.c_str(), b.body.c_str());
  TEST_ASSERT_TRUE(a.body.find(std::string(R"("output_config":{"effort":"medium","format":{"type":"json_schema","schema":)") +
                               plain::texts::summarizer::kChangesSchema + "}}") != std::string::npos);
  TEST_ASSERT_TRUE(a.view.systemBlocks == out.view.systemBlocks && a.view.toolsJson == out.view.toolsJson);
  // No effort when the head sets none; no tools array when there are no tools.
  rig.fx.head.effort.clear();
  rig.fx.tools.clear();
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "no tools");
  TEST_ASSERT_TRUE(out.body.find("output_config") == std::string::npos && out.body.find("\"tools\"") == std::string::npos);
  RENDER_OK(rig, c, a, form);   // the form alone
  EXPECT_API_OK(a, "the structured form without effort");
  TEST_ASSERT_TRUE(a.body.find(R"("output_config":{"format":{"type":"json_schema")") != std::string::npos);
  // The key: x-api-key direct, a bearer token routed, none on a cleartext socket.
  auto header = [](const agent::HttpRequest& r, const char* k) {
    for (const auto& h : r.headers)
      if (h.first == k) return h.second;
    return std::string();
  };
  TEST_ASSERT_EQUAL_STRING("sk-fake-battery", header(out.req, "x-api-key").c_str());
  rig.fx.head.routed = true;
  rig.fx.head.host = "router.example";
  rig.fx.head.basePath = "/router/anthropic/v1";
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "routed");
  TEST_ASSERT_EQUAL_STRING("Bearer sk-fake-battery", header(out.req, "authorization").c_str());
  TEST_ASSERT_TRUE(header(out.req, "x-api-key").empty());
  TEST_ASSERT_EQUAL_STRING("/router/anthropic/v1/messages", out.req.path.c_str());
  rig.fx.head.tls = false;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "cleartext");
  // Refusals: no output cap, a missing input, an empty transcript, a schema that is not JSON.
  Opts zero;
  zero.maxTokens = 0;
  TEST_ASSERT_EQUAL(tx::Err::InvalidArgument, rig.render(c, out, zero).err);
  TEST_ASSERT_EQUAL(tx::Err::InvalidArgument, rig.render(txb::Convo{}, out).err);
  rig.fx.tools = {{"read_file", "r", "{not json"}};
  TEST_ASSERT_EQUAL(tx::Err::InvalidArgument, rig.render(c, out).err);
  TEST_ASSERT_TRUE(out.req.body.empty());
  const plain::WireCodec* w = nullptr;
  TXB_OK(plain::renderWireFor(plain::Dialect::AnthropicMessages, w));
  TEST_ASSERT_TRUE(w->dialect() == plain::Dialect::AnthropicMessages);
  agent::HttpRequest req;
  TEST_ASSERT_EQUAL(tx::Err::InvalidArgument, w->render(plain::RenderInput{}, req).err);
  // The wire is complete (render RB-W1a, parse RB-W1b): wireFor hands the engine the same codec,
  // and its parse reads answers (an object that is not a message is refused, never pending).
  plain::ParsedStep step;
  agent::HttpResponse resp;
  resp.status = 200;
  resp.body = "{}";
  const tx::Status parsed = w->parse(resp, step);
  TEST_ASSERT_FALSE(parsed.notImplemented());
  TEST_ASSERT_EQUAL(tx::Err::InvalidArgument, parsed.err);
  const plain::WireCodec* engineWire = nullptr;
  TXB_OK(plain::wireFor(plain::Dialect::AnthropicMessages, engineWire));
  TEST_ASSERT_TRUE(engineWire == w);
  resp.body = R"({"type":"message","content":[{"type":"text","text":"hi"}],"stop_reason":"end_turn"})";
  TXB_OK(engineWire->parse(resp, step));
  TEST_ASSERT_EQUAL_STRING("hi", step.text.c_str());
}

// ---- 12. a refused allocation (ruling 30) ------------------------------------------------------------
void test_a_refused_allocation_at_every_site_answers_a_status() {
  Rig rig;
  const txb::Convo c = randomConvo(7, rig);
  tx::TxArena arena;
  TXB_OK(arena.init(1 << 20, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); }));
  for (const tx::Item& it : c.items) TXB_OK(arena.append(it));
  plain::TurnCtx turn;
  const Opts opts;   // outlives `in`: in.ladder points into it
  const plain::RenderInput in = rig.input(c, arena, opts, turn, 0);
  const plain::WireCodec* w = nullptr;
  TXB_OK(plain::renderWireFor(plain::Dialect::AnthropicMessages, w));
  agent::HttpRequest good;
  TXB_OK(w->render(in, good));
  int refused = 0;
  for (long k = 0;; ++k) {
    agent::HttpRequest req;
    req.headers.reserve(8);
    gAllocs = 0;
    gFailAt = k;
    gHit = false;
    gArmed = true;
    const tx::Status st = w->render(in, req);
    gArmed = false;
    if (!gHit) {
      TXB_OK(st);
      break;
    }
    ++refused;
    if (st.ok()) {
      TEST_ASSERT_TRUE(req.body == good.body);   // a refusal it recovered from changes nothing
    } else {
      TEST_ASSERT_EQUAL(tx::Err::TooLarge, st.err);
      TEST_ASSERT_TRUE(req.body.empty() && req.headers.empty() && req.splices.empty());
    }
  }
  TEST_ASSERT_TRUE(refused > 20);
  agent::HttpRequest after;
  TXB_OK(w->render(in, after));
  TEST_ASSERT_TRUE(after.body == good.body);
}

// ---- 12b. stored facts the wire reads --------------------------------------------------------------
// The pointer of the newest x item, its superseded reads and the newest digest's thru.
void test_pointer_moves_superseded_reads_and_thru_come_from_the_stored_items() {
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.owner("ask 1: read");
  const std::string a = c.call("read_file", R"({"path":"a"})");
  const uint64_t bulkQ = c.bulkResult(a, "read_file", txb::payload("OLDBULK", 3000));
  const std::string b = c.call("read_file", R"({"path":"b"})");
  const uint64_t readQ = c.receipt(b, "read_file", "STALE-RECEIPT");
  c.nextRound();
  c.say("read both");
  c.close();
  c.advance(600);
  c.openTurn();
  c.move(c.q - 1, "cold");                  // the pointer moves to the last closed turn
  // A superseded-read list is a change point, not a stub list (E2-VERB): the stale receipt stays whole.
  {
    txb::Convo sup = c;
    sup.move(sup.q - 2, "superseded", {readQ});
    sup.owner("ask 2: again");
    Out whole;
    RENDER_OK(rig, sup, whole);
    TEST_ASSERT_TRUE_MESSAGE(whole.body.find("STALE-RECEIPT\"") != std::string::npos, "a superseded read became a stub");
  }
  c.move(c.q - 2, "tools", {readQ});   // a compaction tool fold listed the receipt
  c.owner("ask 2: again");
  Out out;
  Out rung1;
  txb::Convo plainTurns = c;
  plainTurns.items.erase(plainTurns.items.end() - 3, plainTurns.items.end());   // no x items, no owner yet
  plainTurns.owner("ask 2: again");
  Opts r1;
  r1.ladder.closedThrough = bulkQ;   // rung 1 stubs closed turns as the pointer would
  RENDER_OK(rig, plainTurns, rung1, r1);
  TEST_ASSERT_TRUE(rung1.body.find("OLDBULK\nrow") == std::string::npos);
  RENDER_OK(rig, c, out);   // RenderInput::stubPointer stays 0: the stored move counts
  EXPECT_API_OK(out, "stored moves");
  for (const tx::Item& it : c.items)
    if (it.q == bulkQ || it.q == readQ)
      TEST_ASSERT_EQUAL_STRING(it.st.c_str(), txb::detail::blocksOf(out.view, sp::BlockKind::ToolResult, it.id)[0]->text.c_str());
  TEST_ASSERT_TRUE(out.body.find("OLDBULK\\nrow") == std::string::npos && out.body.find("STALE-RECEIPT\"") == std::string::npos);
  // Items at or below the newest digest's thru are the digests' job.
  tx::Item& d = c.add(tx::ItemKind::Digest);
  d.day = "2026-11-17";
  d.x = "digest";
  d.thru = 9;   // through turn 1's e item
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "thru");
  TEST_ASSERT_TRUE(out.body.find("ask 1:") == std::string::npos && out.body.find("ask 2:") != std::string::npos);
  TEST_ASSERT_TRUE(out.body.find(a) == std::string::npos);
}

// A turn that opens with a device note (an agent result) never joins the message the turn
// before it ended with: the earlier request's messages stay as they were sent.
void test_a_new_turn_never_rewrites_the_message_a_turn_ended_with() {
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.owner("ask 1: start the job");
  const std::string id = c.call("agent_start", R"({"name":"j"})", true);
  c.receipt(id, "agent_start", "not run: the answer was cut", false);
  c.nextRound();
  Out first;
  RENDER_OK(rig, c, first);
  c.close();
  c.advance(30);
  c.openTurn("agent");
  c.note("[device] agent j finished: report saved");
  Out second;
  RENDER_OK(rig, c, second);
  EXPECT_API_OK(second, "agent turn");
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(sp::isPrefix(first.view, second.view, why), why.c_str());
  TEST_ASSERT_EQUAL(first.view.messages.size() + 1, second.view.messages.size());
}

// The stamp is tx::messageStamp's with the turn's day start (RB-REC5), then one space, then the
// stored text: before the day start it names the day still running, after it it does not.

void test_an_owner_message_is_its_stamp_and_its_text() {
  static_assert(tx::kStampMaxBytes >= 52, "the render's stamp buffer holds the night clause");
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.owner("hello there");
  c.items.back().tzMinutes = 60;
  Out out;
  RENDER_OK(rig, c, out);
  TEST_ASSERT_EQUAL_UINT16(240, rig.fx.turn.dayStartMin);   // the setting's default, 04:00
  TEST_ASSERT_EQUAL_STRING((stampOf(c.items.back().t, 60, 240) + " hello there").c_str(),
                           out.view.messages[0].blocks[0].text.c_str());
  // 2026-11-18 00:30 UTC with the day starting at 04:00: the clause names Tuesday's day.
  const uint64_t night = 1794961800ULL;   // 2026-11-18 00:30:00 UTC
  c.items.back().t = night;
  c.items.back().tzMinutes = 0;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "night stamp");
  TEST_ASSERT_EQUAL_STRING("[2026-11-18 Wed 00:30 (day 2026-11-17 until 04:00)] hello there",
                           out.view.messages[0].blocks[0].text.c_str());
  // 08:10 the same morning: past the day start, no clause.
  c.items.back().t = night + 7 * 3600 + 40 * 60;
  RENDER_OK(rig, c, out);
  TEST_ASSERT_EQUAL_STRING("[2026-11-18 Wed 08:10] hello there", out.view.messages[0].blocks[0].text.c_str());
  // Another device's day start moves the clause with it.
  rig.fx.turn.dayStartMin = 0;
  c.items.back().t = night;
  RENDER_OK(rig, c, out);
  TEST_ASSERT_EQUAL_STRING("[2026-11-18 Wed 00:30] hello there", out.view.messages[0].blocks[0].text.c_str());
  // No clock: the stamp says so.
  c.items.back().t = 0;
  RENDER_OK(rig, c, out);
  TEST_ASSERT_EQUAL_STRING((stampOf(0, 0, 0) + " hello there").c_str(), out.view.messages[0].blocks[0].text.c_str());
}

// Outside content is never sent whole after its turn, not even when no stub was stored.
void test_outside_content_without_a_stub_is_never_sent_whole() {
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.owner("ask 1: search");
  const std::string id = c.call("web_search", R"({"q":"x"})");
  c.outside(id, "web_search", R"({"q":"x"})", txb::payload("OUTSIDE-SECRET", 500));
  c.items.back().st.clear();
  c.nextRound();
  c.say("found it");
  c.close();
  c.openTurn();
  c.owner("ask 2: thanks");
  Out out;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "outside no stub");
  TEST_ASSERT_TRUE(out.body.find("OUTSIDE-SECRET") == std::string::npos);
}

// Lane E2-FIX2A, finding P11: a result shown whole whose bulk file is gone or short never goes out as less
// without a word. Its place holds the marker and then its stub, and the device log says so once per turn.
std::vector<std::string> g_wireLog;
void recordWireLog(const char* line) { g_wireLog.push_back(line); }

void test_a_missing_bulk_file_sends_a_marker_and_logs_once_per_turn() {
  Rig rig;
  txb::Convo c;
  c.openTurn();
  c.owner("ask 1: read the log");
  const std::string id = c.call("read_file", R"({"path":"logs/a.csv"})");
  c.bulkResult(id, "read_file", txb::payload("ROWS", 3000));
  const tx::Item stored = c.items.back();
  c.bulk.erase(stored.handle);   // the bulk file is gone (or short: the reader answers the same)
  c.nextRound();
  c.say("read it");
  c.close();
  c.openTurn();
  c.owner("ask 2: and the total?");
  g_wireLog.clear();
  agent::hlog::setSink(recordWireLog);
  Out out, again;
  Opts o;
  RENDER_OK(rig, c, out, o);
  RENDER_OK(rig, c, again, o);   // the next round of the same turn
  agent::hlog::setSink(nullptr);
  EXPECT_API_OK(out, "bulk missing");
  const auto blocks = txb::detail::blocksOf(out.view, sp::BlockKind::ToolResult, id);
  TEST_ASSERT_EQUAL(1, (int)blocks.size());
  const std::string want = std::string(plain::texts::stubs::kResultUnread) + "\n" + stored.st;
  TEST_ASSERT_EQUAL_STRING(want.c_str(), blocks[0]->text.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)g_wireLog.size(), "logged once per turn");
  TEST_ASSERT_TRUE(g_wireLog[0].find(stored.handle) != std::string::npos);
}

// What the API refuses is never sent, whatever was stored.
void test_stored_states_the_api_refuses_are_rendered_into_ones_it_takes() {
  Rig rig;
  // Tool blocks need a tools array: a render without tools over tool history is refused.
  txb::Convo c;
  c.openTurn();
  c.owner("ask 1:");
  const std::string id = c.call("read_file", "{}");
  c.receipt(id, "read_file", "", false);   // an error with no text: sent without is_error
  c.nextRound();
  Out out;
  RENDER_OK(rig, c, out);
  EXPECT_API_OK(out, "empty error");
  TEST_ASSERT_FALSE(out.view.messages[2].blocks[0].isError);
  Rig bare;
  bare.fx.tools.clear();
  TEST_ASSERT_EQUAL(tx::Err::InvalidArgument, bare.render(c, out).err);
  // Names the API cannot take; blank text in any script; thinking stored after text.
  txb::Convo n;
  n.openTurn();
  n.owner("ask 1:");
  n.say("\xC2\xA0\xE3\x80\x80 \xE2\x80\xAF", false);
  n.add(tx::ItemKind::RoundMeta).x = R"({"type":"thinking","thinking":"t","signature":"s"})";
  std::vector<std::string> ids;
  for (const char* name : {"files.read", "", "a_name_far_longer_than_sixty_four_bytes_which_the_api_refuses_outright_x"})
    ids.push_back(n.call(name, "{}"));
  for (const std::string& cid : ids) n.receipt(cid, "read_file", "ok");
  n.note("\xE2\x80\x83\xE2\x80\x83");
  n.nextRound();
  RENDER_OK(rig, n, out);
  EXPECT_API_OK(out, "names, blanks, thinking order");
  TEST_ASSERT_TRUE(out.view.messages[1].blocks[0].kind == sp::BlockKind::Thinking);
  TEST_ASSERT_EQUAL(4, (int)out.view.messages[1].blocks.size());   // thinking and three calls, no blank text
  TEST_ASSERT_EQUAL_MESSAGE(3, (int)out.view.messages[2].blocks.size(), out.body.c_str());
  // A result answers the earliest open call of its id, in its own turn only.
  txb::Convo two;
  two.openTurn();
  two.owner("ask 1:");
  two.call("read_file", R"({"n":1})");
  two.items.back().id = "toolu_x";
  two.call("read_file", R"({"n":2})");
  two.items.back().id = "toolu_x";
  two.receipt("toolu_x", "read_file", "one result");
  two.nextRound();
  RENDER_OK(rig, two, out);
  EXPECT_API_OK(out, "latest call");
  TEST_ASSERT_EQUAL(1, (int)out.view.messages[1].blocks.size());
  TEST_ASSERT_EQUAL_STRING(R"({"n":1})", out.view.messages[1].blocks[0].args.c_str());
  // Two calls of one round sharing an id get their two results in order.
  two.receipt("toolu_x", "read_file", "second result");
  RENDER_OK(rig, two, out);
  EXPECT_API_OK(out, "two same-id calls");
  TEST_ASSERT_EQUAL(2, (int)out.view.messages[1].blocks.size());
  for (int k = 0; k < 2; ++k) {
    const sp::Block& call = out.view.messages[1].blocks[k];
    const auto res = txb::detail::blocksOf(out.view, sp::BlockKind::ToolResult, call.id);
    TEST_ASSERT_EQUAL(1, (int)res.size());
    TEST_ASSERT_EQUAL_STRING(k == 0 ? R"({"n":1})" : R"({"n":2})", call.args.c_str());
    TEST_ASSERT_EQUAL_STRING(k == 0 ? "one result" : "second result", res[0]->text.c_str());
  }
  txb::Convo late;
  late.openTurn();
  late.owner("ask 1:");
  late.call("read_file", "{}");
  late.items.back().id = "toolu_y";   // never answered in its turn
  late.say("cut");
  late.close();
  late.openTurn();
  late.owner("ask 2:");
  late.receipt("toolu_y", "read_file", "LATE-RESULT");
  RENDER_OK(rig, late, out);
  EXPECT_API_OK(out, "late result");
  TEST_ASSERT_TRUE(out.body.find("toolu_y") == std::string::npos && out.body.find("LATE-RESULT") == std::string::npos);
  // A blank system block is left out; the stable point moves to the core.
  Rig blank;
  blank.fx.system.notes = " \n\xE3\x80\x80";
  RENDER_OK(blank, two, out);
  EXPECT_API_OK(out, "blank notes");
  TEST_ASSERT_EQUAL(2, (int)out.view.systemBlocks.size());
  TEST_ASSERT_TRUE(marksOf(out.body)[0].system && marksOf(out.body)[0].block == 0);
  // A round whose calls were all left out leaves no thinking alone; a turn cut by thru opens
  // with the user's turn; a blank note after the final text ends nothing.
  txb::Convo t;
  t.openTurn();
  t.say("leading text of a compacted turn", false);
  t.owner("ask 1:");
  t.add(tx::ItemKind::RoundMeta).x = R"({"type":"thinking","thinking":"t","signature":"s"})";
  t.call("read_file", "{}");   // dangling
  t.note("[device] still here");
  t.say("final");
  t.note("   ");
  RENDER_OK(rig, t, out);
  EXPECT_API_OK(out, "thinking alone, leading and trailing assistant");
  TEST_ASSERT_TRUE(out.body.find("leading text") == std::string::npos && out.body.find("\"thinking\"") == std::string::npos);
  // Ids a dialect numbers per answer ("call_0" every turn) keep every call, each its own id.
  txb::Convo r;
  for (int turn = 0; turn < 3; ++turn) {
    r.openTurn();
    r.owner("ask " + std::to_string(r.turn) + ":");
    r.call("read_file", "{}");
    r.items.back().id = "call_0";
    r.receipt("call_0", "read_file", "RESULT-" + std::to_string(turn));
    r.nextRound();
    if (turn < 2) {
      r.say("ok");
      r.close();
    }
  }
  RENDER_OK(rig, r, out);
  EXPECT_API_OK(out, "reused ids");
  for (int turn = 0; turn < 3; ++turn) TEST_ASSERT_TRUE(out.body.find("RESULT-" + std::to_string(turn)) != std::string::npos);
}

// ---- 12c. thinking under a ladder rung (settled live by RB-W1b) -------------------------------------
// Between two renders of one turn a rung changes bytes BEFORE the open turn's signed thinking
// (rungs 1 and 2 stub earlier results, rung 3 replaces earlier turns with the omission line). Live
// on claude-haiku-5-5 (lanes/RB-W1b/evidence/LIVE-CONFORMANCE.md): with the thinking replayed the
// API refuses that request where it enforces its prefix check ("bound to a different
// conversation", HTTP 400); the same request with the open turn's thinking left out is accepted,
// enforced or not. So once a rung is applied, no open-turn thinking is sent: the body is byte for
// byte the render of the same transcript without the open turn's k items, every call keeps its
// result, and without a rung (or with rung 3 named but no line) the thinking rides as stored.
void test_a_ladder_rung_between_rounds_of_a_turn_with_signed_thinking_is_accepted_by_the_provider() {
  Rig rig;
  const std::string th0 = R"({"type":"thinking","thinking":"","signature":"EqQBsig0"})";
  const std::string th1 = R"({"type":"redacted_thinking","data":"EqQBdata1"})";
  auto build = [&](bool withThinking) {
    txb::Convo c;
    c.openTurn();
    c.owner("show the log");
    const std::string id = c.call("read_file", R"({"path":"notes/log.md"})");
    c.bulkResult(id, "read_file", txb::payload("LOG", 6000));
    c.nextRound();
    c.say("Here it is.");
    c.close();
    c.openTurn();
    c.owner("which file says Thursday?");
    if (withThinking) c.add(tx::ItemKind::RoundMeta).x = th0;
    const std::string a = c.call("read_file", R"({"path":"notes/a.md"})");
    if (withThinking) c.add(tx::ItemKind::RoundMeta).x = th1;
    c.receipt(a, "read_file", "a says Thursday");
    c.nextRound();
    return c;
  };
  const txb::Convo c = build(true), bare = build(false);
  Opts none;
  Out out;
  RENDER_OK(rig, c, out, none);
  EXPECT_API_OK(out, "no rung");
  TEST_ASSERT_TRUE(out.req.body.find(th0) != std::string::npos && out.req.body.find(th1) != std::string::npos);
  Opts r1, r2, r3, r3NoLine;
  r1.ladder.closedThrough = 7;   // the closed turn's items (t u c r a d e)
  r2.ladder.stubThrough = 13;    // the open turn's first round
  r3.ladder.omitThrough = 7;
  r3.ladder.omissionLine = "[1 earlier turn left out]";
  r3NoLine.ladder.omitThrough = 7;   // no line: rung 3 is not applied, nothing changes
  for (const Opts* o : {&r1, &r2, &r3}) {
    Out with, without;
    RENDER_OK(rig, c, with, *o);
    RENDER_OK(rig, bare, without, *o);
    EXPECT_API_OK(with, "a rung applied");
    TEST_ASSERT_TRUE(with.req.body.find("signature") == std::string::npos && with.req.body.find("redacted") == std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(with.req.body == without.req.body, "a rung's body is not the body without the open turn's thinking");
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(sp::callsPaired(with.view, why), why.c_str());
    TEST_ASSERT_TRUE(with.req.body != out.req.body);
  }
  RENDER_OK(rig, c, out, r3NoLine);
  TEST_ASSERT_TRUE(out.req.body.find(th0) != std::string::npos);
  // Turns a full arena already dropped from RAM: the engine's line names them though no resident
  // turn is left out (omitThrough below every stored q); it opens the request, and it counts as a rung.
  Opts dropped;
  dropped.ladder.omitThrough = 1;
  dropped.ladder.omissionLine = "[3 earlier turns, 16 Nov, not shown]";
  txb::Convo late = build(true);
  for (tx::Item& it : late.items) it.q += 100;   // every stored item after the dropped ones
  Out named;
  RENDER_OK(rig, late, named, dropped);
  EXPECT_API_OK(named, "dropped turns named");
  TEST_ASSERT_TRUE(named.view.messages[0].blocks[0].text == "[3 earlier turns, 16 Nov, not shown]");
  TEST_ASSERT_TRUE(named.req.body.find(th0) == std::string::npos);
  // The line opens the request, so an assistant item stored before the first owner message (a turn
  // compaction cut) follows it, as it does after a rung-3 omission of resident turns.
  txb::Convo cut;
  cut.openTurn();
  cut.say("the tail of a cut turn");
  cut.close();
  cut.openTurn();
  cut.owner("next");
  RENDER_OK(rig, cut, named, dropped);
  EXPECT_API_OK(named, "line then a cut turn's tail");
  TEST_ASSERT_TRUE(txb::detail::textIn(named.view, "assistant", "the tail of a cut turn"));
}

// ---- 13. the framing, measured (ruling 16) --------------------------------------------------------------
long overheadOf(const Rig& rig, const txb::Convo& c, Out& out) {
  TXB_OK(rig.render(c, out));
  txb::CoverInput cov = coverOf(c, 0, 0);
  txb::OverheadInput in;
  txb::renderedText(c, cov, in);
  in.bodyBytes = out.body.size();
  in.systemBytes = txb::systemJsonBytes(out.view);
  in.toolsBytes = txb::toolsJsonBytes(out.view);
  return (long)in.bodyBytes - (long)in.itemBytes - (long)in.systemBytes - (long)in.toolsBytes;
}

void test_the_measured_framing_per_block_is_inside_the_battery_allowances() {
  Rig rig;
  txb::Convo base;
  base.openTurn();
  base.owner("hi");
  Out out;
  const long envelopeAndOwner = overheadOf(rig, base, out);
  // The envelope alone: the body without its one message, less the message.
  txb::Convo two = base;
  two.owner("again");
  const long owner = overheadOf(rig, two, out) - envelopeAndOwner;
  const long envelope = envelopeAndOwner - owner;
  // The same message between midnight and the day start (RB-REC5's running-day clause).
  txb::Convo night = two;
  night.items.back().t = 1794961800ULL;   // 2026-11-18 00:30 UTC, day start 04:00
  const long nightOwner = overheadOf(rig, night, out) - envelopeAndOwner;
  std::string nightWhy;
  const bool nightLabelOk = txb::noUnstoredText(out.view, night.items, nightWhy);
  // Assistant text is measured where it sits: before the next owner message.
  txb::Convo said = base, said2 = base;
  said.say("words", false);
  said2.say("words", false);
  said2.say("more", false);
  for (txb::Convo* x : {&said, &said2}) x->owner("next");
  const long text = overheadOf(rig, said, out) - overheadOf(rig, two, out);
  const long textJoined = overheadOf(rig, said2, out) - overheadOf(rig, said, out);
  txb::Convo called = base;
  const std::string id = called.call("read_file", R"({"path":"a"})");
  called.receipt(id, "read_file", "A");
  const long pair = overheadOf(rig, called, out) - envelopeAndOwner;
  txb::Convo called2 = base;
  const std::string a2 = called2.call("read_file", R"({"path":"a"})");
  const std::string b2 = called2.call("read_file", R"({"path":"b"})");
  called2.receipt(a2, "read_file", "A");
  called2.receipt(b2, "read_file", "B");
  const long pairJoined = overheadOf(rig, called2, out) - overheadOf(rig, called, out);
  txb::Convo failed = called;
  failed.items.back().ok = false;
  const long isError = overheadOf(rig, failed, out) - overheadOf(rig, called, out);
  txb::Convo photo = base;
  photo.items[0].photos = {"pab12cd34"};
  photo.items[1].parts.push_back(tx::Part{tx::PartKind::Image, "pab12cd34", "image/jpeg", 1000});
  const long fileImage = overheadOf(rig, photo, out) - envelopeAndOwner;
  txb::Convo stub = base;
  stub.items[1].parts.push_back(tx::Part{tx::PartKind::Image, "pab12cd34", "image/jpeg", 1000});
  const long photoStub = overheadOf(rig, stub, out) - envelopeAndOwner;
  txb::Convo noted = called;
  noted.note("[device] n");
  const long noteJoined = overheadOf(rig, noted, out) - overheadOf(rig, called, out);
  char line[640];
  std::snprintf(line, sizeof line,
                "measured framing B: envelope %ld (system 3 blocks, 3 tools, tool_choice, 4 points); owner message %ld "
                "(its 23 B stamp and space included), %ld between midnight and the day start (time label check %s); assistant text %ld opening a message / %ld joined; call+result "
                "%ld opening messages / %ld joined; is_error %ld; device note joined %ld; photo by file id %ld "
                "(file_pab12cd34); photo stub block %ld",
                envelope, owner, nightOwner, nightLabelOk ? "passes" : "fails", text, textJoined, pair, pairJoined, isError, noteJoined, fileImage, photoStub);
  TEST_MESSAGE(line);
  // The battery allows each owner message its framing plus its own stamp, exactly as RB-REC5 makes it
  // (props.h: kOverheadPerOwnerMessage beside OverheadInput::stampBytes; ruling 68).
  const long dayStamp = (long)txb::stampOf(two.items.back(), plain::kDayStartDefaultMin).size();
  const long nightStamp = (long)txb::stampOf(night.items.back(), plain::kDayStartDefaultMin).size();
  TEST_ASSERT_TRUE_MESSAGE(dayStamp > 0 && nightStamp > dayStamp, line);
  TEST_ASSERT_TRUE_MESSAGE(envelope <= (long)txb::kOverheadEnvelope, line);
  TEST_ASSERT_TRUE_MESSAGE(owner <= (long)txb::kOverheadPerOwnerMessage + dayStamp, line);
  TEST_ASSERT_TRUE_MESSAGE(nightOwner <= (long)txb::kOverheadPerOwnerMessage + nightStamp, line);
  TEST_ASSERT_TRUE_MESSAGE(nightLabelOk, (std::string(line) + " " + nightWhy).c_str());
  TEST_ASSERT_TRUE_MESSAGE(pair + isError <= (long)(txb::kOverheadPerCall + txb::kOverheadPerResult), line);
  TEST_ASSERT_TRUE_MESSAGE(owner + fileImage <= (long)(txb::kOverheadPerOwnerMessage + dayStamp + txb::kOverheadPerPhotoLabel), line);
  // Over random transcripts the bound of test_tx_overhead holds.
  for (unsigned seed = 1; seed <= 200; ++seed) {
    Rig r;
    GenOpts go;
    go.thinking = false;   // RB-E1a's meta items are counted by the engine-level case
    const txb::Convo c = randomConvo(seed, r, go);
    RENDER_OK(r, c, out);
    txb::CoverInput cov = coverOf(c, 0, 0);
    txb::OverheadInput in;
    txb::renderedText(c, cov, in);
    if (out.body.find("file_p") == std::string::npos) in.itemBytes += 0;
    in.bodyBytes = out.body.size();
    in.systemBytes = txb::systemJsonBytes(out.view);
    in.toolsBytes = txb::toolsJsonBytes(out.view);
    long over = 0;
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), ("seed " + std::to_string(seed) + ": " + why).c_str());
  }
}

}  // namespace

#include "parse_tests.h"

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_checker_accepts_the_documented_shape_and_rejects_what_the_api_refuses);
  RUN_TEST(test_the_day_renders_bodies_the_api_accepts_at_every_pointer);
  RUN_TEST(test_the_same_transcript_and_facts_give_the_same_bytes);
  RUN_TEST(test_every_call_has_exactly_one_result_after_it_whatever_was_stored);
  RUN_TEST(test_a_block_stored_between_a_call_and_its_result_waits_for_the_result);
  RUN_TEST(test_the_four_cache_points_sit_at_the_stated_places);
  RUN_TEST(test_nothing_before_a_cache_point_changes_when_items_are_appended);
  RUN_TEST(test_no_string_passes_the_guard_and_every_body_is_valid_utf8);
  RUN_TEST(test_a_cut_or_broken_call_renders_as_an_empty_object);
  RUN_TEST(test_stubs_photo_stubs_and_the_omission_line_are_the_rules_texts);
  RUN_TEST(test_each_rung_of_the_ladder_changes_the_bytes_and_keeps_every_pair);
  RUN_TEST(test_omitting_whole_turns_frees_at_least_the_loops_lower_bound);
  RUN_TEST(test_photos_follow_the_open_turns_visible_set);
  RUN_TEST(test_signed_thinking_rides_the_open_turn_only);
  RUN_TEST(test_the_envelope_carries_the_facts_and_no_orch_turn);
  RUN_TEST(test_a_refused_allocation_at_every_site_answers_a_status);
  RUN_TEST(test_pointer_moves_superseded_reads_and_thru_come_from_the_stored_items);
  RUN_TEST(test_a_new_turn_never_rewrites_the_message_a_turn_ended_with);
  RUN_TEST(test_an_owner_message_is_its_stamp_and_its_text);
  RUN_TEST(test_outside_content_without_a_stub_is_never_sent_whole);
  RUN_TEST(test_a_missing_bulk_file_sends_a_marker_and_logs_once_per_turn);
  RUN_TEST(test_stored_states_the_api_refuses_are_rendered_into_ones_it_takes);
  RUN_TEST(test_a_ladder_rung_between_rounds_of_a_turn_with_signed_thinking_is_accepted_by_the_provider);
  RUN_TEST(test_the_measured_framing_per_block_is_inside_the_battery_allowances);
  parse_tests::run();
  return UNITY_END();
}
